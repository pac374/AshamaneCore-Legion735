/*
 * Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* ScriptData
Name: arttest_commandscript
Comment: .arttest - in-game checks for the artifact trait scripts (spell_artifact_traits*.cpp). Each test prepares the
         situation on the GM's character (trait aura, cooldown, health, training dummy), triggers the ability and reports
         PASS/FAIL in chat and in the server log. Traits whose result is a damage number are listed as manual checks.
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PhasingHandler.h"
#include "Player.h"
#include "RBAC.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// counters written by spell_artifact_traits_ext.cpp
namespace ArtifactTraitTest
{
    extern int32 LastObsidianLancePct;
    extern int32 LastBalancedBladesPct;
    extern uint32 GlacialEruptionCasts;
    extern uint32 KnightHits;
    extern uint32 SacredDawnHits;
    extern uint32 CosmicRippleCasts;
    extern uint32 TimesAndMeasuresResets;
    extern uint32 KnightCasts;
    extern int32 BalancedBladesTargets;
    extern uint32 ThalkielTicks;
    extern std::map<uint32, uint32> ProcCount;
    // written by spell_artifact_traits_gen.cpp: trait spell id -> how often the script body ran
    extern std::map<uint32, uint32> GenRan;
}

// test hook defined in Player.cpp, right after the anonymous namespace that holds ApplyUnderlightAnglerBonusCatch
// (see the comment there) - forwards to it so the Angling / Better Luck Next Time traits can be tested here (round
// LCF2 R38 follow-up, 26.09.2026)
extern void Test_ApplyUnderlightAnglerBonusCatch(Player* player, GameObject const* go);

namespace
{
enum ArtTestSpells
{
    // Elementalist
    TRAIT_ELEMENTALIST      = 191512,
    SPELL_LAVA_BURST        = 51505,
    SPELL_FIRE_ELEMENTAL    = 198067,
    // Deception
    TRAIT_DECEPTION         = 202755,
    SPELL_SPRINT            = 2983,
    SPELL_FEINT             = 1966,
    // Knight of the Silver Hand
    TRAIT_KNIGHT            = 200302,
    SPELL_JUDGMENT          = 20271,
    SPELL_KNIGHT_BUFF       = 211422,
    // Sweet Souls
    TRAIT_SWEET_SOULS       = 199220,
    SPELL_HEALTHSTONE       = 6262,
    // Fatal Echoes
    TRAIT_FATAL_ECHOES      = 199257,
    SPELL_UNSTABLE_AFFLICTION = 30108,


    // Obsidian Lance
    TRAIT_OBSIDIAN_LANCE    = 238056,
    SPELL_ICE_LANCE_DAMAGE  = 228598,
    // Balanced Blades
    TRAIT_BALANCED_BLADES   = 201470,
    SPELL_BLADE_DANCE_DAMAGE = 199552,
    // Glacial Eruption
    TRAIT_GLACIAL_ERUPTION  = 238128,
    SPELL_EBONBOLT_DAMAGE   = 228599,
    // Sacred Dawn
    TRAIT_SACRED_DAWN       = 238132,
    SPELL_SACRED_DAWN_BUFF  = 243174,
    SPELL_LIGHT_OF_DAWN     = 85222,
    // Cosmic Ripple
    TRAIT_COSMIC_RIPPLE     = 238136,
    SPELL_HOLY_WORD_SERENITY = 2050,
    // Times and Measures
    TRAIT_TIMES_AND_MEASURES = 238100,
    SPELL_DESPERATE_PRAYER  = 19236,
    // Demon Speed
    TRAIT_DEMON_SPEED       = 201469,
    SPELL_BLUR              = 212800,
    SPELL_FEL_RUSH          = 195072,
    // Anguish
    TRAIT_ANGUISH           = 202443,
    // Thal'kiel's Discord
    SPELL_THALKIELS_DISCORD = 211729,

    // round LCF2 R38 follow-up (26.09.2026): the 6 traits R38 finished but left untested, see the class comments
    // above TestDoomWolves/TestShatterTheSouls/TestDeathAndGlory/TestAngling for what each constant is used for
    TRAIT_DOOM_WOLVES           = 198505,
    SPELL_FERAL_SPIRIT          = 198506,   // spell_script_names -> spell_lcf2_sha_doom_wolves_summon (live-checked)
    TRAIT_SHATTER_THE_SOULS_PROC = 203783,  // Shear proc aura; spell_script_names -> spell_dh_shear_proc (live-checked)
    SPELL_DH_SHEAR_ABILITY      = 203782,
    TRAIT_DEATH_AND_GLORY       = 238148,
    SPELL_ODYNS_FURY            = 205545,   // spell_script_names -> gen_arti_war_death_and_glory (live-checked)
    SPELL_ODYNS_GLORY_ENERGIZE  = 243228,
    SPELL_HELYAS_SCORN_LEECH    = 243223,
    ARTIFACT_UNDERLIGHT_ANGLER  = 73,
    ITEM_OLD_BOOT_TEST          = 45199,
    GO_FISHING_BOBBER_TEST      = 35591,    // any FISHINGNODE entry outside the 6 school entries below (world.gameobject_template, live-checked)
    MAP_BROKEN_ISLES_TEST       = 1220,

    NPC_TRAINING_DUMMY      = 31144,
    NPC_REAL_TARGET         = 3121,

    // .arttest gen pet support
    // Durotar Tiger: creature_template 3121 is type 1 (beast), family 2 (cat), type_flags 1 (tameable) - checked live,
    // and IsTameable() is checked again before every use
    NPC_TEST_HUNTER_PET     = NPC_REAL_TARGET,
    // Summon Voidwalker: client SpellEffect 275 = effect 56 (SUMMON_PET), misc value 1860 (Voidwalker)
    SPELL_SUMMON_VOIDWALKER = 697,
    NPC_VOIDWALKER          = 1860
};

// which pet a gen candidate needs
enum GenPetKind : uint8
{
    GEN_PET_NONE    = 0,
    GEN_PET_HUNTER  = 1,    // a hunter pet (HUNTER_PET); only on a hunter test character
    GEN_PET_WARLOCK = 2     // a warlock demon (SUMMON_PET); only on a warlock test character
};

// what the pet does in the test
enum GenPetRole : uint8
{
    GEN_PET_PRESENT   = 0,  // the pet only has to exist (Kill Command checks it, Soul Link comes from it)
    GEN_PET_IS_TARGET = 1,  // the ability is cast at the pet (Misdirection on the own pet)
    GEN_PET_IS_CASTER = 2   // the pet casts the ability itself (83381, the pet half of Kill Command)
};

uint32 const UnstableAfflictionDebuffs[] = { 233490, 233496, 233497, 233498, 233499 };

// runs a callback on the player's event queue after a delay
class DelayedTestEvent : public BasicEvent
{
public:
    explicit DelayedTestEvent(std::function<void()> fn) : _fn(std::move(fn)) { }

    bool Execute(uint64 /*execTime*/, uint32 /*diff*/) override
    {
        _fn();
        return true;
    }

private:
    std::function<void()> _fn;
};
}

class arttest_commandscript : public CommandScript
{
public:
    arttest_commandscript() : CommandScript("arttest_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> commandTable =
        {
            { "arttest", rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleArtTest, "" },
        };
        return commandTable;
    }

    static void Report(ChatHandler* handler, char const* name, bool pass, std::string const& detail)
    {
        std::string line = std::string("[arttest] ") + name + ": " + (pass ? "PASS" : "FAIL") + " - " + detail;
        handler->SendSysMessage(line.c_str());
        TC_LOG_INFO("server", "%s", line.c_str());
    }

    // removes the training dummies of earlier tests so that they are not counted as targets
    static void CleanupDummies(Player* player)
    {
        std::list<Creature*> dummies;
        player->GetCreatureListWithEntryInGrid(dummies, NPC_TRAINING_DUMMY, 80.0f);
        for (Creature* creature : dummies)
            if (TempSummon* summon = creature->ToTempSummon())
                summon->UnSummon();
    }

    // A plain hostile creature (Durotar Tiger, no script) that really takes damage, held in place and passive.
    // The training dummy cannot be used for damage checks: its script sets all damage to zero.
    static Creature* SpawnRealTarget(Player* player, float distance = 6.0f, float angle = 0.0f)
    {
        Creature* target = player->SummonCreature(NPC_REAL_TARGET, player->GetNearPosition(distance, angle), TEMPSUMMON_TIMED_DESPAWN, 60000);
        if (target)
        {
            target->SetReactState(REACT_PASSIVE);
            target->SetControlled(true, UNIT_STATE_ROOT);
            target->SetMaxHealth(2000000000);
            target->SetFullHealth();
        }
        return target;
    }

    static TempSummon* SpawnDummy(Player* player)
    {
        TempSummon* dummy = player->SummonCreature(NPC_TRAINING_DUMMY, player->GetNearPosition(6.0f, 0.0f), TEMPSUMMON_TIMED_DESPAWN, 60000);
        if (dummy)
        {
            // the stock training dummy has almost no health, damage tests need a real health pool
            dummy->SetMaxHealth(2000000000);
            dummy->SetFullHealth();
        }
        return dummy;
    }

    // Elementalist: Lava Burst reduces the remaining cooldown of Fire Elemental
    static bool TestElementalist(ChatHandler* handler, Player* player)
    {

        SpellInfo const* elemental = sSpellMgr->GetSpellInfo(SPELL_FIRE_ELEMENTAL);
        TempSummon* dummy = SpawnDummy(player);
        if (!elemental || !dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_ELEMENTALIST);
        player->AddAura(TRAIT_ELEMENTALIST, player);
        player->GetSpellHistory()->AddCooldown(SPELL_FIRE_ELEMENTAL, 0, std::chrono::seconds(60));
        uint32 before = player->GetSpellHistory()->GetRemainingCooldown(elemental);

        player->CastSpell(dummy, SPELL_LAVA_BURST, TRIGGERED_FULL_MASK);
        uint32 after = player->GetSpellHistory()->GetRemainingCooldown(elemental);

        // 2 seconds are expected; allow the time that passed between the two reads
        bool pass = before > after && before - after >= 1500;
        Report(handler, "elementalist", pass, "cooldown before " + std::to_string(before) + " ms, after " + std::to_string(after) + " ms (expected about 2000 ms less)");

        player->GetSpellHistory()->ResetCooldown(SPELL_FIRE_ELEMENTAL, true);
        player->RemoveAurasDueToSpell(TRAIT_ELEMENTALIST);
        return true;
    }

    // Deception: Sprint casts Feint
    static bool TestDeception(ChatHandler* handler, Player* player)
    {

        player->RemoveAurasDueToSpell(SPELL_FEINT);
        player->RemoveAurasDueToSpell(TRAIT_DECEPTION);
        player->AddAura(TRAIT_DECEPTION, player);
        player->CastSpell(player, SPELL_SPRINT, TRIGGERED_FULL_MASK);

        Report(handler, "deception", player->HasAura(SPELL_FEINT), "Feint aura present after Sprint");
        player->RemoveAurasDueToSpell(SPELL_FEINT);
        player->RemoveAurasDueToSpell(TRAIT_DECEPTION);
        return true;
    }

