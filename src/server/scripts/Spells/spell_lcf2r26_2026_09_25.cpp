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

// Legion-Server round "LCF2 R26" (2026-09-25): quest credits that a reference implementation only grants from its
// compiled C++ (list: reports\lcf2r25_2026-09-25_quest_credit_endstand.tsv, status "Kandidat R26").
// Logic ported from that reference implementation:
//   scripts/Legion/warden_prison.cpp (npc_q38723, npc_q39683, npc_dh_questgiver_96675, go_q39687,
//   npc_dh_questgiver_97644, npc_q39694), scripts/World/npcs_special.cpp (npc_iot_ritual_stone),
//   scripts/EasternKingdoms/zone_stormwind_city.cpp + scripts/Kalimdor/zone_durotar.cpp (Spectral Sight credit).
// The code is our own (our hook signatures, our APIs); ids/numbers checked against client 7.3.5.26972.
// Binding SQL: C:\LegionServer\fixes\lcf2r26_2026-09-25_scripts.sql - without it every script here is inert.

#include "Creature.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include <list>

namespace
{
    bool HasAnyQuestIncomplete(Player const* player, std::initializer_list<uint32> quests)
    {
        for (uint32 questId : quests)
            if (player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                return true;
        return false;
    }
}

// ============================================================================================================
// Demon Hunter start, Vault of the Wardens (map 1468). Our zone_vault_of_wardens.cpp is empty; the bosses fought as
// plain creatures and never granted the "slain & power taken" credits, so the main line stopped at 38723/40253.
// ============================================================================================================

enum R26VaultData : uint32
{
    NPC_SLEDGE                      = 92990,
    NPC_CRUSHER                     = 97632,

    QUEST_STOP_GULDAN_1             = 38723,
    QUEST_STOP_GULDAN_2             = 40253,
    QUEST_FORGED_IN_FIRE_1          = 39683,
    QUEST_FORGED_IN_FIRE_2          = 40254,
    QUEST_BETWEEN_US_1              = 39688,
    QUEST_BETWEEN_US_2              = 39694,
    QUEST_BETWEEN_US_3              = 40255,
    QUEST_BETWEEN_US_4              = 40256,
    QUEST_ALL_THE_WAY_UP            = 39686,
    QUEST_A_NEW_DIRECTION           = 40373,
    QUEST_A_NEW_DIRECTION_LC        = 39687, // LC go_q39687 checks this id; our DB has the objective in 40373

    CREDIT_CRUSHER_SLEDGE_POWER     = 106241,
    CREDIT_IMMOLANTH_POWER          = 106254,
    CREDIT_BASTILLAX_POWER          = 106255,
    CREDIT_KHADGAR_ARRIVED          = 113812,
    CREDIT_USE_ELEVATOR             = 96814,
    CREDIT_POOL_OF_JUDGMENT         = 100166,

    SPELL_STOP_GULDAN_TAKING_POWER  = 210461, // client: "Stop Gul'dan!: Taking Power 02"
    SPELL_LEARN_VENGEFUL_RETREAT    = 195327, // client: LEARN_SPELL 198793
    SPELL_FORGED_TAKING_POWER       = 210486, // client: "Forged in Fire: Taking Power 02"
    SPELL_LEARN_CHAOS_NOVA          = 195440, // client: LEARN_SPELL 179057
    SPELL_BETWEEN_US_TAKING_POWER   = 210500, // client: "Between Us and Freedom: Taking Power 02"
    SPELL_LEARN_BLUR                = 195450, // client: LEARN_SPELL 198589
    SPELL_PLAY_MOVIE_POOL           = 226867, // client: Play Movie 478
    SPELL_CHOICE_KAYN_ALTRUIS       = 196650, // client: LAUNCH_QUEST_CHOICE 234
    SPELL_CHOSE_KAYN                = 196661, // client: KILL_CREDIT + QUEST_COMPLETE
    SPELL_CHOSE_ALTRUIS             = 196662,

    PLAYER_CHOICE_KAYN_ALTRUIS      = 234,
    PLAYER_CHOICE_RESPONSE_KAYN     = 486,
    PLAYER_CHOICE_RESPONSE_ALTRUIS  = 487,

