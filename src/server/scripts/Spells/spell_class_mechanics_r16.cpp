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

// Legion-Server round 16 (2026-09-24): class mechanics found by a systematic scan of the 7.3.5 client data.
//
// Two groups of defects, both invisible in the spell/script tables because the affected spells never had a script:
//
//  A) "over-proc": talents / spec passives whose SpellAuraOptions carry a proc mask but whose trigger effect has no
//     SpellClassMask. The core then generates a proc entry without family filter (SpellMgr::LoadSpellProcs,
//     "Generating spell proc data from SpellMap"), and SpellInfo::IsAffected(0, ...) accepts every spell. Example:
//     Rime (59057) - chance 100, internal cooldown 0.5 s, mask 0x11110 - gave a free Howling Blast after almost every
//     damaging Death Knight spell instead of 45 % of Obliterates. The script below restricts every such aura to the
//     spells its 7.3.5 tooltip names; all other numbers stay the client ones.
//
//  B) "never-proc": DUMMY auras with a proc mask (or a mechanic in the tooltip) that nothing handled at all, e.g.
//     Crimson Scourge, Cenarion Ward, Mastery: Main Gauche, Shadowy Apparitions.
//
// Semantics were ported from TrinityCore master where it has the same spell (file named in each comment, raw files
// downloaded 2026-09-24), adapted to the 7.3.5 data; values are always read from the client spell data at runtime.
// The SQL that binds these scripts is C:\LegionServer\fixes\r16_2026-09-24_class_mechanics.sql - without it the
// scripts are inert.

#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include <queue>
#include <unordered_map>
#include <vector>

namespace
{
    int32 R16EffectValue(uint32 spellId, uint8 effIndex)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            if (SpellEffectInfo const* effect = info->GetEffect(effIndex))
                return effect->BasePoints;
        return 0;
    }

    int32 R16PowerCost(ProcEventInfo& eventInfo, Powers power)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        if (!spell)
            return 0;

        for (SpellPowerCost const& cost : spell->GetPowerCost())
            if (cost.Power == power && cost.Amount > 0)
                return cost.Amount;
        return 0;
    }

    // Group A: which spells may trigger the aura, taken from the 7.3.5 tooltip of the aura.
    struct R16ProcFilter
    {
        std::vector<uint32> Spells;   // empty: every spell and auto attack the client proc mask allows
        bool RequireCrit;             // tooltip says "critical strike(s)"
        uint32 RequireActorAura;      // tooltip names a state of the player ("While Pillar of Frost is active")
        bool RequireFinisher;         // tooltip says "finishing moves" -> spell costs combo points
    };

    std::unordered_map<uint32, R16ProcFilter> const R16ProcFilters =
    {
        // Avalanche: "While Pillar of Frost (51271) is active, your melee critical strikes cause jagged icicles ..."
        { 207142, { {}, true, 51271, false } },
        // Obliteration: "Frost Strike (49143) and Howling Blast (49184) grant Killing Machine" (E1 -> 51124)
        { 207256, { { 49143, 49184 }, false, 0, false } },
        // Frost Fever Runic Power (195621 = $@spelldesc195617): only the Frost Fever (55095) periodic damage
        { 195621, { { 55095 }, false, 0, false } },
        // Guardian of Elune: "Mangle (33917) increases the duration of your next Ironfur ..."
        { 155578, { { 33917 }, false, 0, false } },
        // Eye of the Tiger: "Tiger Palm (100780) also applies Eye of the Tiger"
        { 196607, { { 100780 }, false, 0, false } },
        // Blackout Combo: "Blackout Strike (205523) also empowers your next ability" (E4 -> 228563)
        { 196736, { { 205523 }, false, 0, false } },
        // Special Delivery: "Drinking Ironskin (115308) or Purifying Brew (119582) has a $h% chance ..."
        { 196730, { { 115308, 119582 }, false, 0, false } },
        // Shadowy Insight: "Shadow Word: Pain (589) periodic damage has a $h% chance ..."
        { 162452, { { 589 }, false, 0, false } },
        // Divinity: "When you heal with a Holy Word spell" - Serenity 2050, Sanctify 34861
        { 197031, { { 2050, 34861 }, false, 0, false } },
        // Landslide: "Rockbiter (193786) enhances your weapon" (201897 from the tooltip switch does not exist in 7.3.5)
        { 197992, { { 193786 }, false, 0, false } },
        // Ancestral Vigor: "Healing Wave (77472), Healing Surge (8004), Chain Heal (1064), or Riptide's (61295) initial heal"
        { 207401, { { 77472, 8004, 1064, 61295 }, false, 0, false } },
        // Empowered Life Tap: "Life Tap (1454) increases your damage dealt ..."
        { 235157, { { 1454 }, false, 0, false } },
        // In For The Kill: "Colossus Smash (167105) grants you $248622s1% Haste"
        { 248621, { { 167105 }, false, 0, false } },
        // Deep Wounds: "Your Devastator/Devastate (20243) and Revenge (6572) also cause ... Bleed" - the Devastator
        // talent (236279) replaces Devastate by the auto attack bonus 236282 (triggered, see spell_proc row)
        { 115768, { { 20243, 6572, 236282 }, false, 0, false } },
        // Elaborate Planning: "Your finishing moves grant ..."
        { 193640, { {}, false, 0, true } },
        // Curse of the Dreadblades: "each Saber Slash (193315), Ghostly Strike (196937), Ambush (8676), or Pistol Shot (185763)"
        { 202665, { { 193315, 196937, 8676, 185763 }, false, 0, false } },
        // Hot Hand: "Melee attacks with Flametongue (194084) active have a chance ..."
        { 201900, { {}, false, 194084, false } },
        // Seal Fate: "When you critically strike with a melee attack that generates combo points" - the Assassination
        // combo point generators of 7.3.5: Mutilate main/off hand (5374/27576, triggered), Hemorrhage 16511,
        // Fan of Knives 51723, Poisoned Knife 185565, Toxic Blade 245388, Garrote 703
        { 14190, { { 5374, 27576, 16511, 51723, 185565, 245388, 703 }, true, 0, false } },
    };
}

// Group A - one script bound to all auras of the table above.
class spell_r16_proc_filter : public AuraScript
{
    PrepareAuraScript(spell_r16_proc_filter);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        auto itr = R16ProcFilters.find(GetId());
        if (itr == R16ProcFilters.end())
            return true;

        R16ProcFilter const& filter = itr->second;

