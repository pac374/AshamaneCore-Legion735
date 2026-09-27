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

// Legion-Server round "LCF2" (2026-09-25): artifact abilities / traits whose behaviour was checked against
// an external reference implementation and against SimulationCraft legion-dev 7742eb6 (GPL). Only the MECHANIC was
// taken over (which spell triggers which, on whom, when); the code is our own and every number comes from our client
// 7.3.5.26972 (SpellEffect/SpellAuraOptions/SpellDescriptionVariables) unless a comment names another source.
//
// Reference tables used for comparison:
//   DB (reference schema):  spell_linked_spell, spell_aura_trigger, spell_dummy_trigger, spell_trigger,
//                                 spell_pet_auras, spell_target_filter, spell_proc_event, spell_proc_check,
//                                 areatrigger_data/actions
//   code: SpellAuraEffects.cpp AuraSpellTrigger (aura_trigger option 0 = cast on tick), Spell.cpp linked actions
//         (17 CAST_COUNT, 21 CAST_DURATION, 22 CAST_ON_SUMMON), Pet.cpp CastPetAuras (spellId>0 on summon,
//         spellId<0 on unsummon, target 3 = summon's target, option 8 = owner as original caster),
//         Unit.cpp (Judge Unworthy, Surge of the Stormgod, Effusive Mists, Hati), spell_hunter.cpp (Hati's Bond,
//         Broken Bond, Titan's Thunder, Master of Beasts), spell_monk.cpp (Sheilun's Gift), spell_generic.cpp
//         (Undercurrent), spell_shaman.cpp (Alpha Wolf), spell_dk.cpp (Claw), spell_warlock.cpp (Stolen Power).
// Binding SQL: C:\LegionServer\fixes\lcf2_2026-09-25_trait_scripts.sql - without it every script here is inert.

#include "AreaTrigger.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PetAI.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "Unit.h"

namespace
{
    enum Lcf2Spells : uint32
    {
        // Mage - Aluneth
        SPELL_MARK_OF_ALUNETH_AURA      = 224968,
        SPELL_MARK_OF_ALUNETH_PULSE     = 211088,
        SPELL_MARK_OF_ALUNETH_DETONATE  = 211076,

        // Paladin
        SPELL_JUDGMENT_RET_DEBUFF       = 197277,
        SPELL_JUDGE_UNWORTHY            = 238134,

        // Hunter - Titanstrike
        SPELL_TITANS_THUNDER            = 207068,
        SPELL_TITANS_THUNDER_PULSE      = 207081,
        SPELL_TITANS_THUNDER_AURA       = 207094,
        SPELL_TITANS_THUNDER_TICK       = 207097,
        SPELL_TITANS_THUNDER_DF_MARKER  = 218638,
        SPELL_TITANS_THUNDER_DF_DAMAGE  = 218635,
        SPELL_DIRE_FRENZY               = 217200,
        SPELL_MULTI_SHOT                = 2643,
        SPELL_SURGE_OF_THE_STORMGOD     = 197354,
        SPELL_SURGE_OF_THE_STORMGOD_DMG = 197465,
        SPELL_HATIS_BOND                = 197344,
        SPELL_BROKEN_BOND               = 211117,
        SPELL_STORMBOUND_DEFAULT        = 197388,
        SPELL_STORMBOUND_SET_117        = 211145,
        SPELL_STORMBOUND_SET_118        = 211146,
        SPELL_STORMBOUND_SET_119        = 211147,
        SPELL_STORMBOUND_SET_120        = 211148,
        SPELL_MASTER_OF_BEASTS          = 197248,
        SPELL_KILL_COMMAND_DAMAGE       = 83381,
        SPELL_KILL_COMMAND_CHARGE       = 118171,
        SPELL_BESTIAL_WRATH_HATI        = 207033,
        SPELL_BEAST_CLEAVE_PASSIVE      = 115939,
        SPELL_BEAST_CLEAVE_BUFF         = 118455,
        SPELL_TALON_BOND                = 238089,
        SPELL_TALON_SLASH               = 242735,
        SPELL_ADAPTATION                = 152244,
        SPELL_SPIKED_COLLAR             = 53184,
        SPELL_BLINK_STRIKES             = 130392,

        // Monk
        SPELL_SHEILUNS_GIFT_CLOUD       = 214501,
        SPELL_WHISPERS_OF_SHAOHAO       = 238130,
        SPELL_WHISPERS_OF_SHAOHAO_HEAL  = 242400,
        SPELL_EFFUSE                    = 116694,
        SPELL_REVIVAL                   = 115310,
        SPELL_BLESSINGS_OF_YULON        = 199665,
        SPELL_BLESSINGS_OF_YULON_HOT    = 199668,
        SPELL_BLESSINGS_OF_YULON_SUMMON = 199671,
        SPELL_FISTS_OF_FURY             = 113656,
        SPELL_CROSSWINDS                = 195650,
        SPELL_CROSSWINDS_DRIVER         = 195651,
        SPELL_CROSSWINDS_DAMAGE         = 196061,

        // Fishing artifact
        SPELL_UNDERCURRENT_TELEPORT     = 216426,

        // Priest
        SPELL_LASH_OF_INSANITY          = 238137,
        SPELL_LASH_OF_INSANITY_ENERGIZE = 240843,

        // Warlock
        SPELL_STOLEN_POWER              = 211530,
        SPELL_STOLEN_POWER_STACK        = 211529,
        SPELL_STOLEN_POWER_BUFF         = 211583,
        SPELL_DOOM                      = 603,
        SPELL_DOOM_DOUBLED_BUFF         = 218571,

        // Shaman
        SPELL_ALPHA_WOLF                = 198434,
        SPELL_ALPHA_WOLF_AURA           = 198486,
        SPELL_SPIRIT_BOMB               = 198455,
        SPELL_FIRE_NOVA_WOLF            = 198480,
        SPELL_SNOWSTORM                 = 198483,
        SPELL_THUNDER_BITE              = 198485,
        SPELL_FIERY_JAWS                = 224125,
        SPELL_FROZEN_BITE               = 224126,
        SPELL_CRACKLING_SURGE           = 224127,
        SPELL_RIPTIDE                   = 61295,
        SPELL_TIDAL_TOTEM_AT            = 233487,
        SPELL_TIDAL_TOTEM_HEAL          = 209069,
        SPELL_TIDAL_TOTEM_SUMMON        = 208932,

        // Death Knight
        SPELL_SUMMON_SHAMBLING_HORROR   = 191759,
        SPELL_NECROBOMB                 = 191758,

        // Rogue
        SPELL_SHADOWSTRIKE              = 185438,
        SPELL_CHEAP_SHOT                = 1833,
        SPELL_AKAARIS_SOUL_SUMMON       = 209837,
        SPELL_SOUL_RIP                  = 220893
    };

    enum Lcf2Creatures : uint32
    {
        NPC_HATI                        = 100324,
        NPC_HATI_SET_117                = 106548,
        NPC_HATI_SET_118                = 106549,
        NPC_HATI_SET_119                = 106550,
        NPC_HATI_SET_120                = 106551,
        NPC_SPIRIT_WOLF                 = 29264,
        NPC_DOOM_WOLF                   = 100820,
        NPC_VOID_TENDRIL                = 98167
    };

    // Doom Wolf models (creature_template 100820 modelid1..3). Element per model from the reference core spell_shaman.cpp
    // spell_sha_alpha_wolf (66843 -> Fire Nova, 66844 -> Snowstorm, 66845 -> Thunder Bite); SimulationCraft
    // sc_shaman.cpp pairs the same Alpha Wolf spells with fire/frost/lightning wolves.
    enum Lcf2WolfModels : uint32
    {
        MODEL_DOOM_WOLF_FIRE            = 66843,
        MODEL_DOOM_WOLF_FROST           = 66844,
        MODEL_DOOM_WOLF_LIGHTNING       = 66845
    };

    bool IsHatiEntry(uint32 entry)
    {
        switch (entry)
        {
            case NPC_HATI:
            case NPC_HATI_SET_117:
            case NPC_HATI_SET_118:
            case NPC_HATI_SET_119:
            case NPC_HATI_SET_120:
                return true;
            default:
                return false;
        }
    }