    // combat spells (LC timers)
    SPELL_ANNIHILATE                = 199602, // Sledge
    SPELL_FACE_KICK                 = 199645, // Crusher
    SPELL_BRUTAL_ATTACKS            = 199556, // both
    SPELL_BURNING_FEL               = 199758, // Immolanth
    SPELL_FEL_SPIRIT                = 199836,
    SPELL_IMMOLANTH_CHAOS_NOVA      = 199828,
    SPELL_CRUSHING_SHADOWS          = 200027, // Bastillax
    SPELL_FEL_ANNIHILATION          = 200007,
    SPELL_PORTAL_SURGE              = 200353,
    SPELL_BLUR_OF_SHADOWS           = 200002,
    NPC_SAVAGE_FELSTALKER           = 101505,

    EVENT_R26_1 = 1,
    EVENT_R26_2,
    EVENT_R26_3,
    EVENT_R26_4
};

// 92990 Sledge, 97632 Crusher (LC npc_q38723). Shared health (LC DamageTaken), credit + Vengeful Retreat on death.
// Differences to LC: partner lookup fixed (LC searched its own entry), no uint32 underflow when mirroring health,
// LC conversations 528-530 and the "attack nearby NPCs on reset" ambience are not ported (cosmetic).
struct npc_r26_vault_crusher_sledge : public ScriptedAI
{
    npc_r26_vault_crusher_sledge(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        _events.Reset();
    }

    void EnterCombat(Unit* /*who*/) override
    {
        if (me->GetEntry() == NPC_SLEDGE)
            _events.ScheduleEvent(EVENT_R26_1, 20000);
        else
            _events.ScheduleEvent(EVENT_R26_2, 16000);
        _events.ScheduleEvent(EVENT_R26_3, 42000);
    }

    Creature* GetPartner() const
    {
        return me->FindNearestCreature(me->GetEntry() == NPC_SLEDGE ? NPC_CRUSHER : NPC_SLEDGE, 100.0f, true);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage) override
    {
        Creature* partner = GetPartner();
        if (!partner)
            return;

        if (damage >= me->GetHealth())
            me->Kill(partner);
        else
            partner->SetHealth(std::min<uint64>(partner->GetHealth(), me->GetHealth() - damage));
    }

    void JustDied(Unit* /*killer*/) override
    {
        std::list<Player*> players;
        me->GetPlayerListInGrid(players, 200.0f);
        for (Player* player : players)
        {
            if (!HasAnyQuestIncomplete(player, { QUEST_STOP_GULDAN_1, QUEST_STOP_GULDAN_2 }))
                continue;

            me->CastSpell(player, SPELL_STOP_GULDAN_TAKING_POWER, true);
            player->KilledMonsterCredit(CREDIT_CRUSHER_SLEDGE_POWER);
            player->CastSpell(player, SPELL_LEARN_VENGEFUL_RETREAT, true);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_R26_1: DoCastVictim(SPELL_ANNIHILATE);  _events.Repeat(20000); break;
                case EVENT_R26_2: DoCastVictim(SPELL_FACE_KICK);   _events.Repeat(16000); break;
                case EVENT_R26_3: DoCastVictim(SPELL_BRUTAL_ATTACKS); _events.Repeat(42000); break;
                default: break;
            }
        }
        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
};

// 96682 Immolanth (LC npc_q39683). Credit + Chaos Nova on death. LC's health hacks (21 % start health, damage
// multiplier "until event finished") are deliberately not ported. LC grants only the killer; here every player within
// 60 yd who has the quest (same as LC does for Beliash/Bastillax).
struct npc_r26_vault_immolanth : public ScriptedAI
{
    npc_r26_vault_immolanth(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        _events.Reset();
        _novaDone = false;
    }

