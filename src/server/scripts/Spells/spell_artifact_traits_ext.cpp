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

// Artifact traits whose effect is applied from the ability that the trait modifies. Each script reads the
// value of the trait from the aura effect of the trait itself, all numbers come from the client spell data.

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"

enum ArtifactTraitExtSpells
{
    SPELL_TRAIT_ELEMENTALIST            = 191512,
    SPELL_SHAMAN_FIRE_ELEMENTAL         = 198067,
    SPELL_SHAMAN_STORM_ELEMENTAL        = 192249,

    SPELL_TRAIT_DECEPTION               = 202755,
    SPELL_ROGUE_FEINT                   = 1966,

    SPELL_TRAIT_KNIGHT_OF_SILVER_HAND   = 200302,
    SPELL_PALADIN_KNIGHT_OF_SILVER_HAND = 211422,

    SPELL_TRAIT_OBSIDIAN_LANCE          = 238056,

    SPELL_TRAIT_BALANCED_BLADES         = 201470,

    SPELL_TRAIT_FATAL_ECHOES            = 199257,
    SPELL_WARLOCK_UNSTABLE_AFFLICTION   = 30108,

    SPELL_TRAIT_SWEET_SOULS             = 199220,
    SPELL_WARLOCK_SWEET_SOULS_HEAL      = 199221,

    SPELL_TRAIT_GLACIAL_ERUPTION        = 238128,
    SPELL_MAGE_GLACIAL_ERUPTION         = 242851,

    SPELL_TRAIT_SACRED_DAWN             = 238132,
    SPELL_PALADIN_SACRED_DAWN           = 243174,

    SPELL_TRAIT_GUARDIANS_OF_THE_LIGHT  = 196437,
    SPELL_PRIEST_GUARDIAN_SPIRIT        = 47788,

    SPELL_TRAIT_POWER_OF_THE_NAARU      = 196489,
    SPELL_PRIEST_POWER_OF_THE_NAARU     = 196490,

    SPELL_TRAIT_TRUST_IN_THE_LIGHT      = 196355,
    SPELL_PRIEST_TRUST_IN_THE_LIGHT     = 196356,

    SPELL_TRAIT_FOCUS_IN_THE_LIGHT      = 196419,
    SPELL_PRIEST_FOCUS_SLOW             = 210979,
    SPELL_PRIEST_FOCUS_SPEED            = 210980,
    SPELL_PRIEST_FOCUSED_WILL           = 45242,

    SPELL_TRAIT_TIMES_AND_MEASURES      = 238100,
    SPELL_PRIEST_DESPERATE_PRAYER       = 19236
};

// Times and Measures: the client data only says "proportional to the magnitude of that damage" and holds no number.
// Assumption: the chance in percent equals the damage taken in percent of maximum health (a hit for 20% of maximum health = 20%).
float const TimesAndMeasuresChancePerHealthPct = 1.0f;

// Counters read by the .arttest command (cs_arttest.cpp) to check that the trait scripts ran with the expected values
namespace ArtifactTraitTest
{
    int32 LastObsidianLancePct = -1;
    int32 LastBalancedBladesPct = -1;
    uint32 GlacialEruptionCasts = 0;
    uint32 KnightHits = 0;
    uint32 KnightCasts = 0;
    int32 BalancedBladesTargets = 0;
    uint32 SacredDawnHits = 0;
    uint32 CosmicRippleCasts = 0;
    uint32 TimesAndMeasuresResets = 0;
}

// 51505 - Lava Burst
// Elementalist (191512): each Lava Burst reduces the remaining cooldown of Fire/Storm Elemental
class spell_arti_sha_elementalist : public SpellScript
{
    PrepareSpellScript(spell_arti_sha_elementalist);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_ELEMENTALIST, SPELL_SHAMAN_FIRE_ELEMENTAL, SPELL_SHAMAN_STORM_ELEMENTAL });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        AuraEffect const* trait = caster->GetAuraEffect(SPELL_TRAIT_ELEMENTALIST, EFFECT_0);
        if (!trait)
            return;

        // the value is stored as negative milliseconds
        int32 reduction = trait->GetAmount();
        for (uint32 elemental : { SPELL_SHAMAN_FIRE_ELEMENTAL, SPELL_SHAMAN_STORM_ELEMENTAL })
            if (caster->GetSpellHistory()->HasCooldown(elemental))
                caster->GetSpellHistory()->ModifyCooldown(elemental, reduction);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_arti_sha_elementalist::HandleAfterCast);
    }
};

