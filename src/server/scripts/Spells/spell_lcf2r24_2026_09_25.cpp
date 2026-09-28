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

// Legion-Server round "LCF2 R24" (2026-09-25): DBErrors.log compared against LegionCore-7.3.5 + rest list of R23.
// Mechanics taken from LegionCore-7.3.5 (github.com/The-Legion-Preservation-Project/LegionCore-7.3.5, derived from the
// UWOW 2020 leak - used with explicit user permission; local clone C:\LegionServer\downloads\lc_src\repo) and, where
// named, SimulationCraft legion-dev. The code is our own; numbers come from our client 7.3.5.26972.
// Binding SQL: C:\LegionServer\fixes\lcf2r24_2026-09-25_scripts.sql - without it every script here is inert
// (exception: spell_dk_army_of_the_dead, whose binding to 42651 already exists in the live world DB).

#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include <algorithm>

namespace
{
    enum R24Spells : uint32
    {
        // Death Knight (Unholy)
        SPELL_DK_FESTERING_WOUND         = 194310,
        SPELL_DK_FESTERING_WOUND_BURST   = 194311,
        SPELL_DK_FESTERING_WOUND_RP      = 195757, // client: DUMMY 3 + ENERGIZE 30 runic power (= 3 RP), "$195757s1 Runic Power" in 194310
        SPELL_DK_APOCALYPSE_GHOUL        = 205491, // client: "Army of the Dead" SUMMON 24207, 15 s
        SPELL_DK_T20_UNHOLY_2P           = 242064, // "Each ghoul summoned by Army of the Dead increases your damage dealt by ..."
        SPELL_DK_MASTER_OF_GHOULS        = 246995, // client: +15 % damage, 3 s (DurationIndex 27), "Duration extends and does not stack"

        // Priest (Shadow)
        SPELL_PRI_VOIDFORM_BUFFS         = 194249,
        SPELL_PRI_VOID_TORRENT           = 205065,

        // Shaman
        SPELL_SHA_EARTH_SHIELD_PVP_HEAL  = 204290
    };
}

// ============================================================================================================
// Death Knight: Festering Wound runic power.
// Client 194310: "A pustulent lesion that will burst on death or when damaged by Scourge Strike, dealing $194311s1 Shadow
// damage and generating $195757s1 Runic Power." Our core burst every wound with 194311 but never cast 195757, so bursting
// wounds generated no runic power at all (Scourge Strike, Clawing Shadows, Castigator, Black Claws, Apocalypse).
// LegionCore (spell_dk.cpp: scourge strike / apocalypse / black claws / spell_dk_festering_wound_dummy) casts 195757 on the
// death knight for each burst and, on the target's death, once per remaining stack.
// ============================================================================================================

// 194311 - Festering Wound (burst): one 195757 per burst, on the bursting death knight
class spell_r24_dk_festering_wound_burst : public SpellScript
{
    PrepareSpellScript(spell_r24_dk_festering_wound_burst);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_DK_FESTERING_WOUND_RP });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster || caster->GetTypeId() != TYPEID_PLAYER)
            return;

        caster->CastSpell(caster, SPELL_DK_FESTERING_WOUND_RP, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_r24_dk_festering_wound_burst::HandleAfterHit);
    }
};