    void EnterCombat(Unit* /*who*/) override
    {
        _events.ScheduleEvent(EVENT_R26_1, 20000);
        _events.ScheduleEvent(EVENT_R26_2, 15000);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& /*damage*/) override
    {
        if (!_novaDone && me->HealthBelowPct(5))
        {
            _novaDone = true;
            DoCastSelf(SPELL_IMMOLANTH_CHAOS_NOVA);
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        std::list<Player*> players;
        me->GetPlayerListInGrid(players, 60.0f);
        for (Player* player : players)
        {
            if (!HasAnyQuestIncomplete(player, { QUEST_FORGED_IN_FIRE_1, QUEST_FORGED_IN_FIRE_2 }))
                continue;

            player->CastSpell(player, SPELL_FORGED_TAKING_POWER, true);
            player->CastSpell(player, SPELL_LEARN_CHAOS_NOVA, true);
            player->KilledMonsterCredit(CREDIT_IMMOLANTH_POWER);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_R26_1: DoCastSelf(SPELL_BURNING_FEL); _events.Repeat(40000); break;
                case EVENT_R26_2: DoCastVictim(SPELL_FEL_SPIRIT); _events.Repeat(31000); break;
                default: break;
            }
        }
        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
    bool _novaDone = false;
};

// 96783 Bastillax (LC npc_q39694). Credit 106255 + 113812 + Blur on death; LC spell timers and the 2x4 Savage
// Felstalker adds (101505) of Portal Surge. LC creature_text is not in our DB -> no Talk().
struct npc_r26_vault_bastillax : public ScriptedAI
{
    npc_r26_vault_bastillax(Creature* creature) : ScriptedAI(creature), _summons(creature) { }

    void Reset() override
    {
        _events.Reset();
        _summons.DespawnAll();
    }

    void EnterCombat(Unit* /*who*/) override
    {
        _events.ScheduleEvent(EVENT_R26_1, 45000);
        _events.ScheduleEvent(EVENT_R26_2, 5000);
        _events.ScheduleEvent(EVENT_R26_3, 15000);
        _events.ScheduleEvent(EVENT_R26_4, 23000);
    }

    void JustSummoned(Creature* summon) override
    {
        _summons.Summon(summon);
    }

    void JustDied(Unit* /*killer*/) override
    {
        _summons.DespawnAll();

        std::list<Player*> players;
        me->GetPlayerListInGrid(players, 60.0f);
        for (Player* player : players)
        {
            if (!HasAnyQuestIncomplete(player, { QUEST_BETWEEN_US_1, QUEST_BETWEEN_US_2, QUEST_BETWEEN_US_3, QUEST_BETWEEN_US_4 }))
                continue;

            player->CastSpell(player, SPELL_BETWEEN_US_TAKING_POWER, true);
            player->CastSpell(player, SPELL_LEARN_BLUR, true);
            player->KilledMonsterCredit(CREDIT_BASTILLAX_POWER);
            player->KilledMonsterCredit(CREDIT_KHADGAR_ARRIVED);
        }
    }

    void SummonFelstalkers(float x, float y, float o)
    {
        for (uint8 i = 0; i < 4; ++i)
        {
            if (Creature* add = me->SummonCreature(NPC_SAVAGE_FELSTALKER, x + irand(-3, 3), y + irand(-3, 3), 255.12f, o,
                TEMPSUMMON_CORPSE_TIMED_DESPAWN, 10000))
            {
                if (Unit* victim = me->GetVictim())
                    add->AI()->AttackStart(victim);
            }
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_R26_1: DoCastSelf(SPELL_CRUSHING_SHADOWS); _events.Repeat(45000); break;
                case EVENT_R26_2: DoCastVictim(SPELL_FEL_ANNIHILATION); _events.Repeat(35000); break;
                case EVENT_R26_3:
                    DoCastSelf(SPELL_PORTAL_SURGE);
                    SummonFelstalkers(4221.51f, -627.92f, 3.10f);
                    SummonFelstalkers(4146.97f, -626.29f, 6.27f);
                    _events.Repeat(50000);
                    break;
                case EVENT_R26_4: DoCastSelf(SPELL_BLUR_OF_SHADOWS); _events.Repeat(23000); break;
                default: break;
            }
        }
        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
    SummonList _summons;
};

// 96675 Allari the Souleater (LC npc_dh_questgiver_96675): "All The Way Up" - LC grants the elevator credit on accept.
// Not ported: LC homebind to the Vault and its AddToExtraLook elevator visibility hack (no such API in our core).
class npc_r26_vault_allari : public CreatureScript
{
public:
    npc_r26_vault_allari() : CreatureScript("npc_r26_vault_allari") { }

    bool OnQuestAccept(Player* player, Creature* /*creature*/, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_ALL_THE_WAY_UP)
            player->KilledMonsterCredit(CREDIT_USE_ELEVATOR);
        return true;
    }
};

// 97644 Kor'vas Bloodthorn (LC npc_dh_questgiver_97644): "A New Direction" opens the Kayn/Altruis choice (234).
// LC conversation 536 on line of sight is cosmetic and not ported.
class npc_r26_vault_korvas : public CreatureScript
{
public:
    npc_r26_vault_korvas() : CreatureScript("npc_r26_vault_korvas") { }