// 2983 - Sprint
// Deception (202755): Sprint casts Feint for no energy
class spell_arti_rog_deception : public SpellScript
{
    PrepareSpellScript(spell_arti_rog_deception);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_DECEPTION, SPELL_ROGUE_FEINT });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (caster->HasAura(SPELL_TRAIT_DECEPTION))
            caster->CastSpell(caster, SPELL_ROGUE_FEINT, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_arti_rog_deception::HandleAfterCast);
    }
};

// 20271 - Judgment
// Knight of the Silver Hand (200302): after Judgment strikes an enemy, damage taken is reduced (aura 211422)
class spell_arti_pal_knight_of_the_silver_hand : public SpellScript
{
    PrepareSpellScript(spell_arti_pal_knight_of_the_silver_hand);

    void HandleCast()
    {
        ++ArtifactTraitTest::KnightCasts;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_KNIGHT_OF_SILVER_HAND, SPELL_PALADIN_KNIGHT_OF_SILVER_HAND });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (caster->HasAura(SPELL_TRAIT_KNIGHT_OF_SILVER_HAND))
            {
            ++ArtifactTraitTest::KnightHits;
            caster->CastSpell(caster, SPELL_PALADIN_KNIGHT_OF_SILVER_HAND, true);
        }
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_arti_pal_knight_of_the_silver_hand::HandleCast);
        OnEffectHitTarget += SpellEffectFn(spell_arti_pal_knight_of_the_silver_hand::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 228598 - Ice Lance (damage)
// Obsidian Lance (238056): increases the damage of Ice Lance against frozen targets
class spell_arti_mage_obsidian_lance : public SpellScript
{
    PrepareSpellScript(spell_arti_mage_obsidian_lance);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_OBSIDIAN_LANCE });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* trait = caster->GetAuraEffect(SPELL_TRAIT_OBSIDIAN_LANCE, EFFECT_0);
        if (!target || !trait || !target->HasAuraState(AURA_STATE_FROZEN, GetSpellInfo(), caster))
            return;

        int32 damage = GetHitDamage();
        AddPct(damage, trait->GetAmount());
        SetHitDamage(damage);
        ArtifactTraitTest::LastObsidianLancePct = trait->GetAmount();
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_arti_mage_obsidian_lance::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 199552, 200685 - Blade Dance (damage)
// Balanced Blades (201470): Blade Dance deals more damage for each target hit
class spell_arti_dh_balanced_blades : public SpellScript
{
    PrepareSpellScript(spell_arti_dh_balanced_blades);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_BALANCED_BLADES });
    }

    void CountTargets(std::list<WorldObject*>& targets)
    {
        _targetCount = uint32(targets.size());
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        AuraEffect const* trait = GetCaster()->GetAuraEffect(SPELL_TRAIT_BALANCED_BLADES, EFFECT_0);
        if (!trait || !_targetCount)
            return;

        int32 damage = GetHitDamage();
        AddPct(damage, trait->GetAmount() * int32(_targetCount));
        SetHitDamage(damage);
        ArtifactTraitTest::LastBalancedBladesPct = trait->GetAmount() * int32(_targetCount);
        ArtifactTraitTest::BalancedBladesTargets = int32(_targetCount);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_arti_dh_balanced_blades::CountTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        OnEffectHitTarget += SpellEffectFn(spell_arti_dh_balanced_blades::HandleHit, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }

private:
    uint32 _targetCount = 0;
};

// 233490, 233496, 233497, 233498, 233499 - Unstable Affliction (debuffs)
// Fatal Echoes (199257): when Unstable Affliction expires it has a chance to reapply itself
class aura_arti_warl_fatal_echoes : public AuraScript
{
    PrepareAuraScript(aura_arti_warl_fatal_echoes);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_FATAL_ECHOES, SPELL_WARLOCK_UNSTABLE_AFFLICTION });
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !target->IsAlive())
            return;

        AuraEffect const* trait = caster->GetAuraEffect(SPELL_TRAIT_FATAL_ECHOES, EFFECT_0);
        if (!trait || !roll_chance_i(trait->GetAmount()))
            return;

        caster->CastSpell(target, SPELL_WARLOCK_UNSTABLE_AFFLICTION, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_arti_warl_fatal_echoes::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 6262 - Healthstone
// Sweet Souls (199220): the warlock heals for an additional percentage of maximum health whenever
// they or any member of their party or raid uses a Healthstone
class spell_arti_warl_sweet_souls : public SpellScript
{
    PrepareSpellScript(spell_arti_warl_sweet_souls);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_SWEET_SOULS, SPELL_WARLOCK_SWEET_SOULS_HEAL });
    }

    void HandleHit()
    {
        Player* user = GetCaster()->ToPlayer();
        if (!user)
            return;

        std::list<Unit*> units;
        user->GetFriendlyUnitListInRange(units, 100.0f);
        units.push_back(user);

        for (Unit* unit : units)
        {
            Player* warlock = unit->ToPlayer();
            if (!warlock || (warlock != user && !warlock->IsInSameRaidWith(user)))
                continue;

            if (AuraEffect const* trait = warlock->GetAuraEffect(SPELL_TRAIT_SWEET_SOULS, EFFECT_0))
                warlock->CastCustomSpell(SPELL_WARLOCK_SWEET_SOULS_HEAL, SPELLVALUE_BASE_POINT0, trait->GetAmount(), warlock, true);
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_arti_warl_sweet_souls::HandleHit);
    }
};