    // Knight of the Silver Hand: Judgment gives the damage reduction buff
    static bool TestKnight(ChatHandler* handler, Player* player)
    {

        Creature* dummy = SpawnRealTarget(player);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(SPELL_KNIGHT_BUFF);
        player->RemoveAurasDueToSpell(TRAIT_KNIGHT);
        player->AddAura(TRAIT_KNIGHT, player);
        ArtifactTraitTest::KnightHits = 0;
        ArtifactTraitTest::KnightCasts = 0;
        bool cast = player->CastSpell(dummy, SPELL_JUDGMENT, TRIGGERED_FULL_MASK);

        // Judgment may hit after a short flight time, so the result is read later
        ObjectGuid playerGuid = player->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, cast]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            bool pass = p->HasAura(SPELL_KNIGHT_BUFF);
            std::string line = std::string("[arttest] knight: ") + (pass ? "PASS" : "FAIL") + " - buff 211422 " + (pass ? "present" : "missing") + " 2 seconds after Judgment (cast " + (cast ? "accepted" : "rejected") + ", spell reached the cast step " + std::to_string(ArtifactTraitTest::KnightCasts) + " times, hit hook ran " + std::to_string(ArtifactTraitTest::KnightHits) + " times)";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            p->RemoveAurasDueToSpell(SPELL_KNIGHT_BUFF);
            p->RemoveAurasDueToSpell(TRAIT_KNIGHT);
        }), player->m_Events.CalculateTime(2000));

        handler->SendSysMessage("[arttest] knight: waiting 2 seconds for the result ...");
        return true;
    }

    // Sweet Souls: Healthstone heals for more with the trait
    static bool TestSweetSouls(ChatHandler* handler, Player* player)
    {

        uint32 maxHealth = player->GetMaxHealth();
        player->RemoveAurasDueToSpell(TRAIT_SWEET_SOULS);

        player->SetHealth(maxHealth / 10);
        player->CastSpell(player, SPELL_HEALTHSTONE, TRIGGERED_FULL_MASK);
        uint32 gainWithout = player->GetHealth() - maxHealth / 10;

        player->AddAura(TRAIT_SWEET_SOULS, player);
        player->SetHealth(maxHealth / 10);
        player->CastSpell(player, SPELL_HEALTHSTONE, TRIGGERED_FULL_MASK);
        uint32 gainWith = player->GetHealth() - maxHealth / 10;

        // the trait adds 25 percent of maximum health on top of the normal Healthstone heal
        bool pass = gainWith > gainWithout && gainWith - gainWithout >= maxHealth / 5;
        Report(handler, "sweetsouls", pass, "Healthstone heal " + std::to_string(gainWithout) + " without, " + std::to_string(gainWith) + " with the trait (max health " + std::to_string(maxHealth) + ")");

        player->RemoveAurasDueToSpell(TRAIT_SWEET_SOULS);
        player->SetHealth(maxHealth);
        return true;
    }

    // Fatal Echoes: an expiring Unstable Affliction is reapplied (chance forced to 100 percent)
    static bool TestFatalEchoes(ChatHandler* handler, Player* player)
    {

        TempSummon* dummy = SpawnDummy(player);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_FATAL_ECHOES);
        Aura* trait = player->AddAura(TRAIT_FATAL_ECHOES, player);
        if (!trait || !trait->GetEffect(EFFECT_0))
            return false;
        trait->GetEffect(EFFECT_0)->SetAmount(100);

        player->CastSpell(dummy, SPELL_UNSTABLE_AFFLICTION, TRIGGERED_FULL_MASK);

        Aura* debuff = nullptr;
        for (uint32 id : UnstableAfflictionDebuffs)
            if (!debuff)
                debuff = dummy->GetAura(id, player->GetGUID());
        if (!debuff)
        {
            Report(handler, "fatalechoes", false, "Unstable Affliction debuff was not applied to the dummy");
            return true;
        }

        // let it expire and look again after the tick that follows
        debuff->SetDuration(500);
        ObjectGuid playerGuid = player->GetGUID();
        ObjectGuid dummyGuid = dummy->GetGUID();
        ChatHandler* chat = handler;
        (void)chat;
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, dummyGuid]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            Unit* target = ObjectAccessor::GetUnit(*p, dummyGuid);
            bool reapplied = false;
            if (target)
                for (uint32 id : UnstableAfflictionDebuffs)
                    if (target->GetAura(id, p->GetGUID()))
                        reapplied = true;

            std::string line = std::string("[arttest] fatalechoes: ") + (reapplied ? "PASS" : "FAIL") + " - Unstable Affliction " + (reapplied ? "was" : "was not") + " reapplied after it expired";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            p->RemoveAurasDueToSpell(TRAIT_FATAL_ECHOES);
        }), player->m_Events.CalculateTime(3000));

        handler->SendSysMessage("[arttest] fatalechoes: waiting 3 seconds for the result ...");
        return true;
    }

    static int32 TraitAmount(Player* player, uint32 traitId)
    {
        AuraEffect const* effect = player->GetAuraEffect(traitId, EFFECT_0);
        return effect ? effect->GetAmount() : 0;
    }

    // Obsidian Lance: the script raises Ice Lance damage against frozen targets only
    static bool TestObsidianLance(ChatHandler* handler, Player* player)
    {
        TempSummon* dummy = SpawnDummy(player);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_OBSIDIAN_LANCE);
        player->AddAura(TRAIT_OBSIDIAN_LANCE, player);
        int32 expected = TraitAmount(player, TRAIT_OBSIDIAN_LANCE);

        dummy->ModifyAuraState(AURA_STATE_FROZEN, true);
        ArtifactTraitTest::LastObsidianLancePct = -1;
        player->CastSpell(dummy, SPELL_ICE_LANCE_DAMAGE, TRIGGERED_FULL_MASK);
        int32 frozen = ArtifactTraitTest::LastObsidianLancePct;

        dummy->ModifyAuraState(AURA_STATE_FROZEN, false);
        ArtifactTraitTest::LastObsidianLancePct = -1;
        player->CastSpell(dummy, SPELL_ICE_LANCE_DAMAGE, TRIGGERED_FULL_MASK);
        int32 notFrozen = ArtifactTraitTest::LastObsidianLancePct;

        bool pass = expected > 0 && frozen == expected && notFrozen == -1;
        Report(handler, "obsidianlance", pass, "bonus " + std::to_string(frozen) + "% vs frozen target (expected " + std::to_string(expected) + "%), " + (notFrozen == -1 ? "none" : std::to_string(notFrozen) + "%") + " vs unfrozen target (expected none)");
        player->RemoveAurasDueToSpell(TRAIT_OBSIDIAN_LANCE);
        return true;
    }

    // Balanced Blades: bonus per target hit by Blade Dance, three dummies close together
    static bool TestBalancedBlades(ChatHandler* handler, Player* player)
    {
        for (int i = 0; i < 3; ++i)
            player->SummonCreature(NPC_TRAINING_DUMMY, player->GetNearPosition(3.0f, i * 2.1f), TEMPSUMMON_TIMED_DESPAWN, 60000);

        player->RemoveAurasDueToSpell(TRAIT_BALANCED_BLADES);
        player->AddAura(TRAIT_BALANCED_BLADES, player);
        int32 perTarget = TraitAmount(player, TRAIT_BALANCED_BLADES);

        // every dummy within reach counts as a target, also those of earlier tests
        ArtifactTraitTest::BalancedBladesTargets = 0;
        ArtifactTraitTest::LastBalancedBladesPct = -1;
        player->CastSpell(player, SPELL_BLADE_DANCE_DAMAGE, TRIGGERED_FULL_MASK);
        int32 result = ArtifactTraitTest::LastBalancedBladesPct;
        int32 targets = ArtifactTraitTest::BalancedBladesTargets;

        // the target selection belongs to the core; the trait must add its percentage once per target the spell selected
        bool pass = perTarget > 0 && targets >= 3 && result == perTarget * targets;
        Report(handler, "balancedblades", pass, "bonus " + std::to_string(result) + "% for " + std::to_string(targets) + " selected targets (expected " + std::to_string(perTarget * targets) + "%; the spell must select at least the three dummies)");
        player->RemoveAurasDueToSpell(TRAIT_BALANCED_BLADES);
        return true;
    }

    // Glacial Eruption: the delayed pillar spell is cast after Ebonbolt hits
    static bool TestGlacialEruption(ChatHandler* handler, Player* player)
    {
        TempSummon* dummy = SpawnDummy(player);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_GLACIAL_ERUPTION);
        player->AddAura(TRAIT_GLACIAL_ERUPTION, player);
        uint32 before = ArtifactTraitTest::GlacialEruptionCasts;
        player->CastSpell(dummy, SPELL_EBONBOLT_DAMAGE, TRIGGERED_FULL_MASK);

        ObjectGuid playerGuid = player->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, before]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            bool pass = ArtifactTraitTest::GlacialEruptionCasts > before;
            std::string line = std::string("[arttest] glacialeruption: ") + (pass ? "PASS" : "FAIL") + " - ice pillar spell " + (pass ? "was" : "was not") + " cast after the delay";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            p->RemoveAurasDueToSpell(TRAIT_GLACIAL_ERUPTION);
        }), player->m_Events.CalculateTime(2000));

        handler->SendSysMessage("[arttest] glacialeruption: waiting 2 seconds for the result ...");
        return true;
    }

    // Sacred Dawn: Light of Dawn heals the caster too, which must give the buff
    static bool TestSacredDawn(ChatHandler* handler, Player* player)
    {
        player->RemoveAurasDueToSpell(SPELL_SACRED_DAWN_BUFF);
        player->RemoveAurasDueToSpell(TRAIT_SACRED_DAWN);
        player->AddAura(TRAIT_SACRED_DAWN, player);
        ArtifactTraitTest::SacredDawnHits = 0;
        bool cast = player->CastSpell(player, SPELL_LIGHT_OF_DAWN, TRIGGERED_FULL_MASK);

        Report(handler, "sacreddawn", player->HasAura(SPELL_SACRED_DAWN_BUFF), std::string("buff 243174 present after Light of Dawn (cast ") + (cast ? "accepted" : "rejected") + ", trait script ran " + std::to_string(ArtifactTraitTest::SacredDawnHits) + " times)");
        player->RemoveAurasDueToSpell(SPELL_SACRED_DAWN_BUFF);
        player->RemoveAurasDueToSpell(TRAIT_SACRED_DAWN);
        return true;
    }

    // Cosmic Ripple: the heal is cast when the cooldown of Holy Word: Serenity is reduced to zero and when it runs out by itself
    static bool TestCosmicRipple(ChatHandler* handler, Player* player)
    {
        player->RemoveAurasDueToSpell(TRAIT_COSMIC_RIPPLE);
        player->AddAura(TRAIT_COSMIC_RIPPLE, player);

        uint32 before = ArtifactTraitTest::CosmicRippleCasts;
        player->GetSpellHistory()->AddCooldown(SPELL_HOLY_WORD_SERENITY, 0, std::chrono::hours(1));
        player->GetSpellHistory()->ModifyCooldown(SPELL_HOLY_WORD_SERENITY, -2 * 3600 * 1000);
        Report(handler, "cosmicripple (reduced to zero)", ArtifactTraitTest::CosmicRippleCasts == before + 1, "heal 243241 cast " + std::to_string(ArtifactTraitTest::CosmicRippleCasts - before) + " times");

        uint32 beforeExpire = ArtifactTraitTest::CosmicRippleCasts;
        player->GetSpellHistory()->AddCooldown(SPELL_HOLY_WORD_SERENITY, 0, std::chrono::seconds(2));
        ObjectGuid playerGuid = player->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, beforeExpire]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            bool pass = ArtifactTraitTest::CosmicRippleCasts == beforeExpire + 1;
            std::string line = std::string("[arttest] cosmicripple (cooldown ran out): ") + (pass ? "PASS" : "FAIL") + " - heal 243241 cast " + std::to_string(ArtifactTraitTest::CosmicRippleCasts - beforeExpire) + " times";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            p->RemoveAurasDueToSpell(TRAIT_COSMIC_RIPPLE);
        }), player->m_Events.CalculateTime(4000));

        handler->SendSysMessage("[arttest] cosmicripple: waiting 4 seconds for the natural expiry ...");
        return true;
    }

    // Times and Measures: 100 simulated hits for 30% of maximum health, Desperate Prayer is put on cooldown before each hit;
    // with a chance of 30% per hit the cooldown must be reset a few times, but not after every hit
    static bool TestTimesAndMeasures(ChatHandler* handler, Player* player)
    {
        TempSummon* dummy = SpawnDummy(player);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_TIMES_AND_MEASURES);
        player->AddAura(TRAIT_TIMES_AND_MEASURES, player);
        uint32 const hits = 100;
        uint32 resets = 0;
        uint32 damage = player->GetMaxHealth() * 30 / 100;
        uint32 before = ArtifactTraitTest::TimesAndMeasuresResets;
        for (uint32 i = 0; i < hits; ++i)
        {
            player->GetSpellHistory()->AddCooldown(SPELL_DESPERATE_PRAYER, 0, std::chrono::hours(1));
            DamageInfo damageInfo(dummy, player, damage, nullptr, SPELL_SCHOOL_MASK_NORMAL, DIRECT_DAMAGE, BASE_ATTACK);
            dummy->ProcSkillsAndAuras(player, PROC_FLAG_DONE_MELEE_AUTO_ATTACK, PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK, PROC_SPELL_TYPE_DAMAGE, PROC_SPELL_PHASE_HIT, PROC_HIT_NORMAL, nullptr, &damageInfo, nullptr);
            if (!player->GetSpellHistory()->HasCooldown(SPELL_DESPERATE_PRAYER))
                ++resets;
        }

        Report(handler, "timesandmeasures", resets > 0 && resets < hits && resets == ArtifactTraitTest::TimesAndMeasuresResets - before,
            "Desperate Prayer reset " + std::to_string(resets) + " of " + std::to_string(hits) + " hits for 30% of maximum health (expected about 30)");
        player->GetSpellHistory()->ResetCooldown(SPELL_DESPERATE_PRAYER, true);
        player->RemoveAurasDueToSpell(TRAIT_TIMES_AND_MEASURES);
        return true;
    }

    // Demon Speed: Blur restores charges of Fel Rush
    static bool TestDemonSpeed(ChatHandler* handler, Player* player)
    {
        SpellInfo const* felRush = sSpellMgr->GetSpellInfo(SPELL_FEL_RUSH);
        if (!felRush || !felRush->ChargeCategoryId)
        {
            Report(handler, "demonspeed", false, "Fel Rush has no charge category");
            return true;
        }

        uint32 category = felRush->ChargeCategoryId;
        for (int i = 0; i < 10 && player->GetSpellHistory()->ConsumeCharge(category); ++i) { }
        bool emptied = !player->GetSpellHistory()->HasCharge(category);

        player->RemoveAurasDueToSpell(TRAIT_DEMON_SPEED);
        player->AddAura(TRAIT_DEMON_SPEED, player);
        player->CastSpell(player, SPELL_BLUR, TRIGGERED_FULL_MASK);
        bool restored = player->GetSpellHistory()->HasCharge(category);

        Report(handler, "demonspeed", emptied && restored, std::string("Fel Rush charges emptied: ") + (emptied ? "yes" : "no") + ", restored by Blur: " + (restored ? "yes" : "no"));
        player->RemoveAurasDueToSpell(TRAIT_DEMON_SPEED);
        return true;
    }

    // Anguish: removing the debuff deals damage with its stacks
    static bool TestAnguish(ChatHandler* handler, Player* player)
    {
        Creature* dummy = SpawnRealTarget(player);
        if (!dummy)
            return false;

        Aura* debuff = player->AddAura(TRAIT_ANGUISH, dummy);
        if (!debuff)
        {
            Report(handler, "anguish", false, "debuff 202443 could not be applied to the dummy");
            return true;
        }

        debuff->SetStackAmount(3);
        uint32 before = dummy->GetHealth();
        dummy->RemoveAurasDueToSpell(TRAIT_ANGUISH);
        uint32 after = dummy->GetHealth();

        Report(handler, "anguish", after < before, "dummy health " + std::to_string(before) + " -> " + std::to_string(after) + " after the debuff was removed");
        return true;
    }

    // Thal'kiel's Discord: the area trigger damages the target area repeatedly
    static bool TestThalkielsDiscord(ChatHandler* handler, Player* player)
    {
        Creature* dummy = SpawnRealTarget(player);
        if (!dummy)
            return false;

        dummy->SetFullHealth();
        uint32 before = dummy->GetHealth();
        ArtifactTraitTest::ThalkielTicks = 0;
        player->CastSpell(dummy, SPELL_THALKIELS_DISCORD, true);

        ObjectGuid playerGuid = player->GetGUID();
        ObjectGuid dummyGuid = dummy->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, dummyGuid, before]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            Unit* target = ObjectAccessor::GetUnit(*p, dummyGuid);
            uint32 after = target ? target->GetHealth() : before;
            bool pass = ArtifactTraitTest::ThalkielTicks >= 1;
            std::string line = std::string("[arttest] thalkielsdiscord: ") + (pass ? "PASS" : "FAIL") + " - target health " + std::to_string(before) + " -> " + std::to_string(after) + " after 3 seconds, area trigger damage casts: " + std::to_string(ArtifactTraitTest::ThalkielTicks) + " (the damage itself is 0 for a character without spell power)";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
        }), player->m_Events.CalculateTime(3000));

        handler->SendSysMessage("[arttest] thalkielsdiscord: waiting 3 seconds for the result ...");
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------
    // round LCF2 R38 follow-up (2026-09-26): R38 (report lcf2r38_2026-09-26_traits_final.md) finished 12 traits but
    // only added a test case for one of them (Echoing Stars, already in GenTraits below). The 4 tests in this block
    // cover the other 6 relevant traits; Doom Wolves/Shatter the Souls/Death and Glory only got comment updates in
    // R38 (no logic change, re-verified here as-is), the Angling traits and Better Luck Next Time are new logic in
    // Player.cpp. Test Power 2 (247841) needs no test case: R38 closed it as (c) - a pure Blizzard QA test entry
    // (ArtifactID 0, no tree position, part of the "Test Power 1-8" family) with nothing to implement; this file was
    // checked for an existing (possibly misnamed) 247841 test case before writing this comment - there was none.
    // ---------------------------------------------------------------------------------------------------------

    // Doom Wolves (198505): each Feral Spirit wolf gets an extra elemental attack on a roughly 5 s cadence
    // (line_cd=5, ANNAHME - R38 found even SimC's own default carries a "// TODO: Proper delay" comment, i.e. no
    // verified value exists anywhere, see report section "Doom Wolves"). This harness cannot give the wolf real
    // owner-target AI the way a live client fight would; it can only put player and dummy in combat and hope the
    // wolf's default guardian AI copies the owner's target. A FAIL where the target's health never drops at all
    // (not even once) most likely means the wolf never acquired a victim - a harness limitation - and should be
    // followed up with a manual check in game, not read as disproving the 5 s value.
    static bool TestDoomWolves(ChatHandler* handler, Player* player)
    {
        if (player->getClass() != CLASS_SHAMAN)
        {
            handler->SendSysMessage("[arttest] doomwolves: SKIP - Feral Spirit (198506) needs a shaman test character (Doom Wolves, trait 198505, is Enhancement Shaman only)");
            TC_LOG_INFO("server", "[arttest] doomwolves: SKIP - needs a shaman test character");
            return true;
        }

        Creature* dummy = SpawnRealTarget(player, 4.0f);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_DOOM_WOLVES);
        player->AddAura(TRAIT_DOOM_WOLVES, player);
        player->SetInCombatWith(dummy);
        dummy->SetInCombatWith(player);
        player->SetTarget(dummy->GetGUID());

        player->GetSpellHistory()->ResetCooldown(SPELL_FERAL_SPIRIT, true);
        bool cast = player->CastSpell(dummy, SPELL_FERAL_SPIRIT, TRIGGERED_FULL_MASK);

        ObjectGuid playerGuid = player->GetGUID();
        ObjectGuid dummyGuid = dummy->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, dummyGuid, cast]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;
            Unit* target = ObjectAccessor::GetUnit(*p, dummyGuid);
            uint32 firstCheckHealth = target ? target->GetHealth() : 0;
            std::string line = std::string("[arttest] doomwolves: wolves summon ") + (cast ? "accepted" : "REJECTED")
                + ", target health 2 s after the summon: " + std::to_string(firstCheckHealth) + " - waiting 5 more seconds for a possible second hit ...";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());

            p->m_Events.AddEvent(new DelayedTestEvent([playerGuid, dummyGuid, firstCheckHealth]()
            {
                Player* p2 = ObjectAccessor::FindPlayer(playerGuid);
                if (!p2)
                    return;
                Unit* target2 = ObjectAccessor::GetUnit(*p2, dummyGuid);
                uint32 secondCheckHealth = target2 ? target2->GetHealth() : 0;
                bool secondHitSeen = target2 && secondCheckHealth < firstCheckHealth;
                bool anyHitSeen = target2 && (firstCheckHealth < target2->GetMaxHealth() || secondHitSeen);
                std::string result = secondHitSeen ? "PASS" : (anyHitSeen ? "FAIL" : "FAIL (harness limitation, see class comment - no hit at all was observed, the wolf likely never acquired a victim)");
                std::string line2 = std::string("[arttest] doomwolves: ") + result + " - target health " + std::to_string(firstCheckHealth)
                    + " -> " + std::to_string(secondCheckHealth) + " between the 2 s and 7 s checkpoints (about 5 s apart, matching the ANNAHME line_cd=5)";
                ChatHandler(p2->GetSession()).SendSysMessage(line2.c_str());
                TC_LOG_INFO("server", "%s", line2.c_str());
                p2->RemoveAurasDueToSpell(TRAIT_DOOM_WOLVES);
                p2->SetTarget(ObjectGuid::Empty);
            }), p->m_Events.CalculateTime(5000));
        }), player->m_Events.CalculateTime(2000));

        handler->SendSysMessage("[arttest] doomwolves: waiting about 7 seconds for the result ...");
        return true;
    }

    // Shatter the Souls (212827), tested through the Shear proc aura (203783) it modifies: spell_dh.cpp keeps a
    // static per-caster pity counter (FailedShears) that raises the proc chance to shatterChance[7] = 100% after 7
    // consecutive non-procs - so casting Shear (203782) 10 times in a row makes at least one Shattered Souls proc a
    // certainty, not a probabilistic result. A resulting Lesser Soul Fragment consume energizes Fury (Havoc) or Pain
    // (Vengeance), which is what this test reads. It proves the whole proc chain runs end to end; it does not on its
    // own isolate the Shatter the Souls percentage bonus from the base 8-step table (see report lcf2r38... for that).
    static bool TestShatterTheSouls(ChatHandler* handler, Player* player)
    {
        if (player->getClass() != CLASS_DEMON_HUNTER)
        {
            handler->SendSysMessage("[arttest] shatterthesouls: SKIP - Shear (203782) needs a demon hunter test character (Shatter the Souls, trait 212827, works through the Shear proc aura 203783, Demon Hunter only)");
            TC_LOG_INFO("server", "[arttest] shatterthesouls: SKIP - needs a demon hunter test character");
            return true;
        }

        Creature* dummy = SpawnRealTarget(player, 3.0f);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_SHATTER_THE_SOULS_PROC);
        Aura* procAura = player->AddAura(TRAIT_SHATTER_THE_SOULS_PROC, player);
        if (!procAura)
        {
            Report(handler, "shatterthesouls", false, "the Shear proc aura (203783) could not be applied to the test character");
            return true;
        }

        player->SetInCombatWith(dummy);
        dummy->SetInCombatWith(player);
        player->SetPower(POWER_FURY, 0);
        player->SetPower(POWER_PAIN, 0);

        uint32 const casts = 10;
        for (uint32 i = 0; i < casts; ++i)
        {
            player->GetSpellHistory()->ResetCooldown(SPELL_DH_SHEAR_ABILITY, true);
            player->CastSpell(dummy, SPELL_DH_SHEAR_ABILITY, TRIGGERED_FULL_MASK);
        }

        ObjectGuid playerGuid = player->GetGUID();
        player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, casts]()
        {
            Player* p = ObjectAccessor::FindPlayer(playerGuid);
            if (!p)
                return;

            int32 fury = p->GetPower(POWER_FURY);
            int32 pain = p->GetPower(POWER_PAIN);
            bool pass = fury > 0 || pain > 0;
            std::string line = std::string("[arttest] shatterthesouls: ") + (pass ? "PASS" : "FAIL")
                + " - after " + std::to_string(casts) + " Shear casts (pity counter guarantees a proc by then): Fury "
                + std::to_string(fury) + ", Pain " + std::to_string(pain)
                + " (a Lesser Soul Fragment consume must energize one of the two for this to be nonzero)";
            ChatHandler(p->GetSession()).SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            p->RemoveAurasDueToSpell(TRAIT_SHATTER_THE_SOULS_PROC);
            p->SetPower(POWER_FURY, 0);
            p->SetPower(POWER_PAIN, 0);
        }), player->m_Events.CalculateTime(2000));

        handler->SendSysMessage("[arttest] shatterthesouls: waiting 2 seconds for the result ...");
        return true;
    }

    // Death and Glory (238148): Odyn's Fury (205545) is empowered by either Odyn (Rage generation, 243228) or Helya
    // (self-heal via health leech, 243223), chosen by an unweighted 50/50 coin flip (SimC + MMO-Champion, both
    // upgraded from UNSICHER to belegt in R38). Odyn's Fury is cast repeatedly and both outcomes are expected to
    // occur at least once; with 30 casts the chance of never seeing one side is 0.5^30, i.e. a FAIL here is a real
    // signal about the coin flip, not bad luck.
    static bool TestDeathAndGlory(ChatHandler* handler, Player* player)
    {
        if (player->getClass() != CLASS_WARRIOR)
        {
            handler->SendSysMessage("[arttest] deathandglory: SKIP - Odyn's Fury (205545) needs a warrior test character (Death and Glory, trait 238148, is warrior-only)");
            TC_LOG_INFO("server", "[arttest] deathandglory: SKIP - needs a warrior test character");
            return true;
        }

        Creature* dummy = SpawnRealTarget(player, 6.0f);
        if (!dummy)
            return false;

        player->RemoveAurasDueToSpell(TRAIT_DEATH_AND_GLORY);
        player->AddAura(TRAIT_DEATH_AND_GLORY, player);
        player->SetInCombatWith(dummy);
        dummy->SetInCombatWith(player);

        bool odynSeen = false;
        bool helyaSeen = false;
        uint32 const rounds = 30;
        for (uint32 i = 0; i < rounds; ++i)
        {
            player->SetPower(POWER_RAGE, 0);
            if (player->GetHealth() == player->GetMaxHealth())
                player->SetHealth(player->GetMaxHealth() - 1);   // leave room to see the Helya heal
            uint32 healthBefore = player->GetHealth();
            dummy->SetFullHealth();

            player->GetSpellHistory()->ResetCooldown(SPELL_ODYNS_FURY, true);
            player->CastSpell(player, SPELL_ODYNS_FURY, TRIGGERED_FULL_MASK);

            if (player->GetPower(POWER_RAGE) > 0)
                odynSeen = true;
            if (player->GetHealth() > healthBefore)
                helyaSeen = true;
        }

        bool pass = odynSeen && helyaSeen;
        Report(handler, "deathandglory", pass, "over " + std::to_string(rounds) + " casts of Odyn's Fury (205545): Odyn (Rage generated via "
            + std::to_string(uint32(SPELL_ODYNS_GLORY_ENERGIZE)) + ") " + (odynSeen ? "seen" : "NOT seen") + ", Helya (self heal via "
            + std::to_string(uint32(SPELL_HELYAS_SCORN_LEECH)) + ") " + (helyaSeen ? "seen" : "NOT seen") + " - both are expected from an unweighted 50/50 coin flip");

        player->RemoveAurasDueToSpell(TRAIT_DEATH_AND_GLORY);
        player->SetFullHealth();
        return true;
    }

    // finds (and if necessary equips, swapping out whatever is in the main hand) an artifact weapon of the given
    // ArtifactID on the test character - reuses GiveArtifactWeapon (which only stores it, in the best slot it can
    // find) and forces the main-hand equip itself, because ApplyUnderlightAnglerBonusCatch only ever looks at
    // GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) - a weapon merely carried in a bag is not enough.
    static Item* EquipTestArtifactWeapon(ChatHandler* handler, Player* player, uint32 artifactId)
    {
        Item* rod = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (rod && rod->GetTemplate()->GetArtifactID() == artifactId)
            return rod;

        if (!GiveArtifactWeapon(handler, player, artifactId))
            return nullptr;

        uint32 itemId = 0;
        for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& proto = pair.second;
            if (proto.GetArtifactID() == artifactId && proto.GetClass() == ITEM_CLASS_WEAPON && proto.GetQuality() == ITEM_QUALITY_ARTIFACT)
            {
                itemId = pair.first;
                break;
            }
        }
        if (!itemId)
            return nullptr;

        Item* item = player->GetItemByEntry(itemId);
        if (!item)
            return nullptr;

        if (item->GetBagSlot() == INVENTORY_SLOT_BAG_0 && item->GetSlot() == EQUIPMENT_SLOT_MAINHAND)
            return item;

        uint16 dest;
        if (player->CanEquipItem(EQUIPMENT_SLOT_MAINHAND, dest, item, true) != EQUIP_ERR_OK)
        {
            handler->PSendSysMessage("[arttest] angling: could not equip item %u (artifact %u) in the main hand", itemId, artifactId);
            return nullptr;
        }
        player->SwapItem(item->GetPos(), dest);
        return player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    }

    // The 6 "X Angling" traits (201880-201884, 201887) and Better Luck Next Time (201943): implemented in
    // Player.cpp (ApplyUnderlightAnglerBonusCatch), hooked into Player::SendLoot right after go->getFishLoot(...) -
    // not a spell/aura path, so this harness cannot reach it through a cast. Instead: equip the Underlight Angler
    // (artifact 73), set the artifact power rank for one school directly on the item (Item::SetArtifactPower - no
    // client interaction needed for that), spawn a real (but not DB-persisted - see WorldObject::SummonGameObject)
    // copy of that school's fishing-pool GameObject template next to the character, and call the exact function the
    // live fishing path calls (Test_ApplyUnderlightAnglerBonusCatch, the forwarding hook added to Player.cpp for
    // this test - see its comment there). Repeated 200 times per case because the chance is only 10/20/30% (ANNAHME
    // per rank); with 200 tries the chance of seeing zero successes even at the lowest rank (10%) is 0.9^200 ~ 7e-10,
    // so a FAIL here is a real signal, not bad luck. A genuine end-to-end test (real GameObject spawned in the
    // world, the harness cannot fake that away) - the one thing it cannot cover is the real client fishing bobber
    // minigame itself, which never runs in this harness at all (also true of every other .arttest case).
    static bool TestAngling(ChatHandler* handler, Player* player)
    {
        struct SchoolCase { uint32 GameObjectEntry; uint32 ArtifactPowerId; uint32 FishItemId; uint8 Rank; char const* Name; };
        static SchoolCase const schools[] =
        {
            { 246488, 1022, 124107, 1, "angling_cursed_queenfish_r1" },
            { 246489, 1023, 124108, 2, "angling_mossgill_perch_r2" },
            { 246490, 1024, 124109, 3, "angling_highmountain_salmon_r3" },
            { 246491, 1025, 124110, 1, "angling_stormray_r1" },
            { 246492, 1026, 124111, 1, "angling_runescale_koi_r1" },
            { 246493, 1027, 124112, 1, "angling_black_barracuda_r1" }
        };

        Item* rod = EquipTestArtifactWeapon(handler, player, ARTIFACT_UNDERLIGHT_ANGLER);
        if (!rod)
        {
            Report(handler, "angling", false, "could not equip an Underlight Angler (artifact 73) test weapon, see message above");
            return true;
        }

        uint32 passed = 0;
        uint32 const tries = 200;
        for (SchoolCase const& school : schools)
        {
            if (!sObjectMgr->GetGameObjectTemplate(school.GameObjectEntry))
            {
                Report(handler, school.Name, false, "gameobject_template " + std::to_string(school.GameObjectEntry) + " not found in the world database");
                continue;
            }

            ItemDynamicFieldArtifactPowers power;
            power.ArtifactPowerId = school.ArtifactPowerId;
            power.PurchasedRank = school.Rank;
            power.CurrentRankWithBonus = school.Rank;
            power.Padding = 0;
            rod->SetArtifactPower(&power, true);

            GameObject* go = player->SummonGameObject(school.GameObjectEntry, *player, QuaternionData(), 30);
            if (!go)
            {
                Report(handler, school.Name, false, "the test gameobject (entry " + std::to_string(school.GameObjectEntry) + ") could not be summoned");
                continue;
            }

            uint32 hits = 0;
            for (uint32 i = 0; i < tries; ++i)
            {
                uint32 countBefore = player->GetItemCount(school.FishItemId, false);
                Test_ApplyUnderlightAnglerBonusCatch(player, go);
                uint32 countAfter = player->GetItemCount(school.FishItemId, false);
                if (countAfter > countBefore)
                {
                    ++hits;
                    player->DestroyItemCount(school.FishItemId, countAfter - countBefore, true);
                }
            }
            go->Delete();

            ItemDynamicFieldArtifactPowers reset = power;
            reset.PurchasedRank = 0;
            reset.CurrentRankWithBonus = 0;
            rod->SetArtifactPower(&reset);

            bool pass = hits > 0;
            if (pass)
                ++passed;
            Report(handler, school.Name, pass, std::to_string(hits) + " of " + std::to_string(tries) + " tries granted item " + std::to_string(school.FishItemId)
                + " (ANNAHME chance " + std::to_string(10 * school.Rank) + "% at rank " + std::to_string(school.Rank) + ")");
        }

        // Better Luck Next Time (201943, ArtifactPower 1028): 5% ANNAHME chance while fishing anywhere in Broken
        // Isles (map 1220), independent of any school pool - a plain fishing bobber entry outside the 6 school
        // entries is used so ApplyUnderlightAnglerBonusCatch actually reaches the "else" branch instead of matching
        // one of the schools above and returning early.
        {
            ItemDynamicFieldArtifactPowers power;
            power.ArtifactPowerId = 1028;
            power.PurchasedRank = 1;
            power.CurrentRankWithBonus = 1;
            power.Padding = 0;
            rod->SetArtifactPower(&power, true);

            bool onBrokenIsles = player->GetMapId() == MAP_BROKEN_ISLES_TEST;
            uint32 hits = 0;
            if (GameObject* go = player->SummonGameObject(GO_FISHING_BOBBER_TEST, *player, QuaternionData(), 30))
            {
                for (uint32 i = 0; i < tries; ++i)
                {
                    uint32 countBefore = player->GetItemCount(ITEM_OLD_BOOT_TEST, false);
                    Test_ApplyUnderlightAnglerBonusCatch(player, go);
                    uint32 countAfter = player->GetItemCount(ITEM_OLD_BOOT_TEST, false);
                    if (countAfter > countBefore)
                    {
                        ++hits;
                        player->DestroyItemCount(ITEM_OLD_BOOT_TEST, countAfter - countBefore, true);
                    }
                }
                go->Delete();

                std::string detail = std::to_string(hits) + " of " + std::to_string(tries) + " tries granted item 45199 (Old Boot, ANNAHME chance 5%)";
                bool pass;
                if (onBrokenIsles)
                    pass = hits > 0;
                else
                {
                    pass = hits == 0;
                    detail += " - test character is on map " + std::to_string(player->GetMapId()) + ", NOT Broken Isles (1220), so 0 hits is the CORRECT result here";
                }
                if (pass)
                    ++passed;
                Report(handler, "angling_better_luck_next_time", pass, detail);
            }
            else
                Report(handler, "angling_better_luck_next_time", false, "the test gameobject (entry " + std::to_string(uint32(GO_FISHING_BOBBER_TEST)) + ") could not be summoned");

            ItemDynamicFieldArtifactPowers reset = power;
            reset.PurchasedRank = 0;
            reset.CurrentRankWithBonus = 0;
            rod->SetArtifactPower(&reset);
        }

        handler->PSendSysMessage("[arttest] angling: %u of %u cases passed", passed, uint32(sizeof(schools) / sizeof(schools[0]) + 1));
        return true;
    }

    // Simulated fight against a training dummy: melee swings, direct spells and a heal are used repeatedly while all proc
    // traits are active, then it is reported which trait handlers ran. Handlers that never ran either need a spell that
    // this battery does not use or the aura has no proc flags in the spell data.
    static bool TestCombat(ChatHandler* handler, Player* player)
    {
        // "need" names the condition the trait carries in the client data (SpellAuraOptions.ProcTypeMask and the
        // description), so a handler that never ran can be judged without looking the trait up again.
        struct ProcTrait { uint32 id; char const* name; char const* need; };
        static ProcTrait const traits[] =
        {
            { 201463, "Deceiver's Fury",      "taken hits (mask 0xA22A8), the description asks for a dodge" },
            { 201471, "Inner Demons",         "Chaos Strike, real time proc (114 per minute, 5 s category cooldown)" },
            { 201472, "Rage of the Illidari", "Fury of the Illidari has to end" },
            { 213010, "Charred Warblades",    "own fire damage (mask 0x51214)" },
            { 213017, "Fueled by Pain",       "a consumed soul fragment (86 per minute)" },
            { 212817, "Fiery Demise",         "Fiery Brand on the target" },
            { 199471, "Soul Flame",           "a killed target" },
            { 199472, "Wrath of Consumption", "a killed target" },
            { 196305, "Eternal Struggle",     "Life Tap (mask 0x4000, own positive magic spell)" },
            { 196301, "Devourer of Life",     "Drain Life (mask 0x50000)" },
            { 196236, "Soulsnatcher",         "Chaos Bolt (mask 0x10000)" },
            { 219415, "Dimension Ripper",     "Incinerate, 5% chance" },
            { 211720, "Thal'kiel's Discord",  "Demonbolt / Demonwrath, 15% chance, 5 s category cooldown" }
        };
        // typical damage and heal spells of the classes that own these traits; unknown ids are skipped.
        // The spells added in the second row are the ones the conditions above ask for and that the first
        // version of the battery did not contain.
        static uint32 const battery[] =
        {
            162794, 162243, 228477, 203782, 204021, 198013,                 // Demon Hunter
            686, 116858, 172, 980, 30108, 348, 29722, 603, 157695, 105174,  // Warlock
            228598, 6262,
            201467,                                                         // Fury of the Illidari (its end drives 201472)
            1454, 234153, 193440,                                           // Life Tap, Drain Life, Demonwrath
            187827, 200166                                                  // Metamorphosis (DH), so fragment/fire traits see their state
        };
        // spells the dummy casts at the character so that the "taken" proc flags are reached as well
        static uint32 const takenBattery[] = { 686, 348 };

        TempSummon* dummy = SpawnDummy(player);
        if (!dummy)
            return false;

        for (ProcTrait const& trait : traits)
        {
            player->RemoveAurasDueToSpell(trait.id);
            player->AddAura(trait.id, player);
            ArtifactTraitTest::ProcCount[trait.id] = 0;
        }

        player->SetInCombatWith(dummy);
        dummy->SetInCombatWith(player);
        for (int round = 0; round < 40; ++round)
        {
            player->AttackerStateUpdate(dummy, BASE_ATTACK);
            for (uint32 spellId : battery)
                if (sSpellMgr->GetSpellInfo(spellId))
                    player->CastSpell(spellId == 6262 || spellId == 1454 || spellId == 201467 || spellId == 187827 || spellId == 200166
                        ? static_cast<Unit*>(player) : static_cast<Unit*>(dummy), spellId, TRIGGERED_FULL_MASK);

            // taken side: the dummy hits back, otherwise every trait with a "taken" proc flag can never run
            dummy->AttackerStateUpdate(player, BASE_ATTACK);
            for (uint32 spellId : takenBattery)
                if (sSpellMgr->GetSpellInfo(spellId))
                    dummy->CastSpell(player, spellId, TRIGGERED_FULL_MASK);

            // Fury of the Illidari has to end for Rage of the Illidari, the aura is removed again every round
            player->RemoveAurasDueToSpell(201467);

            dummy->SetFullHealth();
            player->SetFullHealth();
        }

        // kill phase: two traits only react to a killed target (PROC_FLAG_KILL), which the rounds above never produce
        for (int kill = 0; kill < 3; ++kill)
        {
            if (Creature* victim = SpawnRealTarget(player))
            {
                victim->SetMaxHealth(100);
                victim->SetFullHealth();
                player->SetInCombatWith(victim);
                player->Kill(victim, false);
            }
        }

        uint32 fired = 0;
        for (ProcTrait const& trait : traits)
        {
            uint32 count = ArtifactTraitTest::ProcCount[trait.id];
            fired += count ? 1 : 0;
            std::string line = std::string("[arttest] combat ") + trait.name + ": "
                + (count ? "handler ran " + std::to_string(count) + " times"
                         : std::string("handler never ran - needs ") + trait.need);
            handler->SendSysMessage(line.c_str());
            TC_LOG_INFO("server", "%s", line.c_str());
            player->RemoveAurasDueToSpell(trait.id);
        }

        handler->PSendSysMessage("[arttest] combat: %u of %u proc traits ran their handler (40 rounds: melee swing done and taken, %u spells, heal, then 3 kills)", fired, uint32(sizeof(traits) / sizeof(traits[0])), uint32(sizeof(battery) / sizeof(battery[0])));
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------
    // ".arttest gen" - the trait candidates from spell_artifact_traits_gen.cpp
    //
    // For every candidate the trait aura is put on the character, the ability the script is bound to is cast at a
    // training dummy (or at the character itself for self targeted abilities) and it is checked that the script
    // body ran. Where the trait produces an aura the presence of that aura is checked too, which is the stronger
    // result. A FAIL here usually means that the SQL binding is missing
    // (tools\apply_trait_candidates.ps1 -Class all) or that the ability could not be cast by this character.
    // ---------------------------------------------------------------------------------------------------------
    struct GenTrait
    {
        uint32 trait;        // trait spell id, the counter key
        uint32 ability;      // spell the script is bound to
        uint32 resultAura;   // aura that must be present afterwards, 0 = only the counter is checked
        bool   selfTarget;   // cast at the character instead of the dummy
        char const* name;
        uint32 targetAura;   // aura the target has to carry for the trait to do anything, 0 = none
        char const* note;    // set: the candidate cannot be checked by this harness, printed as SKIP with this reason
        // --- preconditions the harness produces itself (all optional, 0/false = not used) ---
        uint32 casterAura;      // form/stance the ability needs, put on the character first (e.g. 768 Cat Form)
        uint32 prepSpell;       // spell the character casts on itself first (e.g. 101643 Transcendence)
        int32  targetHealthPct; // > 0: the target is set to this much health first (Execute and friends)
        bool   friendlyTarget;  // cast at a friendly creature instead of the hostile one (ally only abilities)
        float  destRange;       // > 0: cast at a point this many yards in front instead of at a unit
        bool   delayed;         // projectile or channel: the result is only readable a moment later
        bool   nearTarget;      // melee ability: use the target that stands inside Unit::GetMeleeRange (2.5 yd)
        bool   selectTarget;    // the ability reads caster->GetTarget(), so the character selects the target
        bool   needsPower;      // the trait works on the damage dealt, so spell/attack power is added for it
        bool   needsCombat;     // the script reads getHostileRefManager()/threat state, so caster and target are put in combat first
        // --- pet support (see GenAcquirePet / GenReleasePet) ---
        GenPetKind needsPet;    // a pet is provided before the casts and removed right after the candidate
        GenPetRole petRole;     // whether the pet is only present, is the target, or casts the ability
        uint32 petPrepSpell;    // spell the pet casts on itself first, unless its aura is already on the character (Soul Link 108446)
    };

    static GenTrait const* GenTraits(uint32& count)
    {
        static GenTrait const traits[] =
        {
            // priest
            { 197711, 586,    216135, true,  "pri_vestments_of_discipline" },
            { 197766, 47540,  197767, false, "pri_speed_of_the_pious" },
            // Void Torrent is channelled; the cast loop steps the event queue so the channel really starts and
            // the AfterCast handler runs inside the command - the result can be read right away
            { 238101, 205065, 240673, false, "pri_mind_quickening" },
            { 197729, 17,     0,      true,  "pri_shield_of_faith" },
            // the trait only fires when somebody else is shielded, so a friendly creature is used as the target
            { 197781, 17,     0,      false, "pri_share_in_the_light", 0, nullptr, 0, 0, 0, true },
            // Atonement is an ally only aura, it cannot be put on the hostile target
            { 198074, 194384, 198076, false, "pri_sins_of_the_many", 0, nullptr, 0, 0, 0, true },
            // paladin
            { 179546, 205273, 0,      false, "pal_ashes_to_ashes" },
            { 209223, 184092, 0,      true,  "pal_scatter_the_shadows" },
            { 193058, 53385,  0,      true,  "pal_healing_storm" },
            // Avenger's Shield flies to the target, the damage hook runs a moment after the cast
            // the trait turns damage into an absorb, and the test character has no spell power of its own
            { 209389, 31935,  209388, false, "pal_bulwark_of_order", 0, nullptr, 0, 0, 0, false, 0.0f, true, false, false, true },
            // shaman
            { 198248, 108271, 198249, true,  "sha_elemental_healing" },
            { 207355, 2825,   208416, true,  "sha_sense_of_urgency" },
            { 207360, 52042,  0,      false, "sha_queens_decree" },
            { 207362, 114942, 0,      false, "sha_cumulative_upkeep" },
            // spell_sha_crash_lightning::CheckCast reads caster->GetTarget() and needs melee range
            { 198299, 187874, 198300, false, "sha_gathering_storms", 0, nullptr, 0, 0, 0, false, 0.0f, false, true, true },
            { 238106, 25504,  0,      false, "sha_winds_of_change" },
            { 238143, 207778, 0,      true,  "sha_deep_waters" },
            // druid
            { 189787, 48438,  0,      true,  "dru_natures_essence" },
            { 202890, 29166,  202842, true,  "dru_rapid_innervation" },
            { 210579, 5217,   210583, true,  "dru_ashamanes_energy" },
            { 238084, 5217,   0,      true,  "dru_fury_of_ashamane" },
            // Berserk is refused outside of cat form (spell_dru_berserk), so the form is set first
            { 210631, 106951, 210649, true,  "dru_feral_instinct", 0, nullptr, 768 },
            { 238121, 106830, 0,      false, "dru_pawsitive_outlook" },
            // warrior
            // Execute needs a target below 20% health (TargetAuraState), the target is wounded first
            { 200875, 5308,   201009, false, "war_juggernaut", 0, nullptr, 0, 0, 10 },
            // Heroic Leap needs a reachable ground destination in jump range; the harness can only offer a point
            // it computes itself, and spell_warr_heroic_leap::CheckElevation refused every one of them
            { 209483, 6544,   209484, true,  "war_tactical_advance", 0, "Heroic Leap needs a reachable ground destination in jump range (spell_warr_heroic_leap::CheckElevation) - the harness cannot produce one reliably" },
            { 238112, 23881,  0,      false, "war_oathblood", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 209462, 845,    188923, false, "war_one_against_many" },
            { 209573, 845,    0,      false, "war_void_cleave" },
            // mage
            { 227481, 2948,   227482, false, "mage_scorched_earth" },
            { 238091, 194466, 0,      false, "mage_warmth_of_the_phoenix" },
            // monk
            // Transcendence: Transfer is refused without the spirit, so Transcendence is cast first
            { 195380, 119996, 195381, true,  "monk_healing_winds", 0, nullptr, 0, 101643 },
            { 195300, 100784, 195321, false, "monk_transfer_the_power", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 238093, 121253, 0,      false, "monk_stave_off", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            // rogue
            { 238102, 185311, 0,      true,  "rog_dense_concoction" },
            { 192424, 32645,  0,      false, "rog_surge_of_toxins", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 192422, 36554,  5277,   false, "rog_shadow_swiftness" },
            { 242707, 196911, 0,      true,  "rog_shadows_whisper" },
            { 197369, 196911, 0,      true,  "rog_fortunes_bite" },
            // hunter
            { 190514, 781,    190515, true,  "hun_survival_of_the_fittest" },
            { 224764, 186270, 0,      false, "hun_bird_of_prey" },
            { 197047, 118459, 0,      false, "hun_furious_swipes" },
            // Windburst is a shot: the core takes its range from the equipped ranged weapon, without one the
            // check ends in SPELL_FAILED_OUT_OF_RANGE at every distance. A warrior cannot wear a bow or gun.
            { 204219, 204147, 187131, false, "hun_mark_of_the_windrunner", 0, "Windburst is a shot; without a ranged weapon the core answers SPELL_FAILED_OUT_OF_RANGE at every distance - needs a hunter test character" },
            // pet support (25.09.2026): Misdirection is cast at the hunter's own pet, the trait puts 211138 (client:
            // APPLY_AURA 87 MOD_DAMAGE_PERCENT_TAKEN -35) on it
            { 197178, 34477,  211138, false, "hun_hunters_advantage", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, false, false, GEN_PET_HUNTER, GEN_PET_IS_TARGET },
            // pet support (25.09.2026): spell_hun_kill_command::CheckCastMeet needs a live pet within 25 yd of the target.
            // Round 19 (25.09.2026): gen_arti_hun_jaws_of_thunder now hangs on 83381 (the pet's damage half, cast from
            // the script effect of 34026) - needs fixes\r19_2026-09-25_jaws_of_thunder_rebind.sql. The harness keeps
            // casting 34026 so the real chain hunter -> pet -> 83381 is exercised. The trait roll is 10 % per hit.
            { 197162, 34026,  0,      false, "hun_jaws_of_thunder",   0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, true, false, GEN_PET_HUNTER, GEN_PET_PRESENT },
            // death knight
            { 192548, 206930, 0,      false, "dk_blood_feast", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 191721, 49576,  191719, false, "dk_gravitational_pull" },
            { 189097, 49020,  0,      false, "dk_over_powered", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 191494, 55090,  0,      false, "dk_scourge_the_unbeliever", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            // demon hunter
            { 238117, 162794, 0,      false, "dh_chaotic_onslaught", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            // round 2: critical strike chance hook (the target needs the aura named in the trait description)
            // same as Windburst: Aimed Shot is a shot and needs a real ranged weapon
            { 190529, 19434,  0,      false, "hun_marked_for_death", 187131, "Aimed Shot is a shot; without a ranged weapon the core answers SPELL_FAILED_OUT_OF_RANGE at every distance - needs a hunter test character" },
            // round LCF2 R23: the bonus is the client buff 248195 (normally from Colossus Smash), consumed by Mortal Strike
            { 248579, 12294,  0,      false, "war_precise_strikes",  0,      nullptr, 248195, 0, 0, false, 0.0f, false, true },
            // round 2: further traits without a proc entry
            { 214996, 106898, 213698, true,  "dru_roar_of_the_crowd" },
            { 210663, 106830, 210664, true,  "dru_scent_of_blood" },
            { 192428, 79140,  192432, false, "rog_from_the_shadows" },
            // Rip runs through spell_dru_predatory_swiftness, whose CheckCast needs at least one combo point.
            // Unit::SetPower cannot give a class that combo points which its class does not have at all, so this
            // candidate can only be checked with a test character that really uses combo points (druid/rogue).
            { 210666, 1079,   210670, false, "dru_open_wounds", 0, "Rip needs combo points (spell_dru_predatory_swiftness::CheckCast); a test character whose class has no combo points cannot get any" },
            // Chaos Bolt is a missile, the hit hook runs a moment after the cast
            { 238110, 116858, 0,      false, "lock_cry_havoc",       80240,  nullptr, 0, 0, 0, false, 0.0f, true },
            { 200859, 23881,  0,      true,  "war_bloodcraze",       0,      "only heals below 20% health, needs a real fight" },
            // round 2: proc based traits - they need a critical strike, which this harness cannot force
            { 194331, 194331, 0,      true,  "mage_pyretic_incantation", 0,  "AuraScript on the trait, needs a critical strike (see .arttest combat)" },
            { 238076, 85288,  0,      false, "war_pulse_of_battle",      0,  "AuraScript on the trait, needs a critical Raging Blow" },
            { 194093, 34914,  0,      false, "pri_unleash_the_shadows",  0,  "AuraScript on the trait, needs a critical Vampiric Touch tick" },
            { 207285, 77472,  0,      true,  "sha_queen_ascendant",      0,  "AuraScript on the trait, needs a critical direct heal (spell_proc row required)" },
            { 238119, 164812, 240606, false, "dru_circadian_invocation", 0,  "AuraScript on the trait, procs on Moonfire/Sunfire damage in a real fight" },

            // ---------------------------------------------------------------------------------------------
            // round 6 (see C:\LegionServer\reports\trait_candidates_report.md, "Runde 6")
            // ---------------------------------------------------------------------------------------------
            // the script hangs on 83381 (the pet cast of Kill Command). Until 25.09.2026 the harness cast 83381 from the
            // character itself: spell_hun_kill_command_proc then read owner->GetUInt32Value() with owner == nullptr and
            // crashed the server (dump 25-9_4-40-33). 83381 is now cast by a real hunter pet, as in the game, and the
            // core script got a null check of its own.
            { 197199, 83381,  0,      false, "hun_spirit_bond", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, true, false, GEN_PET_HUNTER, GEN_PET_IS_CASTER },
            { 203563, 203563, 0,      true,  "hun_talon_strike", 0, "AuraScript on the trait, needs a real melee auto attack proc - the harness casts triggered, which raises no proc events" },
            { 203749, 203749, 0,      true,  "hun_hunters_bounty", 0, "the trait only fires on PROC_FLAG_KILL - the harness never kills its training dummy" },
            { 189185, 189185, 0,      true,  "dk_hypothermia", 0, "AuraScript on the trait, procs on a Frost Fever (55095) tick in a real fight" },
            { 238115, 238115, 0,      true,  "dk_thronebreaker", 0, "AuraScript on the trait, procs on Obliterate - the triggered casts of the harness raise no proc events" },
            { 238078, 205223, 0,      false, "dk_vampiric_aura" },
            { 197604, 197604, 0,      true,  "rog_embrace_of_darkness", 0, "AuraScript on the trait, needs a real proc (client proc mask 2114560)" },
            // Life Cocoon is an ally only ability
            { 199563, 116849, 0,      false, "monk_mists_of_life", 0, nullptr, 0, 0, 0, true },
            { 199401, 191837, 0,      true,  "monk_light_on_your_feet", 0, "the trait only fires when the Essence Font channel ends by AURA_REMOVE_BY_EXPIRE - the cast loop interrupts every channel right after starting it" },
            // Tiger Palm is a melee ability, the script works on the damage it deals
            { 213116, 100780, 0,      false, "monk_face_palm", 0, nullptr, 0, 0, 0, false, 0.0f, false, true, false, true },
            { 238129, 119582, 215479, true,  "monk_quick_sip" },
            { 238122, 774,    0,      true,  "dru_deep_rooted", 0, "the periodic heal tick has to land on a target below 35% health - the harness sets everybody to full health before every candidate and does not wait for a tick" },
            // 189854 heals allies that carry this druid's Rejuvenation, so the druid puts 774 on himself first
            { 189849, 189854, 0,      true,  "dru_dreamwalker", 0, nullptr, 0, 774 },
            // the heal sits on the periodic ticks of Dispersion, so the result is only readable a moment later
            { 194024, 47585,  0,      true,  "pri_thrive_in_the_shadows", 0, nullptr, 0, 0, 0, false, 0.0f, true },
            { 197779, 186263, 0,      false, "pri_taming_the_shadows", 0, nullptr, 0, 0, 0, true },
            { 198238, 2645,   198240, true,  "sha_spirit_of_the_maelstrom" },
            { 207351, 2645,   0,      true,  "sha_ghost_in_the_mist" },
            // Flamestrike is cast at a ground position, the trait repeats it there
            { 194431, 2120,   0,      false, "mage_aftershocks", 0, nullptr, 0, 0, 0, false, 10.0f },

            // ---------------------------------------------------------------------------------------------
            // round 7
            // ---------------------------------------------------------------------------------------------
            // Carve is a cone that has to catch at least two enemies (_targets < 2 ends the script); the filler
            // dummies of the harness stand inside the cone
            { 203673, 187708, 0,      false, "hun_hellcarver", 0, nullptr, 0, 0, 0, false, 0.0f, false, true, false, true },
            // the trait counts allies carrying this priest's Atonement, so Atonement is put on the priest first
            { 207946, 207946, 0,      false, "pri_lights_wrath", 0, nullptr, 0, 194384, 0, false, 0.0f, false, false, false, true },
            { 213183, 115181, 0,      false, "monk_dragonfire_brew", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            { 207387, 203794, 212988, true,  "dh_painbringer" },
            { 201468, 203794, 0,      true,  "dh_feast_on_the_souls" },
            { 188778, 1160,   188783, true,  "war_might_of_the_vrykul" },
            { 198367, 17364,  0,      false, "sha_stormflurry", 0, nullptr, 0, 0, 0, false, 0.0f, false, true, false, true },
            { 238135, 17,     0,      true,  "pri_aegis_of_wrath" },

            // ---------------------------------------------------------------------------------------------
            // round 8
            // ---------------------------------------------------------------------------------------------
            { 186372, 768,    186370, true,  "dru_mark_of_shifting" },
            { 210650, 768,    0,      true,  "dru_protection_of_ashamane", 0, "the trait fires when the druid shifts OUT of Cat Form (AfterEffectRemove); the harness only casts the form spell and never leaves the form" },
            { 202918, 97547,  0,      false, "dru_light_of_the_sun", 0, "the target has to be casting an interruptible spell (Unit::IsNonMeleeSpellCast) - the training dummies of the harness never cast" },
            { 215773, 215773, 0,      true,  "mage_phoenix_reborn", 0, "AuraScript on the trait, procs on Ignite (12654) damage in a real fight" },
            { 238138, 2818,   0,      false, "rog_sinister_circulation" },

            // ---------------------------------------------------------------------------------------------
            // round 9 (see C:\LegionServer\reports\trait_candidates_report.md, "Runde 5 (SimulationCraft-Abgleich)")
            //
            // 18 traits newly implemented after the SimC cross-check, plus the 5 traits implemented as "unsicher"
            // there. Mass Hysteria (194378) is NOT listed here on purpose: it scales Shadow Word: Pain / Vampiric
            // Touch damage per periodic tick with the current Voidform stack count, which only changes over a real
            // fight - a single cast proves nothing here (see the report for the manual test procedure instead).
            // ---------------------------------------------------------------------------------------------

            // Runic Chills: cast one of the Crystalline Swords damage spells, the cooldown reduction has no aura
            // of its own so only the counter is checked; needsPower gives the sword hit real damage (> 0 required)
            { 238079, 205164, 0,      false, "dk_runic_chills", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, true },
            // Erupting Souls: the bonus is per Soul Fragment consumed by Soul Cleave (own area triggers 8867/6710/
            // 6007 within 25 yd); the harness has no ability that spawns a fragment, so none is ever there to consume
            { 238082, 228477, 0,      false, "dh_erupting_souls", 0, "Soul Cleave only awards the bonus for Soul Fragments it actually consumes - the harness has no ability that spawns a soul fragment near the caster, so none is ever present to consume" },
            // Thunderfist: one stack of 242387 per target struck by Strike of the Windlord's main hand hit (melee)
            { 238131, 222029, 242387, false, "monk_thunderfist", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            // Tornado Kicks: a second hit of the Rising Sun Kick damage spell on the same target, needsPower gives
            // the first hit real damage so the second hit's percentage is not of zero
            { 196082, 185099, 0,      false, "monk_tornado_kicks", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, true },
            // Strength of Xuen: casting Combo Breaker (137384) itself runs DoEffectCalcAmount, which adds the trait
            // value to its amount - no target aura, the bonus is only visible in the counter and in the amount
            { 195267, 137384, 0,      true,  "monk_strength_of_xuen" },
            // Righteous Verdict: a Holy Power finisher (Templar's Verdict) has to fire the trait's own proc; melee
            // range because Templar's Verdict is a melee finishing move
            { 238062, 85256,  238996, false, "pal_righteous_verdict", 0, nullptr, 0, 0, 0, false, 0.0f, false, true },
            // Echo of the Highlord: the script sits on the Templar's Verdict/Divine Storm damage spell itself, not
            // on the button - cast 224266 directly, needsPower for a nonzero first hit
            { 186788, 224266, 0,      false, "pal_echo_of_the_highlord", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, true },
            // Sphere of Insanity: Voidform (casterAura) has to be up so the sphere proc aura (194230) is put on the
            // caster, the target needs this priest's Shadow Word: Pain (targetAura, applied directly instead of
            // cast so no separate SW:P tick has to land first), and caster/target need to be in combat for the
            // hostile reference list the proc reads; needsPower gives Mind Blast a nonzero hit for the proc check
            { 194179, 8092,   0,      false, "pri_sphere_of_insanity", 589, nullptr, 194249, 0, 0, false, 0.0f, false, false, false, true, true },
            // Wind Strikes: cast the Stormbringer proc spell directly, the haste buff is the result aura
            { 198292, 201846, 198293, true,  "sha_wind_strikes" },
            // Greed: the heal script (gen_arti_rog_greed_heal) never marks the counter itself, so the only way to
            // prove the trait fired is through Run Through's proc - which needs combo points (see dru_open_wounds)
            { 202820, 2098,   0,      false, "rog_greed", 0, "Run Through needs combo points (rogue finishing move) to be cast at all, and only its proc marks the counter (the heal script does not) - a test character whose class has no combo points cannot get any, see dru_open_wounds" },
            // Blunderbuss: cast Opportunity (only ever cast by Saber Slash's extra strike in the core, but the
            // script only checks the trait aura) directly, the override actionbar buff is the result aura
            { 202897, 195627, 202848, false, "rog_blunderbuss" },
            // Battle Scars: cast Enrage itself, the health bonus has no aura of its own so only the counter is checked
            { 200857, 184362, 0,      true,  "war_battle_scars" },
            // Reflective Plating: only runs on the DoPrepareProc of Spell Reflection actually reflecting a hit
            // (default proc entry, hit mask REFLECT) - the harness has no incoming spell to reflect
            { 188672, 23920,  0,      true,  "war_reflective_plating", 0, "the script only runs when Spell Reflection actually reflects an incoming spell (DoPrepareProc, hit mask REFLECT) - the harness has no incoming spell to reflect, see .arttest combat for other proc traits that need a real fight" },
            // Rule of Threes: BeforeCast rolls the chance synchronously, no delay needed even though Arcane
            // Missiles itself is channelled
            { 215463, 5143,   187292, false, "mage_rule_of_threes" },
            // Time and Space: the echo needs a second Arcane Explosion within the marker's window: the 25 repeat
            // casts of this harness are consecutive, so every cast after the first should trigger the echo
            { 238126, 1449,   0,      false, "mage_time_and_space" },
            // Aegwynn's Ascendance: the explosion only fires when Evocation ends by AURA_REMOVE_BY_EXPIRE - the
            // cast loop interrupts every channel right after starting it (same limitation as monk_light_on_your_feet)
            { 187680, 12051,  0,      true,  "mage_aegwynns_ascendance", 0, "the explosion only fires when Evocation ends by AURA_REMOVE_BY_EXPIRE - the cast loop interrupts every channel right after starting it (see monk_light_on_your_feet for the same limitation)" },
            // Ashamane's Bite: the target needs this druid's Rip (targetAura, applied directly), Cat Form for Shred,
            // melee range; the result is the Ashamane's Rip copy on the target
            { 210702, 5221,   210705, false, "dru_ashamanes_bite", 1079, nullptr, 768, 0, 0, false, 0.0f, false, true },

            // the following 5 are the "unsicher" candidates of the same round: the script runs, but the exact rule
            // (text vs. SimC, or a detail SimC does not model at all) could not be settled without a live test -
            // each note names precisely what to check in game instead of trusting a PASS/FAIL here
            { 189184, 196770, 0,      false, "dk_frozen_soul", 0, "Remorseless Winter has to run its full duration and expire naturally (AURA_REMOVE_BY_EXPIRE) before the burst fires - longer than this harness's fixed 2 s delayed-check window. Live test: hit several enemies during one Remorseless Winter and let it run out; confirm the burst scales with (enemies hit - 1) x 100% (client text 'for each ADDITIONAL enemy'), not with all enemies hit (SimC's reading)" },
            { 199573, 119611, 0,      false, "monk_dancing_mists", 0, "the spread needs a SECOND ally within 25 yd that does not have this monk's Renewing Mist yet - this harness only spawns one friendly creature. Live test: heal two allies, let Renewing Mist jump, confirm it adds a full second HoT on a genuinely different, lower-health ally instead of restacking on the same one" },
            { 197406, 196819, 0,      true,  "rog_finality", 0, "the buff toggles correctly (grants without it, consumes with it), but SimC additionally scales the 20% by combo points / 5, which is not in the client data. Live test: use Eviscerate/Nightblade with 1 and with 5 combo points and compare the bonus" },
            { 214508, 191037, 0,      false, "dru_echoing_stars", 0, "the 'nearby' radius is Starfall's own effect radius (191034), reused here on SimC's authority, not confirmed as the correct radius for Echoing Stars. Live test: place two enemies just inside and one just outside that radius during Starfall, confirm only the inside ones ever receive the echo" },
            { 191048, 185901, 0,      false, "hun_call_of_the_hunter", 187131, "SimC fires the arrow barrage twice per target on every Marked Shot, but no client value supports a count of two - this implementation fires it once per target. Live test: count the Call of the Hunter hits per target after one Marked Shot and compare against Thas'dorah's tooltip/combat log" },

            // ---------------------------------------------------------------------------------------------
            // pet support (25.09.2026)
            // ---------------------------------------------------------------------------------------------
            // Soul Skin: gen_arti_lock_soul_skin (bound to 218565) recalculates the pet-cast Soul Link 108446 on the
            // warlock and only marks the counter when such a Soul Link is there. The demon casts 108446 first
            // (APPLY_AREA_AURA_OWNER, split damage) unless the live spell_pet_auras row 108415 -> 108446 already put it
            // on the warlock; then 218565 itself is cast on the warlock. The counter is the result (218565 has no
            // lasting state the harness could read more reliably).
            { 218567, 218565, 0,      true,  "lock_soul_skin", 0, nullptr, 0, 0, 0, false, 0.0f, false, false, false, false, false, GEN_PET_WARLOCK, GEN_PET_PRESENT, 108446 }
        };

        count = uint32(sizeof(traits) / sizeof(traits[0]));
        return traits;
    }

    // Traits that only fire with a chance are cast several times so that the counter can grow at all
    static uint32 const GenCastRepeats = 25;

    // A projectile or a channel only reaches the script a moment after the cast, those candidates are read later.
    // constexpr (not just const): C++17 makes a static constexpr data member implicitly inline, so it always has a
    // definition even where it's ODR-used (e.g. passed to a variadic/forwarding logging call) - a plain "static
    // uint32 const" here linked only by luck depending on how such call sites happened to pass it.
    static constexpr uint32 GenDelayMs = 2000;

    // One test character has to be able to cast the abilities of every class, so on top of the usual triggered
    // flags the equipped item requirement (weapon/shield class of the ability) and the DBC target checks are
    // ignored as well. Everything the trait itself needs (form, target state, pet, destination) is produced by
    // the harness through the fields of GenTrait instead.
    static TriggerCastFlags GenCastFlags()
    {
        return TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_IGNORE_EQUIPPED_ITEM_REQUIREMENT | TRIGGERED_IGNORE_TARGET_CHECK);
    }

    // A friendly copy of the plain creature above, needed for abilities that can only be cast on an ally
    static Creature* SpawnFriendlyTarget(Player* player, float distance = 3.0f)
    {
        Creature* target = player->SummonCreature(NPC_REAL_TARGET, player->GetNearPosition(distance, 1.5f), TEMPSUMMON_TIMED_DESPAWN, 60000);
        if (target)
        {
            target->SetFaction(player->GetFaction());
            target->SetReactState(REACT_PASSIVE);
            target->SetControlled(true, UNIT_STATE_ROOT);
            target->SetMaxHealth(2000000000);
            target->SetFullHealth();
            target->SetFacingToObject(player);
        }
        return target;
    }

    // The name of a cast result, so the log line says what the core actually answered
    static char const* GenCastResultName(SpellCastResult result)
    {
        switch (result)
        {
            case SPELL_CAST_OK:                     return "SPELL_CAST_OK";
            case SPELL_FAILED_BAD_TARGETS:          return "SPELL_FAILED_BAD_TARGETS";
            case SPELL_FAILED_BAD_IMPLICIT_TARGETS: return "SPELL_FAILED_BAD_IMPLICIT_TARGETS";
            case SPELL_FAILED_CASTER_AURASTATE:     return "SPELL_FAILED_CASTER_AURASTATE";
            case SPELL_FAILED_TARGET_AURASTATE:     return "SPELL_FAILED_TARGET_AURASTATE";
            case SPELL_FAILED_EQUIPPED_ITEM:        return "SPELL_FAILED_EQUIPPED_ITEM";
            case SPELL_FAILED_EQUIPPED_ITEM_CLASS:  return "SPELL_FAILED_EQUIPPED_ITEM_CLASS";
            case SPELL_FAILED_NO_AMMO:              return "SPELL_FAILED_NO_AMMO";
            case SPELL_FAILED_NO_COMBO_POINTS:      return "SPELL_FAILED_NO_COMBO_POINTS";
            case SPELL_FAILED_NO_PET:               return "SPELL_FAILED_NO_PET";
            case SPELL_FAILED_NOT_READY:            return "SPELL_FAILED_NOT_READY";
            case SPELL_FAILED_OUT_OF_RANGE:         return "SPELL_FAILED_OUT_OF_RANGE";
            case SPELL_FAILED_LINE_OF_SIGHT:        return "SPELL_FAILED_LINE_OF_SIGHT";
            case SPELL_FAILED_NOT_BEHIND:           return "SPELL_FAILED_NOT_BEHIND";
            case SPELL_FAILED_NOT_INFRONT:          return "SPELL_FAILED_NOT_INFRONT";
            case SPELL_FAILED_ONLY_SHAPESHIFT:      return "SPELL_FAILED_ONLY_SHAPESHIFT";
            case SPELL_FAILED_NOT_SHAPESHIFT:       return "SPELL_FAILED_NOT_SHAPESHIFT";
            case SPELL_FAILED_ONLY_STEALTHED:       return "SPELL_FAILED_ONLY_STEALTHED";
            case SPELL_FAILED_NOPATH:               return "SPELL_FAILED_NOPATH";
            case SPELL_FAILED_NO_VALID_TARGETS:     return "SPELL_FAILED_NO_VALID_TARGETS";
            case SPELL_FAILED_ROOTED:               return "SPELL_FAILED_ROOTED";
            case SPELL_FAILED_MOVING:               return "SPELL_FAILED_MOVING";
            case SPELL_FAILED_SPELL_IN_PROGRESS:    return "SPELL_FAILED_SPELL_IN_PROGRESS";
            case SPELL_FAILED_SPELL_UNAVAILABLE:    return "SPELL_FAILED_SPELL_UNAVAILABLE";
            case SPELL_FAILED_AFFECTING_COMBAT:     return "SPELL_FAILED_AFFECTING_COMBAT";
            case SPELL_FAILED_TARGETS_DEAD:         return "SPELL_FAILED_TARGETS_DEAD";
            case SPELL_FAILED_TARGET_NOT_PLAYER:    return "SPELL_FAILED_TARGET_NOT_PLAYER";
            case SPELL_FAILED_TARGET_IS_PLAYER:     return "SPELL_FAILED_TARGET_IS_PLAYER";
            case SPELL_FAILED_CUSTOM_ERROR:         return "SPELL_FAILED_CUSTOM_ERROR";
            case SPELL_FAILED_DONT_REPORT:          return "SPELL_FAILED_DONT_REPORT (a script of the ability refused it)";
            case SPELL_FAILED_ERROR:                return "SPELL_FAILED_ERROR";
            default:                                return "see SharedDefines.h SpellCastResult";
        }
    }

    // Names the requirement of the ability that the character does not meet, so a rejected cast can be followed up.
    // The decisive part is the probe: the very same CheckCast the core runs, on a throw away spell object. The
    // probe carries no loaded spell scripts, so SPELL_CAST_OK here means a script of the ability refused the cast.
    static std::string GenCastProblem(Player* player, Unit* caster, Unit* target, SpellInfo const* info)
    {
        if (!info)
            return " (spell not in the spell store)";
        if (!caster)
            caster = player;

        std::string why = " (CheckCast: ";
        if (caster != player)
            why = " (caster: the pet; CheckCast: ";
        {
            SpellCastTargets targets;
            if (target)
                targets.SetUnitTarget(target);

            Spell probe(caster, info, GenCastFlags());
            probe.InitExplicitTargets(targets);
            SpellCastResult result = probe.CheckCast(true);
            // Spell::prepare turns this one into OK because of TRIGGERED_IGNORE_TARGET_CHECK
            if (result == SPELL_FAILED_BAD_TARGETS)
                result = SPELL_CAST_OK;

            why += GenCastResultName(result);
            why += " = " + std::to_string(uint32(result));
            if (result == SPELL_CAST_OK)
            {
                // the probe carries no loaded scripts, so the refusal came from one of these
                why += ", so a spell script of the ability refused it, not the core; bound scripts:";
                SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(info->Id);
                if (bounds.first == bounds.second)
                    why += " none";
                else
                    for (auto itr = bounds.first; itr != bounds.second; ++itr)
                        why += " " + sObjectMgr->GetScriptName(itr->second.first);
            }
        }

        if (caster == player && !player->HasItemFitToSpellRequirements(info))
            why += "; no fitting weapon/shield equipped";
        if (caster == player && info->CheckShapeshift(uint32(player->GetShapeshiftForm())) != SPELL_CAST_OK)
            why += "; wrong shapeshift form";
        if (target && info->CheckExplicitTarget(caster, target) != SPELL_CAST_OK)
            why += "; the target type does not fit (ally/enemy)";
        if (target && info->CheckTarget(caster, target, false) != SPELL_CAST_OK)
            why += "; the target state does not fit (required debuff, health, creature type)";

        return why + ")";
    }

    // Reads the counter of one candidate, reports PASS/FAIL and removes everything the setup put in place
    static void EvaluateGen(ChatHandler* handler, Player* player, Unit* target, GenTrait const& trait, bool cast, std::string const& castProblem, uint32& passed)
    {
        uint32 runs = ArtifactTraitTest::GenRan[trait.trait];
        bool auraOk = trait.resultAura == 0 || player->HasAura(trait.resultAura) || (target && target->HasAura(trait.resultAura));
        bool pass = runs > 0 && auraOk;
        if (pass)
            ++passed;

        std::string detail = "script ran " + std::to_string(runs) + " of " + std::to_string(GenCastRepeats) + " casts of spell " + std::to_string(trait.ability);
        if (!cast)
            detail += " (the ability was rejected by the core)" + castProblem;
        if (trait.resultAura)
            detail += ", aura " + std::to_string(trait.resultAura) + (auraOk ? " present" : " missing");
        if (!runs && cast)
        {
            // everything the script bodies read before they do their work, so a silent early return can be followed up
            detail += " - the ability was cast but the script body did not run;";
            AuraEffect const* traitEffect = player->GetAuraEffect(trait.trait, EFFECT_0);
            detail += " trait aura " + std::string(player->HasAura(trait.trait) ? "present" : "MISSING")
                + ", value " + std::to_string(traitEffect ? traitEffect->GetAmount() : 0);
            if (target && target != player)
                detail += ", target " + std::string(player->IsFriendlyTo(target) ? "friendly" : "hostile")
                    + " at " + std::to_string(uint32(player->GetExactDist(target) * 10.0f)) + " tenth yd (edge to edge "
                    + std::to_string(uint32(player->GetDistance(target) * 10.0f)) + ")";
            if (trait.targetAura)
                detail += ", target aura " + std::to_string(trait.targetAura) + (target && target->HasAura(trait.targetAura) ? " present" : " MISSING");
        }

        Report(handler, trait.name, pass, detail);

        player->RemoveAurasDueToSpell(trait.trait);
        if (trait.casterAura)
            player->RemoveAurasDueToSpell(trait.casterAura);
        if (trait.prepSpell)
            player->RemoveAurasDueToSpell(trait.prepSpell);
        if (trait.targetAura && target)
            target->RemoveAurasDueToSpell(trait.targetAura);
        if (trait.resultAura)
        {
            player->RemoveAurasDueToSpell(trait.resultAura);
            if (target)
                target->RemoveAurasDueToSpell(trait.resultAura);
        }
    }

    // one candidate whose result is only read after GenDelayMs
    struct GenPending
    {
        GenTrait const* trait;
        bool cast;
        std::string castProblem;
    };

    // Puts the test character back the way it was. Everything a single candidate needed (spell power, selection)
    // is already removed right after that candidate, this only clears what may be left over.
    static void GenRestoreCaster(Player* player)
    {
        player->SetTarget(ObjectGuid::Empty);
        player->SetFullHealth();
    }

    // ---------------------------------------------------------------------------------------------------------
    // Pet support for .arttest gen
    //
    // Life cycle, all inside the one command call of a single candidate (no waiting, no event, no loop that waits
    // for a spawn - the pet either exists right after the synchronous create/summon call or the candidate is SKIP):
    //   1. GenAcquirePet: an existing, living pet of the right kind is used as it is and never removed. Otherwise a
    //      pet is made for the test: hunter -> a tamed pet built like .pet create (but never saved to the character
    //      database), warlock -> Summon Voidwalker (697), the regular class spell.
    //   2. optional petPrepSpell (Soul Link), only when its aura is not on the character yet.
    //   3. the casts of the candidate; the pet is looked up again by its GUID before every cast.
    //   4. GenReleasePet right after the evaluation: a pet made for the test is removed with PET_SAVE_AS_DELETED
    //      (or PET_SAVE_DISMISS when the character already had a saved demon of that entry, so it is kept), a prep
    //      aura the harness put on an existing pet is removed again.
    // Candidates with a pet are never "delayed": their pet would have to outlive the command.
    // ---------------------------------------------------------------------------------------------------------
    struct GenPetState
    {
        ObjectGuid petGuid;
        bool created = false;               // made by the harness, so it is removed again
        PetSaveMode releaseMode = PET_SAVE_AS_DELETED;
        bool prepAdded = false;             // the harness cast petPrepSpell, so its aura is removed again
        std::string info;                   // how the pet came about, for the log line
    };

    static Pet* GenCurrentPet(Player* player, GenPetState const& state)
    {
        if (state.petGuid.IsEmpty())
            return nullptr;
        Pet* pet = player->GetPet();
        if (!pet || pet->GetGUID() != state.petGuid || pet->m_removed || !pet->IsInWorld() || !pet->IsAlive())
            return nullptr;
        return pet;
    }

    // a tamed hunter pet made like .pet create (cs_pet.cpp), but from the template instead of a killed creature and
    // without SavePetToDB - nothing of it reaches the character database
    static Pet* GenCreateHunterPet(Player* player)
    {
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(NPC_TEST_HUNTER_PET);
        // .pet create: "Creatures with family CREATURE_FAMILY_NONE crashes the server"
        if (!cinfo || cinfo->family == CREATURE_FAMILY_NONE || !cinfo->IsTameable(player->CanTameExoticPets()))
            return nullptr;

        Pet* pet = new Pet(player, HUNTER_PET);
        if (!pet->CreateBaseAtCreatureInfo(cinfo, player))
        {
            delete pet;
            return nullptr;
        }

        Position pos = player->GetNearPosition(2.0f, float(M_PI) / 2.0f);
        pet->Relocate(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), player->GetOrientation());
        if (!pet->IsPositionValid())
        {
            delete pet;
            return nullptr;
        }

        PhasingHandler::InheritPhaseShift(pet, player);
        pet->SetCreatorGUID(player->GetGUID());
        pet->SetFaction(player->GetFaction());
        if (!pet->InitStatsForLevel(player->getLevel()))
        {
            delete pet;
            return nullptr;
        }

        pet->GetCharmInfo()->SetPetNumber(sObjectMgr->GeneratePetNumber(), true);
        pet->InitPetCreateSpells();
        pet->SetFullHealth();

        // Map::AddToMap only fails before the object is placed anywhere ("Should delete object")
        if (!player->GetMap()->AddToMap(pet->ToCreature()))
        {
            delete pet;
            return nullptr;
        }

        player->SetMinion(pet, true);
        player->PetSpellInitialize();
        return pet;
    }

    // returns an empty string when the pet is ready, otherwise the SKIP reason
    static std::string GenAcquirePet(Player* player, GenTrait const& trait, GenPetState& state)
    {
        bool hunter = trait.needsPet == GEN_PET_HUNTER;
        uint8 wantedClass = hunter ? uint8(CLASS_HUNTER) : uint8(CLASS_WARLOCK);
        if (player->getClass() != wantedClass)
            return std::string("needs a ") + (hunter ? "hunter" : "warlock") + " test character for its pet (the core only sets up "
                + (hunter ? "hunter pets for hunters" : "demons for warlocks") + ", Guardian::InitStatsForLevel)";

        if (Pet* existing = player->GetPet())
        {
            if (existing->m_removed || !existing->IsInWorld())
                return "the character's pet is being removed right now - run the candidate again";
            if (!existing->IsAlive())
                return "the character's own pet is dead - revive or dismiss it first";
            if (hunter != existing->IsHunterPet())
                return "the character already has a pet of the other kind - dismiss it first";

            state.petGuid = existing->GetGUID();
            state.created = false;
            state.info = "own pet " + std::to_string(existing->GetEntry()) + " used as it is";
            return "";
        }

        // a pet GUID without a pet object (other map, being loaded): do not touch that state
        if (!player->GetPetGUID().IsEmpty())
            return "the character has a pet GUID but no pet in this map - dismiss it first";

        if (hunter)
        {
            Pet* pet = GenCreateHunterPet(player);
            if (!pet)
                return "the test hunter pet could not be created (creature " + std::to_string(uint32(NPC_TEST_HUNTER_PET)) + ")";

            state.petGuid = pet->GetGUID();
            state.created = true;
            state.releaseMode = PET_SAVE_AS_DELETED;
            state.info = "test pet " + std::to_string(pet->GetEntry()) + " created (not saved)";
            return "";
        }

        // a demon the character had saved before would be loaded by the summon; it must survive the test
        state.releaseMode = player->GetPlayerPetDataByCreatureId(NPC_VOIDWALKER) ? PET_SAVE_DISMISS : PET_SAVE_AS_DELETED;
        if (!sSpellMgr->GetSpellInfo(SPELL_SUMMON_VOIDWALKER))
            return "Summon Voidwalker (697) is not in the spell store";

        player->CastSpell(player, SPELL_SUMMON_VOIDWALKER, GenCastFlags());
        Pet* pet = player->GetPet();
        if (!pet || pet->GetEntry() != NPC_VOIDWALKER || !pet->IsInWorld() || !pet->IsAlive())
            return "Summon Voidwalker (697) did not produce a demon";

        state.petGuid = pet->GetGUID();
        state.created = true;
        state.info = std::string("demon ") + std::to_string(pet->GetEntry()) + " summoned with 697"
            + (state.releaseMode == PET_SAVE_DISMISS ? " (saved demon of the character, dismissed afterwards)" : " (deleted afterwards)");
        return "";
    }

    static void GenReleasePet(Player* player, GenTrait const& trait, GenPetState& state)
    {
        if (state.petGuid.IsEmpty())
            return;

        Pet* pet = player->GetPet();
        bool ours = pet && pet->GetGUID() == state.petGuid && !pet->m_removed;

        if (state.prepAdded && trait.petPrepSpell)
        {
            if (ours)
                pet->RemoveAurasDueToSpell(trait.petPrepSpell);
            player->RemoveAurasDueToSpell(trait.petPrepSpell, state.petGuid);
        }

        if (state.created && ours)
            player->RemovePet(pet, state.releaseMode, false);

        state = GenPetState();
    }

    static void GenReportPet(ChatHandler* handler, GenTrait const& trait, std::string const& text)
    {
        std::string line = std::string("[arttest] gen ") + trait.name + ": pet - " + text;
        handler->SendSysMessage(line.c_str());
        TC_LOG_INFO("server", "%s", line.c_str());
    }

    static bool TestGen(ChatHandler* handler, Player* player, std::string const& only)
    {
        // The main target keeps the 6 yards of all the other tests, because several candidates only work at that
        // distance. Melee abilities get their own target inside Unit::GetMeleeRange (about 5 yards) through the
        // nearTarget field, and two more targets next to the main one make the multi target traits reproducible
        // instead of depending on leftover creatures of an earlier test.
        Creature* dummy = SpawnRealTarget(player, 6.0f);
        if (!dummy)
            return false;

        Creature* nearDummy = SpawnRealTarget(player, 2.5f, 0.0f);
        Creature* filler1 = SpawnRealTarget(player, 5.5f, 0.35f);
        Creature* filler2 = SpawnRealTarget(player, 5.5f, -0.35f);

        Creature* friendly = SpawnFriendlyTarget(player, 4.0f);

        uint32 count = 0;
        GenTrait const* traits = GenTraits(count);
        uint32 passed = 0;
        uint32 ran = 0;
        std::vector<GenPending> pending;
        Position startPos = player->GetPosition();


        for (uint32 i = 0; i < count; ++i)
        {
            GenTrait const& trait = traits[i];
            if (!only.empty() && only != trait.name)
                continue;

            ++ran;
            if (trait.note)
            {
                // not a failure: the condition of the trait cannot be produced here (critical strike, pet, ...)
                std::string line = std::string("[arttest] gen ") + trait.name + ": SKIP - " + trait.note;
                handler->SendSysMessage(line.c_str());
                TC_LOG_INFO("server", "%s", line.c_str());
                --ran;
                continue;
            }

            if (!sSpellMgr->GetSpellInfo(trait.trait) || !sSpellMgr->GetSpellInfo(trait.ability))
            {
                Report(handler, trait.name, false, "trait or ability spell id is missing in the spell store");
                continue;
            }

            // pet first: when no pet can be provided the candidate is SKIP and nothing else has been set up yet
            GenPetState petState;
            if (trait.needsPet != GEN_PET_NONE)
            {
                std::string petProblem;
                if (trait.delayed)
                    petProblem = "a candidate with a pet cannot be delayed - the test pet only lives inside this command";
                else
                    petProblem = GenAcquirePet(player, trait, petState);

                if (!petProblem.empty())
                {
                    GenReleasePet(player, trait, petState);
                    std::string line = std::string("[arttest] gen ") + trait.name + ": SKIP - " + petProblem;
                    handler->SendSysMessage(line.c_str());
                    TC_LOG_INFO("server", "%s", line.c_str());
                    --ran;
                    continue;
                }
                GenReportPet(handler, trait, petState.info);
            }

            player->RemoveAurasDueToSpell(trait.trait);
            if (trait.resultAura)
                player->RemoveAurasDueToSpell(trait.resultAura);

            Aura* aura = player->AddAura(trait.trait, player);
            if (!aura)
            {
                Report(handler, trait.name, false, "trait aura could not be applied");
                GenReleasePet(player, trait, petState);
                continue;
            }

            ArtifactTraitTest::GenRan[trait.trait] = 0;
            player->SetFullHealth();
            dummy->SetFullHealth();
            if (nearDummy)
                nearDummy->SetFullHealth();
            if (filler1)
                filler1->SetFullHealth();
            if (filler2)
                filler2->SetFullHealth();
            if (friendly)
                friendly->SetFullHealth();

            Unit* target = static_cast<Unit*>(player);
            if (!trait.selfTarget)
            {
                if (trait.friendlyTarget && friendly)
                    target = friendly;
                else if (trait.nearTarget && nearDummy)
                    target = nearDummy;
                else
                    target = dummy;
            }

            if (trait.needsPet != GEN_PET_NONE)
            {
                Pet* pet = GenCurrentPet(player, petState);
                if (!pet)
                {
                    Report(handler, trait.name, false, "the pet was gone before the first cast");
                    player->RemoveAurasDueToSpell(trait.trait);
                    GenReleasePet(player, trait, petState);
                    continue;
                }

                pet->SetFullHealth();
                if (trait.petRole == GEN_PET_IS_TARGET)
                    target = pet;

                // the aura the pet has to provide (Soul Link) - only cast when it is not on the character yet
                if (trait.petPrepSpell && !player->HasAura(trait.petPrepSpell) && sSpellMgr->GetSpellInfo(trait.petPrepSpell))
                {
                    pet->CastSpell(pet, trait.petPrepSpell, GenCastFlags());
                    petState.prepAdded = true;
                    GenReportPet(handler, trait, "pet cast " + std::to_string(trait.petPrepSpell) + ", aura on the character: "
                        + (player->HasAura(trait.petPrepSpell) ? "yes" : "NO"));
                }
                else if (trait.petPrepSpell)
                    GenReportPet(handler, trait, "aura " + std::to_string(trait.petPrepSpell) + " already on the character");
            }

            // only where a script of the ability really reads it (Crash Lightning does), because a selection the
            // character never had changes the target picking of other abilities as well
            if (trait.selectTarget)
                player->SetTarget(target->GetGUID());

            // the trait works on the damage dealt and the test character has no matching power for that school
            if (trait.needsPower)
            {
                player->ApplySpellPowerBonus(20000, true);
                player->HandleStatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, 20000.0f, true);
                player->HandleStatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, 20000.0f, true);
            }

            // some scripts read getHostileRefManager()/threat state (Sphere of Insanity), which is only
            // populated once caster and target are actually in combat with each other
            if (trait.needsCombat && target != player)
            {
                player->SetInCombatWith(target);
                target->SetInCombatWith(player);
            }

            // form or stance the ability needs (Berserk is only allowed in cat form)
            if (trait.casterAura)
            {
                player->RemoveAurasDueToSpell(trait.casterAura);
                player->AddAura(trait.casterAura, player);
            }

            // spell that has to be active before the ability works (Transcendence: Transfer needs the spirit)
            if (trait.prepSpell)
                player->CastSpell(player, trait.prepSpell, GenCastFlags());

            // some traits only work while the target carries a given aura (Vulnerable, Colossus Smash, Havoc)
            if (trait.targetAura)
            {
                target->RemoveAurasDueToSpell(trait.targetAura);
                player->AddAura(trait.targetAura, target);
            }

            // abilities that are only allowed against a wounded target (Execute below 20% health).
            // Unit::Update keeps UNIT_FIELD_AURASTATE up to date only once per world tick, and the whole command
            // runs inside one tick - so the states are set by hand, otherwise the core still sees a healthy target.
            if (trait.targetHealthPct > 0 && target != player)
            {
                target->SetHealth(std::max<uint64>(uint64(1), target->CountPctFromMaxHealth(trait.targetHealthPct)));
                target->ModifyAuraState(AURA_STATE_HEALTHLESS_20_PERCENT, target->HealthBelowPct(20));
                target->ModifyAuraState(AURA_STATE_HEALTHLESS_25_PERCENT, target->HealthBelowPct(25));
                target->ModifyAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, target->HealthBelowPct(35));
                target->ModifyAuraState(AURA_STATE_HEALTH_ABOVE_75_PERCENT, target->HealthAbovePct(75));
            }

            SpellInfo const* abilityInfo = sSpellMgr->GetSpellInfo(trait.ability);
            bool channeled = abilityInfo && abilityInfo->IsChanneled();

            bool cast = false;
            Unit* castCaster = static_cast<Unit*>(player);
            for (uint32 round = 0; round < GenCastRepeats; ++round)
            {
                // the pet is looked up again before every cast: a cast may have dismissed or killed it
                if (trait.needsPet != GEN_PET_NONE)
                {
                    Pet* pet = GenCurrentPet(player, petState);
                    if (!pet)
                    {
                        GenReportPet(handler, trait, "the pet was gone after " + std::to_string(round) + " casts, the remaining casts are left out");
                        break;
                    }
                    if (trait.petRole == GEN_PET_IS_TARGET)
                        target = pet;
                    else if (trait.petRole == GEN_PET_IS_CASTER)
                        castCaster = pet;
                }

                // TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD does NOT clear a plain spell cooldown:
                // SpellHistory::HasCooldown returns true for _spellCooldowns before it even looks at that flag.
                // Without this reset only the first of the 25 casts went through, which made every trait that
                // only fires with a chance depend on a single roll.
                castCaster->GetSpellHistory()->ResetCooldown(trait.ability, true);
                if (abilityInfo->ChargeCategoryId)
                    castCaster->GetSpellHistory()->ResetCharges(abilityInfo->ChargeCategoryId);

                if (castCaster != player)
                    cast = castCaster->CastSpell(target, trait.ability, GenCastFlags()) || cast;
                else if (trait.destRange > 0.0f)
                {
                    // abilities that take a ground destination (Heroic Leap); the character is put back afterwards
                    Position dest = player->GetNearPosition(trait.destRange, 0.0f);
                    cast = player->CastSpell(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), trait.ability, GenCastFlags()) || cast;
                    player->NearTeleportTo(startPos, false);
                }
                else
                    cast = player->CastSpell(target, trait.ability, GenCastFlags()) || cast;

                // A channel is not started by Spell::prepare but only by the next tick of the spell event, and the
                // next candidate would cancel it before that. One step of the event queue lets it really start,
                // which is what runs the AfterCast handler of the trait script.
                if (channeled)
                {
                    player->m_Events.Update(1);
                    player->InterruptNonMeleeSpells(false);
                }
            }

            // a pet that is gone by now must not be touched any more, neither as caster nor as target
            if (trait.needsPet != GEN_PET_NONE && !GenCurrentPet(player, petState))
            {
                if (trait.petRole == GEN_PET_IS_CASTER)
                    castCaster = nullptr;
                if (trait.petRole == GEN_PET_IS_TARGET)
                    target = nullptr;
            }

            // the reason has to be read now, while the situation of this candidate still stands - a delayed
            // result is read two seconds later, when form, auras and selection belong to another candidate
            std::string castProblem;
            if (!cast)
            {
                if (!castCaster || (trait.petRole == GEN_PET_IS_TARGET && !target))
                    castProblem = " (the pet was gone)";
                else
                    castProblem = GenCastProblem(player, castCaster, target, abilityInfo);
            }

            if (trait.needsPower)
            {
                player->ApplySpellPowerBonus(20000, false);
                player->HandleStatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, 20000.0f, false);
                player->HandleStatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, 20000.0f, false);
            }

            if (trait.selectTarget)
                player->SetTarget(ObjectGuid::Empty);

            if (trait.delayed)
            {
                pending.push_back({ &trait, cast, castProblem });
                GenReleasePet(player, trait, petState);    // no-op: candidates with a pet are never delayed
                continue;
            }

            EvaluateGen(handler, player, target, trait, cast, castProblem, passed);

            // after the evaluation, which may still read an aura on the pet (Hunter's Advantage)
            GenReleasePet(player, trait, petState);
        }

        if (!ran)
        {
            handler->SendSysMessage("[arttest] gen: no candidate with that name, use .arttest gen list");
            return true;
        }

        player->SetFullHealth();

        if (!pending.empty())
        {
            // the temporary bonuses stay until the delayed results are read, then they are removed there
            ObjectGuid playerGuid = player->GetGUID();
            ObjectGuid dummyGuid = dummy->GetGUID();
            ObjectGuid friendlyGuid = friendly ? friendly->GetGUID() : ObjectGuid::Empty;
            uint32 passedSoFar = passed;
            uint32 total = ran;
            std::vector<GenPending> list = pending;

            player->m_Events.AddEvent(new DelayedTestEvent([playerGuid, dummyGuid, friendlyGuid, list, passedSoFar, total]()
            {
                Player* p = ObjectAccessor::FindPlayer(playerGuid);
                if (!p)
                    return;

                ChatHandler delayedHandler(p->GetSession());
                Unit* dummyUnit = ObjectAccessor::GetUnit(*p, dummyGuid);
                Unit* friendlyUnit = friendlyGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*p, friendlyGuid);
                uint32 passedNow = passedSoFar;

                for (GenPending const& entry : list)
                {
                    Unit* t = static_cast<Unit*>(p);
                    if (!entry.trait->selfTarget)
                        t = (entry.trait->friendlyTarget && friendlyUnit) ? friendlyUnit : dummyUnit;

                    EvaluateGen(&delayedHandler, p, t, *entry.trait, entry.cast, entry.castProblem, passedNow);
                }

                delayedHandler.PSendSysMessage("[arttest] gen: %u of %u trait candidates passed", passedNow, total);
                p->SetFullHealth();
                GenRestoreCaster(p);
            }), player->m_Events.CalculateTime(GenDelayMs));

            handler->PSendSysMessage("[arttest] gen: %u candidates use a projectile or a channel, their result follows in %u ms ...", uint32(pending.size()), GenDelayMs);
            return true;
        }

        handler->PSendSysMessage("[arttest] gen: %u of %u trait candidates passed", passed, ran);
        GenRestoreCaster(player);
        return true;
    }

    static bool ListGen(ChatHandler* handler)
    {
        uint32 count = 0;
        GenTrait const* traits = GenTraits(count);
        for (uint32 i = 0; i < count; ++i)
            handler->PSendSysMessage("[arttest] gen %s (trait %u, ability %u)", traits[i].name, traits[i].trait, traits[i].ability);

        handler->PSendSysMessage("[arttest] gen: %u trait candidates, run them all with .arttest gen", count);
        return true;
    }

    // Runs a chat command for the GM's own character
    static void RunCommand(ChatHandler* handler, char const* command)
    {
        std::string copy = command;
        handler->ParseCommands(copy.c_str());
    }

    // Level 110, all class spells and talents, GM mode
    static bool Prepare(ChatHandler* handler, Player* player)
    {
        if (player->getLevel() < 110)
            player->GiveLevel(110);

        RunCommand(handler, ".learn all my class");
        RunCommand(handler, ".learn all my spells");
        RunCommand(handler, ".learn all my talents");
        RunCommand(handler, ".gm on");
        RunCommand(handler, ".gm fly on");
        player->SetFullHealth();
        handler->PSendSysMessage("[arttest] prepared: level %u, class spells and talents learned, GM mode and fly on (spec %u)", uint32(player->getLevel()), uint32(player->GetSpecializationId()));
        return true;
    }

    // Gives the artifact weapon of one artifact (or of all artifacts) with enough artifact power to buy all traits
    static bool GiveArtifactWeapon(ChatHandler* handler, Player* player, uint32 artifactId)
    {
        std::vector<uint32> candidates;
        for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& proto = pair.second;
            if (proto.GetArtifactID() == artifactId && proto.GetClass() == ITEM_CLASS_WEAPON && proto.GetQuality() == ITEM_QUALITY_ARTIFACT)
                candidates.push_back(pair.first);
        }

        if (candidates.empty())
        {
            TC_LOG_INFO("server", "[arttest] no weapon item template found for artifact %u", artifactId);
            return false;
        }

        std::sort(candidates.begin(), candidates.end());
        std::string list;
        for (uint32 id : candidates)
            list += std::to_string(id) + " ";
        TC_LOG_INFO("server", "[arttest] artifact %u candidate items: %s", artifactId, list.c_str());

        uint32 itemId = candidates.front();
        // A weapon the character already owns still gets the artifact power (otherwise the command silently does nothing)
        if (!player->HasItemCount(itemId, 1) && !player->StoreNewItemInBestSlots(itemId, 1))
        {
            handler->PSendSysMessage("[arttest] could not store item %u for artifact %u (bags full?)", itemId, artifactId);
            return false;
        }

        if (Item* weapon = player->GetItemByEntry(itemId))
        {
            weapon->SetUInt64Value(ITEM_FIELD_ARTIFACT_XP, UI64LIT(50000000000));
            weapon->SetState(ITEM_CHANGED, player);
        }
        return true;
    }

    static bool GiveWeapon(ChatHandler* handler, Player* player, bool all)
    {
        uint32 given = 0;
        uint32 spec = uint32(player->GetSpecializationId());
        for (ArtifactEntry const* artifact : sArtifactStore)
        {
            if (!all && artifact->ChrSpecializationID != spec)
                continue;

            if (GiveArtifactWeapon(handler, player, artifact->ID))
                ++given;
        }

        handler->PSendSysMessage("[arttest] artifact weapons in your bags: %u (candidate item ids are in the server log)", given);
        if (!all && !given)
            handler->PSendSysMessage("[arttest] nothing found for specialization %u - pick a specialization first or use .arttest weapon all", spec);
        return true;
    }
    static bool HandleArtTest(ChatHandler* handler, char const* args)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        std::string arg = args ? args : "";

        // From the world server console (tools\auto_test_all.ps1) the character is named after the test: ".arttest all @Name".
        // The results go to that character's chat and, as always, to the server log.
        std::unique_ptr<ChatHandler> playerHandler;
        if (!player)
        {
            size_t at = arg.rfind('@');
            if (at != std::string::npos)
            {
                std::string name = arg.substr(at + 1);
                arg = arg.substr(0, at);
                while (!arg.empty() && arg.back() == ' ')
                    arg.pop_back();
                player = ObjectAccessor::FindPlayerByName(name);
                if (player)
                {
                    playerHandler = std::make_unique<ChatHandler>(player->GetSession());
                    handler = playerHandler.get();
                }
            }
        }

        if (!player)
        {
            handler->SendSysMessage("[arttest] run this command in game with your character, or from the console as: .arttest <test> @<character name>");
            return true;
        }

        if (arg.empty() || arg == "list")
        {
            handler->SendSysMessage("[arttest] .arttest gen | gen list | gen <name> - the trait candidates from spell_artifact_traits_gen.cpp");
            handler->SendSysMessage("[arttest] .arttest prepare | weapon [all] | all | combat | elementalist deception knight sweetsouls fatalechoes obsidianlance balancedblades glacialeruption sacreddawn cosmicripple timesandmeasures demonspeed anguish thalkielsdiscord doomwolves shatterthesouls deathandglory angling (all run on any class, class-locked ones SKIP on the wrong class)");
                        return true;
        }

        // Test tooling: ground height check of creature spawns. Reads spawn guids from height_guids.txt (one per line, server dir),
        // writes guid,id,spawnZ,groundZ,diff to height_out.csv. Spawns of other maps than the player's are skipped.
        if (arg == "height")
        {
            std::ifstream in("height_guids.txt");
            std::ofstream out("height_out.csv", std::ios::trunc);
            if (!in || !out)
            {
                handler->SendSysMessage("[arttest] height: cannot open height_guids.txt / height_out.csv");
                return true;
            }
            Map* map = player->GetMap();
            uint32 done = 0, skipped = 0;
            uint64 guid;
            while (in >> guid)
            {
                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (!data || data->mapid != map->GetId())
                {
                    ++skipped;
                    continue;
                }
                map->LoadGrid(data->posX, data->posY);
                float ground = map->GetHeight(player->GetPhaseShift(), data->posX, data->posY, data->posZ + 2.0f, true, 20.0f);
                out << guid << ',' << data->id << ',' << data->posZ << ',' << ground << ',' << (data->posZ - ground) << '\n';
                ++done;
            }
            handler->PSendSysMessage("[arttest] height: %u checked, %u skipped (other map / unknown)", done, skipped);
            return true;
        }

        if (arg == "prepare")
            return Prepare(handler, player);

        if (arg == "weapon all")
            return GiveWeapon(handler, player, true);

        if (arg == "weapon")
            return GiveWeapon(handler, player, false);

        if (arg == "gen list")
            return ListGen(handler);

        if (arg == "gen")
        {
            CleanupDummies(player);
            return TestGen(handler, player, "");
        }

        if (arg.compare(0, 4, "gen ") == 0)
        {
            CleanupDummies(player);
            return TestGen(handler, player, arg.substr(4));
        }

        CleanupDummies(player);
        bool all = arg == "all";
        bool ran = false;
        if (all || arg == "elementalist") { ran = true; TestElementalist(handler, player); }
        if (all || arg == "deception")    { ran = true; TestDeception(handler, player); }
        if (all || arg == "knight")       { ran = true; TestKnight(handler, player); }
        if (all || arg == "sweetsouls")   { ran = true; TestSweetSouls(handler, player); }
        if (all || arg == "fatalechoes")  { ran = true; TestFatalEchoes(handler, player); }
        if (all || arg == "obsidianlance")  { ran = true; TestObsidianLance(handler, player); }
        if (all || arg == "balancedblades") { ran = true; TestBalancedBlades(handler, player); }
        if (all || arg == "glacialeruption") { ran = true; TestGlacialEruption(handler, player); }
        if (all || arg == "sacreddawn")     { ran = true; TestSacredDawn(handler, player); }
        if (all || arg == "cosmicripple")   { ran = true; TestCosmicRipple(handler, player); }
        if (all || arg == "timesandmeasures") { ran = true; TestTimesAndMeasures(handler, player); }
        if (all || arg == "demonspeed")     { ran = true; TestDemonSpeed(handler, player); }
        if (all || arg == "anguish")        { ran = true; TestAnguish(handler, player); }
        if (all || arg == "thalkielsdiscord") { ran = true; TestThalkielsDiscord(handler, player); }
        if (all || arg == "doomwolves")       { ran = true; TestDoomWolves(handler, player); }
        if (all || arg == "shatterthesouls")  { ran = true; TestShatterTheSouls(handler, player); }
        if (all || arg == "deathandglory")    { ran = true; TestDeathAndGlory(handler, player); }
        if (all || arg == "angling")          { ran = true; TestAngling(handler, player); }
        if (arg == "combat")                { ran = true; TestCombat(handler, player); }

        if (!ran)
            handler->SendSysMessage("[arttest] unknown test, use .arttest list");
        return true;
    }
};

void AddSC_arttest_commandscript()
{
    new arttest_commandscript();
}