        if (filter.RequireCrit && !(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        if (filter.RequireActorAura && (!eventInfo.GetActor() || !eventInfo.GetActor()->HasAura(filter.RequireActorAura)))
            return false;

        if (filter.RequireFinisher && R16PowerCost(eventInfo, POWER_COMBO_POINTS) <= 0)
            return false;

        if (!filter.Spells.empty())
        {
            SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
            if (!spellInfo)
                return false;

            bool found = false;
            for (uint32 spellId : filter.Spells)
                if (spellInfo->Id == spellId)
                    found = true;
            if (!found)
                return false;
        }

        return true;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_proc_filter::CheckProc);
    }
};

// 59057 - Rime. "Obliterate has a $s2% chance and Frostscythe has a ${$s2/2}.1% chance to cause your next Howling
// Blast to consume no runes ..." The chance is E1 (45 in 7.3.5), the client ProcChance is 100. One roll per cast:
// the spell_proc row sets SpellPhaseMask CAST (as TrinityCore master, TDB 1210 row 59057), so the two weapon hits of
// Obliterate and the several targets of Frostscythe do not roll separately.
// Semantics: TrinityCore master spell_dk.cpp spell_dk_rime.
class spell_r16_dk_rime : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_rime);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo || (spellInfo->Id != 49020 && spellInfo->Id != 207230))
            return false;

        AuraEffect const* chanceEffect = GetEffect(EFFECT_1);
        if (!chanceEffect)
            return false;

        float chance = float(chanceEffect->GetAmount());
        if (spellInfo->Id == 207230) // Frostscythe
            chance /= 2.0f;

        return roll_chance_f(chance);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_dk_rime::CheckProc);
    }
};

// 81136 - Crimson Scourge. "Your auto attacks on targets infected with your Blood Plague have a chance to make your
// next Death and Decay cost no runes and reset its cooldown." Chance 25 (client). Buff 81141, Blood Plague 55078,
// Death and Decay 43265 (category cooldown in 7.3.5, no charges -> ResetCooldown).
// Semantics: TrinityCore master spell_dk.cpp spell_dk_crimson_scourge.
class spell_r16_dk_crimson_scourge : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_crimson_scourge);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 55078, 81141, 43265 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* actor = eventInfo.GetActor();
        Unit* target = eventInfo.GetActionTarget();
        return actor && target && target->HasAura(55078, actor->GetGUID());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* actor = eventInfo.GetActor();
        actor->GetSpellHistory()->ResetCooldown(43265, true);
        actor->CastSpell(actor, 81141, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_dk_crimson_scourge::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_dk_crimson_scourge::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 85043 - Grand Crusader, avoidance branch. "When you avoid a melee attack or use Hammer of the Righteous, you have a
// $h% chance to reset the remaining cooldown on Avenger's Shield." The Hammer branch already lives in
// spell_pal_grand_crusader (bound to 53595/204019); the "avoid" branch had no handler. The spell_proc row limits the
// client proc mask (taken melee) to miss/dodge/parry. Actions mirror spell_pal_grand_crusader exactly: buff 85416,
// reset of Avenger's Shield 31935, and with Crusader's Judgment (204023) a charge of Judgment (20271).
class spell_r16_pal_grand_crusader_avoid : public AuraScript
{
    PrepareAuraScript(spell_r16_pal_grand_crusader_avoid);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 85416, 31935, 20271 });
    }

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return GetTarget()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* target = GetTarget();
        target->CastSpell(target, 85416, true);
        target->GetSpellHistory()->ResetCooldown(31935, true);

        if (target->HasAura(204023))
            if (SpellInfo const* judgment = sSpellMgr->GetSpellInfo(20271))
                target->GetSpellHistory()->RestoreCharge(judgment->ChargeCategoryId);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_pal_grand_crusader_avoid::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_pal_grand_crusader_avoid::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 102351 - Cenarion Ward. "Any damage taken will consume the ward and heal the target for $102352o1 over $102352d."
// E0 is a DUMMY aura whose client EffectTriggerSpell is 102352; the ward has ProcCharges 1, so the core already drops
// it on the first damage taken - only the heal was missing. The druid casts it so that his spell power applies.
class spell_r16_dru_cenarion_ward : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_cenarion_ward);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 102352 });
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        if (Unit* caster = GetCaster())
            caster->CastSpell(GetTarget(), 102352, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_r16_dru_cenarion_ward::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 206940 - Mark of Blood. "The enemy's damaging auto attacks will also heal their victim for $206945s1% of the
// victim's maximum health." Semantics: TrinityCore master spell_dk.cpp spell_dk_mark_of_blood.
class spell_r16_dk_mark_of_blood : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_mark_of_blood);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 206945 });
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return;

        if (Unit* caster = GetCaster())
            caster->CastSpell(victim, 206945, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_r16_dk_mark_of_blood::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 207200 - Permafrost. "When you deal damage with auto attacks, gain an absorb shield equal to $s1% of the damage
// dealt." Shield 207203 (Frost Shield). Semantics: TrinityCore master spell_dk.cpp spell_dk_permafrost (a new shield
// replaces the running one, as the core's aura refresh recalculates the amount).
class spell_r16_dk_permafrost : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_permafrost);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207203 });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        int32 amount = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (amount <= 0)
            return;

        Unit* target = GetTarget();
        target->CastCustomSpell(207203, SPELLVALUE_BASE_POINT0, amount, target, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_r16_dk_permafrost::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 207126 - Icecap. "Your Frost Strike, Frostscythe, and Obliterate critical strikes reduce the remaining cooldown of
// Pillar of Frost by ${$m1/10}.1 sec." The weapon hits of Frost Strike (222026/66196) and Obliterate (222024/66198)
// are triggered spells; the spell_proc row lets them proc (PROC_ATTR_TRIGGERED_CAN_PROC) and requires a critical hit.
// The client internal cooldown of 0.5 s keeps the two hands of one strike from counting twice.
class spell_r16_dk_icecap : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_icecap);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo)
            return false;

        switch (spellInfo->Id)
        {
            case 222026: case 66196:  // Frost Strike main / off hand
            case 222024: case 66198:  // Obliterate main / off hand
            case 207230:              // Frostscythe
                return true;
            default:
                return false;
        }
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        // $m1 is in tenths of a second ("${$m1/10}.1 sec")
        GetTarget()->GetSpellHistory()->ModifyCooldown(51271, -aurEff->GetAmount() * 100);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_dk_icecap::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_dk_icecap::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 206930 - Heart Strike, talent Heartbreaker (221536): "plus ${$210738s1/10} Runic Power per additional enemy
// struck". The base Runic Power (-150 in SpellPower) already contains "$s3"; 221536 E0 is 0 in 7.3.5.
// Semantics: TrinityCore master spell_dk.cpp spell_dk_heartbreaker, limited to the additional targets per 7.3.5 text.
class spell_r16_dk_heartbreaker : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_heartbreaker);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 221536, 210738 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(221536);
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (++_targetsHit > 1)
            GetCaster()->CastSpell(GetCaster(), 210738, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dk_heartbreaker::HandleHit, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }

    uint32 _targetsHit = 0;
};