// Delayed pillar of ice at the location where Ebonbolt hit
class GlacialEruptionEvent : public BasicEvent
{
public:
    GlacialEruptionEvent(Unit* caster, Position const& dest) : _caster(caster), _dest(dest) { }

    bool Execute(uint64 /*execTime*/, uint32 /*diff*/) override
    {
        _caster->CastSpell(_dest.GetPositionX(), _dest.GetPositionY(), _dest.GetPositionZ(), SPELL_MAGE_GLACIAL_ERUPTION, true);
        ++ArtifactTraitTest::GlacialEruptionCasts;
        return true;
    }

private:
    Unit* _caster;
    Position _dest;
};

// 228599 - Ebonbolt (damage)
// Glacial Eruption (238128): a pillar of ice bursts from the ground at the target after a delay
class spell_arti_mage_glacial_eruption : public SpellScript
{
    PrepareSpellScript(spell_arti_mage_glacial_eruption);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_GLACIAL_ERUPTION, SPELL_MAGE_GLACIAL_ERUPTION });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* trait = caster->GetAuraEffect(SPELL_TRAIT_GLACIAL_ERUPTION, EFFECT_0);
        if (!target || !trait)
            return;

        // the value is the delay in seconds
        caster->m_Events.AddEvent(new GlacialEruptionEvent(caster, target->GetPosition()), caster->m_Events.CalculateTime(trait->GetAmount() * IN_MILLISECONDS));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_arti_mage_glacial_eruption::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 225311 - Light of Dawn (heal)
// Sacred Dawn (238132): allies healed by Light of Dawn receive increased healing from the paladin's spells
class spell_arti_pal_sacred_dawn : public SpellScript
{
    PrepareSpellScript(spell_arti_pal_sacred_dawn);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_SACRED_DAWN, SPELL_PALADIN_SACRED_DAWN });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (target && caster->HasAura(SPELL_TRAIT_SACRED_DAWN))
            {
            ++ArtifactTraitTest::SacredDawnHits;
            caster->CastSpell(target, SPELL_PALADIN_SACRED_DAWN, true);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_arti_pal_sacred_dawn::HandleHit, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// 47788 - Guardian Spirit
// Guardians of the Light (196437): Guardian Spirit cast on another target also places a spirit on the priest
class spell_arti_pri_guardians_of_the_light : public SpellScript
{
    PrepareSpellScript(spell_arti_pri_guardians_of_the_light);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_GUARDIANS_OF_THE_LIGHT, SPELL_PRIEST_GUARDIAN_SPIRIT });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        // the second cast targets the priest, so it does not trigger this script again
        if (target && target != caster && caster->HasAura(SPELL_TRAIT_GUARDIANS_OF_THE_LIGHT))
            caster->CastSpell(caster, SPELL_PRIEST_GUARDIAN_SPIRIT, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_arti_pri_guardians_of_the_light::HandleAfterCast);
    }
};

// 34861 - Holy Word: Sanctify
// Power of the Naaru (196489): increases the healing of Prayer of Healing for a short time (aura 196490)
class spell_arti_pri_power_of_the_naaru : public SpellScript
{
    PrepareSpellScript(spell_arti_pri_power_of_the_naaru);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_POWER_OF_THE_NAARU, SPELL_PRIEST_POWER_OF_THE_NAARU });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (caster->HasAura(SPELL_TRAIT_POWER_OF_THE_NAARU))
            caster->CastSpell(caster, SPELL_PRIEST_POWER_OF_THE_NAARU, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_arti_pri_power_of_the_naaru::HandleAfterCast);
    }
};