    Creature* FindHati(Unit* owner, bool aliveOnly = true)
    {
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && IsHatiEntry(controlled->GetEntry()) && (!aliveOnly || controlled->IsAlive()))
                return controlled->ToCreature();
        return nullptr;
    }

    void KeepNearest(std::list<WorldObject*>& targets, Position const& center, size_t count)
    {
        targets.sort([&center](WorldObject const* left, WorldObject const* right)
        {
            return left->GetExactDistSq(&center) < right->GetExactDistSq(&center);
        });
        if (targets.size() > count)
            targets.resize(count);
    }

    int32 EffectBasePoints(uint32 spellId, SpellEffIndex effIndex, Unit* caster)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            if (SpellEffectInfo const* effect = info->GetEffect(effIndex))
                return effect->CalcValue(caster);
        return 0;
    }
}

// ============================================================================================================
// Mage: Mark of Aluneth (224968) - the artifact ability did nothing (aura 226 PERIODIC_DUMMY without handler), so
// Aluneth's Avarice (238090, gated on 211076 E1) could never fire either.
// the reference core: spell_aura_trigger 224968 -> 211088 option 0 (cast on every tick, caster = aura caster, target = aura
// owner); spell_linked_spell -224968 -> 211076 (cast on removal; SpellAuras.cpp skips removal by death when
// removeMask is 0); SpellEffects.cpp adds 224968 E0 % of max mana to 211076's damage.
// Client: 224968 "inflicting ${$211088s1*6} Arcane damage over $d ... then detonating for Arcane damage equal to
// $s1% of your maximum mana" (E0 BP 20, period 1000 ms).
// ============================================================================================================
class spell_lcf2_mage_mark_of_aluneth : public AuraScript
{
    PrepareAuraScript(spell_lcf2_mage_mark_of_aluneth);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_MARK_OF_ALUNETH_PULSE, SPELL_MARK_OF_ALUNETH_DETONATE });
    }

    void HandleTick(AuraEffect const* aurEff)
    {
        if (Unit* caster = GetCaster())
            caster->CastSpell(GetTarget(), SPELL_MARK_OF_ALUNETH_PULSE, true, nullptr, aurEff);
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_DEATH)
            return;

        if (Unit* caster = GetCaster())
            caster->CastSpell(GetTarget(), SPELL_MARK_OF_ALUNETH_DETONATE, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_mage_mark_of_aluneth::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_lcf2_mage_mark_of_aluneth::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 211076 - Mark of Aluneth (detonation): E0 BP 1 + 224968 E0 % of the mage's maximum mana, added before the damage
// bonus pipeline (same place the reference core adds it).
class spell_lcf2_mage_mark_of_aluneth_detonation : public SpellScript
{
    PrepareSpellScript(spell_lcf2_mage_mark_of_aluneth_detonation);

    void HandleLaunch(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        int32 pct = EffectBasePoints(SPELL_MARK_OF_ALUNETH_AURA, EFFECT_0, caster);
        if (pct <= 0)
            return;
        SetEffectValue(GetEffectValue() + CalculatePct(caster->GetMaxPower(POWER_MANA), pct));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_lcf2_mage_mark_of_aluneth_detonation::HandleLaunch, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ============================================================================================================
// Paladin: Judge Unworthy (238134) "When you deal damage to a target afflicted by Judgment, it has a $s1% chance to
// spread to a nearby target." the reference core Unit.cpp (proc of SPELL_AURA_MOD_DAMAGE_FROM_CASTER, aura 197277): roll
// 238134 E0, pick an enemy within 8 yd of the judged target (8 yd is the reference core's value, the client has none) that is
// in line of sight of the paladin and not already judged, give it 197277 with the remaining duration.
// Our core already generates the proc entry for 197277 (aura 271 is a trigger aura, client ProcTypeMask 664232) and
// only lets it proc for the aura caster (AuraEffect::CheckEffectProc).
// ============================================================================================================
class spell_lcf2_pal_judge_unworthy : public AuraScript
{
    PrepareAuraScript(spell_lcf2_pal_judge_unworthy);

    static constexpr float SPREAD_RANGE = 8.0f; // the reference core Unit.cpp GetAttackableUnitListInRange(targetList, 8.f)

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        Unit* caster = GetCaster();
        Unit* judged = GetTarget();
        if (!caster || !judged)
            return;

        AuraEffect const* trait = caster->GetAuraEffect(SPELL_JUDGE_UNWORTHY, EFFECT_0);
        if (!trait || !roll_chance_i(trait->GetAmount()))
            return;

        std::list<Unit*> units;
        Trinity::AnyUnitInObjectRangeCheck check(judged, SPREAD_RANGE);
        Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(judged, units, check);
        Cell::VisitAllObjects(judged, searcher, SPREAD_RANGE);

        Unit* best = nullptr;
        float bestDist = 0.0f;
        for (Unit* unit : units)
        {
            if (unit == judged || !unit->IsAlive() || !caster->IsValidAttackTarget(unit) || !unit->IsWithinLOSInMap(caster))
                continue;
            if (unit->HasAura(SPELL_JUDGMENT_RET_DEBUFF, caster->GetGUID()))
                continue;
            float dist = judged->GetDistance(unit);
            if (!best || dist < bestDist)
            {
                best = unit;
                bestDist = dist;
            }
        }

        if (!best)
            return;

        int32 duration = GetAura()->GetDuration();
        if (Aura* spread = caster->AddAura(SPELL_JUDGMENT_RET_DEBUFF, best))
        {
            spread->SetMaxDuration(duration);
            spread->SetDuration(duration);
            if (AuraEffect* effect = spread->GetEffect(EFFECT_0))
                effect->ChangeAmount(aurEff->GetAmount());
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_lcf2_pal_judge_unworthy::HandleProc, EFFECT_0, SPELL_AURA_MOD_SPELL_DAMAGE_FROM_CASTER);
    }
};

// ============================================================================================================
// Hunter: Titan's Thunder (207068, artifact ability) - nothing of it was implemented.
// Chain (the reference core): 207068 E0 triggers 207081 (client); 207081 area ally search is filtered to units owned by the
// hunter (spell_target_filter option 21 SPELL_FILTER_BY_OWNER) and its dummy casts 207094 on them
// (spell_dummy_trigger option 5); 207094 ticks (1 s, client) make the pet cast 207097 at its victim
// (spell_hun_titans_thunder, SimC titans_thunder_tick uses the pet's melee target as well).
// Dire Frenzy (217200) variant: 207068 gives the hunter 218638 when 217200 is known (spell_linked_spell hastype 2);
// the client aura says "The next Dire Frenzy will deal an additional $218635s1 Nature damage on each attack" -> one
// 218635 per Dire Frenzy attack (our spell_hun_dire_frenzy does 5 attacks, 200 ms apart), marker consumed.
// ============================================================================================================
class spell_lcf2_hun_titans_thunder : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_titans_thunder);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (caster->HasSpell(SPELL_DIRE_FRENZY))
            caster->CastSpell(caster, SPELL_TITANS_THUNDER_DF_MARKER, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_hun_titans_thunder::HandleAfterCast);
    }
};