// 49998 - Death Strike, talent Heart of Ice (246426): "Death Strike extends the duration of Icebound Fortitude by
// ${$s1/10} sec." (E0 = 20 -> 2 s).
class spell_r16_dk_heart_of_ice : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_heart_of_ice);

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(246426);
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        AuraEffect const* talent = caster->GetAuraEffect(246426, EFFECT_0);
        Aura* ibf = caster->GetAura(48792);
        if (!talent || !ibf)
            return;

        int32 newDuration = ibf->GetDuration() + talent->GetAmount() * 100;
        if (newDuration > ibf->GetMaxDuration())
            ibf->SetMaxDuration(newDuration);
        ibf->SetDuration(newDuration);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_dk_heart_of_ice::HandleAfterCast);
    }
};

// 76806 - Mastery: Main Gauche. "Your main-hand attacks have a ${$m1}.1% chance to trigger an attack with your
// off-hand that deals $86392sw2 Physical damage." E0 carries the mastery value (Player::UpdateMastery,
// BonusCoefficient 2.2); the client ProcChance is 100, so the script rolls E0 itself. Main hand = BASE_ATTACK.
// Semantics: TrinityCore master spell_rogue.cpp spell_rog_mastery_main_gauche (plus the chance and hand check).
class spell_r16_rog_main_gauche : public AuraScript
{
    PrepareAuraScript(spell_r16_rog_main_gauche);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 86392 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetVictim() || !damageInfo->GetDamage())
            return false;

        if (damageInfo->GetAttackType() != BASE_ATTACK)
            return false;

        if (damageInfo->GetSpellInfo() && damageInfo->GetSpellInfo()->Id == 86392)
            return false;

        AuraEffect const* mastery = GetEffect(EFFECT_0);
        return mastery && roll_chance_i(mastery->GetAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        GetTarget()->CastSpell(eventInfo.GetDamageInfo()->GetVictim(), 86392, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_rog_main_gauche::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_rog_main_gauche::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 78203 - Shadowy Apparitions. "When your Shadow Word: Pain damage over time critically strikes, you also create a
// shadowy version of yourself that floats towards the target and deals $148859s1 Shadow damage." Carrier 147193
// (TRIGGER_MISSILE -> 148859), the same spell the artifact trait Unleash the Shadows already uses.
class spell_r16_pri_shadowy_apparitions : public AuraScript
{
    PrepareAuraScript(spell_r16_pri_shadowy_apparitions);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 147193 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 589 && eventInfo.GetProcTarget();
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        GetTarget()->CastSpell(eventInfo.GetProcTarget(), 147193, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_pri_shadowy_apparitions::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_pri_shadowy_apparitions::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 200128 - Trail of Light. "When you cast Flash Heal, $s1% of the healing is replicated to the previous target you
// healed with Flash Heal." Heal 234946. Semantics: TrinityCore master spell_priest.cpp spell_pri_trail_of_light,
// limited to Flash Heal (2061) as the 7.3.5 text says.
class spell_r16_pri_trail_of_light : public AuraScript
{
    PrepareAuraScript(spell_r16_pri_trail_of_light);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 234946 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Unit* target = eventInfo.GetActionTarget();
        if (!spellInfo || spellInfo->Id != 2061 || !target || !eventInfo.GetHealInfo())
            return false;

        if (_healQueue.empty() || _healQueue.back() != target->GetGUID())
            _healQueue.push(target->GetGUID());

        if (_healQueue.size() > 2)
            _healQueue.pop();

        return _healQueue.size() == 2;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Unit* caster = GetTarget();
        Unit* oldTarget = ObjectAccessor::GetUnit(*caster, _healQueue.front());
        if (!oldTarget || !oldTarget->IsAlive() || !caster->IsFriendlyTo(oldTarget))
            return;

        SpellInfo const* healInfo = sSpellMgr->GetSpellInfo(234946);
        if (!healInfo || !caster->IsWithinDist(oldTarget, healInfo->GetMaxRange(true, caster)))
            return;

        int32 amount = CalculatePct(int32(eventInfo.GetHealInfo()->GetHeal()), aurEff->GetAmount());
        if (amount > 0)
            caster->CastCustomSpell(234946, SPELLVALUE_BASE_POINT0, amount, oldTarget, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_pri_trail_of_light::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_pri_trail_of_light::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

    std::queue<ObjectGuid> _healQueue;
};

// 17 - Power Word: Shield, talent Shield Discipline (197045): "When your Power Word: Shield is completely absorbed,
// you instantly regenerate $47755s1% of your maximum mana." The core removes a depleted absorb with
// AURA_REMOVE_BY_ENEMY_SPELL (Unit::CalcAbsorbResist); a dispel uses the same mode, so the remaining amount is checked
// as well. Semantics: TrinityCore master spell_priest.cpp spell_pri_power_word_shield::HandleOnRemove.
class spell_r16_pri_shield_discipline : public AuraScript
{
    PrepareAuraScript(spell_r16_pri_shield_discipline);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197045, 47755 });
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_ENEMY_SPELL || aurEff->GetAmount() > 0)
            return;

        if (Unit* caster = GetCaster())
            if (caster->HasAura(197045))
                caster->CastSpell(caster, 47755, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_r16_pri_shield_discipline::HandleRemove, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL);
    }
};

// 203974 - Earthwarden. "When you deal direct damage with Thrash, you gain a charge of Earthwarden, reducing the
// damage of the next auto attack you take by $s1%. Earthwarden may have up to $203975u charges." Thrash (Bear) 77758.
// One charge per cast (spell_proc row: SpellPhaseMask CAST, as TrinityCore master TDB 1210 row 203974).
// Semantics: TrinityCore master spell_druid.cpp spell_dru_earthwarden.
class spell_r16_dru_earthwarden : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_earthwarden);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203975 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 77758;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        GetTarget()->CastSpell(GetTarget(), 203975, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_dru_earthwarden::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_dru_earthwarden::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 203975 - Earthwarden (charges). SCHOOL_ABSORB with base points 1 in the client; the real effect is "Damage of the
// next autoattack you take will be reduced by ${$203974s1/$m1}%". The absorb is made unlimited (-1) and only takes
// white melee hits (no SpellInfo), $203974s1 percent of each, one stack per hit. TrinityCore master has no script for
// this aura (it would absorb 1 damage there).
class spell_r16_dru_earthwarden_absorb : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_earthwarden_absorb);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1;
    }

    void HandleAbsorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        if (dmgInfo.GetSpellInfo() || (dmgInfo.GetAttackType() != BASE_ATTACK && dmgInfo.GetAttackType() != OFF_ATTACK))
            return;

        int32 pct = 0;
        if (AuraEffect const* talent = GetTarget()->GetAuraEffect(203974, EFFECT_0))
            pct = talent->GetAmount();
        if (pct <= 0)
            pct = R16EffectValue(203974, EFFECT_0);

        absorbAmount = CalculatePct(dmgInfo.GetDamage(), pct);
    }

    void AfterAbsorb(AuraEffect* /*aurEff*/, DamageInfo& /*dmgInfo*/, uint32& absorbAmount)
    {
        if (absorbAmount > 0)
            ModStackAmount(-1);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_r16_dru_earthwarden_absorb::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_r16_dru_earthwarden_absorb::HandleAbsorb, EFFECT_0);
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_r16_dru_earthwarden_absorb::AfterAbsorb, EFFECT_0);
    }
};