// 73325 - Leap of Faith
// Trust in the Light (196355): the target is healed for a percentage of its maximum health over the duration of aura 196356
class spell_arti_pri_trust_in_the_light : public SpellScript
{
    PrepareSpellScript(spell_arti_pri_trust_in_the_light);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_TRUST_IN_THE_LIGHT, SPELL_PRIEST_TRUST_IN_THE_LIGHT });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* trait = caster->GetAuraEffect(SPELL_TRAIT_TRUST_IN_THE_LIGHT, EFFECT_0);
        if (!target || !trait)
            return;

        caster->CastSpell(target, SPELL_PRIEST_TRUST_IN_THE_LIGHT, true);
        // the aura only carries the percentage: turn it into the amount healed per tick
        if (AuraEffect* heal = target->GetAuraEffect(SPELL_PRIEST_TRUST_IN_THE_LIGHT, EFFECT_0, caster->GetGUID()))
            if (int32 ticks = heal->GetTotalTicks())
                heal->SetAmount(CalculatePct(target->GetMaxHealth(), trait->GetAmount()) / ticks);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_arti_pri_trust_in_the_light::HandleAfterHit);
    }
};

// 14914 - Holy Fire, 88625 - Holy Word: Chastise
// Focus in the Light (196419): while Focused Will is active the target is slowed (210979) and the priest gains speed (210980)
class spell_arti_pri_focus_in_the_light : public SpellScript
{
    PrepareSpellScript(spell_arti_pri_focus_in_the_light);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT_FOCUS_IN_THE_LIGHT, SPELL_PRIEST_FOCUSED_WILL, SPELL_PRIEST_FOCUS_SLOW, SPELL_PRIEST_FOCUS_SPEED });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target || !caster->HasAura(SPELL_TRAIT_FOCUS_IN_THE_LIGHT) || !caster->HasAura(SPELL_PRIEST_FOCUSED_WILL))
            return;

        caster->CastSpell(target, SPELL_PRIEST_FOCUS_SLOW, true);
        caster->CastSpell(caster, SPELL_PRIEST_FOCUS_SPEED, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_arti_pri_focus_in_the_light::HandleAfterHit);
    }
};

// 243241 - Cosmic Ripple (heal)
// The trait itself is started from SpellHistory (cooldown of Holy Word: Serenity / Sanctify finished); this script only counts the casts for .arttest
class spell_arti_pri_cosmic_ripple_heal : public SpellScript
{
    PrepareSpellScript(spell_arti_pri_cosmic_ripple_heal);

    void HandleBeforeCast()
    {
        ++ArtifactTraitTest::CosmicRippleCasts;
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_arti_pri_cosmic_ripple_heal::HandleBeforeCast);
    }
};

// 238100 - Times and Measures
// Damage taken has a chance, proportional to the damage, to reset the cooldown of Desperate Prayer
class aura_arti_pri_times_and_measures : public AuraScript
{
    PrepareAuraScript(aura_arti_pri_times_and_measures);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PRIEST_DESPERATE_PRAYER });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetDamage() && GetTarget()->GetSpellHistory()->HasCooldown(SPELL_PRIEST_DESPERATE_PRAYER);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* priest = GetTarget();
        uint32 maxHealth = priest->GetMaxHealth();
        if (!maxHealth)
            return;

        float damagePct = 100.0f * float(eventInfo.GetDamageInfo()->GetDamage()) / float(maxHealth);
        if (!roll_chance_f(damagePct * TimesAndMeasuresChancePerHealthPct))
            return;

        ++ArtifactTraitTest::TimesAndMeasuresResets;
        priest->GetSpellHistory()->ResetCooldown(SPELL_PRIEST_DESPERATE_PRAYER, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_arti_pri_times_and_measures::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_arti_pri_times_and_measures::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddSC_artifact_trait_ext_spell_scripts()
{
    RegisterSpellScript(spell_arti_sha_elementalist);
    RegisterSpellScript(spell_arti_rog_deception);
    RegisterSpellScript(spell_arti_pal_knight_of_the_silver_hand);
    RegisterSpellScript(spell_arti_mage_obsidian_lance);
    RegisterSpellScript(spell_arti_dh_balanced_blades);
    RegisterAuraScript(aura_arti_warl_fatal_echoes);
    RegisterSpellScript(spell_arti_warl_sweet_souls);
    RegisterSpellScript(spell_arti_mage_glacial_eruption);
    RegisterSpellScript(spell_arti_pal_sacred_dawn);
    RegisterSpellScript(spell_arti_pri_guardians_of_the_light);
    RegisterSpellScript(spell_arti_pri_power_of_the_naaru);
    RegisterSpellScript(spell_arti_pri_trust_in_the_light);
    RegisterSpellScript(spell_arti_pri_focus_in_the_light);
    RegisterSpellScript(spell_arti_pri_cosmic_ripple_heal);
    RegisterAuraScript(aura_arti_pri_times_and_measures);
}