// 194310 - Festering Wound (aura): the wounds of a dying target burst too -> runic power per remaining stack
class spell_r24_dk_festering_wound_death : public AuraScript
{
    PrepareAuraScript(spell_r24_dk_festering_wound_death);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_DK_FESTERING_WOUND_RP });
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;

        Unit* caster = GetCaster();
        if (!caster || caster->GetTypeId() != TYPEID_PLAYER || !caster->IsInWorld())
            return;

        for (uint8 i = 0; i < GetStackAmount(); ++i)
            caster->CastSpell(caster, SPELL_DK_FESTERING_WOUND_RP, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_r24_dk_festering_wound_death::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ============================================================================================================
// Death Knight: Apocalypse (220143), the Unholy artifact ability.
// Client: "Strikes the enemy, dealing $sw1 Physical damage and bursting up to $s3 Festering Wounds on the target,
// summoning a member of your Army of the Dead for $221180d for each burst Festering Wound." E2 is a DUMMY with 6 (the
// cap) - nothing in our core handled it, so Apocalypse only dealt its weapon damage (no burst, no ghouls).
// LegionCore spell_dk_apocalypse: on the weapon-damage hit, for min(stacks, E2) wounds: burst (194311) + ghoul 205491
// on the target, then remove the burst stacks. LegionCore's PvP cap (4) is not taken over (no PvP scalar here).
// Runic power of the bursts comes from spell_r24_dk_festering_wound_burst.
// ============================================================================================================
class spell_r24_dk_apocalypse : public SpellScript
{
    PrepareSpellScript(spell_r24_dk_apocalypse);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->GetEffect(EFFECT_2) && ValidateSpellInfo({ SPELL_DK_FESTERING_WOUND, SPELL_DK_FESTERING_WOUND_BURST, SPELL_DK_APOCALYPSE_GHOUL });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        Aura* wound = target->GetAura(SPELL_DK_FESTERING_WOUND, caster->GetGUID());
        if (!wound)
            return;

        int32 cap = GetEffectInfo(EFFECT_2)->CalcValue(caster);
        int32 count = std::min<int32>(wound->GetStackAmount(), cap);
        if (count <= 0)
            return;

        for (int32 i = 0; i < count; ++i)
        {
            caster->CastSpell(target, SPELL_DK_FESTERING_WOUND_BURST, true);
            caster->CastSpell(target, SPELL_DK_APOCALYPSE_GHOUL, true);
        }

        // the burst spell does not touch the stacks (same convention as spell_dk_scourge_strike); the aura may be gone
        // if the bursts killed the target
        if (Aura* woundAfter = target->GetAura(SPELL_DK_FESTERING_WOUND, caster->GetGUID()))
            woundAfter->ModStackAmount(-count);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r24_dk_apocalypse::HandleHit, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

// ============================================================================================================
// Death Knight: T20 Unholy 2P (242064) "Master of Ghouls".
// The world DB always bound 'spell_dk_army_of_the_dead' to 42651, but our core had no such script (DBErrors.log:
// "ScriptName 'spell_dk_army_of_the_dead' exists in database, but no core script found!"). LegionCore's script of that
// name (spell_dk.cpp, 42651 + 205491) implements the set bonus; SimC (sc_death_knight.cpp, army + apocalypse ghouls
// -> buffs.t20_2pc_unholy->trigger()) confirms that both Army of the Dead and Apocalypse ghouls count.
// Client: 246995 lasts 3 s, text "Duration extends and does not stack" -> +3 s per ghoul.
// The script name is intentionally the one already in the database.
// ============================================================================================================
class spell_dk_army_of_the_dead : public SpellScript
{
    PrepareSpellScript(spell_dk_army_of_the_dead);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_DK_T20_UNHOLY_2P, SPELL_DK_MASTER_OF_GHOULS });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_DK_T20_UNHOLY_2P))
            return;

        if (Aura* buff = caster->GetAura(SPELL_DK_MASTER_OF_GHOULS, caster->GetGUID()))
        {
            int32 extension = sSpellMgr->AssertSpellInfo(SPELL_DK_MASTER_OF_GHOULS)->GetMaxDuration();
            int32 duration = buff->GetDuration() + extension;
            if (duration > buff->GetMaxDuration())
                buff->SetMaxDuration(duration);
            buff->SetDuration(duration);
        }
        else
            caster->CastSpell(caster, SPELL_DK_MASTER_OF_GHOULS, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_dk_army_of_the_dead::HandleAfterCast);
    }
};

// ============================================================================================================
// Priest (Shadow): Void Torrent (205065) "Insanity does not drain during this channel."
// Our Voidform (spell_pri_voidform, 194249) drains through 194249 E1 (MOD_POWER_REGEN insanity, x stacks) and nothing
// paused it. LegionCore (spell_pri_voidform CallSpecialFunction, SpellAuras.cpp case 205065) locks the drain while
// the priest has 205065. Here: E1 of 194249 is set to 0 while the channel runs (spell_pri_voidform re-applies the 0
// after each stack tick, spell_priest.cpp) and recalculated from the stacks when the channel ends.
// Difference to LegionCore: there the drain growth pauses too; here the drain resumes at the stack-based value.
// ============================================================================================================
class spell_r24_pri_void_torrent : public AuraScript
{
    PrepareAuraScript(spell_r24_pri_void_torrent);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PRI_VOIDFORM_BUFFS });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (AuraEffect* drain = GetTarget()->GetAuraEffect(SPELL_PRI_VOIDFORM_BUFFS, EFFECT_1, GetTarget()->GetGUID()))
            drain->ChangeAmount(0);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* priest = GetTarget();
        if (AuraEffect* drain = priest->GetAuraEffect(SPELL_PRI_VOIDFORM_BUFFS, EFFECT_1, priest->GetGUID()))
            drain->ChangeAmount(drain->CalculateAmount(priest));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_r24_pri_void_torrent::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_r24_pri_void_torrent::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ============================================================================================================