// 152278 - Anger Management. "Every $?c1[$s1][$s2] Rage you spend reduces the remaining cooldown on Battle
// Cry$?c1[ and Bladestorm][, Last Stand, Shield Wall, and Demoralizing Shout] by 1 sec." c1 = Arms (71):
// E0 (20 Rage) and Battle Cry 1719 + Bladestorm 227847; otherwise E1 (10 Rage) and Battle Cry 1719, Last Stand 12975,
// Shield Wall 871, Demoralizing Shout 1160. The core keeps Rage in tenths. One evaluation per cast (spell_proc row:
// SpellPhaseMask CAST). Semantics (proportional reduction): TrinityCore master spell_warrior.cpp
// spell_warr_anger_management_proc.
class spell_r16_war_anger_management : public AuraScript
{
    PrepareAuraScript(spell_r16_war_anger_management);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return GetTarget()->GetTypeId() == TYPEID_PLAYER && R16PowerCost(eventInfo, POWER_RAGE) > 0;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Player* player = GetTarget()->ToPlayer();
        bool arms = player->GetSpecializationId() == TALENT_SPEC_WARRIOR_ARMS;

        AuraEffect const* thresholdEffect = GetEffect(arms ? EFFECT_0 : EFFECT_1);
        if (!thresholdEffect || thresholdEffect->GetAmount() <= 0)
            return;

        int32 rageSpent = R16PowerCost(eventInfo, POWER_RAGE) / 10;
        int32 reductionMs = rageSpent * IN_MILLISECONDS / thresholdEffect->GetAmount();
        if (reductionMs <= 0)
            return;

        static uint32 const armsSpells[] = { 1719, 227847 };
        static uint32 const otherSpells[] = { 1719, 12975, 871, 1160 };

        if (arms)
        {
            for (uint32 spellId : armsSpells)
                player->GetSpellHistory()->ModifyCooldown(spellId, -reductionMs);
        }
        else
        {
            for (uint32 spellId : otherSpells)
                player->GetSpellHistory()->ModifyCooldown(spellId, -reductionMs);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_war_anger_management::CheckProc);
        OnProc += AuraProcFn(spell_r16_war_anger_management::HandleProc);
    }
};

// 210707 - Aftershock. "Your spells refund $s1% of all Maelstrom spent on them." Carrier 210712 (ENERGIZE Maelstrom,
// $@spelldesc210707). One evaluation per cast (spell_proc row: SpellPhaseMask CAST).
class spell_r16_sha_aftershock : public AuraScript
{
    PrepareAuraScript(spell_r16_sha_aftershock);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 210712 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return R16PowerCost(eventInfo, POWER_MAELSTROM) > 0;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        int32 refund = CalculatePct(R16PowerCost(eventInfo, POWER_MAELSTROM), aurEff->GetAmount());
        if (refund > 0)
            GetTarget()->CastCustomSpell(210712, SPELLVALUE_BASE_POINT0, refund, GetTarget(), true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_sha_aftershock::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_sha_aftershock::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 168534 - Mastery: Elemental Overload. "Your Lightning Bolt, Elemental Blast, Icefury, Chain Lightning, and Lava Burst
// casts have a $s1% chance to trigger a second cast on the same target for $s2% of normal damage and $s3% of normal
// Maelstrom generation." The mastery aura has no SpellAuraOptions row in 7.3.5 (it can not proc), no script existed,
// the five overload spells (45284, 120588, 219271, 45297, 77451) were never cast.
//  - chance: E0 amount (mastery, BonusCoefficient 1.875, Player::UpdateMastery), one roll per cast
//  - damage: E1 (85) - the overload spells carry the same coefficient as the base spells in 7.3.5, so the 85 % is
//    applied on the overload hit (spell_r16_sha_elemental_overload_damage)
//  - Maelstrom: E2 (75). Lava Burst/Icefury overloads already carry 75 % in their own ENERGIZE (9 of 12, 18 of 24).
//    Lightning Bolt and Chain Lightning get their Maelstrom from scripts (214815: 8; Chain Lightning E1: 6 per target);
//    their overloads get E2 percent of that, fractions carried per caster so that nothing is rounded away.
// Semantics: TrinityCore master spell_shaman.cpp spell_sha_mastery_elemental_overload (without the 400 ms delay, the
// modern Chain Lightning 1/3 chance and Stormkeeper's guaranteed overload - none of them is in the 7.3.5 text).
namespace
{
    uint32 R16OverloadSpell(uint32 spellId)
    {
        switch (spellId)
        {
            case 188196: return 45284;  // Lightning Bolt (Elemental) -> Lightning Bolt Overload
            case 117014: return 120588; // Elemental Blast -> Elemental Blast Overload
            case 210714: return 219271; // Icefury -> Icefury Overload
            case 188443: return 45297;  // Chain Lightning -> Chain Lightning Overload
            case 51505:  return 77451;  // Lava Burst -> Lava Burst Overload
            default:     return 0;
        }
    }

    std::unordered_map<ObjectGuid::LowType, int32> R16OverloadMaelstromRemainder; // hundredths of Maelstrom
}

class spell_r16_sha_elemental_overload : public SpellScript
{
    PrepareSpellScript(spell_r16_sha_elemental_overload);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 168534, 45284, 120588, 219271, 45297, 77451 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        uint32 overload = R16OverloadSpell(GetSpellInfo()->Id);
        AuraEffect const* mastery = caster->GetAuraEffect(168534, EFFECT_0);
        if (!target || !overload || !mastery || mastery->GetAmount() <= 0)
            return;

        if (!roll_chance_i(mastery->GetAmount()))
            return;

        caster->CastSpell(target, overload, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_sha_elemental_overload::HandleAfterCast);
    }
};

class spell_r16_sha_elemental_overload_damage : public SpellScript
{
    PrepareSpellScript(spell_r16_sha_elemental_overload_damage);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(168534))
            return;