    bool OnQuestAccept(Player* player, Creature* /*creature*/, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_A_NEW_DIRECTION)
            player->CastSpell(player, SPELL_CHOICE_KAYN_ALTRUIS, true);
        return true;
    }
};

// Choice 234 responses -> LC playerchoice_response_reward.SpellID (our reward table has no SpellID column).
class PlayerScript_r26_kayn_altruis_choice : public PlayerScript
{
public:
    PlayerScript_r26_kayn_altruis_choice() : PlayerScript("PlayerScript_r26_kayn_altruis_choice") { }

    void OnPlayerChoiceResponse(Player* player, uint32 choiceID, uint32 responseID) override
    {
        if (choiceID != PLAYER_CHOICE_KAYN_ALTRUIS)
            return;

        if (responseID == PLAYER_CHOICE_RESPONSE_KAYN)
            player->CastSpell(player, SPELL_CHOSE_KAYN, true);
        else if (responseID == PLAYER_CHOICE_RESPONSE_ALTRUIS)
            player->CastSpell(player, SPELL_CHOSE_ALTRUIS, true);
    }
};

// 244455 Pool of Judgment (LC go_q39687).
class go_r26_vault_pool_of_judgment : public GameObjectScript
{
public:
    go_r26_vault_pool_of_judgment() : GameObjectScript("go_r26_vault_pool_of_judgment") { }

    bool OnGossipHello(Player* player, GameObject* /*go*/) override
    {
        if (HasAnyQuestIncomplete(player, { QUEST_A_NEW_DIRECTION, QUEST_A_NEW_DIRECTION_LC }))
        {
            player->CastSpell(player, SPELL_PLAY_MOVIE_POOL, true);
            player->KilledMonsterCredit(CREDIT_POOL_OF_JUDGMENT);
        }
        return false;
    }
};

// ============================================================================================================
// Isle of Thunder: Lightning/Primal Ritual Stones (LC npc_iot_ritual_stone, 70197/70199). Spellclick 138054
// "Perform Ritual" (3x Shan'ze Ritual Stone) summons the champion of that stone; the champion's kill is the credit of
// 32640/32641 (69339, 69347, 69471) and 32708 (69341). LC matched the stone by exact float position; here within 3 yd.
// Differences: summons despawn 2 min after leaving combat (LC: never), no second summon while the champion is alive.
// ============================================================================================================

struct R26RitualStoneSummon
{
    uint32 stoneEntry;
    float stoneX, stoneY;
    uint32 summonEntry;
    Position summonPos;
};

static R26RitualStoneSummon const R26RitualStoneSummons[] =
{
    // Lightning Ritual Stone 70197
    { 70197, 6851.24f, 5457.84f, 69767, { 6838.29f, 5441.10f, 29.57f, 2.33f } }, // Ancient Mogu Guardian
    { 70197, 7431.16f, 5672.90f, 69749, { 7440.25f, 5677.70f, 49.62f, 4.63f } }, // Qi'nor
    { 70197, 6465.76f, 5815.12f, 69339, { 6473.03f, 5808.09f, 28.52f, 5.52f } }, // Electromancer Ju'le
    { 70197, 6533.31f, 6384.15f, 69633, { 6522.24f, 6372.65f,  8.12f, 0.06f } }, // Kor'dok
    { 70197, 6377.94f, 6180.04f, 69471, { 6382.93f, 6179.49f, -4.29f, 1.61f } }, // Spirit of Warlord Teng
    { 70197, 5723.88f, 5383.63f, 69341, { 5740.23f, 5370.30f,  3.49f, 1.64f } }, // Echo of Kros
    // Primal Ritual Stone 70199
    { 70199, 7577.93f, 5588.54f, 69347, { 7582.00f, 5600.72f, 33.37f, 3.57f } }, // Incomplete Drakkari Colossus
    { 70199, 7080.54f, 4795.18f, 70080, { 7087.83f, 4791.36f,  9.00f, 4.30f } }, // Windweaver Akil'amon
    { 70199, 5964.52f, 5261.80f, 69396, { 5992.25f, 5280.99f,  7.20f, 4.10f } }, // Cera
};