class spell_lcf2_hun_titans_thunder_pulse : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_titans_thunder_pulse);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        ObjectGuid casterGuid = GetCaster()->GetGUID();
        targets.remove_if([casterGuid](WorldObject* object)
        {
            Unit* unit = object->ToUnit();
            return !unit || !unit->IsAlive() || unit->GetOwnerGUID() != casterGuid;
        });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
            GetCaster()->CastSpell(target, SPELL_TITANS_THUNDER_AURA, true);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_lcf2_hun_titans_thunder_pulse::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
        OnEffectHitTarget += SpellEffectFn(spell_lcf2_hun_titans_thunder_pulse::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_lcf2_hun_titans_thunder_aura : public AuraScript
{
    PrepareAuraScript(spell_lcf2_hun_titans_thunder_aura);

    void HandleTick(AuraEffect const* aurEff)
    {
        Unit* pet = GetTarget();
        if (Unit* victim = pet->GetVictim())
            pet->CastSpell(victim, SPELL_TITANS_THUNDER_TICK, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_hun_titans_thunder_aura::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 217200 - Dire Frenzy (second script next to spell_hun_dire_frenzy): Titan's Thunder bonus.
class spell_lcf2_hun_dire_frenzy_titans_thunder : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_dire_frenzy_titans_thunder);

    void HandleOnCast()
    {
        Unit* hunter = GetCaster();
        Unit* target = GetExplTargetUnit();
        Unit* pet = hunter->GetGuardianPet();
        if (!target || !pet || !hunter->HasAura(SPELL_TITANS_THUNDER_DF_MARKER))
            return;

        hunter->RemoveAurasDueToSpell(SPELL_TITANS_THUNDER_DF_MARKER);

        ObjectGuid targetGuid = target->GetGUID();
        for (uint32 timer = 0; timer <= 800; timer += 200) // same cadence as the 5 attacks in spell_hun_dire_frenzy
        {
            pet->GetScheduler().Schedule(Milliseconds(timer), [targetGuid](TaskContext context)
            {
                Unit* caster = context.GetUnit();
                if (!caster)
                    return;
                if (Unit* victim = ObjectAccessor::GetUnit(*caster, targetGuid))
                    caster->CastSpell(victim, SPELL_TITANS_THUNDER_DF_DAMAGE, true);
            });
        }
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_lcf2_hun_dire_frenzy_titans_thunder::HandleOnCast);
    }
};

// ============================================================================================================
// Hunter: Multi-Shot (2643) - second script. Two artifact effects hang on it:
//  - Surge of the Stormgod (197354): client proc chance 25; SimulationCraft sc_hunter.cpp rolls it once per
//    Multi-Shot execute and fires once for the pet and once for Hati; the reference core Unit.cpp casts 197465 on the pet and
//    on Hati with bp = ranged AP * 2 (client tooltip "${$RAP*2}"); 197465 = DEST_TARGET_ALLY + enemies around it.
//  - Master of Beasts (197248) "Hati also benefits from ... Beast Cleave": 118455 also on Hati (the reference core
//    spell_hun_beast_cleave_tgr, SimC multi_shot execute).
// ============================================================================================================
class spell_lcf2_hun_multi_shot_titanstrike : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_multi_shot_titanstrike);

    void HandleAfterCast()
    {
        Player* hunter = GetCaster()->ToPlayer();
        if (!hunter)
            return;

        Creature* hati = FindHati(hunter);

        if (hati && hunter->HasAura(SPELL_MASTER_OF_BEASTS) && hunter->HasAura(SPELL_BEAST_CLEAVE_PASSIVE))
            hunter->CastSpell(hati, SPELL_BEAST_CLEAVE_BUFF, true);

        if (Aura const* surge = hunter->GetAura(SPELL_SURGE_OF_THE_STORMGOD))
        {
            if (!roll_chance_i(int32(surge->GetSpellInfo()->ProcChance)))
                return;

            int32 bp = int32(hunter->GetTotalAttackPowerValue(RANGED_ATTACK) * 2.0f);
            if (Pet* pet = hunter->GetPet())
                if (pet->IsAlive())
                    hunter->CastCustomSpell(pet, SPELL_SURGE_OF_THE_STORMGOD_DMG, &bp, nullptr, nullptr, true);
            if (hati)
                hunter->CastCustomSpell(hati, SPELL_SURGE_OF_THE_STORMGOD_DMG, &bp, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_hun_multi_shot_titanstrike::HandleAfterCast);
    }
};

// 34026 - Kill Command (second script): Master of Beasts "... and deals damage from Kill Command" - Hati casts the
// damage spell 83381 (whose damage script computes from the hunter) and charges if not in melee range
// (the reference core spell_hun_kill_command HandleDummy).
class spell_lcf2_hun_kill_command_hati : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_kill_command_hati);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* hunter = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!target || !hunter->HasAura(SPELL_MASTER_OF_BEASTS))
            return;

        if (Creature* hati = FindHati(hunter))
        {
            hati->CastSpell(target, SPELL_KILL_COMMAND_DAMAGE, true);
            if (!hati->IsWithinMeleeRange(target))
                hati->CastSpell(target, SPELL_KILL_COMMAND_CHARGE, true);
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_lcf2_hun_kill_command_hati::HandleHit, EFFECT_1, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 19574 - Bestial Wrath: Master of Beasts -> 207033 on Hati (the reference core spell_hun_bestial_wrath / SpellAuras.cpp).
class spell_lcf2_hun_bestial_wrath_hati : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_bestial_wrath_hati);

    void HandleAfterCast()
    {
        Unit* hunter = GetCaster();
        if (!hunter->HasAura(SPELL_MASTER_OF_BEASTS))
            return;
        if (Creature* hati = FindHati(hunter))
            hunter->CastSpell(hati, SPELL_BESTIAL_WRATH_HATI, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_hun_bestial_wrath_hati::HandleAfterCast);
    }
};

// ============================================================================================================
// Hunter: Hati's Bond (197344) "Hati will now fight for you as a companion."
// the reference core spell_hun_hatis_bond / spell_hun_broken_bond / Unit.cpp:
//  - summon spell by Titanstrike appearance set: 117 -> 211145, 118 -> 211146, 119 -> 211147, 120/221 -> 211148,
//    otherwise 197388 (client: all six sets 116-120/221 are Titanstrike, all five spells summon a "Hati" entry)
//  - every 2 s: summon Hati if the hunter has a pet and no Hati, despawn Hati when the pet is gone or Hati is more
//    than 100 yd away, copy the pet's react state to Hati
//  - Hati dies -> the hunter gets Broken Bond 211117 (30 s, client); Hati comes back when it has expired
//  - trait removed -> Hati despawns, Broken Bond removed
// The summon spells use SummonProperties 3725 (Control 1 = pet category) -> Guardian with charm info; Hati uses
// npc_lcf2_hati (PetAI + death hook) via creature_template.ScriptName.
// ============================================================================================================
class spell_lcf2_hun_hatis_bond : public AuraScript
{
    PrepareAuraScript(spell_lcf2_hun_hatis_bond);

    static constexpr uint32 TASK_GROUP_HATI = 197344;

    static uint32 SelectSummonSpell(Player* hunter, ObjectGuid const& castItemGuid)
    {
        Item* artifact = castItemGuid.IsEmpty() ? nullptr : hunter->GetItemByGuid(castItemGuid);
        if (!artifact)
            artifact = hunter->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!artifact)
            return SPELL_STORMBOUND_DEFAULT;

        ArtifactAppearanceEntry const* appearance = sArtifactAppearanceStore.LookupEntry(artifact->GetModifier(ITEM_MODIFIER_ARTIFACT_APPEARANCE_ID));
        if (!appearance)
            return SPELL_STORMBOUND_DEFAULT;