        // "$s2% of normal damage"
        int32 damagePct = R16EffectValue(168534, EFFECT_1);
        if (damagePct > 0)
            SetHitDamage(CalculatePct(GetHitDamage(), damagePct));

        // "$s3% of normal Maelstrom generation" for the two spells whose Maelstrom comes from a script
        int32 baseMaelstrom = 0;
        if (GetSpellInfo()->Id == 45284)
            baseMaelstrom = R16EffectValue(214815, EFFECT_0);   // Lightning Bolt: 214815 ENERGIZE 8
        else if (GetSpellInfo()->Id == 45297)
            baseMaelstrom = R16EffectValue(188443, EFFECT_1);   // Chain Lightning: 6 per target hit

        int32 maelstromPct = R16EffectValue(168534, EFFECT_2);
        if (baseMaelstrom <= 0 || maelstromPct <= 0)
            return;

        int32& remainder = R16OverloadMaelstromRemainder[caster->GetGUID().GetCounter()];
        remainder += baseMaelstrom * maelstromPct;
        int32 gain = remainder / 100;
        remainder -= gain * 100;
        if (gain > 0)
            caster->ModifyPower(POWER_MAELSTROM, gain);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_sha_elemental_overload_damage::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 80313 - Pulverize. "A devastating blow that consumes $s3 stacks of your Thrash on the target to deal $sw1 Physical
// damage, and reduces all damage you take by $158792s1% for $158792d." Pulverize requires the target aura 158790
// (SpellAuraRestrictions.TargetAuraSpell) which nothing ever applied - the talent could not be cast at all. Marker:
// cast after a bear Thrash (77758) once the bleed 192090 of the druid has at least $s3 (E2 = 2) stacks.
// Semantics: TrinityCore master spell_druid.cpp spell_dru_pulverize / spell_dru_pulverize_thrash; the damage
// reduction buff 158792 is cast by the script because 80313 has no aura effect in 7.3.5.
class spell_r16_dru_pulverize_thrash : public SpellScript
{
    PrepareSpellScript(spell_r16_dru_pulverize_thrash);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 80313, 158790, 192090 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->GetTypeId() == TYPEID_PLAYER && GetCaster()->ToPlayer()->HasSpell(80313);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;

        Aura* bleed = target->GetAura(192090, caster->GetGUID());
        int32 threshold = R16EffectValue(80313, EFFECT_2);
        if (bleed && threshold > 0 && bleed->GetStackAmount() >= threshold)
            caster->CastSpell(target, 158790, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_r16_dru_pulverize_thrash::HandleAfterHit);
    }
};

class spell_r16_dru_pulverize : public SpellScript
{
    PrepareSpellScript(spell_r16_dru_pulverize);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 158790, 158792, 192090 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        // E2 = "$s3 stacks"
        if (Aura* bleed = target->GetAura(192090, caster->GetGUID()))
            bleed->ModStackAmount(-GetEffectValue());

        target->RemoveAurasDueToSpell(158790, caster->GetGUID());
        caster->CastSpell(caster, 158792, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dru_pulverize::HandleHit, EFFECT_2, SPELL_EFFECT_DUMMY);
    }
};

// 49143 - Frost Strike, talent Icy Talons (194878): "Frost Strike also increases your melee attack speed by
// $194879s1% for $194879d, stacking up to $194879u times." 194878 only carries a spell effect DUMMY, nothing applied
// the buff 194879.
class spell_r16_dk_icy_talons : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_icy_talons);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194878, 194879 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(194878);
    }

    void HandleAfterCast()
    {
        GetCaster()->CastSpell(GetCaster(), 194879, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_dk_icy_talons::HandleAfterCast);
    }
};

// 236279 - Devastator. "Your auto attacks deal an additional ... Physical damage, generate ... Rage, and have a $s2%
// chance to reset the remaining cooldown on Shield Slam." E0 (the extra hit 236282) works through the client proc;
// E1 is a plain spell effect DUMMY (30) that nothing read. Shield Slam 23922.
// Semantics: TrinityCore master spell_warrior.cpp spell_warr_devastator (without the modern marker spell).
class spell_r16_war_devastator : public AuraScript
{
    PrepareAuraScript(spell_r16_war_devastator);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* target = GetTarget();
        if (!target->GetSpellHistory()->HasCooldown(23922))
            return;

        if (roll_chance_i(R16EffectValue(236279, EFFECT_1)))
            target->GetSpellHistory()->ResetCooldown(23922, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_r16_war_devastator::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 194311 - Festering Wound (burst), talent Soul Reaper (130736): "Bursting a Festering Wound on an enemy afflicted by
// Soul Reaper grants $215711s1% Haste for $215711d, stacking up to 3 times." The bound spell_dk_soul_reaper is the
// Mists-of-Pandaria execute version (hooks do not match the 7.3.5 effects, see Server.log "did not match").
class spell_r16_dk_soul_reaper_haste : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_soul_reaper_haste);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 130736, 215711 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && target->HasAura(130736, caster->GetGUID()))
            caster->CastSpell(caster, 215711, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dk_soul_reaper_haste::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Batch 2: talents whose passive aura is a pure DUMMY (no proc data) - the mechanic sits in the talent text only.
// ---------------------------------------------------------------------------------------------------------------

// 408 - Kidney Shot, talent Internal Bleeding (154904): "Kidney Shot also deals $154953o1 Bleed damage per combo point
// over $154953d." The bleed is applied with its client amount (AP coefficient 0.24 per tick) and multiplied by the
// combo points Kidney Shot consumed.
class spell_r16_rog_internal_bleeding : public SpellScript
{
    PrepareSpellScript(spell_r16_rog_internal_bleeding);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 154904, 154953 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(154904);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;

        int32 comboPoints = 0;
        for (SpellPowerCost const& cost : GetSpell()->GetPowerCost())
            if (cost.Power == POWER_COMBO_POINTS)
                comboPoints = cost.Amount;
        if (comboPoints <= 0)
            return;

        caster->CastSpell(target, 154953, true);
        if (AuraEffect* bleed = target->GetAuraEffect(154953, EFFECT_0, caster->GetGUID()))
            bleed->ChangeAmount(bleed->GetAmount() * comboPoints);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_r16_rog_internal_bleeding::HandleAfterHit);
    }
};