enum R26RitualStone : uint32
{
    SPELL_PERFORM_RITUAL = 138054
};

struct npc_r26_iot_ritual_stone : public ScriptedAI
{
    npc_r26_iot_ritual_stone(Creature* creature) : ScriptedAI(creature)
    {
        me->SetReactState(REACT_PASSIVE);
    }

    void SpellHit(Unit* /*caster*/, SpellInfo const* spell) override
    {
        if (spell->Id != SPELL_PERFORM_RITUAL)
            return;

        for (R26RitualStoneSummon const& entry : R26RitualStoneSummons)
        {
            if (entry.stoneEntry != me->GetEntry() || me->GetExactDist2d(entry.stoneX, entry.stoneY) > 3.0f)
                continue;

            if (me->FindNearestCreature(entry.summonEntry, 60.0f, true))
                return; // champion of this stone is still up

            me->SummonCreature(entry.summonEntry, entry.summonPos, TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 120000);
            return;
        }
    }

    void UpdateAI(uint32 /*diff*/) override { }
};

// ============================================================================================================
// 188501 Spectral Sight - "Second Sight" 44471 (Alliance, Jace Darkweaver 102585 in Stormwind) and 40982 (Horde,
// LC: Illidari Enforcer 100874 in Orgrimmar). LC checks every 2 s for players with the aura within 20 yd of the NPC.
// 100874 has no spawn in our DB; LC spawns it ~3 yd from the quest ender 100873, which we have -> 100873 used as anchor.
// Implemented as an aura script (checks while the aura is up) instead of permanent creature AI.
// ============================================================================================================

enum R26SpectralSight : uint32
{
    SPELL_SPECTRAL_SIGHT        = 188501,
    QUEST_SECOND_SIGHT_ALLIANCE = 44471,
    QUEST_SECOND_SIGHT_HORDE    = 40982,
    NPC_JACE_STORMWIND          = 102585,
    NPC_SECOND_SIGHT_HORDE_NPC  = 100873,
    CREDIT_SPECTRAL_SIGHT_USED  = 102563
};

static bool R26CheckSecondSight(Player* player)
{
    if (!player->HasAura(SPELL_SPECTRAL_SIGHT))
        return true; // stop

    if ((player->GetQuestStatus(QUEST_SECOND_SIGHT_ALLIANCE) == QUEST_STATUS_INCOMPLETE && player->FindNearestCreature(NPC_JACE_STORMWIND, 20.0f, true)) ||
        (player->GetQuestStatus(QUEST_SECOND_SIGHT_HORDE) == QUEST_STATUS_INCOMPLETE && player->FindNearestCreature(NPC_SECOND_SIGHT_HORDE_NPC, 20.0f, true)))
    {
        player->KilledMonsterCredit(CREDIT_SPECTRAL_SIGHT_USED);
        return true;
    }
    return false;
}

class spell_r26_spectral_sight_second_sight : public AuraScript
{
    PrepareAuraScript(spell_r26_spectral_sight_second_sight);

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        if (player->GetQuestStatus(QUEST_SECOND_SIGHT_ALLIANCE) != QUEST_STATUS_INCOMPLETE &&
            player->GetQuestStatus(QUEST_SECOND_SIGHT_HORDE) != QUEST_STATUS_INCOMPLETE)
            return;

        if (R26CheckSecondSight(player))
            return;

        // the scheduler belongs to the player, so the task dies with it
        player->GetScheduler().Schedule(Seconds(1), [player](TaskContext context)
        {
            if (!R26CheckSecondSight(player))
                context.Repeat(Seconds(1));
        });
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_r26_spectral_sight_second_sight::AfterApply, EFFECT_2, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddSC_lcf2r26_2026_09_25_spell_scripts()
{
    RegisterCreatureAI(npc_r26_vault_crusher_sledge);
    RegisterCreatureAI(npc_r26_vault_immolanth);
    RegisterCreatureAI(npc_r26_vault_bastillax);
    new npc_r26_vault_allari();
    new npc_r26_vault_korvas();
    new PlayerScript_r26_kayn_altruis_choice();
    new go_r26_vault_pool_of_judgment();
    RegisterCreatureAI(npc_r26_iot_ritual_stone);
    RegisterAuraScript(spell_r26_spectral_sight_second_sight);
}