// Shaman (Restoration): Gift of the Queen (207778) "Heals up to six injured allies within $A1 yards ...".
// The client has no MaxTargets for 207778 (SpellTargetRestrictions 30075: MaxTargets 0), so our core healed and buffed
// every ally in the area. LegionCore caps it with spell_target_filter (count 6, effect mask 7). "Injured" is taken from
// the tooltip: injured allies first, lowest health percentage first; E1 (max health buff) gets the same six.
// ============================================================================================================
class spell_r24_sha_gift_of_the_queen : public SpellScript
{
    PrepareSpellScript(spell_r24_sha_gift_of_the_queen);

    static constexpr size_t MAX_TARGETS = 6; // tooltip "up to six", LegionCore spell_target_filter count 6

    GuidList _selected;

    void SelectHealTargets(std::list<WorldObject*>& targets)
    {
        std::vector<Unit*> units;
        for (WorldObject* object : targets)
            if (Unit* unit = object->ToUnit())
                units.push_back(unit);

        std::stable_sort(units.begin(), units.end(), [](Unit const* a, Unit const* b)
        {
            bool injuredA = !a->IsFullHealth();
            bool injuredB = !b->IsFullHealth();
            if (injuredA != injuredB)
                return injuredA;
            return a->GetHealthPct() < b->GetHealthPct();
        });

        if (units.size() > MAX_TARGETS)
            units.resize(MAX_TARGETS);

        targets.clear();
        _selected.clear();
        for (Unit* unit : units)
        {
            targets.push_back(unit);
            _selected.push_back(unit->GetGUID());
        }
    }

    void SelectBuffTargets(std::list<WorldObject*>& targets)
    {
        targets.remove_if([this](WorldObject* object)
        {
            return std::find(_selected.begin(), _selected.end(), object->GetGUID()) == _selected.end();
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_r24_sha_gift_of_the_queen::SelectHealTargets, EFFECT_0, TARGET_UNIT_DEST_AREA_ALLY);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_r24_sha_gift_of_the_queen::SelectBuffTargets, EFFECT_1, TARGET_UNIT_DEST_AREA_ALLY);
    }
};

// ============================================================================================================
// Shaman: Earth Shield honor talent (204288). Client: E0 -10 % damage taken (core handles), E1 DUMMY 4 = "causes the
// target to be healed for $204290s1 when they take an attack equal to $m2% of their total health. $N Charges."
// (4 charges, proc on damage taken). Nothing handled E1 (our 'spell_sha_earth_shield' is the old 974 version, unbound).
// LegionCore spell_sha_earth_shield (204288): heal 204290 from the shaman when a hit from someone else reaches E1 % of
// max health. Here the check is in DoCheckProc, so only such hits use up a charge.
// ============================================================================================================
class spell_r24_sha_earth_shield : public AuraScript
{
    PrepareAuraScript(spell_r24_sha_earth_shield);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->GetEffect(EFFECT_1) && ValidateSpellInfo({ SPELL_SHA_EARTH_SHIELD_PVP_HEAL });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* target = GetTarget();
        if (!damageInfo || !damageInfo->GetDamage() || eventInfo.GetActor() == target)
            return false;

        AuraEffect const* threshold = GetEffect(EFFECT_1);
        if (!threshold)
            return false;

        return damageInfo->GetDamage() >= target->CountPctFromMaxHealth(threshold->GetAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        if (Unit* caster = GetCaster())
            caster->CastSpell(target, SPELL_SHA_EARTH_SHIELD_PVP_HEAL, true);
        else
            target->CastSpell(target, SPELL_SHA_EARTH_SHIELD_PVP_HEAL, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r24_sha_earth_shield::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r24_sha_earth_shield::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddSC_lcf2r24_2026_09_25_spell_scripts()
{
    RegisterSpellScript(spell_r24_dk_festering_wound_burst);
    RegisterAuraScript(spell_r24_dk_festering_wound_death);
    RegisterSpellScript(spell_r24_dk_apocalypse);
    RegisterSpellScript(spell_dk_army_of_the_dead);
    RegisterAuraScript(spell_r24_pri_void_torrent);
    RegisterSpellScript(spell_r24_sha_gift_of_the_queen);
    RegisterAuraScript(spell_r24_sha_earth_shield);
}