// 164812 Moonfire / 164815 Sunfire (damage over time), talent Shooting Stars (202342): "Moonfire and Sunfire damage
// over time has a $s1% chance to call down a falling star, dealing $202497s1 Astral damage and generating
// ${$202497m2/10} Astral Power." Both carry the periodic damage on EFFECT_1.
class spell_r16_dru_shooting_stars : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_shooting_stars);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 202342, 202497 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        AuraEffect const* talent = caster->GetAuraEffect(202342, EFFECT_0);
        if (talent && roll_chance_i(talent->GetAmount()))
            caster->CastSpell(GetTarget(), 202497, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_r16_dru_shooting_stars::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Talent Blood Frenzy (203962): "Thrash also generates ${$203961s1/10} Rage each time it deals damage." Direct hit of
// bear Thrash (77758) and every tick of its bleed (192090) cast 203961 (ENERGIZE Rage 20 = 2 Rage).
class spell_r16_dru_blood_frenzy_hit : public SpellScript
{
    PrepareSpellScript(spell_r16_dru_blood_frenzy_hit);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203962, 203961 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(203962);
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (GetHitDamage() > 0)
            GetCaster()->CastSpell(GetCaster(), 203961, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dru_blood_frenzy_hit::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

class spell_r16_dru_blood_frenzy_tick : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_blood_frenzy_tick);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203962, 203961 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Unit* caster = GetCaster())
            if (caster->HasAura(203962))
                caster->CastSpell(caster, 203961, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_r16_dru_blood_frenzy_tick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 23881 - Bloodthirst, talent Fresh Meat (215568): "Bloodthirst has a $s1% increased critical strike chance against
// targets above $s2% health."
class spell_r16_war_fresh_meat : public SpellScript
{
    PrepareSpellScript(spell_r16_war_fresh_meat);

    void HandleCritChance(Unit* victim, float& chance)
    {
        Unit* caster = GetCaster();
        AuraEffect const* bonus = caster ? caster->GetAuraEffect(215568, EFFECT_0) : nullptr;
        AuraEffect const* threshold = caster ? caster->GetAuraEffect(215568, EFFECT_1) : nullptr;
        if (!victim || !bonus || !threshold)
            return;

        if (victim->GetHealthPct() > float(threshold->GetAmount()))
            chance += float(bonus->GetAmount());
    }

    void Register() override
    {
        OnCalcCritChance += SpellOnCalcCritChanceFn(spell_r16_war_fresh_meat::HandleCritChance);
    }
};

// 774 Rejuvenation / 155777 Rejuvenation (Germination), talents Abundance (207383) and Cultivation (200390).
//  Abundance: "For each Rejuvenation you have active, the cast time of Healing Touch is reduced by $207640s1%, and the
//  critical effect chance of Regrowth is increased by $207640s2%." -> one stack of 207640 per active Rejuvenation of
//  the druid (207640 carries both modifiers natively, CumulativeAura 100).
//  Cultivation: "When Rejuvenation heals a target below $s1% health, it applies Cultivation (200389) to the target."
class spell_r16_dru_rejuvenation_talents : public AuraScript
{
    PrepareAuraScript(spell_r16_dru_rejuvenation_talents);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207383, 207640, 200390, 200389 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(207383))
            return;

        if (Aura* abundance = caster->GetAura(207640))
        {
            abundance->ModStackAmount(1);
            abundance->RefreshDuration();
        }
        else
            caster->CastSpell(caster, 207640, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            if (Aura* abundance = caster->GetAura(207640))
                abundance->ModStackAmount(-1);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        AuraEffect const* cultivation = caster->GetAuraEffect(200390, EFFECT_0);
        if (cultivation && GetTarget()->HealthBelowPct(cultivation->GetAmount()))
            caster->CastSpell(GetTarget(), 200389, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_r16_dru_rejuvenation_talents::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_r16_dru_rejuvenation_talents::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_r16_dru_rejuvenation_talents::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
    }
};

// 81269 - Efflorescence (heal), talent Spring Blossoms (207385): "Each target healed by Efflorescence is healed for an
// additional $207386o1 over $207386d."
class spell_r16_dru_spring_blossoms : public SpellScript
{
    PrepareSpellScript(spell_r16_dru_spring_blossoms);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207385, 207386 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(207385);
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
            GetCaster()->CastSpell(target, 207386, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dru_spring_blossoms::HandleHit, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// 139 - Renew, talent Perseverance (235189): "When you cast Renew on yourself, it reduces all damage you take by
// $193065s2%." In 7.3.5 the reduction is Renew's own EFFECT_2 (MOD_DAMAGE_PERCENT_TAKEN -10, target = the healed
// unit); the text only borrows the number from 193065. Without a condition every Renew - on any target, with or
// without the talent - reduced damage taken by 10 %. The effect now only keeps its value when the priest cast Renew
// on himself and has Perseverance.
class spell_r16_pri_perseverance : public AuraScript
{
    PrepareAuraScript(spell_r16_pri_perseverance);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* caster = GetCaster();
        if (!caster || caster->GetGUID() != GetUnitOwner()->GetGUID() || !caster->HasAura(235189))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_r16_pri_perseverance::CalcAmount, EFFECT_2, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

// 47788 - Guardian Spirit, talent Guardian Angel (200209): "When Guardian Spirit expires without saving the target from
// death, reduce its remaining cooldown to $s1 seconds." spell_pri_guardian_spirit removes the aura with
// AURA_REMOVE_BY_ENEMY_SPELL when it saves the target; a plain expiry is AURA_REMOVE_BY_EXPIRE.
class spell_r16_pri_guardian_angel : public AuraScript
{
    PrepareAuraScript(spell_r16_pri_guardian_angel);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* caster = GetCaster();
        if (!caster)
            return;

        AuraEffect const* talent = caster->GetAuraEffect(200209, EFFECT_0);
        if (!talent)
            return;

        uint32 wanted = uint32(talent->GetAmount()) * IN_MILLISECONDS;
        uint32 remaining = caster->GetSpellHistory()->GetRemainingCooldown(GetSpellInfo());
        if (remaining > wanted)
            caster->GetSpellHistory()->ModifyCooldown(GetId(), -int32(remaining - wanted));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_r16_pri_guardian_angel::HandleRemove, EFFECT_0, SPELL_AURA_MOD_HEALING_PCT, AURA_EFFECT_HANDLE_REAL);
    }
};

// 61295 - Riptide, talent Crashing Waves (197464): "Riptide grants an additional stack of Tidal Waves (53390)."
class spell_r16_sha_crashing_waves : public SpellScript
{
    PrepareSpellScript(spell_r16_sha_crashing_waves);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197464, 53390 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(197464);
    }

    void HandleAfterCast()
    {
        GetCaster()->CastSpell(GetCaster(), 53390, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_sha_crashing_waves::HandleAfterCast);
    }
};

// 49998 Death Strike / 194844 Bonestorm, talent Red Thirst (205723): "Spending Runic Power will decrease the remaining
// cooldown on Vampiric Blood (55233) by $s1 sec per $s2 Runic Power." The talent has only spell-effect DUMMYs (no
// aura), so the talent is checked with HasSpell. The core keeps Runic Power in tenths. These are the two Blood spells
// that cost Runic Power in 7.3.5 (SpellPower.csv).
class spell_r16_dk_red_thirst : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_red_thirst);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 205723, 55233 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->GetTypeId() == TYPEID_PLAYER && GetCaster()->ToPlayer()->HasSpell(205723);
    }

    void HandleAfterCast()
    {
        int32 spent = 0;
        for (SpellPowerCost const& cost : GetSpell()->GetPowerCost())
            if (cost.Power == POWER_RUNIC_POWER && cost.Amount > 0)
                spent = cost.Amount / 10;

        int32 secondsPer = R16EffectValue(205723, EFFECT_0);
        int32 per = R16EffectValue(205723, EFFECT_1);
        if (spent <= 0 || secondsPer <= 0 || per <= 0)
            return;

        int32 reductionMs = spent * secondsPer * IN_MILLISECONDS / per;
        GetCaster()->GetSpellHistory()->ModifyCooldown(55233, -reductionMs);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_dk_red_thirst::HandleAfterCast);
    }
};