        switch (appearance->ArtifactAppearanceSetID)
        {
            case 117: return SPELL_STORMBOUND_SET_117;
            case 118: return SPELL_STORMBOUND_SET_118;
            case 119: return SPELL_STORMBOUND_SET_119;
            case 120:
            case 221: return SPELL_STORMBOUND_SET_120;
            default:  return SPELL_STORMBOUND_DEFAULT;
        }
    }

    static void CheckHati(Unit* unit)
    {
        Player* hunter = unit ? unit->ToPlayer() : nullptr;
        if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive())
            return;

        Aura* bond = hunter->GetAura(SPELL_HATIS_BOND);
        if (!bond)
            return;

        Pet* pet = hunter->GetPet();
        bool petActive = pet && pet->IsAlive();
        Creature* hati = FindHati(hunter);

        if (hati)
        {
            if (!petActive || hunter->GetDistance(hati) > 100.0f)
            {
                hati->DespawnOrUnsummon(1000);
                return;
            }
            hati->SetReactState(pet->GetReactState());
            return;
        }

        if (!petActive || hunter->HasAura(SPELL_BROKEN_BOND))
            return;

        // a dead Hati still attached to the hunter: remove the corpse before summoning a new one
        if (Creature* corpse = FindHati(hunter, false))
            corpse->DespawnOrUnsummon();

        hunter->CastSpell(hunter, SelectSummonSpell(hunter, bond->GetCastItemGUID()), true);
    }

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->GetScheduler().CancelGroup(TASK_GROUP_HATI);
        target->GetScheduler().Schedule(Seconds(2), TASK_GROUP_HATI, [](TaskContext context)
        {
            CheckHati(context.GetUnit());
            context.Repeat(Seconds(2));
        });
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->GetScheduler().CancelGroup(TASK_GROUP_HATI);
        target->RemoveAurasDueToSpell(SPELL_BROKEN_BOND);
        while (Creature* hati = FindHati(target, false))
        {
            hati->DespawnOrUnsummon();
            if (FindHati(target, false) == hati) // not removed from the control list synchronously - stop here
                break;
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_lcf2_hun_hatis_bond::AfterApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_lcf2_hun_hatis_bond::AfterRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Hati (100324, 106548-106551): normal pet AI; on death the owner gets Broken Bond (the reference core Unit.cpp setDeathState:
// m_isHati -> owner->CastSpell(owner, 211117)).
struct npc_lcf2_hati : public PetAI
{
    npc_lcf2_hati(Creature* creature) : PetAI(creature) { }

    void JustDied(Unit* /*killer*/) override
    {
        if (Unit* owner = me->GetOwner())
            owner->CastSpell(owner, SPELL_BROKEN_BOND, true);
        me->DespawnOrUnsummon(1000);
    }
};

// ============================================================================================================
// Hunter: Talon Bond (238089) "When Talon Strike triggers, your pet immediately attacks $s1 times."
// the reference core spell_linked_spell 203560 -> 242735, caster = pet, actiontype 17 CAST_COUNT param 2; SimC sc_hunter.cpp
// executes the pet's talon_slash effectN(1).base_value() times at the Talon Strike target.
// 242735 damage from the client (SpellDescriptionVariables 274):
//   $damage = $<ce> * $<spiked> * $<blink> * ($RAP * 0.333) * (1 + $@versadmg)
//   $ce = 1.7 with 152244 else 1.5, $spiked = 1.1 with 53184, $blink = 1.5 with 130392
// The versatility factor is left to the normal damage bonus of the pet.
// ============================================================================================================
class spell_lcf2_hun_talon_bond : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_talon_bond);

    void HandleAfterCast()
    {
        Unit* hunter = GetCaster();
        AuraEffect const* trait = hunter->GetAuraEffect(SPELL_TALON_BOND, EFFECT_0);
        Unit* pet = hunter->GetGuardianPet();
        if (!trait || !pet || !pet->IsAlive())
            return;

        Unit* target = GetExplTargetUnit();
        if (!target || target == hunter)
            target = hunter->GetVictim();
        if (!target)
            return;

        for (int32 i = 0; i < trait->GetAmount(); ++i)
            pet->CastSpell(target, SPELL_TALON_SLASH, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_hun_talon_bond::HandleAfterCast);
    }
};

class spell_lcf2_hun_talon_slash : public SpellScript
{
    PrepareSpellScript(spell_lcf2_hun_talon_slash);

    void HandleLaunch(SpellEffIndex /*effIndex*/)
    {
        Unit* pet = GetCaster();
        Unit* hunter = pet->GetOwner();
        if (!hunter)
            return;

        auto has = [pet, hunter](uint32 spellId) { return hunter->HasAura(spellId) || pet->HasAura(spellId) || hunter->HasSpell(spellId); };
        float ce = has(SPELL_ADAPTATION) ? 1.7f : 1.5f;
        float spiked = has(SPELL_SPIKED_COLLAR) ? 1.1f : 1.0f;
        float blink = has(SPELL_BLINK_STRIKES) ? 1.5f : 1.0f;

        SetEffectValue(int32(ce * spiked * blink * hunter->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.333f));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_lcf2_hun_talon_slash::HandleLaunch, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ============================================================================================================
// Hunter: Thunderslash (238087) "While Aspect of the Wild is active, Hati and your primary pet also trigger a
// Thunderslash with each auto attack, dealing ${$243234s1*$<mult>} Nature damage."
// the reference core Unit.cpp (dummy proc 238087): if the attacker's owner has Aspect of the Wild 193530 -> attacker casts
// 243234 at the victim. The client puts the trait aura on the hunter (effect 202 on the caster, proc = melee auto
// attack), so the pet's auto attack is caught with the melee damage script hook instead. $<mult> is not resolvable
// from our client data - the spell's own coefficient (AP 0.5 of the pet) is used unchanged.
// ============================================================================================================
class lcf2_hun_thunderslash : public UnitScript
{
public:
    lcf2_hun_thunderslash() : UnitScript("lcf2_hun_thunderslash") { }

    enum : uint32
    {
        SPELL_THUNDERSLASH_TRAIT  = 238087,
        SPELL_THUNDERSLASH_DAMAGE = 243234,
        SPELL_ASPECT_OF_THE_WILD  = 193530
    };

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& /*damage*/) override
    {
        if (!attacker || !target || attacker->GetTypeId() != TYPEID_UNIT)
            return;

        Unit* owner = attacker->GetOwner();
        if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
            return;

        bool isPrimaryPet = attacker->IsPet() && owner->ToPlayer()->GetPet() == attacker;
        if (!isPrimaryPet && !IsHatiEntry(attacker->GetEntry()))
            return;

        if (!owner->HasAura(SPELL_THUNDERSLASH_TRAIT) || !owner->HasAura(SPELL_ASPECT_OF_THE_WILD))
            return;

        // not from inside the melee damage calculation: next update of the attacker
        ObjectGuid victimGuid = target->GetGUID();
        attacker->GetScheduler().Schedule(Milliseconds(1), [victimGuid](TaskContext context)
        {
            Unit* unit = context.GetUnit();
            if (!unit || !unit->IsAlive())
                return;
            if (Unit* victim = ObjectAccessor::GetUnit(*unit, victimGuid))
                if (victim->IsAlive())
                    unit->CastSpell(victim, SPELL_THUNDERSLASH_DAMAGE, true);
        });
    }
};

// ============================================================================================================
// Fishing artifact: Way of the Flounder (201952) "Reduce enemy detection range by $216425s1 yards while fishing."
// Client only: 216425 (aura 152 MOD_DETECTED_RANGE -5, 30 s) while the fishing channel 131476 is active (its E2 is
// a DUMMY aura on the fisher for the channel duration) - applied with the channel aura, removed with it.
// ============================================================================================================
class spell_lcf2_way_of_the_flounder : public AuraScript
{
    PrepareAuraScript(spell_lcf2_way_of_the_flounder);

    enum : uint32
    {
        SPELL_WAY_OF_THE_FLOUNDER      = 201952,
        SPELL_WAY_OF_THE_FLOUNDER_AURA = 216425
    };

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* fisher = GetTarget();
        if (fisher->HasAura(SPELL_WAY_OF_THE_FLOUNDER))
            fisher->CastSpell(fisher, SPELL_WAY_OF_THE_FLOUNDER_AURA, true);
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_WAY_OF_THE_FLOUNDER_AURA);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_lcf2_way_of_the_flounder::AfterApply, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_lcf2_way_of_the_flounder::AfterRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ============================================================================================================
// Monk (Mistweaver): Sheilun, Staff of the Mists - the mist clouds were never generated nor consumed.
//  - 214483 (equip effect of item 128937, aura 226 every 10 s): in combat cast 214501 (cloud AreaTrigger 7267, our
//    spell_areatrigger already maps it). the reference core spell_aura_trigger 214483 -> 214501, hastype 17 LINK_IN_COMBAT.
//  - Effusive Mists (238094) "Effuse causes Sheilun to generate a cloud of mist": the reference core Unit.cpp - on the Effuse
//    heal proc (spell_proc_event family 53 mask0 0x2000000 = Effuse 116694), only in combat, cast 214501.
//  - Sheilun's Gift (205406) "healing the target for $s1 per cloud absorbed": the reference core spell_monk_sheiluns_gift -
//    heal * number of the monk's 214501 clouds, all clouds removed.
//  - Whispers of Shaohao (238130) "each active mist additionally heals a nearby target for $242400s1": the reference core
//    areatrigger_actions 7267 (on despawn, hasspell 238130) -> 242400 from the cloud; spell_target_filter 242400 =
//    nearest 1 ally.
// ============================================================================================================
class spell_lcf2_monk_sheiluns_gift_passive : public AuraScript
{
    PrepareAuraScript(spell_lcf2_monk_sheiluns_gift_passive);