// 49143 - Frost Strike, talent Shattering Strikes (207057): "If there are 5 stacks of Razorice on the target, Frost
// Strike will consume them and deal $s1% additional damage." The damage sits on the weapon hits 222026 / 66196,
// Razorice is 51714 (CumulativeAura 5).
class spell_r16_dk_shattering_strikes_hit : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_shattering_strikes_hit);

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(207057);
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* talent = caster->GetAuraEffect(207057, EFFECT_0);
        if (!target || !talent)
            return;

        Aura* razorice = target->GetAura(51714, caster->GetGUID());
        if (razorice && razorice->GetStackAmount() >= 5)
            SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), talent->GetAmount()));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_r16_dk_shattering_strikes_hit::HandleHit, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

class spell_r16_dk_shattering_strikes_consume : public SpellScript
{
    PrepareSpellScript(spell_r16_dk_shattering_strikes_consume);

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(207057);
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!target)
            return;

        Aura* razorice = target->GetAura(51714, caster->GetGUID());
        if (razorice && razorice->GetStackAmount() >= 5)
            razorice->Remove();
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_dk_shattering_strikes_consume::HandleAfterCast);
    }
};

// 51124 - Killing Machine, talent Murderous Efficiency (207061): "Consuming the Killing Machine effect has a $s1% chance
// to cause you to gain $207062s1 Rune." Consumption = removal with AURA_REMOVE_BY_DEFAULT (charge / proc use); expiry
// (AURA_REMOVE_BY_EXPIRE), death and cancel do not count.
class spell_r16_dk_murderous_efficiency : public AuraScript
{
    PrepareAuraScript(spell_r16_dk_murderous_efficiency);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207061, 207062 });
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            return;

        Unit* target = GetTarget();
        AuraEffect const* talent = target->GetAuraEffect(207061, EFFECT_0);
        if (talent && roll_chance_i(talent->GetAmount()))
            target->CastSpell(target, 207062, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_r16_dk_murderous_efficiency::HandleRemove, EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Feral finishers, talent Soul of the Forest (158476): "Your finishing moves grant $s1 Energy per combo point spent and
// deal $s2% increased damage." E1/E2 are native percent modifiers (the damage part); E0 (5 Energy per combo point) was
// never granted. Bound to the four Feral finishers of 7.3.5: Rip 1079, Ferocious Bite 22568, Maim 22570,
// Savage Roar 52610.
class spell_r16_dru_soul_of_the_forest_feral : public SpellScript
{
    PrepareSpellScript(spell_r16_dru_soul_of_the_forest_feral);

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(158476);
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        AuraEffect const* talent = caster->GetAuraEffect(158476, EFFECT_0);
        if (!talent)
            return;

        int32 comboPoints = 0;
        for (SpellPowerCost const& cost : GetSpell()->GetPowerCost())
            if (cost.Power == POWER_COMBO_POINTS)
                comboPoints = cost.Amount;

        if (comboPoints > 0 && talent->GetAmount() > 0)
            caster->EnergizeBySpell(caster, 158476, comboPoints * talent->GetAmount(), POWER_ENERGY);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_dru_soul_of_the_forest_feral::HandleAfterCast);
    }
};

// 100 - Charge, talent Furious Charge (202224): "Charge also increases the healing from your next Bloodthirst by
// $202225s1%." 202225 carries the modifier and one charge natively.
class spell_r16_war_furious_charge : public SpellScript
{
    PrepareSpellScript(spell_r16_war_furious_charge);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 202224, 202225 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(202224);
    }

    void HandleAfterCast()
    {
        GetCaster()->CastSpell(GetCaster(), 202225, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_war_furious_charge::HandleAfterCast);
    }
};

// 196834 - Frostbrand, talent Hailstorm (210853): "Frostbrand now also enhances your weapon's damage, causing each of
// your weapon attacks to also deal $210854sw1 Frost damage." Frostbrand's own proc (mask 0x14: melee auto + melee
// special) already fires on every weapon attack (E1 -> 147732).
class spell_r16_sha_hailstorm : public AuraScript
{
    PrepareAuraScript(spell_r16_sha_hailstorm);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 210853, 210854 });
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* target = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || victim == target || !target->HasAura(210853))
            return;

        if (eventInfo.GetSpellInfo() && eventInfo.GetSpellInfo()->Id == 210854)
            return;

        target->CastSpell(victim, 210854, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_r16_sha_hailstorm::HandleProc, EFFECT_1, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 205478 - Desperate Instincts (helper). Talent 205411: "you automatically trigger Blur when you fall below $s1%
// health. This effect can only occur when Blur is not on cooldown." 205411 E0 is aura 468 (below 35 % -> 205478,
// now on the 468 allow list in SpellAuraEffects.cpp); 205478 is a 0.5 s periodic trigger without trigger spell.
// Reading of the client data (the helper is periodic and unlimited): while it is up, each tick checks Blur (198589);
// when Blur is ready it is triggered and put on its normal cooldown, and the helper ends. Above the threshold again
// the helper ends without effect, so the next fall below 35 % starts over.
class spell_r16_dh_desperate_instincts : public AuraScript
{
    PrepareAuraScript(spell_r16_dh_desperate_instincts);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 205411, 198589 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();

        Unit* target = GetTarget();
        AuraEffect const* talent = target->GetAuraEffect(205411, EFFECT_0);
        if (!talent || !target->HealthBelowPct(talent->GetAmount()))
        {
            Remove();
            return;
        }

        SpellInfo const* blur = sSpellMgr->GetSpellInfo(198589);
        if (!blur || target->GetSpellHistory()->HasCooldown(blur))
            return;

        target->CastSpell(target, blur, true);
        target->GetSpellHistory()->StartCooldown(blur, 0, nullptr, false, true);
        Remove();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_r16_dh_desperate_instincts::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Batch 3: artifact traits of the 216 list that were skipped as "companion logic" but only need a summon or a cast
// the client fully describes.
// ---------------------------------------------------------------------------------------------------------------

// 238123 - Cobra Commander (Titanstrike trait). "Cobra Shot has a $h% chance to create $243042m2-$243042M2 Sneaky Snakes
// that attack the target for $243042d." Chance 10 and proc mask come from the client (auto-generated proc of the
// DUMMY aura); 243042 E1 is SUMMON 121661 (SummonProperties 4057, guardian) with the count range of the client.
class spell_r16_hun_cobra_commander : public AuraScript
{
    PrepareAuraScript(spell_r16_hun_cobra_commander);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 243042 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 193455 && eventInfo.GetActionTarget();
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        GetTarget()->CastSpell(eventInfo.GetActionTarget(), 243042, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_r16_hun_cobra_commander::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_r16_hun_cobra_commander::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 193396 - Demonic Empowerment, trait Thal'kiel's Ascendance (238145): "Demonic Empowerment has a $s1% chance to
// enrage Thal'kiel, causing each of your pets to deal $242832s1 Shadow damage to its current target." Every
// controlled unit of the warlock that has a victim casts 242832 (SP coefficient 0.5) on it.
class spell_r16_lock_thalkiels_ascendance : public SpellScript
{
    PrepareSpellScript(spell_r16_lock_thalkiels_ascendance);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238145, 242832 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(238145);
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        AuraEffect const* trait = caster->GetAuraEffect(238145, EFFECT_0);
        if (!trait || !roll_chance_i(trait->GetAmount()))
            return;

        std::vector<Unit*> pets(caster->m_Controlled.begin(), caster->m_Controlled.end());
        for (Unit* pet : pets)
            if (pet && pet->IsAlive())
                if (Unit* victim = pet->GetVictim())
                    pet->CastSpell(victim, 242832, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_lock_thalkiels_ascendance::HandleAfterCast);
    }
};

// 1122 - Summon Infernal, trait Lord of Flames (224103): "Once every $s2 minutes, Summon Infernal will summon $s3
// additional Infernals to serve you for $226804d." (s2 = E1 = 10, s3 = E2 = 3; summon 226804 = creature 108452,
// SummonProperties 3959, target DEST_DEST.) The ten-minute lockout is kept as a cooldown on the trait spell itself.
// Not covered: the Grimoire of Supremacy variant ("your Infernal's Meteor Strike").
class spell_r16_lock_lord_of_flames : public SpellScript
{
    PrepareSpellScript(spell_r16_lock_lord_of_flames);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 224103, 226804 });
    }

    bool Load() override
    {
        return GetCaster() && GetCaster()->HasAura(224103);
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        WorldLocation const* dest = GetExplTargetDest();
        if (!dest || caster->GetSpellHistory()->HasCooldown(224103))
            return;

        int32 minutes = R16EffectValue(224103, EFFECT_1);
        int32 count = R16EffectValue(224103, EFFECT_2);
        if (minutes <= 0 || count <= 0)
            return;

        for (int32 i = 0; i < count; ++i)
            caster->CastSpell(*dest, 226804, true);

        caster->GetSpellHistory()->AddCooldown(224103, 0, std::chrono::minutes(minutes));
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r16_lock_lord_of_flames::HandleAfterCast);
    }
};

void AddSC_class_mechanics_r16_spell_scripts()
{
    RegisterAuraScript(spell_r16_hun_cobra_commander);
    RegisterSpellScript(spell_r16_lock_thalkiels_ascendance);
    RegisterSpellScript(spell_r16_lock_lord_of_flames);
    RegisterSpellScript(spell_r16_rog_internal_bleeding);
    RegisterAuraScript(spell_r16_dru_shooting_stars);
    RegisterSpellScript(spell_r16_dru_blood_frenzy_hit);
    RegisterAuraScript(spell_r16_dru_blood_frenzy_tick);
    RegisterSpellScript(spell_r16_war_fresh_meat);
    RegisterAuraScript(spell_r16_dru_rejuvenation_talents);
    RegisterSpellScript(spell_r16_dru_spring_blossoms);
    RegisterAuraScript(spell_r16_pri_perseverance);
    RegisterAuraScript(spell_r16_pri_guardian_angel);
    RegisterSpellScript(spell_r16_sha_crashing_waves);
    RegisterSpellScript(spell_r16_dk_red_thirst);
    RegisterSpellScript(spell_r16_dk_shattering_strikes_hit);
    RegisterSpellScript(spell_r16_dk_shattering_strikes_consume);
    RegisterAuraScript(spell_r16_dk_murderous_efficiency);
    RegisterSpellScript(spell_r16_dru_soul_of_the_forest_feral);
    RegisterSpellScript(spell_r16_war_furious_charge);
    RegisterAuraScript(spell_r16_sha_hailstorm);
    RegisterAuraScript(spell_r16_dh_desperate_instincts);
    RegisterSpellScript(spell_r16_sha_elemental_overload);
    RegisterSpellScript(spell_r16_sha_elemental_overload_damage);
    RegisterSpellScript(spell_r16_dru_pulverize_thrash);
    RegisterSpellScript(spell_r16_dru_pulverize);
    RegisterSpellScript(spell_r16_dk_icy_talons);
    RegisterAuraScript(spell_r16_war_devastator);
    RegisterSpellScript(spell_r16_dk_soul_reaper_haste);
    RegisterAuraScript(spell_r16_proc_filter);
    RegisterAuraScript(spell_r16_dk_rime);
    RegisterAuraScript(spell_r16_dk_crimson_scourge);
    RegisterAuraScript(spell_r16_pal_grand_crusader_avoid);
    RegisterAuraScript(spell_r16_dru_cenarion_ward);
    RegisterAuraScript(spell_r16_dk_mark_of_blood);
    RegisterAuraScript(spell_r16_dk_permafrost);
    RegisterAuraScript(spell_r16_dk_icecap);
    RegisterSpellScript(spell_r16_dk_heartbreaker);
    RegisterSpellScript(spell_r16_dk_heart_of_ice);
    RegisterAuraScript(spell_r16_rog_main_gauche);
    RegisterAuraScript(spell_r16_pri_shadowy_apparitions);
    RegisterAuraScript(spell_r16_pri_trail_of_light);
    RegisterAuraScript(spell_r16_pri_shield_discipline);
    RegisterAuraScript(spell_r16_dru_earthwarden);
    RegisterAuraScript(spell_r16_dru_earthwarden_absorb);
    RegisterAuraScript(spell_r16_war_anger_management);
    RegisterAuraScript(spell_r16_sha_aftershock);
}