    void HandleTick(AuraEffect const* aurEff)
    {
        Unit* monk = GetTarget();
        if (monk->IsInCombat())
            monk->CastSpell(monk, SPELL_SHEILUNS_GIFT_CLOUD, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_monk_sheiluns_gift_passive::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

class spell_lcf2_monk_effusive_mists : public AuraScript
{
    PrepareAuraScript(spell_lcf2_monk_effusive_mists);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_EFFUSE && GetTarget()->IsInCombat();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        GetTarget()->CastSpell(GetTarget(), SPELL_SHEILUNS_GIFT_CLOUD, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_monk_effusive_mists::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_lcf2_monk_effusive_mists::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_monk_sheiluns_gift : public SpellScript
{
    PrepareSpellScript(spell_lcf2_monk_sheiluns_gift);

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* monk = GetCaster();
        std::vector<AreaTrigger*> clouds = monk->GetAreaTriggers(SPELL_SHEILUNS_GIFT_CLOUD);
        SetHitHeal(GetHitHeal() * int32(clouds.size()));

        bool whispers = monk->HasAura(SPELL_WHISPERS_OF_SHAOHAO);
        for (AreaTrigger* cloud : clouds)
        {
            if (whispers)
                monk->CastSpell(cloud->GetPositionX(), cloud->GetPositionY(), cloud->GetPositionZ(), SPELL_WHISPERS_OF_SHAOHAO_HEAL, true);
            cloud->Remove();
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_lcf2_monk_sheiluns_gift::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// 242400 - Whispers of Shaohao: nearest single ally around the cloud (the reference core spell_target_filter option 4
// SORT_BY_DISTANCE, resizeType 1, count 1).
class spell_lcf2_monk_whispers_of_shaohao_heal : public SpellScript
{
    PrepareSpellScript(spell_lcf2_monk_whispers_of_shaohao_heal);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        WorldLocation const* dest = GetExplTargetDest();
        if (!dest || targets.size() <= 1)
            return;
        KeepNearest(targets, *dest, 1);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_lcf2_monk_whispers_of_shaohao_heal::FilterTargets, EFFECT_0, TARGET_UNIT_DEST_AREA_ALLY);
    }
};

// ============================================================================================================
// Monk (Mistweaver): Blessings of Yu'lon (199665) "Activating Revival summons the spirit of Yu'lon, healing all
// Revival targets for an additional $s1% of Revival's heal over $199671d."
// the reference core spell_trigger 199665 -> 199668 option 3 (DAM_HEALTH: bp = (heal + absorb) * trait% / bp1, bp1 = 6) =
// per-tick amount of the 6 s / 1 s HoT (client 199668); spell_linked_spell 115310 -> 199671 (Yu'lon, visual only,
// its pet aura 210110 is a plain dummy). The divisor 6 equals 199668 duration / period, so it is taken from there.
// ============================================================================================================
class spell_lcf2_monk_blessings_of_yulon : public AuraScript
{
    PrepareAuraScript(spell_lcf2_monk_blessings_of_yulon);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_REVIVAL && eventInfo.GetHealInfo() && eventInfo.GetProcTarget();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        SpellInfo const* hot = sSpellMgr->GetSpellInfo(SPELL_BLESSINGS_OF_YULON_HOT);
        SpellEffectInfo const* hotEffect = hot ? hot->GetEffect(EFFECT_0) : nullptr;
        if (!hotEffect || !hotEffect->ApplyAuraPeriod)
            return;

        int32 ticks = std::max(1, hot->GetMaxDuration() / int32(hotEffect->ApplyAuraPeriod));
        HealInfo* healInfo = eventInfo.GetHealInfo();
        int32 perTick = CalculatePct(int32(healInfo->GetHeal() + healInfo->GetAbsorb()), aurEff->GetAmount()) / ticks;
        if (perTick <= 0)
            return;

        GetTarget()->CastCustomSpell(SPELL_BLESSINGS_OF_YULON_HOT, SPELLVALUE_BASE_POINT0, perTick, eventInfo.GetProcTarget(), true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_monk_blessings_of_yulon::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_lcf2_monk_blessings_of_yulon::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_monk_revival_yulon : public SpellScript
{
    PrepareSpellScript(spell_lcf2_monk_revival_yulon);

    void HandleAfterCast()
    {
        Unit* monk = GetCaster();
        if (monk->HasAura(SPELL_BLESSINGS_OF_YULON))
            monk->CastSpell(monk, SPELL_BLESSINGS_OF_YULON_SUMMON, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_monk_revival_yulon::HandleAfterCast);
    }
};

// ============================================================================================================
// Monk (Windwalker): Crosswinds (195650) "During Fists of Fury, Wind Spirit images of you attack your Fists of Fury
// targets for a total of ${8*$196061s1} additional Physical damage."  (Round LCF2 R23: target choice settled, see tick):
// the reference core spell_trigger 195650 -> 195651 on Fists of Fury (spell_proc_event mask1 0x800000 = 113656).
// 195651 (client) is a 4 s PERIODIC_TRIGGER_SPELL every 0.5 s -> 195653 (Wind Spirit image, visual) = 8 ticks, which
// is exactly the "8 *" of the tooltip -> one 196061 hit per tick. SimC sc_monk.cpp: each image hits a random Fists of
// Fury target. We have no list of Fists of Fury targets here, so the image hits the monk's current victim.
// ============================================================================================================
class spell_lcf2_monk_fists_of_fury_crosswinds : public SpellScript
{
    PrepareSpellScript(spell_lcf2_monk_fists_of_fury_crosswinds);

    void HandleAfterCast()
    {
        Unit* monk = GetCaster();
        if (monk->HasAura(SPELL_CROSSWINDS))
            monk->CastSpell(monk, SPELL_CROSSWINDS_DRIVER, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_monk_fists_of_fury_crosswinds::HandleAfterCast);
    }
};

class spell_lcf2_monk_crosswinds_driver : public AuraScript
{
    PrepareAuraScript(spell_lcf2_monk_crosswinds_driver);

    // Round LCF2 R23: target choice now as the reference core (npcs_special.cpp npc_monk_wind_spirit + spell_generic.cpp
    // spell_gen_monk_crosswinds): each Wind Spirit image picks the NEAREST unit (<= 40 yd, the reference core value) that carries
    // this monk's "Fists of Fury Visual Target" 123154 - i.e. a Fists of Fury target - and the monk casts 196061 on it.
    // 123154 is refreshed on every Fists of Fury hit and lasts 1 s (spell_monk_fists_of_fury_visual), so it marks the
    // current Fists of Fury targets. Fallback (no marked target): the monk's victim / selection as before.
    void HandleTick(AuraEffect const* aurEff)
    {
        Unit* monk = GetTarget();
        Unit* victim = nullptr;

        std::list<Unit*> enemies;
        monk->GetAttackableUnitListInRange(enemies, 40.0f);
        float best = 0.0f;
        for (Unit* enemy : enemies)
        {
            if (!enemy->HasAura(123154, monk->GetGUID()))
                continue;
            float dist = monk->GetExactDist(enemy);
            if (!victim || dist < best)
            {
                victim = enemy;
                best = dist;
            }
        }

        if (!victim)
            victim = monk->GetVictim();
        if (!victim)
            if (Player* player = monk->ToPlayer())
                victim = player->GetSelectedUnit();
        if (victim && monk->IsValidAttackTarget(victim))
            monk->CastSpell(victim, SPELL_CROSSWINDS_DAMAGE, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_monk_crosswinds_driver::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// ============================================================================================================
// Fishing artifact (Underlight Angler): Undercurrent (201891) "Teleport to the nearest fishing node."
// the reference core spell_undercurrent_fishing / _tele: not in combat, nearest GAMEOBJECT_TYPE_FISHINGHOLE within 100 yd
// (the reference core value), then 216426 (client: SPELL_EFFECT_TELEPORT_UNITS to the destination) to the node.
// ============================================================================================================
class spell_lcf2_undercurrent : public SpellScript
{
    PrepareSpellScript(spell_lcf2_undercurrent);

    static constexpr float SEARCH_RANGE = 100.0f; // the reference core spell_generic.cpp

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        if (caster->IsInCombat())
            return SPELL_FAILED_AFFECTING_COMBAT;
        if (!caster->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_FISHINGHOLE, SEARCH_RANGE))
            return SPELL_FAILED_NO_VALID_TARGETS;
        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (GameObject* node = caster->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_FISHINGHOLE, SEARCH_RANGE))
            caster->CastSpell(node->GetPositionX(), node->GetPositionY(), node->GetPositionZ(), SPELL_UNDERCURRENT_TELEPORT, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_lcf2_undercurrent::CheckCast);
        OnEffectHit += SpellEffectFn(spell_lcf2_undercurrent::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// ============================================================================================================
// Priest: Lash of Insanity (238137) - Void Tendril (98167) Mind Flay 193473 ticks give the priest 240843
// (ENERGIZE 300 = 3 Insanity). the reference core spell_aura_trigger 193473 -> 240843, caster 2 (owner), hastype 3 (aura on
// owner) 238137, slot 98167 (caster entry).
// ============================================================================================================
class spell_lcf2_pri_lash_of_insanity : public AuraScript
{
    PrepareAuraScript(spell_lcf2_pri_lash_of_insanity);

    void HandleTick(AuraEffect const* aurEff)
    {
        Unit* tendril = GetCaster();
        if (!tendril || tendril->GetEntry() != NPC_VOID_TENDRIL)
            return;
        Unit* priest = tendril->GetOwner();
        if (priest && priest->HasAura(SPELL_LASH_OF_INSANITY))
            priest->CastSpell(priest, SPELL_LASH_OF_INSANITY_ENERGIZE, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_pri_lash_of_insanity::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// ============================================================================================================
// Warlock: Stolen Power (211530) "When your Wild Imps cast Firebolt, you gain an application of Stolen Power. After
// you reach $211529u applications, your next Shadowbolt/Demonbolt deals ... increased damage."
// the reference core: spell_pet_auras 55659 -> 211592 (only with 211530 on the owner), spell_proc_check 211592 = Firebolt
// 104318/3110 -> stack 211529 on the warlock; spell_warl_stolen_power: on a 211529 tick with >= 100 stacks cast 211583
// and drop the stacks. Our Wild Imps (99739, npc_pet_warlock_wild_imp) cast 104318 directly, so the stack is added
// from the Firebolt itself (same trigger) with the warlock as caster so all imps feed one stack.
// ============================================================================================================
class spell_lcf2_warl_stolen_power_firebolt : public SpellScript
{
    PrepareSpellScript(spell_lcf2_warl_stolen_power_firebolt);

    void HandleAfterHit()
    {
        Unit* imp = GetCaster();
        Unit* warlock = imp->GetOwner();
        if (!warlock || !warlock->HasAura(SPELL_STOLEN_POWER))
            return;
        warlock->AddAura(SPELL_STOLEN_POWER_STACK, warlock);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_lcf2_warl_stolen_power_firebolt::HandleAfterHit);
    }
};

class spell_lcf2_warl_stolen_power_stack : public AuraScript
{
    PrepareAuraScript(spell_lcf2_warl_stolen_power_stack);

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        if (GetStackAmount() < GetSpellInfo()->StackAmount)
            return;
        GetTarget()->CastSpell(GetTarget(), SPELL_STOLEN_POWER_BUFF, true);
        Remove();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_warl_stolen_power_stack::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// ============================================================================================================
// Warlock: Doom, Doubled (218572) "Doom has a chance to deal double damage."
// the reference core: spell_proc_event 218572 (DONE_PERIODIC, chance 35 = client E0 BP), spell_proc_check = Doom 603 ->
// 218572 E0 trigger 218571 (client, charges 1); spell_aura_dummy 603/218571 option 9 DAMAGE_ADD_PERC on Doom E0 while
// the warlock has 218571 (+218571 E0 = 100 %). 218571's own proc is suppressed; it is consumed by the doubled tick.
// ============================================================================================================
class spell_lcf2_warl_doom_doubled : public AuraScript
{
    PrepareAuraScript(spell_lcf2_warl_doom_doubled);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_DOOM;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        GetTarget()->CastSpell(GetTarget(), SPELL_DOOM_DOUBLED_BUFF, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_warl_doom_doubled::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_lcf2_warl_doom_doubled::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_warl_doom_doubled_buff : public AuraScript
{
    PrepareAuraScript(spell_lcf2_warl_doom_doubled_buff);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return false; // consumed by spell_lcf2_warl_doom_tick
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_warl_doom_doubled_buff::CheckProc);
    }
};

class spell_lcf2_warl_doom_tick : public AuraScript
{
    PrepareAuraScript(spell_lcf2_warl_doom_tick);

    int32 _restoreAmount = 0;

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        AuraEffect* effect = GetAura()->GetEffect(EFFECT_0);
        if (!effect)
            return;

        if (_restoreAmount)
        {
            effect->SetAmount(_restoreAmount);
            _restoreAmount = 0;
        }

        Unit* caster = GetCaster();
        if (!caster)
            return;

        AuraEffect const* doubled = caster->GetAuraEffect(SPELL_DOOM_DOUBLED_BUFF, EFFECT_0);
        if (!doubled)
            return;

        int32 bonusPct = doubled->GetAmount();
        caster->RemoveAurasDueToSpell(SPELL_DOOM_DOUBLED_BUFF);

        _restoreAmount = effect->GetAmount();
        effect->SetAmount(AddPct(_restoreAmount, bonusPct));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_warl_doom_tick::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// ============================================================================================================
// Shaman: Alpha Wolf (198434) "While Feral Spirits are active, Crash Lightning causes your wolves to attack all nearby
// enemies for the next $198486d." the reference core spell_linked_spell 187874 -> 198486 actiontype 22 CAST_ON_SUMMON on
// 29264 and 100820 (with 198434); spell_sha_alpha_wolf: every 198486 tick (2 s, client) the wolf casts by model
// Spirit Bomb / Fire Nova / Snowstorm / Thunder Bite (victim). Our plain Spirit Wolf 29264 uses model 21114 instead of
// the reference core's 55290, so it is matched by entry (SimC: spirit wolf -> spirit_bomb).
// ============================================================================================================
class spell_lcf2_sha_crash_lightning_alpha_wolf : public SpellScript
{
    PrepareSpellScript(spell_lcf2_sha_crash_lightning_alpha_wolf);

    void HandleAfterCast()
    {
        Unit* shaman = GetCaster();
        if (!shaman->HasAura(SPELL_ALPHA_WOLF))
            return;

        std::vector<Unit*> wolves;
        for (Unit* controlled : shaman->m_Controlled)
            if (controlled && controlled->IsAlive() && (controlled->GetEntry() == NPC_SPIRIT_WOLF || controlled->GetEntry() == NPC_DOOM_WOLF))
                wolves.push_back(controlled);

        for (Unit* wolf : wolves)
            shaman->CastSpell(wolf, SPELL_ALPHA_WOLF_AURA, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_lcf2_sha_crash_lightning_alpha_wolf::HandleAfterCast);
    }
};

class spell_lcf2_sha_alpha_wolf_aura : public AuraScript
{
    PrepareAuraScript(spell_lcf2_sha_alpha_wolf_aura);

    void HandleTick(AuraEffect const* aurEff)
    {
        Unit* wolf = GetTarget();
        if (wolf->GetEntry() == NPC_SPIRIT_WOLF)
        {
            wolf->CastSpell(wolf, SPELL_SPIRIT_BOMB, true, nullptr, aurEff);
            return;
        }

        switch (wolf->GetDisplayId())
        {
            case MODEL_DOOM_WOLF_FIRE:
                wolf->CastSpell(wolf, SPELL_FIRE_NOVA_WOLF, true, nullptr, aurEff);
                break;
            case MODEL_DOOM_WOLF_FROST:
                wolf->CastSpell(wolf, SPELL_SNOWSTORM, true, nullptr, aurEff);
                break;
            case MODEL_DOOM_WOLF_LIGHTNING:
                if (Unit* victim = wolf->GetVictim())
                    wolf->CastSpell(victim, SPELL_THUNDER_BITE, true, nullptr, aurEff);
                break;
            default:
                wolf->CastSpell(wolf, SPELL_SPIRIT_BOMB, true, nullptr, aurEff);
                break;
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_lcf2_sha_alpha_wolf_aura::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 198506 - Feral Spirit (Doom Wolves summon, used by spell_sha_feral_spirit when 198505 is known).
// Doom Wolves (198505) "imbues each of your Feral Spirits with Fire, Frost, or Lightning, granting them an extra
// ability". Round LCF2 R38 (2026-09-26), final closure round - re-verified with new methods, still ANNAHME:
// - Client DB2 spellcooldowns.csv (build 26972) has no row for 224125/224126/224127 - no cooldown is data-driven.
// - SimulationCraft (github.com/simulationcraft/simc, engine/class_modules/sc_shaman.cpp, frost_wolf_t::create_default_apl
//   / fire_wolf_t::create_default_apl / lightning_wolf_t::create_default_apl) sets "line_cd=5" for all three abilities,
//   but each is directly preceded by the developer comment "// TODO: Proper delay" - i.e. SimC's own authors explicitly
//   flag 5 s as an unverified placeholder, not a datamined or tested value. This is new, harder evidence than before:
//   it proves the uncertainty is structural (nobody, including the reference theorycrafting tool, ever confirmed a
//   real cadence), not just "no source found yet" - see report lcf2r38_2026-09-26_traits_final.md, section "Doom Wolves".
// - No further source exists (the reference core has no Doom Wolf AI at all; TDB837/1210/SkyFire/Draenor-Core precede or
//   postdate Legion; no relevant Wowhead/MMO-Champion/Reddit/Icy-Veins Legion-era comment found for the wolves'
//   attack cadence specifically, only for the trait's existence).
// Kept at 5 s (matches the widely-used SimC default so damage output stays roughly comparable), explicitly marked
// ANNAHME - correct on request if a real source ever surfaces.
class spell_lcf2_sha_doom_wolves_summon : public SpellScript
{
    PrepareSpellScript(spell_lcf2_sha_doom_wolves_summon);

    void HandleSummon(Creature* wolf)
    {
        wolf->GetScheduler().Schedule(Seconds(1), [](TaskContext context)
        {
            Unit* unit = context.GetUnit();
            if (!unit || !unit->IsAlive())
                return;

            Unit* victim = unit->GetVictim();
            if (victim && unit->IsWithinMeleeRange(victim))
            {
                switch (unit->GetDisplayId())
                {
                    case MODEL_DOOM_WOLF_FIRE:      unit->CastSpell(victim, SPELL_FIERY_JAWS, true); break;
                    case MODEL_DOOM_WOLF_FROST:     unit->CastSpell(victim, SPELL_FROZEN_BITE, true); break;
                    case MODEL_DOOM_WOLF_LIGHTNING: unit->CastSpell(unit, SPELL_CRACKLING_SURGE, true); break;
                    default: break;
                }
                context.Repeat(Seconds(5));
                return;
            }
            context.Repeat(Seconds(1));
        });
    }

    void Register() override
    {
        OnEffectSummon += SpellOnEffectSummonFn(spell_lcf2_sha_doom_wolves_summon::HandleSummon);
    }
};

// ============================================================================================================
// Shaman (Restoration): Tidal Pools (207358) "Riptide has a chance to summon a Tidal Totem at the target's location,
// which heals nearby allies for ${7*$209069s1} over $208932d." (client proc chance 20 %)
// the reference core: spell_proc_event 207358 = Riptide, spell_trigger -> 208932 (Tidal Totem 105422), pet aura 233487
// (AreaTrigger 9449: radius 8, updateDelay 850 ms, each update casts 209069; spell_target_filter 209069 = nearest 6).
// 6000 ms / 850 ms = 7 pulses = the "7 *" of the tooltip. The totem creature itself is not summoned here: its
// SummonProperties 3803 (Control 1) would make it a following PetAI guardian in our core. Instead the AreaTrigger
// visual 233487 is cast at the Riptide target and the 7 heal pulses come from that fixed spot (shaman as caster).
// ============================================================================================================
class spell_lcf2_sha_tidal_pools : public AuraScript
{
    PrepareAuraScript(spell_lcf2_sha_tidal_pools);

    static constexpr uint32 PULSE_MS = 850; // the reference core areatrigger_data 9449 updateDelay

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == SPELL_RIPTIDE && eventInfo.GetProcTarget();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* shaman = GetTarget();
        Unit* target = eventInfo.GetProcTarget();
        shaman->CastSpell(target, SPELL_TIDAL_TOTEM_AT, true, nullptr, aurEff);

        SpellInfo const* totem = sSpellMgr->GetSpellInfo(SPELL_TIDAL_TOTEM_SUMMON);
        int32 duration = totem ? totem->GetMaxDuration() : 0;
        if (duration <= 0)
            return;

        Position pos = target->GetPosition();
        uint32 pulses = uint32(duration) / PULSE_MS;
        for (uint32 i = 1; i <= pulses; ++i)
        {
            shaman->GetScheduler().Schedule(Milliseconds(i * PULSE_MS), [pos](TaskContext context)
            {
                if (Unit* caster = context.GetUnit())
                    caster->CastSpell(pos, SPELL_TIDAL_TOTEM_HEAL, true);
            });
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_sha_tidal_pools::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_lcf2_sha_tidal_pools::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_sha_tidal_totem_heal : public SpellScript
{
    PrepareSpellScript(spell_lcf2_sha_tidal_totem_heal);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        WorldLocation const* dest = GetExplTargetDest();
        if (!dest || targets.size() <= 6)
            return;
        KeepNearest(targets, *dest, 6); // the reference core spell_target_filter 209069: count 6
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_lcf2_sha_tidal_totem_heal::FilterTargets, EFFECT_0, TARGET_UNIT_DEST_AREA_ALLY);
    }
};

// ============================================================================================================
// Death Knight: The Shambler (191760, client RPPM 1.5 on melee) "Your attacks have a chance to summon a Super Zombie
// that shambles forward and explodes, dealing $191758s1 Shadow damage to nearby enemies."
// the reference core spell_trigger 191760 -> 191759 (summon 97055, 3 s); spell_pet_auras 97055: 191759 option 7 (move to its
// target), -191758 option 8 (on unsummon cast Necrobomb, owner as original caster). The Shambling Horror is a PetAI
// guardian here (SummonProperties 3871 Control 1) and runs to the target by itself; it explodes just before expiry.
// ============================================================================================================
class spell_lcf2_dk_the_shambler : public AuraScript
{
    PrepareAuraScript(spell_lcf2_dk_the_shambler);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* target = eventInfo.GetProcTarget();
        if (target && target != GetTarget())
            GetTarget()->CastSpell(target, SPELL_SUMMON_SHAMBLING_HORROR, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_lcf2_dk_the_shambler::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_dk_summon_shambling_horror : public SpellScript
{
    PrepareSpellScript(spell_lcf2_dk_summon_shambling_horror);

    void HandleSummon(Creature* horror)
    {
        Unit* dk = GetCaster();
        if (Unit* target = GetExplTargetUnit())
            if (horror->IsAIEnabled && dk->IsValidAttackTarget(target))
                horror->AI()->AttackStart(target);

        int32 duration = GetSpellInfo()->GetMaxDuration();
        uint32 fuse = duration > 200 ? uint32(duration - 200) : 0;
        ObjectGuid dkGuid = dk->GetGUID();
        horror->GetScheduler().Schedule(Milliseconds(fuse), [dkGuid](TaskContext context)
        {
            Unit* unit = context.GetUnit();
            if (unit && unit->IsAlive())
                unit->CastSpell(unit, SPELL_NECROBOMB, true, nullptr, nullptr, dkGuid);
        });
    }

    void Register() override
    {
        OnEffectSummon += SpellOnEffectSummonFn(spell_lcf2_dk_summon_shambling_horror::HandleSummon);
    }
};

// ============================================================================================================
// Death Knight: Armies of the Damned (191731) "Ghouls summoned by Army of the Dead apply additional effects with their
// Claw attack" - Death / War / Famine / Pestilence = 191730 / 191729 / 191727 / 191728 (client).
// the reference core spell_dk_claw_owner (on 199373): on every Claw hit one of the four at random, DK as original caster.
// R37 (26.09.2026): RESOLVED in favor of the reference core's "every hit" model. Client DB2 spellauraoptions.csv
// (build 26972) gives all four buffs (191727-191730) ProcChance = 101, the same non-percentage sentinel value
// already confirmed on a known script-applied buff (Precise Strikes 248195, Round LCF2 R23) - i.e. the client
// itself marks these as script-driven, not RNG-gated. SimulationCraft's 20 % was a configurable default/estimate
// (sc_death_knight.cpp comment, 2016-08-23), not a sourced value. the reference core's deterministic "every Claw hit" is
// the correct model and is what this script already implements; no code change needed, this is a documentation
// upgrade only. The ghoul's Claw usage itself is in pet_dk.cpp (npc_pet_dk_army_of_the_dead_ghoul).
// ============================================================================================================
class spell_lcf2_dk_army_claw : public SpellScript
{
    PrepareSpellScript(spell_lcf2_dk_army_claw);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* ghoul = GetCaster();
        Unit* dk = ghoul->GetOwner();
        Unit* target = GetHitUnit();
        if (!dk || !target || !dk->HasAura(191731))
            return;

        static uint32 const effects[] = { 191727, 191728, 191729, 191730 };
        ghoul->CastSpell(target, effects[urand(0, 3)], true, nullptr, nullptr, dk->GetGUID());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_lcf2_dk_army_claw::HandleHit, EFFECT_0, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

// 218321 - Dragged to Helheim (Portal to the Underworld, cast by the Army ghoul in pet_dk.cpp): the client puts the
// damage ("$s1", E0: BP 0, AP coefficient 1.62) on an effect without targets and the area hit on E1 (BP 0, no
// coefficient). E1 gets E0's value: 1.62 * attack power of the original caster (the death knight, whose tooltip shows
// "$218321s1"). the reference core instead overrides the AP bonus in spell_bonus_data (3.24) - not used.
class spell_lcf2_dk_dragged_to_helheim : public SpellScript
{
    PrepareSpellScript(spell_lcf2_dk_dragged_to_helheim);

    void HandleLaunch(SpellEffIndex /*effIndex*/)
    {
        Unit* source = GetOriginalCaster() ? GetOriginalCaster() : GetCaster();
        SpellEffectInfo const* valueEffect = GetSpellInfo()->GetEffect(EFFECT_0);
        if (!source || !valueEffect)
            return;
        SetEffectValue(GetEffectValue() + int32(valueEffect->BonusCoefficientFromAP * source->GetTotalAttackPowerValue(BASE_ATTACK)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_lcf2_dk_dragged_to_helheim::HandleLaunch, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ============================================================================================================
// Rogue: Akaari's Soul (209835) "After using Shadowstrike or Cheap Shot, Akaari's Soul appears $m1 sec later and Soul
// Rips your target, dealing $220893s1 Shadow damage." the reference core spell_trigger 209835 -> 209837 option 45 CAST_DELAY
// bp0 2000 (= client $m1 2 s), spell_proc_event masks = Cheap Shot / Shadowstrike; spell_pet_auras 105850 -> 220893
// at the owner's selected target on summon. The rogue is original caster of Soul Rip (tooltip value is the rogue's).
// ============================================================================================================
class spell_lcf2_rog_akaaris_soul : public AuraScript
{
    PrepareAuraScript(spell_lcf2_rog_akaaris_soul);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && (spellInfo->Id == SPELL_SHADOWSTRIKE || spellInfo->Id == SPELL_CHEAP_SHOT);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        uint32 delayMs = uint32(std::max(0, aurEff->GetAmount())) * IN_MILLISECONDS;
        GetTarget()->GetScheduler().Schedule(Milliseconds(delayMs), [](TaskContext context)
        {
            if (Unit* rogue = context.GetUnit())
                if (rogue->IsAlive())
                    rogue->CastSpell(rogue, SPELL_AKAARIS_SOUL_SUMMON, true);
        });
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_lcf2_rog_akaaris_soul::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_lcf2_rog_akaaris_soul::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_lcf2_rog_akaaris_soul_summon : public SpellScript
{
    PrepareSpellScript(spell_lcf2_rog_akaaris_soul_summon);

    void HandleSummon(Creature* akaari)
    {
        Unit* rogue = GetCaster();
        Unit* target = nullptr;
        if (Player* player = rogue->ToPlayer())
            target = player->GetSelectedUnit();
        if (!target || !rogue->IsValidAttackTarget(target))
            target = rogue->GetVictim();
        if (target && rogue->IsValidAttackTarget(target))
            akaari->CastSpell(target, SPELL_SOUL_RIP, true, nullptr, nullptr, rogue->GetGUID());
    }

    void Register() override
    {
        OnEffectSummon += SpellOnEffectSummonFn(spell_lcf2_rog_akaaris_soul_summon::HandleSummon);
    }
};

void AddSC_lcf2_2026_09_25_spell_scripts()
{
    RegisterAuraScript(spell_lcf2_mage_mark_of_aluneth);
    RegisterSpellScript(spell_lcf2_mage_mark_of_aluneth_detonation);
    RegisterAuraScript(spell_lcf2_pal_judge_unworthy);
    RegisterSpellScript(spell_lcf2_hun_titans_thunder);
    RegisterSpellScript(spell_lcf2_hun_titans_thunder_pulse);
    RegisterAuraScript(spell_lcf2_hun_titans_thunder_aura);
    RegisterSpellScript(spell_lcf2_hun_dire_frenzy_titans_thunder);
    RegisterSpellScript(spell_lcf2_hun_multi_shot_titanstrike);
    RegisterSpellScript(spell_lcf2_hun_kill_command_hati);
    RegisterSpellScript(spell_lcf2_hun_bestial_wrath_hati);
    RegisterAuraScript(spell_lcf2_hun_hatis_bond);
    RegisterCreatureAI(npc_lcf2_hati);
    RegisterSpellScript(spell_lcf2_hun_talon_bond);
    RegisterSpellScript(spell_lcf2_hun_talon_slash);
    RegisterAuraScript(spell_lcf2_monk_sheiluns_gift_passive);
    RegisterAuraScript(spell_lcf2_monk_effusive_mists);
    RegisterSpellScript(spell_lcf2_monk_sheiluns_gift);
    RegisterSpellScript(spell_lcf2_monk_whispers_of_shaohao_heal);
    RegisterAuraScript(spell_lcf2_monk_blessings_of_yulon);
    RegisterSpellScript(spell_lcf2_monk_revival_yulon);
    RegisterSpellScript(spell_lcf2_monk_fists_of_fury_crosswinds);
    RegisterAuraScript(spell_lcf2_monk_crosswinds_driver);
    RegisterSpellScript(spell_lcf2_undercurrent);
    RegisterAuraScript(spell_lcf2_pri_lash_of_insanity);
    RegisterSpellScript(spell_lcf2_warl_stolen_power_firebolt);
    RegisterAuraScript(spell_lcf2_warl_stolen_power_stack);
    RegisterAuraScript(spell_lcf2_warl_doom_doubled);
    RegisterAuraScript(spell_lcf2_warl_doom_doubled_buff);
    RegisterAuraScript(spell_lcf2_warl_doom_tick);
    RegisterSpellScript(spell_lcf2_sha_crash_lightning_alpha_wolf);
    RegisterAuraScript(spell_lcf2_sha_alpha_wolf_aura);
    RegisterSpellScript(spell_lcf2_sha_doom_wolves_summon);
    RegisterAuraScript(spell_lcf2_sha_tidal_pools);
    RegisterSpellScript(spell_lcf2_sha_tidal_totem_heal);
    RegisterAuraScript(spell_lcf2_dk_the_shambler);
    RegisterSpellScript(spell_lcf2_dk_summon_shambling_horror);
    RegisterSpellScript(spell_lcf2_dk_army_claw);
    RegisterSpellScript(spell_lcf2_dk_dragged_to_helheim);
    new lcf2_hun_thunderslash();
    RegisterAuraScript(spell_lcf2_way_of_the_flounder);
    RegisterAuraScript(spell_lcf2_rog_akaaris_soul);
    RegisterSpellScript(spell_lcf2_rog_akaaris_soul_summon);
}
