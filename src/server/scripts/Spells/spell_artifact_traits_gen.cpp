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

// Artifact trait candidates, kept separate from the stable scripts so that they can be switched on and off.
//
// Every script in this file follows one of a handful of patterns (see the macros below). All numbers are read at
// runtime from the client spell data of the trait itself (aura effect amount, EffectTriggerSpell, ProcChance),
// nothing is hard coded except the spell ids that bind a script to an ability.
//
// The scripts only become active once the rows in C:\LegionServer\trait_candidates\gen_*.sql are loaded
// (tools\apply_trait_candidates.ps1). Script names all start with "gen_arti_".

#include "AreaTrigger.h"
#include "Containers.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "Pet.h"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include "AreaTriggerAI.h"
#include <G3D/Vector3.h>

// Counters read by the ".arttest gen" command (cs_arttest.cpp): trait spell id -> how often the script body ran.
namespace ArtifactTraitTest
{
    std::map<uint32, uint32> GenRan;

    void MarkGenRan(uint32 traitId)
    {
        ++GenRan[traitId];
    }
}

namespace
{
    // amount of the first effect of the trait aura on the caster, 0 when the trait is not learned.
    // Some trait auras end up with an applied amount of 0 (the rank data of the artifact power is what normally
    // fills them). The base points of the same effect from the client data are then used instead, so the trait
    // still works with the value the client ships - no invented number.
    int32 TraitValue(Unit* caster, uint32 traitId, uint8 effIndex = EFFECT_0)
    {
        if (!caster)
            return 0;

        if (AuraEffect const* effect = caster->GetAuraEffect(traitId, effIndex))
        {
            if (int32 amount = effect->GetAmount())
                return amount;

            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(traitId))
                if (SpellEffectInfo const* spellEffect = info->GetEffect(effIndex))
                    return spellEffect->BasePoints;
        }
        return 0;
    }

    // spell that the trait triggers according to the client data (SpellEffect.EffectTriggerSpell)
    uint32 TraitTriggerSpell(uint32 traitId, uint32 effIndex = EFFECT_0)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(traitId))
            if (SpellEffectInfo const* effect = info->GetEffect(effIndex))
                return effect->TriggerSpell;
        return 0;
    }

    // proc chance of the trait according to the client data (SpellAuraOptions.ProcChance, "$h" in the description)
    uint32 TraitProcChance(uint32 traitId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(traitId))
            return info->ProcChance;
        return 0;
    }

    // base points of an effect of any spell, used where the description refers to another spell ("$197163s2")
    int32 SpellEffectValue(uint32 spellId, uint32 effIndex)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            if (SpellEffectInfo const* effect = info->GetEffect(effIndex))
                return effect->BasePoints;
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Pattern 1: casting the ability puts a helper aura on the caster ("Activating X grants/heals/reduces ...").
// The helper spell id is taken from the trait data where the trait carries an EffectTriggerSpell, otherwise it is
// given explicitly because the description names it.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_SELF_ON_CAST(scriptName, traitId, helperId)                                   \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId, helperId });                                        \
    }                                                                                           \
                                                                                                \
    void HandleAfterCast()                                                                      \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !caster->HasAura(traitId))                                               \
            return;                                                                             \
                                                                                                \
        caster->CastSpell(caster, uint32(helperId), true);                                      \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterCast += SpellCastFn(scriptName::HandleAfterCast);                                  \
    }                                                                                           \
};

// Pattern 1b: same, but the helper spell is the EffectTriggerSpell of the trait itself
#define GEN_TRAIT_SELF_ON_CAST_TRIGGER(scriptName, traitId)                                     \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId }) && TraitTriggerSpell(traitId) != 0;               \
    }                                                                                           \
                                                                                                \
    void HandleAfterCast()                                                                      \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !caster->HasAura(traitId))                                               \
            return;                                                                             \
                                                                                                \
        if (uint32 trigger = TraitTriggerSpell(traitId))                                        \
        {                                                                                       \
            caster->CastSpell(caster, trigger, true);                                           \
            ArtifactTraitTest::MarkGenRan(traitId);                                             \
        }                                                                                       \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterCast += SpellCastFn(scriptName::HandleAfterCast);                                  \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 2: the ability also puts a helper aura on the target it hit.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_TARGET_ON_HIT(scriptName, traitId, helperId)                                  \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId, helperId });                                        \
    }                                                                                           \
                                                                                                \
    void HandleAfterHit()                                                                       \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        Unit* target = GetHitUnit();                                                            \
        if (!caster || !target || !caster->HasAura(traitId))                                    \
            return;                                                                             \
                                                                                                \
        caster->CastSpell(target, uint32(helperId), true);                                      \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterHit += SpellHitFn(scriptName::HandleAfterHit);                                     \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 3: the ability has a chance (value of the trait aura, in percent) to cast itself a second time.
// The repeated cast must not start the same roll again. Earlier this was guarded with GetSpell()->IsTriggered(),
// which also blocked every other triggered cast of the ability - including the ones the ".arttest gen" harness
// makes, so the trait could never be checked. The guard below only skips the repeat this script started itself.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_REPEAT_CHANCE(scriptName, traitId, abilityId)                                 \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    /* true while this script casts the repeat, so the repeat does not roll again */            \
    static bool& RepeatGuard()                                                                  \
    {                                                                                           \
        static thread_local bool repeating = false;                                             \
        return repeating;                                                                       \
    }                                                                                           \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId, abilityId });                                       \
    }                                                                                           \
                                                                                                \
    void HandleAfterCast()                                                                      \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || RepeatGuard() || !caster->HasAura(traitId))                              \
            return;                                                                             \
                                                                                                \
        int32 chance = TraitValue(caster, traitId);                                             \
        if (chance <= 0 || !roll_chance_i(chance))                                              \
            return;                                                                             \
                                                                                                \
        Unit* target = GetExplTargetUnit();                                                     \
        {                                                                                       \
            /* scope guard: the flag is cleared even if the repeated cast leaves early */       \
            struct Scope                                                                        \
            {                                                                                   \
                explicit Scope(bool& flag) : _flag(flag) { _flag = true; }                      \
                ~Scope() { _flag = false; }                                                     \
                bool& _flag;                                                                    \
            } scope(RepeatGuard());                                                             \
            caster->CastSpell(target ? target : caster, uint32(abilityId), true);               \
        }                                                                                       \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterCast += SpellCastFn(scriptName::HandleAfterCast);                                  \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 4: the ability has a chance to cast the spell the trait triggers. The chance is the value of the trait
// aura when the description says "$s1%/$m1%", or the proc chance of the trait when the description says "$h%".
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_TRIGGER_CHANCE(scriptName, traitId, useProcChance, onTarget)                  \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId }) && TraitTriggerSpell(traitId) != 0;               \
    }                                                                                           \
                                                                                                \
    void HandleAfterCast()                                                                      \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !caster->HasAura(traitId))                                               \
            return;                                                                             \
                                                                                                \
        int32 chance = useProcChance ? int32(TraitProcChance(traitId)) : TraitValue(caster, traitId); \
        if (chance <= 0 || !roll_chance_i(chance))                                              \
            return;                                                                             \
                                                                                                \
        uint32 trigger = TraitTriggerSpell(traitId);                                            \
        if (!trigger)                                                                           \
            return;                                                                             \
                                                                                                \
        Unit* target = caster;                                                                  \
        if (onTarget)                                                                           \
            if (Unit* explTarget = GetExplTargetUnit())                                         \
                target = explTarget;                                                            \
                                                                                                \
        caster->CastSpell(target, trigger, true);                                               \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterCast += SpellCastFn(scriptName::HandleAfterCast);                                  \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 5: the ability heals the caster for a percentage (value of the trait aura) of the damage it dealt.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_LEECH(scriptName, traitId)                                                    \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId });                                                  \
    }                                                                                           \
                                                                                                \
    void HandleAfterHit()                                                                       \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster)                                                                            \
            return;                                                                             \
                                                                                                \
        int32 pct = TraitValue(caster, traitId);                                                \
        int32 damage = GetHitDamage();                                                          \
        if (pct <= 0 || damage <= 0)                                                            \
            return;                                                                             \
                                                                                                \
        uint32 heal = uint32(CalculatePct(damage, pct));                                        \
        if (!heal)                                                                              \
            return;                                                                             \
                                                                                                \
        HealInfo healInfo(caster, caster, heal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask()); \
        caster->HealBySpell(healInfo);                                                          \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterHit += SpellHitFn(scriptName::HandleAfterHit);                                     \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 6: the healing of the ability is raised by the percentage in the trait aura.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_HEAL_PCT(scriptName, traitId, effIndex)                                       \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId });                                                  \
    }                                                                                           \
                                                                                                \
    void HandleHit(SpellEffIndex /*effIndex*/)                                                  \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        int32 pct = TraitValue(caster, traitId);                                                \
        if (pct <= 0)                                                                           \
            return;                                                                             \
                                                                                                \
        int32 heal = GetHitHeal();                                                              \
        AddPct(heal, pct);                                                                      \
        SetHitHeal(heal);                                                                       \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        OnEffectHitTarget += SpellEffectFn(scriptName::HandleHit, effIndex, SPELL_EFFECT_HEAL); \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 7: the damage of the ability is raised by the percentage in the trait aura.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_DAMAGE_PCT(scriptName, traitId, effIndex)                                     \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId });                                                  \
    }                                                                                           \
                                                                                                \
    void HandleHit(SpellEffIndex /*effIndex*/)                                                  \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        int32 pct = TraitValue(caster, traitId);                                                \
        if (pct <= 0)                                                                           \
            return;                                                                             \
                                                                                                \
        int32 damage = GetHitDamage();                                                          \
        AddPct(damage, pct);                                                                    \
        SetHitDamage(damage);                                                                   \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        OnEffectHitTarget += SpellEffectFn(scriptName::HandleHit, effIndex, SPELL_EFFECT_SCHOOL_DAMAGE); \
    }                                                                                           \
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 8: every target the ability hits puts one stack of a helper aura on the caster.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_STACK_PER_TARGET(scriptName, traitId, helperId)                               \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId, helperId });                                        \
    }                                                                                           \
                                                                                                \
    void HandleAfterHit()                                                                       \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !GetHitUnit() || !caster->HasAura(traitId))                              \
            return;                                                                             \
                                                                                                \
        caster->CastSpell(caster, uint32(helperId), true);                                      \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        AfterHit += SpellHitFn(scriptName::HandleAfterHit);                                     \
    }                                                                                           \
};

// ===============================================================================================================
// Priest
// ===============================================================================================================

// 586 - Fade; Vestments of Discipline (197711): Fade also reduces damage taken (216135)
GEN_TRAIT_SELF_ON_CAST(gen_arti_pri_vestments_of_discipline, 197711, 216135)

// 47540 - Penance; Speed of the Pious (197766): movement speed after Penance (197767)
GEN_TRAIT_SELF_ON_CAST(gen_arti_pri_speed_of_the_pious, 197766, 197767)

// 205065 - Void Torrent; Mind Quickening (238101): haste for the priest and nearby allies (240673)
GEN_TRAIT_SELF_ON_CAST_TRIGGER(gen_arti_pri_mind_quickening, 238101)

// 17 - Power Word: Shield; Shield of Faith (197729): the absorb is raised by the value of the trait
class gen_arti_pri_shield_of_faith : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_shield_of_faith);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197729 });
    }

    void CalculateAmount(AuraEffect const* aurEff, int32& amount, bool& /*canBeRecalculated*/)
    {
        int32 pct = TraitValue(aurEff->GetCaster(), 197729);
        if (pct <= 0)
            return;

        AddPct(amount, pct);
        ArtifactTraitTest::MarkGenRan(197729);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_pri_shield_of_faith::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
    }
};

// 17 - Power Word: Shield; Share in the Light (197781): shielding somebody else also shields the priest
// for the given percentage of the same absorb (client helper 210027, value from the target's shield).
class gen_arti_pri_share_in_the_light : public SpellScript
{
    PrepareSpellScript(gen_arti_pri_share_in_the_light);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197781, 17, 210027 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;

        int32 pct = TraitValue(caster, 197781);
        if (pct <= 0)
            return;

        AuraEffect const* shield = target->GetAuraEffect(17, EFFECT_0, caster->GetGUID());
        if (!shield)
            return;

        int32 amount = CalculatePct(shield->GetAmount(), pct);
        if (amount <= 0)
            return;

        // Round LCF2 R23: LegionCore-7.3.5 (spell_priest.cpp, PW:S CalculateAmount) puts the share on the client's own
        // helper 210027 "Share in the Light" (SCHOOL_ABSORB, 15 s) with bp = $s1 % of the target's shield - not on a
        // second Power Word: Shield (that one would also bring Weakened Soul / Atonement side effects of 17).
        caster->CastCustomSpell(210027, SPELLVALUE_BASE_POINT0, amount, caster, true);

        ArtifactTraitTest::MarkGenRan(197781);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_pri_share_in_the_light::HandleAfterHit);
    }
};

// 194384 - Atonement; Sins of the Many (198074): one stack of 198076 per ally that carries Atonement
class gen_arti_pri_sins_of_the_many : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_sins_of_the_many);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198074, 198076 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(198074))
            return;

        caster->CastSpell(caster, 198076u, true);
        ArtifactTraitTest::MarkGenRan(198074);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            if (caster->HasAura(198076))
                caster->RemoveAuraFromStack(198076);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_pri_sins_of_the_many::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pri_sins_of_the_many::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ===============================================================================================================
// Paladin
// ===============================================================================================================

// 205273 - Wake of Ashes; Ashes to Ashes (179546): Wake of Ashes also generates Holy Power (218001)
GEN_TRAIT_SELF_ON_CAST(gen_arti_pal_ashes_to_ashes, 179546, 218001)

// 184092 - Light of the Protector, 213652 - Hand of the Protector; Scatter the Shadows (209223)
GEN_TRAIT_HEAL_PCT(gen_arti_pal_scatter_the_shadows, 209223, EFFECT_0)

// 53385 - Divine Storm; Healing Storm (193058): heals up to <trait value> nearby allies with 215257
class gen_arti_pal_healing_storm : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_healing_storm);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 193058, 215257 });
    }

    // Round 15 (24.09.2026): the radius is in the client after all. Divine Storm 53385 E1 is a DUMMY on
    // TARGET_SRC_CASTER / TARGET_UNIT_SRC_AREA_ALLY with EffectRadiusIndex 14 (spellradius.csv: 8 yd) - the ally search of
    // Healing Storm (E0 is the enemy search, 224239 the damage). Before: a guessed 30 yd around the paladin.
    // Only players ("nearby allied players"). If more than $s1 are in range the pick is random (order not in the data).
    void CollectAllies(std::list<WorldObject*>& targets)
    {
        _allies.clear();
        for (WorldObject* obj : targets)
            if (Unit* unit = obj->ToUnit())
                if (unit->IsAlive() && unit->GetTypeId() == TYPEID_PLAYER)
                    _allies.push_back(unit->GetGUID());
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 count = TraitValue(caster, 193058);
        if (!caster || count <= 0 || _allies.empty())
            return;

        if (_allies.size() > std::size_t(count))
            Trinity::Containers::RandomResize(_allies, std::size_t(count));

        int32 healed = 0;
        for (ObjectGuid const& guid : _allies)
            if (Unit* ally = ObjectAccessor::GetUnit(*caster, guid))
            {
                caster->CastSpell(ally, 215257u, true);
                ++healed;
            }

        if (healed)
            ArtifactTraitTest::MarkGenRan(193058);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(gen_arti_pal_healing_storm::CollectAllies, EFFECT_1, TARGET_UNIT_SRC_AREA_ALLY);
        AfterCast += SpellCastFn(gen_arti_pal_healing_storm::HandleAfterCast);
    }

    GuidList _allies;
};

// 31935 - Avenger's Shield; Bulwark of Order (209389): absorb shield worth <trait value>% of the damage dealt
class gen_arti_pal_bulwark_of_order : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_bulwark_of_order);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 209389 }) && TraitTriggerSpell(209389) != 0;
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        int32 pct = TraitValue(caster, 209389);
        int32 damage = GetHitDamage();
        if (!caster || pct <= 0 || damage <= 0)
            return;

        uint32 trigger = TraitTriggerSpell(209389);
        if (!trigger)
            return;

        caster->CastCustomSpell(trigger, SPELLVALUE_BASE_POINT0, CalculatePct(damage, pct), caster, true);
        ArtifactTraitTest::MarkGenRan(209389);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_pal_bulwark_of_order::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ===============================================================================================================
// Shaman
// ===============================================================================================================

// 108271 - Astral Shift; Elemental Healing (198248): heal over time (198249)
GEN_TRAIT_SELF_ON_CAST(gen_arti_sha_elemental_healing, 198248, 198249)

// 2825 - Bloodlust, 32182 - Heroism; Sense of Urgency (207355): more healing done (208416)
GEN_TRAIT_SELF_ON_CAST(gen_arti_sha_sense_of_urgency, 207355, 208416)

// 52042 - Healing Stream Totem heal; Queen's Decree (207360): additional heal over time (208899)
class gen_arti_sha_queens_decree : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_queens_decree);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207360, 208899 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        // the totem casts the heal, the trait sits on the shaman who summoned it
        Unit* shaman = GetOriginalCaster();
        Unit* target = GetHitUnit();
        if (!shaman || !target || !shaman->HasAura(207360))
            return;

        shaman->CastSpell(target, 208899u, true);
        ArtifactTraitTest::MarkGenRan(207360);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_sha_queens_decree::HandleHit, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// 114942 - Healing Tide; Cumulative Upkeep (207362): stacking bonus to the healing received (208205)
class gen_arti_sha_cumulative_upkeep : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_cumulative_upkeep);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207362, 208205 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* shaman = GetOriginalCaster();
        Unit* target = GetHitUnit();
        if (!shaman || !target || !shaman->HasAura(207362))
            return;

        shaman->CastSpell(target, 208205u, true);
        ArtifactTraitTest::MarkGenRan(207362);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_sha_cumulative_upkeep::HandleHit, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// 187874 - Crash Lightning; Gathering Storms (198299): one stack of 198300 per target hit
class gen_arti_sha_gathering_storms : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_gathering_storms);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198299, 198300 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(198299))
            return;

        caster->CastSpell(caster, 198300u, true);
        ArtifactTraitTest::MarkGenRan(198299);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_sha_gathering_storms::HandleHit, EFFECT_1, SPELL_EFFECT_NORMALIZED_WEAPON_DMG);
    }
};

// 25504, 33750 - Windfury Attack; Winds of Change (238106): additional Maelstrom per Windfury attack
class gen_arti_sha_winds_of_change : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_winds_of_change);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238106 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 amount = TraitValue(caster, 238106);
        if (!caster || amount <= 0)
            return;

        caster->EnergizeBySpell(caster, 238106, amount, POWER_MAELSTROM);
        ArtifactTraitTest::MarkGenRan(238106);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_sha_winds_of_change::HandleAfterCast);
    }
};

// true while the delayed second Gift of the Queen is cast, so that cast does not schedule another one
static thread_local bool DeepWatersRepeating = false;

// Second cast of Gift of the Queen at the same place after the delay of the trait
class DeepWatersEvent : public BasicEvent
{
public:
    DeepWatersEvent(Unit* caster, Position const& dest) : _caster(caster), _dest(dest) { }

    bool Execute(uint64 /*execTime*/, uint32 /*diff*/) override
    {
        DeepWatersRepeating = true;
        _caster->CastSpell(_dest.GetPositionX(), _dest.GetPositionY(), _dest.GetPositionZ(), 207778, true);
        DeepWatersRepeating = false;
        return true;
    }

private:
    Unit* _caster;
    Position _dest;
};

// 207778 - Gift of the Queen; Deep Waters (238143)
class gen_arti_sha_deep_waters : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_deep_waters);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238143, 207778 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || DeepWatersRepeating)
            return;

        int32 delay = TraitValue(caster, 238143);
        if (delay <= 0)
            return;

        Position dest = caster->GetPosition();
        if (WorldLocation const* explDest = GetExplTargetDest())
            dest = explDest->GetPosition();

        caster->m_Events.AddEvent(new DeepWatersEvent(caster, dest), caster->m_Events.CalculateTime(uint32(delay) * IN_MILLISECONDS));
        ArtifactTraitTest::MarkGenRan(238143);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_sha_deep_waters::HandleAfterCast);
    }
};

// ===============================================================================================================
// Druid
// ===============================================================================================================

// 48438 - Wild Growth; Nature's Essence (189787): the targets are healed instantly as well (189800)
GEN_TRAIT_TARGET_ON_HIT(gen_arti_dru_natures_essence, 189787, 189800)

// 29166 - Innervate; Rapid Innervation (202890): the target also gains haste (202842)
GEN_TRAIT_TARGET_ON_HIT(gen_arti_dru_rapid_innervation, 202890, 202842)

// 5217 - Tiger's Fury; Ashamane's Energy (210579): additional energy over time (210583)
GEN_TRAIT_SELF_ON_CAST(gen_arti_dru_ashamanes_energy, 210579, 210583)

// 5217 - Tiger's Fury; Fury of Ashamane (238084): versatility for the druid and nearby allies (240670)
GEN_TRAIT_SELF_ON_CAST_TRIGGER(gen_arti_dru_fury_of_ashamane, 238084)

// 106951 - Berserk, 102543 - Incarnation: King of the Jungle; Feral Instinct (210631): more damage done (210649)
GEN_TRAIT_SELF_ON_CAST(gen_arti_dru_feral_instinct, 210631, 210649)

// 106830 - Thrash; Pawsitive Outlook (238121): chance for a second Thrash
GEN_TRAIT_REPEAT_CHANCE(gen_arti_dru_pawsitive_outlook, 238121, 106830)

// ===============================================================================================================
// Warrior
// ===============================================================================================================

// 5308, 163201 - Execute; Juggernaut (200875): stacking damage bonus for Execute (201009)
GEN_TRAIT_SELF_ON_CAST_TRIGGER(gen_arti_war_juggernaut, 200875)

// 6544 - Heroic Leap; Tactical Advance (209483): armor and parry after the leap (209484)
GEN_TRAIT_SELF_ON_CAST(gen_arti_war_tactical_advance, 209483, 209484)

// 23881 - Bloodthirst; Oathblood (238112): chance for a second Bloodthirst
GEN_TRAIT_REPEAT_CHANCE(gen_arti_war_oathblood, 238112, 23881)

// 845 - Cleave; One Against Many (209462): one stack of 188923 per target hit, empowers the next Whirlwind
GEN_TRAIT_STACK_PER_TARGET(gen_arti_war_one_against_many, 209462, 188923)

// 845 - Cleave; Void Cleave (209573): burst of void energy when at least <trait value> targets were struck
class gen_arti_war_void_cleave : public SpellScript
{
    PrepareSpellScript(gen_arti_war_void_cleave);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 209573, 209700 });
    }

    void CountHit()
    {
        ++_hits;
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 needed = TraitValue(caster, 209573);
        if (!caster || needed <= 0 || int32(_hits) < needed)
            return;

        caster->CastSpell(caster, 209700u, true);
        ArtifactTraitTest::MarkGenRan(209573);
    }

    void Register() override
    {
        OnHit += SpellHitFn(gen_arti_war_void_cleave::CountHit);
        AfterCast += SpellCastFn(gen_arti_war_void_cleave::HandleAfterCast);
    }

private:
    uint32 _hits = 0;
};

// ===============================================================================================================
// Mage
// ===============================================================================================================

// 2948 - Scorch; Scorched Earth (227481): stacking movement speed (227482)
GEN_TRAIT_SELF_ON_CAST(gen_arti_mage_scorched_earth, 227481, 227482)

// 194466 - Phoenix's Flames; Warmth of the Phoenix (238091): chance ($h) for critical strike rating (240671)
GEN_TRAIT_TRIGGER_CHANCE(gen_arti_mage_warmth_of_the_phoenix, 238091, true, false)

// ===============================================================================================================
// Monk
// ===============================================================================================================

// 119996 - Transcendence: Transfer; Healing Winds (195380): heal over time (195381)
GEN_TRAIT_SELF_ON_CAST(gen_arti_monk_healing_winds, 195380, 195381)

// 100784 - Blackout Kick, 107428 - Rising Sun Kick; Transfer the Power (195300): stacking buff (195321)
GEN_TRAIT_SELF_ON_CAST_TRIGGER(gen_arti_monk_transfer_the_power, 195300)

// 121253 - Keg Smash; Stave Off (238093): chance for a second Keg Smash
GEN_TRAIT_REPEAT_CHANCE(gen_arti_monk_stave_off, 238093, 121253)

// ===============================================================================================================
// Rogue
// ===============================================================================================================

// 185311 - Crimson Vial; Dense Concoction (238102): growing damage reduction (240523 -> 240525)
GEN_TRAIT_SELF_ON_CAST_TRIGGER(gen_arti_rog_dense_concoction, 238102)

// 32645 - Envenom, 196819 - Eviscerate, 1943 - Rupture, 195452 - Nightblade, 2098 - Run Through;
// Surge of Toxins (192424): the target takes more poison damage (192425)
GEN_TRAIT_TARGET_ON_HIT(gen_arti_rog_surge_of_toxins, 192424, 192425)

// 36554 - Shadowstep; Shadow Swiftness (192422): Evasion for the number of seconds in the trait
class gen_arti_rog_shadow_swiftness : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_shadow_swiftness);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 192422, 5277 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 seconds = TraitValue(caster, 192422);
        if (!caster || seconds <= 0)
            return;

        caster->CastSpell(caster, 5277u, true);
        // Evasion normally lasts longer, the trait only grants it for a few seconds
        if (Aura* evasion = caster->GetAura(5277, caster->GetGUID()))
            evasion->SetDuration(seconds * IN_MILLISECONDS);

        ArtifactTraitTest::MarkGenRan(192422);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_rog_shadow_swiftness::HandleAfterCast);
    }
};

// 196911 - Shadow Techniques; Shadow's Whisper (242707): the proc also grants energy
class gen_arti_rog_shadows_whisper : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_shadows_whisper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 242707 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 amount = TraitValue(caster, 242707);
        if (!caster || amount <= 0)
            return;

        caster->EnergizeBySpell(caster, 242707, amount, POWER_ENERGY);
        ArtifactTraitTest::MarkGenRan(242707);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_rog_shadows_whisper::HandleAfterCast);
    }
};

// 196911 - Shadow Techniques; Fortune's Bite (197369): chance for two combo points instead of one
class gen_arti_rog_fortunes_bite : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_fortunes_bite);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197369 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 chance = TraitValue(caster, 197369);
        if (!caster || chance <= 0 || !roll_chance_i(chance))
            return;

        // Shadow Techniques itself awards one combo point, the trait adds the second one
        caster->EnergizeBySpell(caster, 197369, 1, POWER_COMBO_POINTS);
        ArtifactTraitTest::MarkGenRan(197369);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_rog_fortunes_bite::HandleAfterCast);
    }
};

// ===============================================================================================================
// Hunter
// ===============================================================================================================

// 781 - Disengage; Survival of the Fittest (190514): damage reduction after Disengage (190515)
GEN_TRAIT_SELF_ON_CAST(gen_arti_hun_survival_of_the_fittest, 190514, 190515)

// 186270 - Raptor Strike; Bird of Prey (224764): heals the hunter for a part of the damage
GEN_TRAIT_LEECH(gen_arti_hun_bird_of_prey, 224764)

// 118459 - Beast Cleave; Furious Swipes (197047): more Beast Cleave damage
GEN_TRAIT_DAMAGE_PCT(gen_arti_hun_furious_swipes, 197047, EFFECT_0)

// 204147 - Windburst; Mark of the Windrunner (204219): Windburst also applies Vulnerable (187131)
GEN_TRAIT_TARGET_ON_HIT(gen_arti_hun_mark_of_the_windrunner, 204219, 187131)

// 34477 - Misdirection; Hunter's Advantage (197178): the pet takes less damage (211138)
class gen_arti_hun_hunters_advantage : public SpellScript
{
    PrepareSpellScript(gen_arti_hun_hunters_advantage);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197178, 211138 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(197178))
            return;

        // the trait only works when Misdirection was used on the hunter's own pet
        Player* hunter = caster->ToPlayer();
        if (!hunter || hunter->GetPet() != target)
            return;

        caster->CastSpell(target, 211138u, true);
        ArtifactTraitTest::MarkGenRan(197178);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_hun_hunters_advantage::HandleAfterHit);
    }
};

// 83381 - Kill Command (pet damage); Jaws of Thunder (197162): "Kill Command has a $s1% chance to deal an additional
// $197163s2% of its damage as Nature damage." (197162 E0 = 10, 197163 E1 = 50, 197163 E0 = SCHOOL_DAMAGE)
// Round 19 (25.09.2026): moved from 34026 to 83381. 34026 has only DUMMY / SCRIPT_EFFECT(83381) / DUMMY in the client
// and deals no damage itself, so GetHitDamage() there was always 0 and the body never ran (audit report 19.3).
// The damage is dealt by the pet's 83381 (spell_hun_kill_command: pet->CastSpell(target, 83381)), the trait sits on
// the hunter = owner of the caster - same pattern as gen_arti_hun_spirit_bond on 83381.
class gen_arti_hun_jaws_of_thunder : public SpellScript
{
    PrepareSpellScript(gen_arti_hun_jaws_of_thunder);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197162, 197163 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        // the pet casts 83381, the trait sits on the hunter
        Unit* owner = caster->GetOwner();
        if (!owner)
            owner = caster;

        int32 chance = TraitValue(owner, 197162);
        int32 damage = GetHitDamage();
        if (chance <= 0 || damage <= 0 || !roll_chance_i(chance))
            return;

        // the percentage of the damage is in the second effect of the helper spell ("$197163s2%")
        int32 pct = SpellEffectValue(197163, EFFECT_1);
        if (pct <= 0)
            return;

        int32 bonus = CalculatePct(damage, pct);
        if (bonus <= 0)
            return;

        owner->CastCustomSpell(197163u, SPELLVALUE_BASE_POINT0, bonus, target, true);
        ArtifactTraitTest::MarkGenRan(197162);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_hun_jaws_of_thunder::HandleAfterHit);
    }
};

// ===============================================================================================================
// Death Knight
// ===============================================================================================================

// 206930 - Heart Strike; Blood Feast (192548): heals the death knight for a part of the damage
GEN_TRAIT_LEECH(gen_arti_dk_blood_feast, 192548)

// 49576 - Death Grip; Gravitational Pull (191721): the target is also slowed (191719)
class gen_arti_dk_gravitational_pull : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_gravitational_pull);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 191721 }) && TraitTriggerSpell(191721) != 0;
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(191721))
            return;

        if (uint32 trigger = TraitTriggerSpell(191721))
        {
            caster->CastSpell(target, trigger, true);
            ArtifactTraitTest::MarkGenRan(191721);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dk_gravitational_pull::HandleAfterHit);
    }
};

// 49020 - Obliterate; Over-Powered (189097): chance ($h) for additional Runic Power (189096)
GEN_TRAIT_TRIGGER_CHANCE(gen_arti_dk_over_powered, 189097, true, false)

// 55090 - Scourge Strike, 207311 - Clawing Shadows; Scourge the Unbeliever (191494): chance for a Rune (191492)
GEN_TRAIT_TRIGGER_CHANCE(gen_arti_dk_scourge_the_unbeliever, 191494, false, false)

// ===============================================================================================================
// Demon Hunter
// ===============================================================================================================

// 162794 - Chaos Strike; Chaotic Onslaught (238117): chance to slash a second time
GEN_TRAIT_REPEAT_CHANCE(gen_arti_dh_chaotic_onslaught, 238117, 162794)

// ===============================================================================================================
// Round 2: traits that need the proc system or the critical strike chance hook
//
// Two hooks of this core version make a group of traits reachable that the first round had to skip:
//   * AuraScript::DoCheckProc / OnProc gives "eventInfo.GetHitMask() & PROC_HIT_CRITICAL" (Unit.h), so the
//     condition "critical strike" can be evaluated. The trait aura itself carries the script; it procs because
//     SpellMgr::LoadSpellProcs builds a default proc entry from the client ProcTypeMask of the trait
//     (SpellAuraOptions), or - where the client has no mask - from a row in `spell_proc` shipped next to the
//     spell_script_names rows in C:\LegionServer\trait_candidates.
//   * SpellScript::OnCalcCritChance (Unit::SpellCriticalDamageBonus path, Unit.cpp) lets a trait raise the
//     critical strike chance of one ability, optionally only against a target that carries a given aura.
// ===============================================================================================================

// Pyretic Incantation (194331): every consecutive critical strike adds a stack of 194329 (+2% crit damage,
// 5 stacks), a non critical strike ends the series. Client proc mask 0x11000 (done magic / none, negative).
class gen_arti_mage_pyretic_incantation : public AuraScript
{
    PrepareAuraScript(gen_arti_mage_pyretic_incantation);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194331, 194329 });
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        if (!caster)
            return;

        if (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
            caster->CastSpell(caster, 194329u, true);
        else
            caster->RemoveAurasDueToSpell(194329);

        ArtifactTraitTest::MarkGenRan(194331);
    }

    void Register() override
    {
        OnProc += AuraProcFn(gen_arti_mage_pyretic_incantation::HandleProc);
    }
};

// Pulse of Battle (238076): a critical Raging Blow generates ${$s1/10} extra rage. The core keeps rage in
// tenths of a point, so the value of the trait aura (10) is exactly the amount to add.
class gen_arti_war_pulse_of_battle : public AuraScript
{
    PrepareAuraScript(gen_arti_war_pulse_of_battle);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238076 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        // 85288 is the cast, 85384 / 96103 are the two weapon hits it triggers
        return spellInfo && (spellInfo->Id == 85288 || spellInfo->Id == 85384 || spellInfo->Id == 96103);
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        int32 amount = TraitValue(caster, 238076);
        if (!caster || amount <= 0)
            return;

        caster->EnergizeBySpell(caster, 238076, amount, POWER_RAGE);
        ArtifactTraitTest::MarkGenRan(238076);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_war_pulse_of_battle::CheckProc);
        OnProc += AuraProcFn(gen_arti_war_pulse_of_battle::HandleProc);
    }
};

// Unleash the Shadows (194093): a critical Vampiric Touch tick has a $s1% chance to send out a Shadowy
// Apparition (147193, a trigger missile that deals 148859). Client proc mask 0x40000 (done periodic).
class gen_arti_pri_unleash_the_shadows : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_unleash_the_shadows);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194093, 147193 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 34914; // Vampiric Touch
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        int32 chance = TraitValue(caster, 194093);
        if (!caster || !target || chance <= 0 || !roll_chance_i(chance))
            return;

        caster->CastSpell(target, 147193u, true);
        ArtifactTraitTest::MarkGenRan(194093);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pri_unleash_the_shadows::CheckProc);
        OnProc += AuraProcFn(gen_arti_pri_unleash_the_shadows::HandleProc);
    }
};

// Queen Ascendant (207285): a critical direct heal puts 207288 on the shaman, which shortens the cast time of
// the next heal by 5% (the client carries that value in 207288 itself). The trait has no proc mask in the
// client data, the matching `spell_proc` row ships in trait_candidates\gen_shaman.sql.
class gen_arti_sha_queen_ascendant : public AuraScript
{
    PrepareAuraScript(gen_arti_sha_queen_ascendant);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207285, 207288 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return (eventInfo.GetHitMask() & PROC_HIT_CRITICAL) && eventInfo.GetHealInfo() != nullptr;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        if (!caster)
            return;

        caster->CastSpell(caster, 207288u, true);
        ArtifactTraitTest::MarkGenRan(207285);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_sha_queen_ascendant::CheckProc);
        OnProc += AuraProcFn(gen_arti_sha_queen_ascendant::HandleProc);
    }
};

// Circadian Invocation (238119): Moonfire damage stacks 240606 (arcane damage taken) on the target, Sunfire
// damage stacks 240607 (nature damage taken). Client proc mask 0x50000 (done periodic / done magic negative).
class gen_arti_dru_circadian_invocation : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_circadian_invocation);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238119, 240606, 240607 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && (spellInfo->Id == 164812 || spellInfo->Id == 164815);
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!caster || !target || !spellInfo)
            return;

        caster->CastSpell(target, spellInfo->Id == 164812 ? 240606u : 240607u, true);
        ArtifactTraitTest::MarkGenRan(238119);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dru_circadian_invocation::CheckProc);
        OnProc += AuraProcFn(gen_arti_dru_circadian_invocation::HandleProc);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Pattern 9: the trait raises the critical strike chance of one ability, only while the victim carries a
// given aura (the aura id comes from the description of the trait).
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_CRIT_CHANCE_VS_AURA(scriptName, traitId, victimAuraId)                        \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId, victimAuraId });                                    \
    }                                                                                           \
                                                                                                \
    void CalcCritChance(Unit* victim, float& chance)                                            \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !victim)                                                                 \
            return;                                                                             \
                                                                                                \
        int32 bonus = TraitValue(caster, traitId);                                              \
        if (bonus <= 0 || !victim->HasAura(uint32(victimAuraId), caster->GetGUID()))            \
            return;                                                                             \
                                                                                                \
        chance += float(bonus);                                                                 \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        OnCalcCritChance += SpellOnCalcCritChanceFn(scriptName::CalcCritChance);                \
    }                                                                                           \
};

// 19434 - Aimed Shot; Marked for Death (190529): more critical strike chance against a Vulnerable (187131) target
GEN_TRAIT_CRIT_CHANCE_VS_AURA(gen_arti_hun_marked_for_death, 190529, 187131)

// 12294 - Mortal Strike, 163201/5308 - Execute; Precise Strikes (248579).
// Round LCF2 R23: the earlier assumption ("no separate buff spell in the client") was wrong. LegionCore-7.3.5
// spell_trigger 248579 -> 248195 (proc on Colossus Smash 167105 / Warbreaker 209577, spell_proc_event family masks)
// shows the client buff 248195 "Precise Strikes" (15 s, ADD_PCT/FLAT_MODIFIER SPELLMOD_CRITICAL_CHANCE on the class
// masks of Mortal Strike + Execute, $s1 = trait value). The crit itself is therefore done by the client spell
// modifier; "next" = one use (LegionCore SpellMgr.cpp sets ProcCharges = 1 for 248195). This script only consumes
// the buff after a Mortal Strike / Execute. The buff is given by spell_r23_war_precise_strikes_apply
// (spell_lcf2r23_2026_09_25.cpp) on Colossus Smash / Warbreaker.
class gen_arti_war_precise_strikes : public SpellScript
{
    PrepareSpellScript(gen_arti_war_precise_strikes);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 248579, 248195 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(248195))
            return;

        caster->RemoveAurasDueToSpell(248195);
        ArtifactTraitTest::MarkGenRan(248579);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_war_precise_strikes::HandleAfterCast);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Further traits of round 2 that need no proc entry
// ---------------------------------------------------------------------------------------------------------------

// 106898 - Stampeding Roar; Roar of the Crowd (214996): one stack of 213698 per ally hit
GEN_TRAIT_STACK_PER_TARGET(gen_arti_dru_roar_of_the_crowd, 214996, 213698)

// 106830 - Thrash; Scent of Blood (210663): one stack of 210664 (lower Swipe cost) per target hit
GEN_TRAIT_STACK_PER_TARGET(gen_arti_dru_scent_of_blood, 210663, 210664)

// 79140 - Vendetta; From the Shadows (192428): the target also gets the poison dagger barrage (192432)
GEN_TRAIT_TARGET_ON_HIT(gen_arti_rog_from_the_shadows, 192428, 192432)

// 1079 - Rip; Open Wounds (210666): while Rip runs, the target carries 210670 (armor ignored)
class gen_arti_dru_open_wounds : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_open_wounds);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 210666, 210670 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !caster->HasAura(210666))
            return;

        caster->CastSpell(target, 210670u, true);
        ArtifactTraitTest::MarkGenRan(210666);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (Unit* target = GetTarget())
            target->RemoveAurasDueToSpell(210670, caster ? caster->GetGUID() : ObjectGuid::Empty);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_dru_open_wounds::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_dru_open_wounds::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 116858 - Chaos Bolt; Cry Havoc (238110): a target that carries Havoc (80240) explodes for 243011
class gen_arti_lock_cry_havoc : public SpellScript
{
    PrepareSpellScript(gen_arti_lock_cry_havoc);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238110, 243011, 80240 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(238110))
            return;

        if (!target->HasAura(80240))
            return;

        caster->CastSpell(target, 243011u, true);
        ArtifactTraitTest::MarkGenRan(238110);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_lock_cry_havoc::HandleAfterHit);
    }
};

// 23881 - Bloodthirst; Bloodcraze (200859): below 20% health (the threshold is part of the client description)
// Bloodthirst restores an extra $s1% of the warrior's health
class gen_arti_war_bloodcraze : public SpellScript
{
    PrepareSpellScript(gen_arti_war_bloodcraze);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 200859 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 pct = TraitValue(caster, 200859);
        if (!caster || pct <= 0)
            return;

        if (caster->GetHealthPct() >= 20.0f)
            return;

        uint32 heal = uint32(CalculatePct(caster->GetMaxHealth(), pct));
        if (!heal)
            return;

        HealInfo healInfo(caster, caster, heal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        caster->HealBySpell(healInfo);
        ArtifactTraitTest::MarkGenRan(200859);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_war_bloodcraze::HandleAfterCast);
    }
};

// ===============================================================================================================
// Round 6: traits that were skipped in round 1 for "missing numbers" or "no matching hook", but whose numbers and
// helper spell ids are in fact fully contained in the client data once the referenced spells are looked up too.
//
// Three observations made this group reachable:
//   * A trait whose aura effect is a plain dummy (aura 4) but which carries both an EffectTriggerSpell and a
//     ProcTypeMask in SpellAuraOptions is completely described by the client: the condition is the proc mask
//     (SpellMgr::LoadSpellProcs builds the default entry from it, including ProcChance and ProcCategoryRecovery),
//     the effect is the trigger spell. GEN_TRAIT_PROC_TRIGGER below is that pattern.
//   * Several descriptions refer to a second spell ("$227679s1%", "$198240t", "$213672s1"). That spell carries
//     the missing number, so nothing has to be guessed - it only has to be read from the referenced spell.
//   * Periodic dummy auras (aura 226) are the client's way of saying "a script ticks here". Dispersion (47585),
//     Spirit of the Maelstrom (198240) and Ghost in the Mist (207524) all carry one, including the period.
// ===============================================================================================================

// ---------------------------------------------------------------------------------------------------------------
// Pattern 10: dummy trait aura with a client proc mask and an EffectTriggerSpell - proc casts the trigger spell.
// The condition comes entirely from the client proc mask, the chance from SpellAuraOptions.ProcChance.
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_PROC_TRIGGER(scriptName, traitId, onTarget)                                   \
class scriptName : public AuraScript                                                            \
{                                                                                               \
    PrepareAuraScript(scriptName);                                                              \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId }) && TraitTriggerSpell(traitId) != 0;               \
    }                                                                                           \
                                                                                                \
    void HandleProc(ProcEventInfo& eventInfo)                                                   \
    {                                                                                           \
        Unit* caster = eventInfo.GetActor();                                                    \
        uint32 trigger = TraitTriggerSpell(traitId);                                            \
        if (!caster || !trigger)                                                                \
            return;                                                                             \
                                                                                                \
        Unit* target = caster;                                                                  \
        if (onTarget)                                                                           \
        {                                                                                       \
            target = eventInfo.GetProcTarget();                                                 \
            if (!target)                                                                        \
                return;                                                                         \
        }                                                                                       \
                                                                                                \
        caster->CastSpell(target, trigger, true);                                               \
        ArtifactTraitTest::MarkGenRan(traitId);                                                 \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        OnProc += AuraProcFn(scriptName::HandleProc);                                           \
    }                                                                                           \
};

// ===============================================================================================================
// Hunter (round 6)
// ===============================================================================================================

// Spirit Bond (197199): "Kill Command heals you for $s1% of the damage it deals."
// Kill Command (34026) does not deal the damage itself - its effect 1 is a script effect that names 83381, and
// that spell is cast by the pet. The script therefore hangs on 83381 and heals the owner of the caster.
class gen_arti_hun_spirit_bond : public SpellScript
{
    PrepareSpellScript(gen_arti_hun_spirit_bond);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197199 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        // the pet casts 83381, the trait sits on the hunter
        Unit* owner = caster->GetOwner();
        if (!owner)
            owner = caster;

        int32 pct = TraitValue(owner, 197199);
        int32 damage = GetHitDamage();
        if (pct <= 0 || damage <= 0)
            return;

        uint32 heal = uint32(CalculatePct(damage, pct));
        if (!heal)
            return;

        HealInfo healInfo(owner, owner, heal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        owner->HealBySpell(healInfo);
        ArtifactTraitTest::MarkGenRan(197199);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_hun_spirit_bond::HandleAfterHit);
    }
};

// Talon Strike (203563): "Your basic attacks have a chance to trigger two rapid additional blows."
// Client proc mask 20 (melee auto attack / melee ability), ProcChance 100. The two blows are inside the trigger
// spell 203560 itself - both of its effects trigger 203525.
GEN_TRAIT_PROC_TRIGGER(gen_arti_hun_talon_strike, 203563, false)

// Hunter's Bounty (203749): "Reduces the remaining cooldown on Exhilaration by $m1 sec each time you kill an
// enemy." The client proc mask of the trait is 4118, which contains PROC_FLAG_KILL (0x2), so the default proc
// entry already fires on a kill. The trigger spell of the trait (203807 Talonclaw Marker) is only a marker
// debuff, so the cooldown is shortened directly - by the number of seconds in the trait aura.
class gen_arti_hun_hunters_bounty : public AuraScript
{
    PrepareAuraScript(gen_arti_hun_hunters_bounty);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203749, 109304 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return (eventInfo.GetTypeMask() & PROC_FLAG_KILL) != 0;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Player* hunter = eventInfo.GetActor() ? eventInfo.GetActor()->ToPlayer() : nullptr;
        if (!hunter)
            return;

        int32 seconds = TraitValue(hunter, 203749);
        if (seconds <= 0)
            return;

        hunter->GetSpellHistory()->ModifyCooldown(109304, -seconds * IN_MILLISECONDS);
        ArtifactTraitTest::MarkGenRan(203749);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_hun_hunters_bounty::CheckProc);
        OnProc += AuraProcFn(gen_arti_hun_hunters_bounty::HandleProc);
    }
};

// ===============================================================================================================
// Death Knight (round 6)
// ===============================================================================================================

// Hypothermia (189185): "Each time Frost Fever deals damage, it has a chance to erupt, dealing $228322s1 Frost
// damage to the target." Client proc mask 262144 (done periodic), ProcChance 10 - the chance is applied by the
// default proc entry, the script only restricts the source to Frost Fever (55095) and casts 228322.
class gen_arti_dk_hypothermia : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_hypothermia);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189185, 228322, 55095 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 55095;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        if (!caster || !target)
            return;

        caster->CastSpell(target, 228322u, true);
        ArtifactTraitTest::MarkGenRan(189185);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dk_hypothermia::CheckProc);
        OnProc += AuraProcFn(gen_arti_dk_hypothermia::HandleProc);
    }
};

// Thronebreaker (238115): "Obliterate has a $h% chance to cause the target to be pierced from behind by a
// Crystalline Sword." $h is the ProcChance of the trait in the client data, the sword is the EffectTriggerSpell
// (243122). The internal cooldown (ProcCategoryRecovery 200 ms) also comes from the client entry.
class gen_arti_dk_thronebreaker : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_thronebreaker);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238115 }) && TraitTriggerSpell(238115) != 0;
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 49020; // Obliterate
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        uint32 trigger = TraitTriggerSpell(238115);
        if (!caster || !target || !trigger)
            return;

        caster->CastSpell(target, trigger, true);
        ArtifactTraitTest::MarkGenRan(238115);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dk_thronebreaker::CheckProc);
        OnProc += AuraProcFn(gen_arti_dk_thronebreaker::HandleProc);
    }
};

// Vampiric Aura (238078): "Consumption grants $238698s1% Leech to you and $s2 allies." The ally selection and
// the radius are part of 238698 itself (implicit target 118, radius index 12), so the script only has to cast
// the trigger spell of the trait after Consumption (205223) - no radius has to be assumed.
class gen_arti_dk_vampiric_aura : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_vampiric_aura);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238078 }) && TraitTriggerSpell(238078) != 0;
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        uint32 trigger = TraitTriggerSpell(238078);
        if (!caster || !trigger || !caster->HasAura(238078))
            return;

        caster->CastSpell(caster, trigger, true);
        ArtifactTraitTest::MarkGenRan(238078);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_dk_vampiric_aura::HandleAfterCast);
    }
};

// ===============================================================================================================
// Rogue (round 6)
// ===============================================================================================================

// Embrace of Darkness (197604): "When you enter Stealth or Shadow Dance, you gain an absorb shield for $AP
// damage." Client proc mask 2114560 with a 100 ms category recovery; the shield is the EffectTriggerSpell
// (197603, a school absorb that scales with attack power).
GEN_TRAIT_PROC_TRIGGER(gen_arti_rog_embrace_of_darkness, 197604, false)

// ===============================================================================================================
// Monk (round 6)
// ===============================================================================================================

// Mists of Life (199563): "Life Cocoon applies Renewing Mist and Enveloping Mist to the target."
// Life Cocoon 116849, Renewing Mist 115151, Enveloping Mist 124682 - no numbers are involved.
class gen_arti_monk_mists_of_life : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_mists_of_life);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 199563, 115151, 124682 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(199563))
            return;

        caster->CastSpell(target, 115151u, true);
        caster->CastSpell(target, 124682u, true);
        ArtifactTraitTest::MarkGenRan(199563);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_monk_mists_of_life::HandleAfterHit);
    }
};

// Light on Your Feet (199401): "Channeling Essence Font for its full duration increases your movement speed by
// $s1% for $199407d." The channel is the periodic trigger aura of Essence Font (191837); "for its full duration"
// is exactly AURA_REMOVE_BY_EXPIRE, every interruption removes the aura with a different mode.
class gen_arti_monk_light_on_your_feet : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_light_on_your_feet);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 199401, 199407 });
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(199401))
            return;

        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        caster->CastSpell(caster, 199407u, true);
        ArtifactTraitTest::MarkGenRan(199401);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_monk_light_on_your_feet::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Face Palm (213116): "Tiger Palm has a $s1% chance to deal $227679s1% of normal damage and reduce the remaining
// cooldown of your Brews by $227679s2 additional sec." The chance is the trait value, both other numbers are in
// 227679 (300% damage, 1 extra second). The brews are Ironskin Brew (115308) and Purifying Brew (119582), both
// named in the description of the trait.
class gen_arti_monk_face_palm : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_face_palm);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 213116, 227679 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 chance = TraitValue(caster, 213116);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        int32 pct = SpellEffectValue(227679, EFFECT_0);
        if (pct > 0)
            SetHitDamage(CalculatePct(GetHitDamage(), pct));

        if (Player* monk = caster->ToPlayer())
            if (int32 seconds = SpellEffectValue(227679, EFFECT_1))
            {
                monk->GetSpellHistory()->ModifyCooldown(115308, -seconds * IN_MILLISECONDS);
                monk->GetSpellHistory()->ModifyCooldown(119582, -seconds * IN_MILLISECONDS);
            }

        ArtifactTraitTest::MarkGenRan(213116);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_monk_face_palm::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Quick Sip (238129): "Drinking Purifying Brew grants Ironskin Brew for ${$s1/10} sec." The trait value is in
// tenths of a second (10 = 1 s). Ironskin Brew (115308) puts its buff 215479 on the monk; an already running
// buff is extended instead of replaced.
class gen_arti_monk_quick_sip : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_quick_sip);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238129, 115308, 215479 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 tenths = TraitValue(caster, 238129);
        if (tenths <= 0)
            return;

        int32 duration = tenths * IN_MILLISECONDS / 10;
        if (Aura* ironskin = caster->GetAura(215479, caster->GetGUID()))
            ironskin->ModDuration(duration);
        else
        {
            caster->CastSpell(caster, 115308u, true);
            if (Aura* granted = caster->GetAura(215479, caster->GetGUID()))
                granted->SetDuration(duration);
        }

        ArtifactTraitTest::MarkGenRan(238129);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_monk_quick_sip::HandleAfterCast);
    }
};

// ===============================================================================================================
// Druid (round 6)
// ===============================================================================================================

// Deep Rooted (238122): "When your Rejuvenation, Regrowth, or Wild Growth heals a target below $s1% health, its
// duration is refreshed." The script hangs on the three heal over time auras themselves (774, 8936, 48438);
// EFFECT_FIRST_FOUND picks the periodic heal effect of each of them.
class gen_arti_dru_deep_rooted : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_deep_rooted);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238122 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target)
            return;

        int32 pct = TraitValue(caster, 238122);
        if (pct <= 0 || target->GetHealthPct() >= float(pct))
            return;

        if (Aura* aura = GetAura())
            aura->RefreshDuration();

        ArtifactTraitTest::MarkGenRan(238122);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_dru_deep_rooted::HandlePeriodic, EFFECT_FIRST_FOUND, SPELL_AURA_PERIODIC_HEAL);
    }
};

// Dreamwalker (189849): "Wild Growth has a $h% chance to ... instantly heal all allies affected by your
// Rejuvenation for $189853s1." The chance ($h = ProcChance 50) and the trigger of 189854 are already handled by
// the client data (effect 0 of the trait is a real proc trigger aura). What is missing is only the body of
// 189854: it is a dummy effect on an area of allies (implicit target 18, radius index 12), so the script runs on
// every ally the core selected and heals the ones that carry this druid's Rejuvenation (774) with 189853.
class gen_arti_dru_dreamwalker : public SpellScript
{
    PrepareSpellScript(gen_arti_dru_dreamwalker);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189849, 189853, 774 });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(189849))
            return;

        if (!target->HasAura(774, caster->GetGUID()))
            return;

        caster->CastSpell(target, 189853u, true);
        ArtifactTraitTest::MarkGenRan(189849);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_dru_dreamwalker::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// ===============================================================================================================
// Priest (round 6)
// ===============================================================================================================

// Thrive in the Shadows (194024): "Dispersion heals you for $s1% of maximum health over its duration."
// Dispersion (47585) carries a periodic dummy on effect 4 (one tick per second), so the total amount is split
// over the ticks the aura actually has - the number of ticks is read from the aura, nothing is assumed.
class gen_arti_pri_thrive_in_the_shadows : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_thrive_in_the_shadows);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194024 });
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* caster = GetTarget();
        if (!caster)
            return;

        int32 pct = TraitValue(caster, 194024);
        int32 ticks = aurEff->GetTotalTicks();
        if (pct <= 0 || ticks <= 0)
            return;

        uint32 heal = uint32(CalculatePct(caster->GetMaxHealth(), pct) / ticks);
        if (!heal)
            return;

        HealInfo healInfo(caster, caster, heal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        caster->HealBySpell(healInfo);
        ArtifactTraitTest::MarkGenRan(194024);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_pri_thrive_in_the_shadows::HandlePeriodic, EFFECT_4, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Taming the Shadows (197779): "Shadow Mend has a $s1% chance to deal no damage." The damage of Shadow Mend
// (186263) is the aura 187464 that spell_pri_shadow_mend puts on the healed target; this script runs after it
// and takes the aura away again on a successful roll.
class gen_arti_pri_taming_the_shadows : public SpellScript
{
    PrepareSpellScript(gen_arti_pri_taming_the_shadows);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197779, 187464 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        int32 chance = TraitValue(caster, 197779);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        target->RemoveAurasDueToSpell(187464, caster->GetGUID());
        ArtifactTraitTest::MarkGenRan(197779);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_pri_taming_the_shadows::HandleAfterHit);
    }
};

// ===============================================================================================================
// Shaman (round 6)
// ===============================================================================================================

// Ghost Wolf (2645) carries the two Ghost Wolf traits:
//   Spirit of the Maelstrom (198238): "While in combat in Ghost Wolf form, you generate $s1 Maelstrom every
//     $198240t sec" - 198240 is a periodic dummy with the period from the client data.
//   Ghost in the Mist (207351): "Reduces all damage you take while Ghost Wolf is active by $m1%, increasing by
//     another $m1% every $207524t1 sec, stacking up to $207527u times" - 207524 is a periodic dummy whose
//     EffectTriggerSpell is the stacking damage reduction 207527 (cumulative aura 6 in the client data).
class gen_arti_sha_ghost_wolf_traits : public AuraScript
{
    PrepareAuraScript(gen_arti_sha_ghost_wolf_traits);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198238, 198240, 207351 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetTarget();
        if (!caster)
            return;

        if (caster->HasAura(198238))
        {
            caster->CastSpell(caster, 198240u, true);
            ArtifactTraitTest::MarkGenRan(198238);
        }

        if (caster->HasAura(207351))
            if (uint32 trigger = TraitTriggerSpell(207351))
            {
                caster->CastSpell(caster, trigger, true);
                ArtifactTraitTest::MarkGenRan(207351);
            }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetTarget();
        if (!caster)
            return;

        caster->RemoveAurasDueToSpell(198240);
        if (uint32 trigger = TraitTriggerSpell(207351))
        {
            caster->RemoveAurasDueToSpell(trigger);
            if (uint32 stack = TraitTriggerSpell(trigger))
                caster->RemoveAurasDueToSpell(stack);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_sha_ghost_wolf_traits::HandleApply, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_sha_ghost_wolf_traits::HandleRemove, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
    }
};

// 198240 - Spirit of the Maelstrom: one tick generates the Maelstrom of the trait, but only in combat
class gen_arti_sha_spirit_of_the_maelstrom : public AuraScript
{
    PrepareAuraScript(gen_arti_sha_spirit_of_the_maelstrom);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198238 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        if (!caster || !caster->IsInCombat())
            return;

        int32 amount = TraitValue(caster, 198238);
        if (amount <= 0)
            return;

        caster->EnergizeBySpell(caster, 198238, amount, POWER_MAELSTROM);
        ArtifactTraitTest::MarkGenRan(198238);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_sha_spirit_of_the_maelstrom::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 207524 - Ghost in the Mist: one tick adds a stack of the damage reduction named in its own client data
class gen_arti_sha_ghost_in_the_mist : public AuraScript
{
    PrepareAuraScript(gen_arti_sha_ghost_in_the_mist);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207351 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        uint32 trigger = TraitTriggerSpell(GetId());
        if (!caster || !trigger)
            return;

        caster->CastSpell(caster, trigger, true);
        ArtifactTraitTest::MarkGenRan(207351);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_sha_ghost_in_the_mist::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// ===============================================================================================================
// Mage (round 6)
// ===============================================================================================================

// true while this script casts the additional pillars, so they do not call down pillars of their own
static thread_local bool AftershocksRepeating = false;

// Aftershocks (194431): "Flamestrike calls down $m1 additional pillar of fire each time it is cast."
// The number of additional pillars is the value of the trait aura; each pillar is another cast of Flamestrike
// (2120) at the same ground position.
class gen_arti_mage_aftershocks : public SpellScript
{
    PrepareSpellScript(gen_arti_mage_aftershocks);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194431, 2120 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || AftershocksRepeating || !caster->HasAura(194431))
            return;

        int32 extra = TraitValue(caster, 194431);
        if (extra <= 0)
            return;

        WorldLocation const* dest = GetExplTargetDest();
        if (!dest)
            return;

        Position pos = dest->GetPosition();
        AftershocksRepeating = true;
        for (int32 i = 0; i < extra; ++i)
            caster->CastSpell(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), 2120u, true);
        AftershocksRepeating = false;

        ArtifactTraitTest::MarkGenRan(194431);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_mage_aftershocks::HandleAfterCast);
    }
};

// ===============================================================================================================
// Round 7: the groups that had never been checked against the CSV data at all (companion/summon, "already
// covered by client aura types", area/summon effects) plus a second pass over the remaining ones with the
// patterns found in round 6.
//
// Two more things turned out to be available after all:
//   * The number of targets an area ability struck can be read with `OnObjectAreaTargetSelect` - the core hands
//     the finished target list to the script before the damage is applied. `spell_arti_dh_balanced_blades`
//     already does exactly this, so "counting targets would be core work" was wrong.
//   * Several traits only gate values that the referenced spell already carries natively (Rage of the Sleeper,
//     Might of the Vrykul); there the script only has to put the ready-made spell on the player.
// ===============================================================================================================

// ===============================================================================================================
// Hunter (round 7)
// ===============================================================================================================

// Hellcarver (203673): "$?s212436[Butchery][Carve] deals $m1% increased damage for each additional target hit."
// Carve (187708) and Butchery (212436) carry their main damage on different effect indices and use different
// target types, so each gets its own script; the bonus per additional target is the value of the trait aura.
class gen_arti_hun_hellcarver_carve : public SpellScript
{
    PrepareSpellScript(gen_arti_hun_hellcarver_carve);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203673 });
    }

    void CountTargets(std::list<WorldObject*>& targets)
    {
        _targets = uint32(targets.size());
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        int32 pct = TraitValue(caster, 203673);
        if (pct <= 0 || _targets < 2)
            return;

        int32 damage = GetHitDamage();
        AddPct(damage, pct * int32(_targets - 1));
        SetHitDamage(damage);
        ArtifactTraitTest::MarkGenRan(203673);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(gen_arti_hun_hellcarver_carve::CountTargets, EFFECT_0, TARGET_UNIT_CONE_ENEMY_104);
        OnEffectHitTarget += SpellEffectFn(gen_arti_hun_hellcarver_carve::HandleHit, EFFECT_0, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }

private:
    uint32 _targets = 0;
};

class gen_arti_hun_hellcarver_butchery : public SpellScript
{
    PrepareSpellScript(gen_arti_hun_hellcarver_butchery);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203673 });
    }

    void CountTargets(std::list<WorldObject*>& targets)
    {
        _targets = uint32(targets.size());
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        int32 pct = TraitValue(caster, 203673);
        if (pct <= 0 || _targets < 2)
            return;

        int32 damage = GetHitDamage();
        AddPct(damage, pct * int32(_targets - 1));
        SetHitDamage(damage);
        ArtifactTraitTest::MarkGenRan(203673);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(gen_arti_hun_hellcarver_butchery::CountTargets, EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
        OnEffectHitTarget += SpellEffectFn(gen_arti_hun_hellcarver_butchery::HandleHit, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }

private:
    uint32 _targets = 0;
};

// ===============================================================================================================
// Priest (round 7)
// ===============================================================================================================

// Light's Wrath (207946): "deals $s1 Radiant damage to the target, increased by $s2% per ally affected by your
// Atonement." $s2 is effect 1 of the ability itself (a dummy, base points 10); the allies are counted over the
// priest's group, so no radius has to be assumed - Atonement (194384) sits on the ally, cast by this priest.
class gen_arti_pri_lights_wrath : public SpellScript
{
    PrepareSpellScript(gen_arti_pri_lights_wrath);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207946, 194384 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 pct = SpellEffectValue(207946, EFFECT_1);
        if (pct <= 0)
            return;

        uint32 atonements = 0;
        if (Player* priest = caster->ToPlayer())
        {
            if (Group* group = priest->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    if (Player* member = itr->GetSource())
                        if (member->IsInWorld() && member->IsAlive() && member->HasAura(194384, caster->GetGUID()))
                            ++atonements;
            }
            else if (priest->HasAura(194384, caster->GetGUID()))
                ++atonements;
        }

        if (!atonements)
            return;

        int32 damage = GetHitDamage();
        AddPct(damage, pct * int32(atonements));
        SetHitDamage(damage);
        ArtifactTraitTest::MarkGenRan(207946);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_pri_lights_wrath::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Aegis of Wrath (238135): "Power Word: Shield absorbs $s1% additional damage, but the absorb amount decays by
// $s2% every $17t2 sec." Same shape as Shield of Faith - the extra absorb is the value of the trait aura.
// Absorb part: trait E0 %. Decay part (round LCF2 R24): see CalculateDecay below - the earlier worry that the decay
// would count $s2 twice was wrong, the flat modifier only fills PW:S E1, which nothing else uses.
class gen_arti_pri_aegis_of_wrath : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_aegis_of_wrath);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238135 });
    }

    void CalculateAmount(AuraEffect const* aurEff, int32& amount, bool& /*canBeRecalculated*/)
    {
        int32 pct = TraitValue(aurEff->GetCaster(), 238135);
        if (pct <= 0)
            return;

        AddPct(amount, pct);
        ArtifactTraitTest::MarkGenRan(238135);
    }

    // Round LCF2 R24 - the decay part, no longer "uncertain": the trait's E1 is a flat modifier (SpellModOp 12 = second
    // effect) on Power Word: Shield, i.e. it fills PW:S E1 (PERIODIC_DUMMY, 1 s, base 0) with $s2 = 3. That E1 amount IS
    // the decay rate - it is not applied anywhere else, so nothing is counted twice. LegionCore spell_pri_power_word_shield
    // (CalculateAmount1/OnTick): step = E1 % of the shield's starting absorb, subtracted every tick, shield removed when
    // used up. Without the trait E1 stays 0 and nothing happens.
    void CalculateDecay(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        _decayStep = 0;
        if (amount <= 0)
            return;

        if (AuraEffect const* absorb = GetAura()->GetEffect(EFFECT_0))
            _decayStep = CalculatePct(absorb->GetAmount(), amount);
    }

    void HandleDecayTick(AuraEffect const* /*aurEff*/)
    {
        if (_decayStep <= 0)
            return;

        AuraEffect* absorb = GetAura()->GetEffect(EFFECT_0);
        if (!absorb)
            return;

        if (absorb->GetAmount() <= _decayStep)
        {
            Remove(AURA_REMOVE_BY_ENEMY_SPELL);
            return;
        }

        absorb->ChangeAmount(absorb->GetAmount() - _decayStep);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_pri_aegis_of_wrath::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_pri_aegis_of_wrath::CalculateDecay, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_pri_aegis_of_wrath::HandleDecayTick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }

    int32 _decayStep = 0;
};

// ===============================================================================================================
// Monk (round 7)
// ===============================================================================================================

// Dragonfire Brew (213183): "After using Breath of Fire, you breathe fire $s1 additional times, each dealing
// $227681s1 Fire damage." The number of extra breaths is the value of the trait aura, 227681 is a real cone
// damage spell of its own - no number has to be invented.
class gen_arti_monk_dragonfire_brew : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_dragonfire_brew);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 213183, 227681 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 extra = TraitValue(caster, 213183);
        if (extra <= 0)
            return;

        for (int32 i = 0; i < extra; ++i)
            caster->CastSpell(caster, 227681u, true);

        ArtifactTraitTest::MarkGenRan(213183);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_monk_dragonfire_brew::HandleAfterCast);
    }
};

// ===============================================================================================================
// Demon Hunter (round 7)
// ===============================================================================================================

// Painbringer (207387): "Each Soul Fragment you consume reduces all damage you take by $212988s1% for
// $212988d." Consuming a fragment is the spell 203794 (Lesser Soul Fragment) or 210042 (Soul Fragment); both
// are named "Consume Soul" in the client data. 212988 carries the percentage and the stack limit itself
// (CumulativeAura 5).
class gen_arti_dh_painbringer : public SpellScript
{
    PrepareSpellScript(gen_arti_dh_painbringer);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 207387, 212988 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(207387))
            return;

        caster->CastSpell(caster, 212988u, true);
        ArtifactTraitTest::MarkGenRan(207387);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_dh_painbringer::HandleAfterCast);
    }
};

// Feast on the Souls (201468): "When you consume a Soul Fragment, the remaining cooldown on Eye Beam and Chaos
// Nova is reduced by ${$m1/1000} sec." The trait value is in milliseconds (5000 = 5 s); Eye Beam is 198013 and
// Chaos Nova is 179057, both confirmed by their own client descriptions.
class gen_arti_dh_feast_on_the_souls : public SpellScript
{
    PrepareSpellScript(gen_arti_dh_feast_on_the_souls);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 201468, 198013, 179057 });
    }

    void HandleAfterCast()
    {
        Player* hunter = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!hunter)
            return;

        int32 ms = TraitValue(hunter, 201468);
        if (ms <= 0)
            return;

        hunter->GetSpellHistory()->ModifyCooldown(198013, -ms);
        hunter->GetSpellHistory()->ModifyCooldown(179057, -ms);
        ArtifactTraitTest::MarkGenRan(201468);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_dh_feast_on_the_souls::HandleAfterCast);
    }
};

// ===============================================================================================================
// Warrior (round 7)
// ===============================================================================================================

// Might of the Vrykul (188778): "Shield Slam and Thunder Clap generate $188783s1% increased Rage during
// Demoralizing Shout." 188783 is a finished spell with two real ADD_PCT_MODIFIER effects (50% each, with the
// spell family masks of Shield Slam and Thunder Clap) and its own duration - the script only has to put it on
// the warrior when Demoralizing Shout (1160) is cast.
class gen_arti_war_might_of_the_vrykul : public SpellScript
{
    PrepareSpellScript(gen_arti_war_might_of_the_vrykul);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 188778, 188783 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(188778))
            return;

        caster->CastSpell(caster, 188783u, true);
        ArtifactTraitTest::MarkGenRan(188778);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_war_might_of_the_vrykul::HandleAfterCast);
    }
};

// ===============================================================================================================
// Shaman (round 7)
// ===============================================================================================================

// Depth of the Stormflurry chain. 0 = the Stormstrike the player cast, > 0 = an extra strike of the trait.
// The upper bound is only a loop guard; it is not a game value (the 20% roll ends the chain on its own).
static thread_local int32 StormflurryDepth = 0;
static int32 const StormflurryMaxDepth = 20;

// Stormflurry (198367): "Stormstrike has a $s1% chance to strike an additional time for $s2% of normal damage.
// This effect can chain off of itself." Both numbers are in the trait (effect 0 = chance, effect 1 = damage).
// Stormstrike (17364) does not deal damage itself, it triggers 32175 (main hand) and 32176 (off hand), so the
// reduced damage of an extra strike is applied in those two.
class gen_arti_sha_stormflurry : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_stormflurry);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198367, 17364 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(198367) || StormflurryDepth >= StormflurryMaxDepth)
            return;

        int32 chance = TraitValue(caster, 198367);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        Unit* target = GetExplTargetUnit();
        ++StormflurryDepth;
        caster->CastSpell(target ? target : caster, 17364u, true);
        --StormflurryDepth;

        ArtifactTraitTest::MarkGenRan(198367);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_sha_stormflurry::HandleAfterCast);
    }
};

// 32175, 32176 - the two weapon hits of Stormstrike: an extra strike of Stormflurry only deals $s2% damage
class gen_arti_sha_stormflurry_damage : public SpellScript
{
    PrepareSpellScript(gen_arti_sha_stormflurry_damage);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 198367 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || StormflurryDepth <= 0)
            return;

        int32 pct = TraitValue(caster, 198367, EFFECT_1);
        if (pct <= 0)
            return;

        SetHitDamage(CalculatePct(GetHitDamage(), pct));
        ArtifactTraitTest::MarkGenRan(198367);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_sha_stormflurry_damage::HandleHit, EFFECT_0, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

// ===============================================================================================================
// Round 8: third pass. Two more things turned out to be available in the client data:
//   * A trait that depends on a shapeshift form does not need a "shapeshift hook" in the core - the form itself
//     is an aura (Cat 768, Bear 5487, Moonkin 24858/197625 carry SPELL_AURA_MOD_SHAPESHIFT, Travel Form 783 a
//     dummy), so a script on the form spell sees entering and leaving it. That is the same trick the Ghost Wolf
//     traits of round 6 already use, only applied to the druid forms.
//   * Where the client describes a condition that a proc mask would only approximate ("successful Lethal Poison
//     applications"), the debuff spells themselves can carry the script instead - that is exact instead of
//     nearly right.
// ===============================================================================================================

// ===============================================================================================================
// Druid (round 8)
// ===============================================================================================================

// Mark of Shifting (186372): "While in Travel Form, Cat Form, Moonkin Form, or Bear Form, you heal for
// $224392s1% of your maximum health every $186370t sec." 186370 is the periodic dummy (3 s from the client
// data), 224392 the heal (effect 136 HEAL_PCT, 1% of maximum health).
class gen_arti_dru_mark_of_shifting_form : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_mark_of_shifting_form);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 186372, 186370 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetTarget();
        if (!caster || !caster->HasAura(186372))
            return;

        caster->CastSpell(caster, 186370u, true);
        ArtifactTraitTest::MarkGenRan(186372);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetTarget())
            caster->RemoveAurasDueToSpell(186370);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_dru_mark_of_shifting_form::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_dru_mark_of_shifting_form::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
    }
};

// Travel Form (783) is the only one of the four that carries a dummy instead of a shapeshift aura
class gen_arti_dru_mark_of_shifting_travel : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_mark_of_shifting_travel);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 186372, 186370 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetTarget();
        if (!caster || !caster->HasAura(186372))
            return;

        caster->CastSpell(caster, 186370u, true);
        ArtifactTraitTest::MarkGenRan(186372);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetTarget())
            caster->RemoveAurasDueToSpell(186370);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_dru_mark_of_shifting_travel::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_dru_mark_of_shifting_travel::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 186370 - the periodic dummy of Mark of Shifting: one tick is one cast of the heal
class gen_arti_dru_mark_of_shifting_tick : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_mark_of_shifting_tick);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 186372, 224392 });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        if (!caster || !caster->HasAura(186372))
            return;

        caster->CastSpell(caster, 224392u, true);
        ArtifactTraitTest::MarkGenRan(186372);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_dru_mark_of_shifting_tick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Protection of Ashamane (210650): "When you shapeshift out of Cat Form, you gain $210655s1% increased dodge
// chance and armor for $210655d or until you shapeshift back into Cat Form. Can only occur once every
// $214274d." Both spells are in the client data: 210655 carries dodge and armor with its own duration, 214274
// is the marker that holds the internal cooldown. Leaving and entering Cat Form is seen on the form aura.
class gen_arti_dru_protection_of_ashamane : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_protection_of_ashamane);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 210650, 210655, 214274 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // shifting back into Cat Form ends the buff
        if (Unit* caster = GetTarget())
            caster->RemoveAurasDueToSpell(210655);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetTarget();
        if (!caster || !caster->HasAura(210650))
            return;

        // 214274 holds the internal cooldown of the trait, its duration comes from the client data
        if (caster->HasAura(214274))
            return;

        caster->CastSpell(caster, 210655u, true);
        caster->CastSpell(caster, 214274u, true);
        ArtifactTraitTest::MarkGenRan(210650);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_dru_protection_of_ashamane::HandleApply, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_dru_protection_of_ashamane::HandleRemove, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
    }
};

// Light of the Sun (202918): "Reduces the remaining cooldown on Solar Beam by $m1 sec when it interrupts the
// primary target." Solar Beam (78675) does not interrupt itself - its effect 0 triggers 97547, and that spell
// carries the SPELL_EFFECT_INTERRUPT_CAST. The script sits on 97547 and only counts when the target really was
// casting something interruptible; the handler runs before the core applies the interrupt.
class gen_arti_dru_light_of_the_sun : public SpellScript
{
    PrepareSpellScript(gen_arti_dru_light_of_the_sun);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 202918, 78675 });
    }

    void HandleInterrupt(SpellEffIndex /*effIndex*/)
    {
        Player* druid = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Unit* target = GetHitUnit();
        if (!druid || !target || !druid->HasAura(202918))
            return;

        if (!target->IsNonMeleeSpellCast(false, false, true))
            return;

        int32 seconds = TraitValue(druid, 202918);
        if (seconds <= 0)
            return;

        druid->GetSpellHistory()->ModifyCooldown(78675, -seconds * IN_MILLISECONDS);
        ArtifactTraitTest::MarkGenRan(202918);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_dru_light_of_the_sun::HandleInterrupt, EFFECT_0, SPELL_EFFECT_INTERRUPT_CAST);
    }
};

// ===============================================================================================================
// Mage (round 8)
// ===============================================================================================================

// Phoenix Reborn (215773): "Targets affected by your Ignite have a chance to erupt in flame, taking $215775m1
// additional Fire damage and reducing the remaining cooldown on Phoenix's Flame by $s1 sec." Everything is in
// the data: the chance is ProcChance 10 with a 3 s category recovery, the eruption is 215775 (a real damage
// spell) and the ability is Phoenix's Flames 194466. The script only restricts the source to Ignite (12654).
class gen_arti_mage_phoenix_reborn : public AuraScript
{
    PrepareAuraScript(gen_arti_mage_phoenix_reborn);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 215773, 215775, 194466, 12654 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 12654;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* caster = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        if (!caster || !target)
            return;

        caster->CastSpell(target, 215775u, true);

        if (Player* mage = caster->ToPlayer())
            if (int32 seconds = TraitValue(mage, 215773))
                mage->GetSpellHistory()->ModifyCooldown(194466, -seconds * IN_MILLISECONDS);

        ArtifactTraitTest::MarkGenRan(215773);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_mage_phoenix_reborn::CheckProc);
        OnProc += AuraProcFn(gen_arti_mage_phoenix_reborn::HandleProc);
    }
};

// ===============================================================================================================
// Rogue (round 8)
// ===============================================================================================================

// Sinister Circulation (238138): "Successful Lethal Poison applications reduce the cooldown of Kingsbane by
// ${$s1/100}.1 sec." The trait value is in hundredths of a second (50 = 0.5 s). Instead of approximating
// "Lethal Poison" with the client proc mask (69648, which also covers plain melee abilities), the script sits
// on the three lethal poison debuffs themselves: Deadly Poison 2818, Wound Poison 8680, Agonizing Poison
// 200803. Kingsbane is 192759.
class gen_arti_rog_sinister_circulation : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_sinister_circulation);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238138, 192759 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        Player* rogue = caster->ToPlayer();
        if (!rogue)
            return;

        int32 hundredths = TraitValue(rogue, 238138);
        if (hundredths <= 0)
            return;

        rogue->GetSpellHistory()->ModifyCooldown(192759, -hundredths * 10);
        ArtifactTraitTest::MarkGenRan(238138);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_rog_sinister_circulation::HandleAfterHit);
    }
};

// ===============================================================================================================
// Round 9: SimulationCraft cross-check
//
// SimulationCraft (GPL, github.com/simulationcraft/simc, branch legion-dev, commit 7742eb6 of 2018-07-10 - the last
// Legion state, patch 7.3.5) was read ONLY to understand what a trait really does: which spell carries the effect,
// when it fires, what is counted. Every number in the scripts below still comes from the client data of build
// 26972 (trait aura value, base points of the named spells, proc chance, PPM, durations) - no SimC number is used.
// Where SimC hard codes a number that is not in the client data, the trait was NOT implemented (see report).
// ===============================================================================================================

namespace
{
    // Sets a pending value for the duration of one nested cast and resets it afterwards, even on early return.
    // Used by the scripts that cast "their" spell a second time (echo, second strike, explosion with a value
    // computed elsewhere): -1 means "normal cast", anything else is the value the nested cast has to use.
    struct PendingValueScope
    {
        PendingValueScope(int32& slot, int32 value) : _slot(slot) { _slot = value; }
        ~PendingValueScope() { _slot = -1; }
        int32& _slot;
    };

    bool SpellCostsPower(Spell const* spell, Powers power)
    {
        if (!spell)
            return false;

        for (SpellPowerCost const& cost : spell->GetPowerCost())
            if (cost.Power == power && cost.Amount > 0)
                return true;
        return false;
    }
}

// ===============================================================================================================
// Death Knight (round 9)
// ===============================================================================================================

// Runic Chills (238079): "Crystalline Swords damage reduces the cooldown of Sindragosa's Fury by ${$s1/-1000} sec."
// Earlier blocker: 189186 names no damage spell. SimC (crystalline_swords_t::impact) shows the damage spells and that
// the reduction happens per damaging hit: 205165 (trait Crystalline Swords) and 243122 (Thronebreaker's sword, same
// class in SimC). 205164 is the second sword of the pair in the client data (same description and coefficient).
// The value is the trait aura (-1000 ms), Sindragosa's Fury is 190778.
class gen_arti_dk_runic_chills : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_runic_chills);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238079, 190778 });
    }

    void HandleAfterHit()
    {
        Player* dk = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!dk || GetHitDamage() <= 0)
            return;

        int32 ms = TraitValue(dk, 238079);
        if (ms >= 0)
            return;

        dk->GetSpellHistory()->ModifyCooldown(190778, ms);
        ArtifactTraitTest::MarkGenRan(238079);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dk_runic_chills::HandleAfterHit);
    }
};

// Frozen Soul (189184): "When Remorseless Winter ends, Icebringer and Frostreaper release a burst of $204959s1 Frost
// damage, increased by $s2% for each additional enemy you hit during Remorseless Winter."
// Earlier blocker: a hit list over the whole duration. SimC (remorseless_winter_damage_t::impact, frozen_soul_buff_t)
// shows what is counted: every DIFFERENT enemy that took damage (> 0) from a Remorseless Winter tick (196771); the
// list is cleared when Remorseless Winter (196770) ends, and the burst fires only when it runs out (not on cancel).
// The percentage is effect 1 of the trait (100). "Additional" is taken literally from the client text: the first
// enemy does not count -> burst x n. Round LCF2 R24: LegionCore spell_dk_frozen_soul (damage x stacks of the client
// aura 204957, one stack per enemy) gives the same x n, only SimC's model gives x (1 + n) -> no longer "unsicher".
// Cap: 204957 stacks to 20 in the client (SpellAuraOptions CumulativeAura 20), so at most 20 enemies count.
namespace
{
    std::unordered_map<ObjectGuid, GuidSet>& FrozenSoulTargets()
    {
        static std::unordered_map<ObjectGuid, GuidSet> targets;
        return targets;
    }

    int32& FrozenSoulPending()
    {
        static thread_local int32 pending = -1;
        return pending;
    }
}

// 196771 - Remorseless Winter damage tick: remember every enemy that took damage
class gen_arti_dk_frozen_soul_tick : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_frozen_soul_tick);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189184 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || GetHitDamage() <= 0 || !caster->HasAura(189184))
            return;

        FrozenSoulTargets()[caster->GetGUID()].insert(target->GetGUID());
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dk_frozen_soul_tick::HandleAfterHit);
    }
};

// 196770 - Remorseless Winter: new list on apply, burst when it runs out
class gen_arti_dk_frozen_soul : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_frozen_soul);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189184, 204959 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* dk = GetTarget())
            FrozenSoulTargets().erase(dk->GetGUID());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* dk = GetTarget();
        if (!dk)
            return;

        int32 hit = 0;
        auto itr = FrozenSoulTargets().find(dk->GetGUID());
        if (itr != FrozenSoulTargets().end())
        {
            hit = int32(itr->second.size());
            FrozenSoulTargets().erase(itr);
        }

        if (!hit || GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || !dk->HasAura(189184))
            return;

        hit = std::min<int32>(hit, 20); // client: 204957 CumulativeAura 20 (round LCF2 R24)
        PendingValueScope scope(FrozenSoulPending(), hit);
        dk->CastSpell(dk, 204959u, true);
        ArtifactTraitTest::MarkGenRan(189184);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_dk_frozen_soul::HandleApply, EFFECT_1, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_dk_frozen_soul::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

// 204959 - Frozen Soul burst: +$s2% of the trait for every additional enemy
class gen_arti_dk_frozen_soul_damage : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_frozen_soul_damage);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189184 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        int32 hit = FrozenSoulPending();
        if (hit <= 1)
            return;

        int32 pct = TraitValue(GetCaster(), 189184, EFFECT_1);
        if (pct <= 0)
            return;

        int32 damage = GetHitDamage();
        damage += CalculatePct(damage, pct * (hit - 1));
        SetHitDamage(damage);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_dk_frozen_soul_damage::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ===============================================================================================================
// Demon Hunter (round 9)
// ===============================================================================================================

// Erupting Souls (238082): "Soul Cleave deals $243160s1 additional damage for each Soul Fragment it consumes."
// Earlier blocker: soul fragment state. SimC (soul_cleave_t::execute) shows the mechanism: the number of fragments
// the Soul Cleave consumed times one 243160 hit on the Soul Cleave target (SimC notes that in game it is one impact
// per fragment, which is what is done here). The fragments are counted with exactly the rule the core uses to
// consume them (spell_dh_soul_cleave::HandleSouls: own area triggers 8867/6710/6007 within 25 yd), before the core
// script consumes them.
class gen_arti_dh_erupting_souls : public SpellScript
{
    PrepareSpellScript(gen_arti_dh_erupting_souls);

    uint32 _fragments = 0;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238082, 243160 });
    }

    void CountFragments()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(238082))
            return;

        std::list<AreaTrigger*> triggers;
        caster->GetAreatriggerListInRange(triggers, 25.0f);
        for (AreaTrigger* at : triggers)
        {
            if (at->GetCaster() != caster)
                continue;

            switch (at->GetEntry())
            {
                case 8867:
                case 6710:
                case 6007:
                    ++_fragments;
                    break;
                default:
                    break;
            }
        }
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target || !_fragments)
            return;

        for (uint32 i = 0; i < _fragments; ++i)
            caster->CastSpell(target, 243160u, true);
        ArtifactTraitTest::MarkGenRan(238082);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(gen_arti_dh_erupting_souls::CountFragments);
        AfterCast += SpellCastFn(gen_arti_dh_erupting_souls::HandleAfterCast);
    }
};

// ===============================================================================================================
// Monk (round 9)
// ===============================================================================================================

// Thunderfist (238131): "Strike of the Windlord grants you a stack of Thunderfist for each enemy struck. Thunderfist
// discharges upon melee strikes, dealing $242390s1 Nature damage."
// Earlier blocker: the stacking aura has no id in the trait. SimC names it: 242387 (buff) / 242390 (damage). In the
// client data 242387 carries everything else: CumulativeAura 10, proc mask 20 and effect 0 PROC_TRIGGER_SPELL -> 242390,
// so the core already discharges it. What SimC adds is that one discharge uses up ONE stack (thunderfist_t::execute ->
// decrement). One stack per enemy struck by the main hand strike 222029 (SimC strike_of_the_windlord_t::impact).
GEN_TRAIT_STACK_PER_TARGET(gen_arti_monk_thunderfist, 238131, 242387)

// 242387 - Thunderfist: one stack per discharge
class gen_arti_monk_thunderfist_discharge : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_thunderfist_discharge);

    void HandleAfterProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        ModStackAmount(-1);
        ArtifactTraitTest::MarkGenRan(238131);
    }

    void Register() override
    {
        AfterEffectProc += AuraEffectProcFn(gen_arti_monk_thunderfist_discharge::HandleAfterProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Tornado Kicks (196082): "Rising Sun Kick strikes a second time for $s1% additional damage."
// Earlier blocker: $s1 could be "damage of the second strike" or "additional damage", and 185099 has no base points.
// SimC (rising_sun_kick_t::impact, rising_sun_kick_tornado_kick_t) answers both: a second hit of the Rising Sun Kick
// damage spell (185099) on the same target for $s1% of the raw damage of the first hit, without any further damage
// multipliers, but with its own crit and armor. Here: the first hit remembers $s1% of its computed damage, the second
// 185099 gets exactly that value in its hit handler (after the done/taken bonuses, before armor and crit).
class gen_arti_monk_tornado_kicks : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_tornado_kicks);

    int32 _secondStrike = 0;

    static int32& Pending()
    {
        static thread_local int32 pending = -1;
        return pending;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 196082 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (Pending() >= 0)
        {
            SetHitDamage(Pending());
            return;
        }

        int32 pct = TraitValue(GetCaster(), 196082);
        if (pct > 0)
            _secondStrike = CalculatePct(GetHitDamage(), pct);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || _secondStrike <= 0 || Pending() >= 0)
            return;

        PendingValueScope scope(Pending(), _secondStrike);
        caster->CastSpell(target, GetSpellInfo()->Id, true);
        ArtifactTraitTest::MarkGenRan(196082);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_monk_tornado_kicks::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(gen_arti_monk_tornado_kicks::HandleAfterHit);
    }
};

// Dancing Mists (199573): "Renewing Mist has a $s1% chance to immediately spread to an additional target when
// initially cast or when traveling to a new target." This script covers the "traveling to a new target" half:
// an additional, complete Renewing Mist HoT (119611, full duration, no cost) - not a split of the existing one,
// and the extra HoT does not roll again. Target choice for THIS half uses the core's own travel rule (friendly
// unit within 25 yd of the healed target with the lowest health percentage, without Renewing Mist from this
// monk) - SimC does not model the target choice here, so this half stays "unsicher" on target selection.
// R37 (26.09.2026): the OTHER half - "initially cast" - is now implemented too, in
// spell_monk.cpp (spell_monk_renewing_mist::HandleHit, bound to 115151), using LegionCore's actual
// spell_monk_renewing_mist_main::HandleBeforeCast logic (group member within 50 yd, group order, not
// health-sorted) - see the comment there for the source. That half is no longer a guess.
class gen_arti_monk_dancing_mists : public SpellScript
{
    PrepareSpellScript(gen_arti_monk_dancing_mists);

    static bool& SpreadGuard()
    {
        static thread_local bool spreading = false;
        return spreading;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 199573 });
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || SpreadGuard() || !caster->HasAura(199573))
            return;

        int32 chance = TraitValue(caster, 199573);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        uint32 const hotId = GetSpellInfo()->Id;
        std::list<Unit*> allies;
        target->GetFriendlyUnitListInRange(allies, 25.0f, true);

        Unit* best = nullptr;
        for (Unit* ally : allies)
        {
            if (ally == target || !ally->IsAlive() || ally->HasAura(hotId, caster->GetGUID()) || !caster->IsValidAssistTarget(ally))
                continue;
            if (!best || ally->GetHealthPct() < best->GetHealthPct())
                best = ally;
        }

        if (!best)
            return;

        SpreadGuard() = true;
        caster->CastSpell(best, hotId, true);
        SpreadGuard() = false;
        ArtifactTraitTest::MarkGenRan(199573);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_monk_dancing_mists::HandleAfterHit);
    }
};

// Strength of Xuen (195267): "Increases the chance for Tiger Palm to make your next Blackout Kick cost no Chi by $s1%."
// SimC (buff.bok_proc chance) shows the chance is ADDED to the Combo Breaker chance (137384 effect 0), in percentage
// points. The core rolls Tiger Palm against the amount of 137384 effect 0 (spell_monk_jab), so the trait value is
// added to that amount. A second script on the trait recalculates the amount when the trait is learned/removed.
class gen_arti_monk_strength_of_xuen : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_strength_of_xuen);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        if (int32 bonus = TraitValue(GetUnitOwner(), 195267))
        {
            amount += bonus;
            ArtifactTraitTest::MarkGenRan(195267);
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_monk_strength_of_xuen::CalcAmount, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 195267 - Strength of Xuen itself: keep Combo Breaker (137384) up to date
class gen_arti_monk_strength_of_xuen_trait : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_strength_of_xuen_trait);

    void Recalculate(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* monk = GetTarget())
            if (AuraEffect* comboBreaker = monk->GetAuraEffect(137384, EFFECT_0))
                comboBreaker->RecalculateAmount();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_monk_strength_of_xuen_trait::Recalculate, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_monk_strength_of_xuen_trait::Recalculate, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ===============================================================================================================
// Paladin (round 9)
// ===============================================================================================================

// Righteous Verdict (238062): "Your Holy Power spending abilities increase the damage of your next Divine Hammer /
// Blade of Justice by $s1%."
// Earlier blocker: no id for the "next ability" buff. SimC names it: 238996. The client data of 238996 already carries
// the rest: ADD_PCT_MODIFIER on the Blade of Justice / Divine Hammer class mask, 1 proc charge (used up by that
// ability), 15 s duration. Its base points are 0 - the value is the trait's $s1 (8). The trigger condition (spending
// Holy Power) is checked on the proc of the trait, whose proc mask and 100 ms internal cooldown come from the client.
class gen_arti_pal_righteous_verdict : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_righteous_verdict);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238062, 238996 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return SpellCostsPower(eventInfo.GetProcSpell(), POWER_HOLY_POWER);
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* paladin = eventInfo.GetActor();
        int32 pct = TraitValue(paladin, 238062);
        if (!paladin || pct <= 0)
            return;

        paladin->CastCustomSpell(238996, SPELLVALUE_BASE_POINT0, pct, paladin, true);
        ArtifactTraitTest::MarkGenRan(238062);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pal_righteous_verdict::CheckProc);
        OnProc += AuraProcFn(gen_arti_pal_righteous_verdict::HandleProc);
    }
};

// Echo of the Highlord (186788): "Ashbringer mimics Templar's Verdict and Divine Storm, dealing $s1% of normal damage."
// SimC (echoed_templars_verdict_t / echoed_divine_storm_t) shows that the echo is the SAME damage spell again - 224266
// for Templar's Verdict, 224239 for Divine Storm (the core casts 224239 once per target) - with $s1% of the damage.
// SimC delays the echo by 800/600 ms; that delay is not in the client data and is not used here, the echo follows
// the original hit directly.
class gen_arti_pal_echo_of_the_highlord : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_echo_of_the_highlord);

    static int32& EchoPct()
    {
        static thread_local int32 pct = -1;
        return pct;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 186788 });
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (EchoPct() < 0)
            return;

        SetHitDamage(CalculatePct(GetHitDamage(), EchoPct()));
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || EchoPct() >= 0)
            return;

        int32 pct = TraitValue(caster, 186788);
        if (pct <= 0)
            return;

        PendingValueScope scope(EchoPct(), pct);
        caster->CastSpell(target, GetSpellInfo()->Id, true);
        ArtifactTraitTest::MarkGenRan(186788);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_pal_echo_of_the_highlord::HandleHit, EFFECT_0, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
        AfterHit += SpellHitFn(gen_arti_pal_echo_of_the_highlord::HandleAfterHit);
    }
};

// ===============================================================================================================
// Priest (round 9)
// ===============================================================================================================

// Mass Hysteria (194378): "Every $194249t5 sec, Voidform increases damage dealt by Shadow Word: Pain and Vampiric
// Touch by $s1%, stacking until Voidform ends."
// Earlier blocker: the stacking buff has no id. SimC (composite_ta_multiplier of SW:P and VT) shows there is none: the
// counter IS the stack count of Voidform (194249, which the core raises every second), damage x (1 + stacks x $s1%),
// evaluated on every tick. The core snapshots periodic damage into AuraEffect::m_damage at apply/refresh
// (Aura::HandleAuraSpecificPeriodics), so the script scales that snapshot per tick and remembers the unscaled value.
#define GEN_TRAIT_MASS_HYSTERIA(scriptName, auraType)                                           \
class scriptName : public AuraScript                                                            \
{                                                                                               \
    PrepareAuraScript(scriptName);                                                              \
                                                                                                \
    int32 _base = 0;                                                                            \
    int32 _lastSet = -1;                                                                        \
                                                                                                \
    void HandlePeriodic(AuraEffect const* aurEff)                                               \
    {                                                                                           \
        AuraEffect* effect = const_cast<AuraEffect*>(aurEff);                                   \
        if (effect->GetDamage() != _lastSet)                                                    \
            _base = effect->GetDamage(); /* new snapshot (apply or refresh) */                  \
                                                                                                \
        int32 value = _base;                                                                    \
        Unit* caster = GetCaster();                                                             \
        int32 pct = TraitValue(caster, 194378);                                                 \
        if (caster && pct > 0)                                                                  \
            if (Aura* voidform = caster->GetAura(194249))                                       \
            {                                                                                   \
                value = _base + CalculatePct(_base, pct * int32(voidform->GetStackAmount()));   \
                ArtifactTraitTest::MarkGenRan(194378);                                          \
            }                                                                                   \
                                                                                                \
        effect->SetDamage(value);                                                               \
        _lastSet = value;                                                                       \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        OnEffectPeriodic += AuraEffectPeriodicFn(scriptName::HandlePeriodic, EFFECT_1, auraType); \
    }                                                                                           \
};

// 589 - Shadow Word: Pain (effect 1 PERIODIC_DAMAGE), 34914 - Vampiric Touch (effect 1 PERIODIC_LEECH)
GEN_TRAIT_MASS_HYSTERIA(gen_arti_pri_mass_hysteria_swp, SPELL_AURA_PERIODIC_DAMAGE)
GEN_TRAIT_MASS_HYSTERIA(gen_arti_pri_mass_hysteria_vt, SPELL_AURA_PERIODIC_LEECH)

// Sphere of Insanity (194179): "The Sphere of Insanity manifests for the duration of Voidform. Your Void Bolts, Mind
// Blasts, and Mind Flays/Mind Spikes cause the sphere to duplicate $194182s3% their damage to all enemies affected by
// your Shadow Word: Pain."
// Earlier blocker: state/hook. SimC (priest_spell_t::assess_damage, sphere_of_insanity_spell_t) gives the mechanism:
// while Voidform is up, the damage actually dealt by those spells times $194182s3% is dealt again to every enemy
// carrying the priest's Shadow Word: Pain. The client data provides the pieces: 194230 is the dummy aura with the
// matching proc mask (direct and periodic magic damage) that 194182 effect 3 hands out, 194238 is the damage spell of
// the sphere, 194182 effect 2 is the percentage. The script puts 194230 on the priest for the duration of Voidform
// instead of summoning the visual sphere creature (98680), which has no script in the core. The enemies with SW:P are
// taken from the priest's hostile reference list (enemies in combat with him).
class gen_arti_pri_sphere_of_insanity_voidform : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_sphere_of_insanity_voidform);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194179, 194230 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* priest = GetTarget();
        if (priest && priest->HasAura(194179))
            priest->CastSpell(priest, 194230u, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* priest = GetTarget())
            priest->RemoveAurasDueToSpell(194230);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_pri_sphere_of_insanity_voidform::HandleApply, EFFECT_4, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pri_sphere_of_insanity_voidform::HandleRemove, EFFECT_4, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 194230 - the sphere's proc aura
class gen_arti_pri_sphere_of_insanity : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_sphere_of_insanity);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194182, 194238, 589 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo || !eventInfo.GetDamageInfo() || !eventInfo.GetDamageInfo()->GetDamage())
            return false;

        switch (spellInfo->Id)
        {
            case 205448: // Void Bolt
            case 8092:   // Mind Blast
            case 15407:  // Mind Flay
            case 73510:  // Mind Spike
                return true;
            default:
                return false;
        }
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* priest = eventInfo.GetActor();
        if (!priest)
            return;

        int32 pct = SpellEffectValue(194182, EFFECT_2);
        int32 amount = int32(CalculatePct(eventInfo.GetDamageInfo()->GetDamage(), pct));
        if (amount <= 0)
            return;

        std::list<Unit*> victims;
        for (HostileReference* ref = priest->getHostileRefManager().getFirst(); ref; ref = ref->next())
            if (Unit* enemy = ref->GetSource()->GetOwner())
                if (enemy->IsAlive() && enemy->HasAura(589, priest->GetGUID()))
                    victims.push_back(enemy);

        for (Unit* enemy : victims)
            priest->CastCustomSpell(194238, SPELLVALUE_BASE_POINT0, amount, enemy, true);

        if (!victims.empty())
            ArtifactTraitTest::MarkGenRan(194179);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pri_sphere_of_insanity::CheckProc);
        OnProc += AuraProcFn(gen_arti_pri_sphere_of_insanity::HandleProc);
    }
};

// ===============================================================================================================
// Shaman (round 9)
// ===============================================================================================================

// 201846 - Stormbringer (proc); Wind Strikes (198292): "When Stormbringer resets the remaining cooldown on Stormstrike,
// you gain $s1% attack speed for $198293d." SimC (trigger of buff.wind_strikes next to the Stormstrike reset) shows the
// event is the Stormbringer proc itself; in the core that is exactly the cast of 201846 (spell_sha_stormbringer).
// 198293 carries the attack speed (aura 342, 3 %) and the 6 s duration.
GEN_TRAIT_SELF_ON_CAST(gen_arti_sha_wind_strikes, 198292, 198293)

// ===============================================================================================================
// Rogue (round 9)
// ===============================================================================================================

// Finality (197406): "After using Nightblade or Eviscerate, your next use of that finishing move is empowered."
// Earlier blocker: no id for the "next use" buff. SimC names both: 197496 "Finality: Eviscerate" (client data:
// ADD_PCT_MODIFIER 20 % on the Eviscerate class mask) and 197498 "Finality: Nightblade" (dummy 20 %, MiscValue =
// Nightblade 195452). SimC's rule: a use WITHOUT the buff grants it, a use WITH the buff consumes it. The client value
// $w1 (20) is the value for 5 combo points.
// Round LCF2 R28 (2026-09-25): the granted buff now scales with the combo points spent by the granting finisher:
// $w1 * CP / 5 = 4 % per combo point (6 CP with Deeper Stratagem -> 24 %). Sources (R27 research, 3 independent hints):
// SimC model (20 % x CP / 5), Wowhead player test comment ("+4 % per combo point ... 2 CP -> +8 %, 6 CP -> +24 %"), and
// the trait tooltip using the variable $w1 instead of a fixed number. Before R28: fixed 20 % regardless of CP.
#define GEN_TRAIT_FINALITY(scriptName, buffId)                                                  \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool _hadBuff = false;                                                                      \
    int32 _comboPoints = 0;                                                                     \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ 197406, buffId });                                           \
    }                                                                                           \
                                                                                                \
    void HandleBeforeCast()                                                                     \
    {                                                                                           \
        if (Unit* caster = GetCaster())                                                         \
            _hadBuff = caster->HasAura(buffId);                                                 \
        _comboPoints = 0;                                                                       \
        if (SpellPowerCost const* cost = GetSpell()->GetPowerCost(POWER_COMBO_POINTS))          \
            _comboPoints = cost->Amount;                                                        \
    }                                                                                           \
                                                                                                \
    void HandleAfterCast()                                                                      \
    {                                                                                           \
        Unit* caster = GetCaster();                                                             \
        if (!caster || !caster->HasAura(197406))                                                \
            return;                                                                             \
                                                                                                \
        if (_hadBuff)                                                                           \
            caster->RemoveAurasDueToSpell(buffId);                                              \
        else if (_comboPoints > 0)                                                              \
        {                                                                                       \
            SpellInfo const* buffInfo = sSpellMgr->GetSpellInfo(buffId);                        \
            SpellEffectInfo const* buffEffect = buffInfo ? buffInfo->GetEffect(EFFECT_0) : nullptr; \
            int32 basePct = buffEffect ? buffEffect->CalcValue(caster) : 20;                    \
            int32 pct = basePct * _comboPoints / 5;                                             \
            if (pct > 0)                                                                        \
                caster->CastCustomSpell(buffId, SPELLVALUE_BASE_POINT0, pct, caster, true);     \
        }                                                                                       \
        ArtifactTraitTest::MarkGenRan(197406);                                                  \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        BeforeCast += SpellCastFn(scriptName::HandleBeforeCast);                                \
        AfterCast += SpellCastFn(scriptName::HandleAfterCast);                                  \
    }                                                                                           \
};

// 196819 - Eviscerate (the bonus itself is the ADD_PCT_MODIFIER of 197496, applied by the core)
GEN_TRAIT_FINALITY(gen_arti_rog_finality_eviscerate, 197496)
// 195452 - Nightblade (the bonus is added to the periodic amount below)
GEN_TRAIT_FINALITY(gen_arti_rog_finality_nightblade, 197498)

// 195452 - Nightblade: +$w1% of 197498 on the periodic damage while the buff is up
class gen_arti_rog_finality_nightblade_dot : public AuraScript
{
    PrepareAuraScript(gen_arti_rog_finality_nightblade_dot);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (AuraEffect const* finality = caster->GetAuraEffect(197498, EFFECT_0))
            if (int32 pct = finality->GetAmount())
                AddPct(amount, pct);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_rog_finality_nightblade_dot::CalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Greed (202820): "Run Through occasionally awakens The Dreadblades, unleashing a sweeping attack against all nearby
// enemies for ${$202822sw2 + $202823sw2} Physical damage, and healing you for ${$MHP*.05} per target hit."
// Earlier blocker: the sweeping attack seemed to have no id of its own. SimC (greed_t, run_through_t::execute) shows
// it does: 202822 IS the sweep (area around the caster in the client data, its effect 2 triggers the off-hand part
// 202823). Trigger: Run Through (2098) with the trait's ProcChance (35) from the client proc entry. Heal: $s1 (5) %
// of maximum health per target hit by 202822.
class gen_arti_rog_greed : public AuraScript
{
    PrepareAuraScript(gen_arti_rog_greed);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 202820, 202822 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 2098;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        if (Unit* rogue = eventInfo.GetActor())
        {
            rogue->CastSpell(rogue, 202822u, true);
            ArtifactTraitTest::MarkGenRan(202820);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_rog_greed::CheckProc);
        OnProc += AuraProcFn(gen_arti_rog_greed::HandleProc);
    }
};

// 202822 - Greed (sweep): heal per target hit
class gen_arti_rog_greed_heal : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_greed_heal);

    void HandleAfterHit()
    {
        Unit* rogue = GetCaster();
        if (!rogue || !GetHitUnit())
            return;

        int32 pct = TraitValue(rogue, 202820);
        if (pct <= 0)
            return;

        uint32 heal = uint32(CalculatePct(rogue->GetMaxHealth(), pct));
        HealInfo healInfo(rogue, rogue, heal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        rogue->HealBySpell(healInfo);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_rog_greed_heal::HandleAfterHit);
    }
};

// Blunderbuss (202897): "When Saber Slash strikes an additional time, there is a $s2% chance that your next Pistol
// Shot will be replaced with Blunderbuss."
// Earlier blocker: "needs an action bar override aura that is not in the data". SimC names the buff: 202848 - and in
// the client data 202848 IS that aura (SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS, value 202895, class mask of Pistol Shot,
// 10 s), which the core handles natively in Unit::GetCastSpellInfo. SimC triggers it next to Opportunity in the Saber
// Slash extra strike; in the core that extra strike is the only place that casts Opportunity (195627,
// spell_rog_saber_slash). SimC removes the buff when Blunderbuss (202895) is fired. Chance = $s2 (33) of the trait.
class gen_arti_rog_blunderbuss : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_blunderbuss);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 202897, 202848 });
    }

    void HandleAfterCast()
    {
        Unit* rogue = GetCaster();
        if (!rogue || !rogue->HasAura(202897))
            return;

        int32 chance = TraitValue(rogue, 202897, EFFECT_1);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        rogue->CastSpell(rogue, 202848u, true);
        ArtifactTraitTest::MarkGenRan(202897);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_rog_blunderbuss::HandleAfterCast);
    }
};

// 202895 - Blunderbuss fired: the override buff is used up
class gen_arti_rog_blunderbuss_fired : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_blunderbuss_fired);

    void HandleAfterCast()
    {
        if (Unit* rogue = GetCaster())
            rogue->RemoveAurasDueToSpell(202848);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_rog_blunderbuss_fired::HandleAfterCast);
    }
};

// ===============================================================================================================
// Warrior (round 9)
// ===============================================================================================================

// Battle Scars (200857): "Increases maximum health by $s1% during Enrage."
// Earlier blocker: Enrage (184362) has no effect that could carry the health. SimC (enrage_t::trigger/expire_override)
// shows it does not need one: the maximum health itself is raised when Enrage starts (not on refresh) and lowered
// again when it ends. Here: UNIT_MOD_HEALTH TOTAL_PCT by $s1 (2) on apply, reverted with the same value on remove.
class gen_arti_war_battle_scars : public AuraScript
{
    PrepareAuraScript(gen_arti_war_battle_scars);

    int32 _pct = 0;

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* warrior = GetTarget();
        _pct = TraitValue(warrior, 200857);
        if (!warrior || _pct <= 0)
        {
            _pct = 0;
            return;
        }

        warrior->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(_pct), true);
        ArtifactTraitTest::MarkGenRan(200857);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (_pct <= 0)
            return;

        if (Unit* warrior = GetTarget())
            warrior->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(_pct), false);
        _pct = 0;
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_war_battle_scars::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_war_battle_scars::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Reflective Plating (188672): "Spell Reflection now reflects an unlimited number of spells during its duration."
// SimC (warrior_t::assess_damage) shows the mechanism: with the trait the reflection simply does not expire when a
// spell is reflected. In the core the reflect uses up the single proc charge of 23920 (default proc entry, hit mask
// REFLECT). Preventing the prepare step keeps the charge; the 5 s duration from the client data stays unchanged.
class gen_arti_war_reflective_plating : public AuraScript
{
    PrepareAuraScript(gen_arti_war_reflective_plating);

    void HandlePrepareProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* warrior = GetTarget();
        if (warrior && warrior->HasAura(188672))
        {
            PreventDefaultAction();
            ArtifactTraitTest::MarkGenRan(188672);
        }
    }

    void Register() override
    {
        DoPrepareProc += AuraProcFn(gen_arti_war_reflective_plating::HandlePrepareProc);
    }
};

// ===============================================================================================================
// Mage (round 9)
// ===============================================================================================================

// Rule of Threes (215463): "You have a ${$m1/10}.1% chance to fire $m2 additional Arcane Missiles."
// Earlier blocker: no hook to add ticks to a running channel. SimC (buff rule_of_threes = 187292, arcane_missiles_t)
// shows no ticks are added: 187292 is rolled when Arcane Missiles is cast and shortens the tick time of that channel.
// The client data of 187292 does exactly that (ADD_PCT_MODIFIER SPELLMOD_ACTIVATION_TIME -43 % on the Arcane Missiles
// mask), which the core applies when the channel aura calculates its period. Chance = trait value / 10 (33.3 %).
// The buff is removed again when the channel ends (SimC: last_tick -> expire).
class gen_arti_mage_rule_of_threes : public SpellScript
{
    PrepareSpellScript(gen_arti_mage_rule_of_threes);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 215463, 187292 });
    }

    void HandleBeforeCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        caster->RemoveAurasDueToSpell(187292);

        int32 value = TraitValue(caster, 215463);
        if (value <= 0 || !roll_chance_f(value / 10.0f))
            return;

        caster->CastSpell(caster, 187292u, true);
        ArtifactTraitTest::MarkGenRan(215463);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(gen_arti_mage_rule_of_threes::HandleBeforeCast);
    }
};

// 5143 - Arcane Missiles channel: Rule of Threes ends with it
class gen_arti_mage_rule_of_threes_end : public AuraScript
{
    PrepareAuraScript(gen_arti_mage_rule_of_threes_end);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            caster->RemoveAurasDueToSpell(187292);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_mage_rule_of_threes_end::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Time and Space (238126): "When you cast Arcane Explosion, Aluneth will echo the Arcane Explosion for $s1% of its
// damage, at the location of your previous Arcane Explosion cast within $240692d."
// SimC (arcane_explosion_t::execute) gives the order: if the marker from the previous Arcane Explosion is still up,
// the echo (240689) fires; then the marker (240692) is (re)applied. 240689 already carries the 20 %: its spell power
// coefficient 0.1815 is 20 % of Arcane Explosion's 0.9075. 240692 has a proc charge that any magic damage would use up
// in the core, so the script keeps the position and time of the previous cast itself and uses the marker's duration
// (6 s) from the client data; 240692 is still cast for its visual area trigger.
class gen_arti_mage_time_and_space : public SpellScript
{
    PrepareSpellScript(gen_arti_mage_time_and_space);

    struct LastExplosion
    {
        Position pos;
        uint32 time = 0;
    };

    static std::unordered_map<ObjectGuid, LastExplosion>& Previous()
    {
        static std::unordered_map<ObjectGuid, LastExplosion> previous;
        return previous;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238126, 240689, 240692 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(238126))
            return;

        uint32 now = getMSTime();
        int32 window = sSpellMgr->GetSpellInfo(240692)->GetDuration();

        auto itr = Previous().find(caster->GetGUID());
        if (itr != Previous().end() && window > 0 && getMSTimeDiff(itr->second.time, now) <= uint32(window))
        {
            caster->CastSpell(itr->second.pos.GetPositionX(), itr->second.pos.GetPositionY(), itr->second.pos.GetPositionZ(), 240689u, true);
            ArtifactTraitTest::MarkGenRan(238126);
        }

        LastExplosion& last = Previous()[caster->GetGUID()];
        last.pos.Relocate(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ());
        last.time = now;
        caster->CastSpell(caster, 240692u, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_mage_time_and_space::HandleAfterCast);
    }
};

// Aegwynn's Ascendance (187680): "When Evocation completes, it explodes for Arcane damage equal to $s1% of the mana it
// restored."
// SimC (evocation_t::tick/last_tick, aegwynns_ascendance_t) gives the mechanism: count the mana Evocation actually
// restored (capped at maximum mana), and when the channel ends deal $s1% of it as 187677 around the mage, with no
// further damage multipliers (snapshot_flags &= STATE_NO_MULTIPLIER). 187677 has base points 1 and no coefficient,
// so the script sets the hit damage itself. "Completes" = the channel aura expired, not cancelled.
namespace
{
    int32& AegwynnPending()
    {
        static thread_local int32 pending = -1;
        return pending;
    }
}

class gen_arti_mage_aegwynns_ascendance : public AuraScript
{
    PrepareAuraScript(gen_arti_mage_aegwynns_ascendance);

    int64 _restored = 0;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 187680, 187677 });
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* mage = GetTarget();
        if (!mage)
            return;

        int64 maxMana = mage->GetMaxPower(POWER_MANA);
        int64 gain = maxMana * aurEff->GetAmount() / 100;
        int64 missing = maxMana - mage->GetPower(POWER_MANA);
        _restored += std::max<int64>(0, std::min(gain, missing));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* mage = GetTarget();
        if (!mage || GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        int32 pct = TraitValue(mage, 187680);
        int32 damage = int32(_restored * pct / 100);
        if (pct <= 0 || damage <= 0)
            return;

        PendingValueScope scope(AegwynnPending(), damage);
        mage->CastSpell(mage, 187677u, true);
        ArtifactTraitTest::MarkGenRan(187680);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_mage_aegwynns_ascendance::HandlePeriodic, EFFECT_0, SPELL_AURA_OBS_MOD_POWER);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_mage_aegwynns_ascendance::HandleRemove, EFFECT_0, SPELL_AURA_OBS_MOD_POWER, AURA_EFFECT_HANDLE_REAL);
    }
};

// 187677 - Aegwynn's Ascendance explosion: the damage computed from the restored mana
class gen_arti_mage_aegwynns_ascendance_damage : public SpellScript
{
    PrepareSpellScript(gen_arti_mage_aegwynns_ascendance_damage);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (AegwynnPending() >= 0)
            SetHitDamage(AegwynnPending());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_mage_aegwynns_ascendance_damage::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ===============================================================================================================
// Druid (round 9)
// ===============================================================================================================

// Ashamane's Bite (210702): "Your combo point generators against targets bleeding from your Rip have a $h% chance to
// awaken the Spirit of Ashamane, which inflicts a Shadowy duplicate of that Rip on the target."
// Earlier blocker: chance / target spell. $h (10) and the proc mask are in the trait's client proc entry; SimC names
// the duplicate: 210705 "Ashamane's Rip", and defines it: a copy of THAT Rip (same combo points and multipliers, i.e.
// the same damage per tick, and the Rip's remaining duration), replacing an older copy instead of stacking.
// "Combo point generator" = a spell with an ENERGIZE effect for combo points (SimC: energize_resource ==
// RESOURCE_COMBO_POINT) - in the client data Shred 5221, Rake 1822 and Moonfire (cat) 155625.
class gen_arti_dru_ashamanes_bite : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_ashamanes_bite);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 210702, 210705, 1079 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Unit* druid = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        if (!spellInfo || !druid || !target || !target->HasAura(1079, druid->GetGUID()))
            return false;

        for (SpellEffectInfo const* effect : spellInfo->GetEffectsForDifficulty(DIFFICULTY_NONE))
            if (effect && effect->Effect == SPELL_EFFECT_ENERGIZE && effect->MiscValue == POWER_COMBO_POINTS)
                return true;
        return false;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* druid = eventInfo.GetActor();
        Unit* target = eventInfo.GetProcTarget();
        if (!druid || !target)
            return;

        Aura* rip = target->GetAura(1079, druid->GetGUID());
        AuraEffect* ripDot = target->GetAuraEffect(1079, EFFECT_0, druid->GetGUID());
        if (!rip || !ripDot)
            return;

        target->RemoveAurasDueToSpell(210705, druid->GetGUID());
        druid->CastSpell(target, 210705u, true);

        if (Aura* copy = target->GetAura(210705, druid->GetGUID()))
        {
            if (AuraEffect* copyDot = copy->GetEffect(EFFECT_0))
                copyDot->SetDamage(ripDot->GetDamage());
            copy->SetMaxDuration(rip->GetDuration());
            copy->SetDuration(rip->GetDuration());
            copy->SetNeedClientUpdateForTargets();
            ArtifactTraitTest::MarkGenRan(210702);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dru_ashamanes_bite::CheckProc);
        OnProc += AuraProcFn(gen_arti_dru_ashamanes_bite::HandleProc);
    }
};

// Echoing Stars (214508): "Each time Starfall deals damage, it also damages another nearby enemy for $226104s1 Astral
// damage." Round LCF2 R38 (2026-09-26), final closure round - re-verified, kept as documented ANNAHME (b), not (a):
// client DB2 (build 26972) confirms yet again that neither 214508 nor 226104 carries a radius (spelleffect.csv has
// blank EffectRadiusIndex for 226104's only effect, spellradius.csv accordingly has nothing keyed to it) - this is a
// genuine, permanent data gap, not a research gap (re-checked this round against spell_dummy_trigger/spell_target_filter
// again, same empty result as R37). The method used is the "cross-reference inference" approach: SimulationCraft
// (github.com/simulationcraft/simc, engine/class_modules/sc_druid.cpp, starfall_tick_t) reuses Starfall's OWN area
// radius (spell 191034, effect 0) as the search radius for the chain/echo target - i.e. the bounce lands on another
// enemy already standing in the same area the original Starfall tick could have hit, not an independently rolled
// "nearby" number. Client DB2 spellradius.csv (build 26972) resolves 191034's RadiusIndex 18 to a flat 15 yards.
// This value (191034's radius, resolved dynamically via CalcRadius below rather than hardcoded, so it stays correct
// if Starfall's own radius is ever changed) is used as the ANNAHME for "nearby" - justified by design consistency
// (Echoing Stars is a Starfall-only bonus effect, so reusing Starfall's own area is the most natural in-universe
// reading of "nearby") and by matching the one concrete implementation (SimC) that anyone has ever shipped for this
// trait. See report lcf2r38_2026-09-26_traits_final.md, section "Echoing Stars", for the full method log.
class gen_arti_dru_echoing_stars : public SpellScript
{
    PrepareSpellScript(gen_arti_dru_echoing_stars);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 214508, 226104, 191034 });
    }

    void HandleAfterHit()
    {
        Unit* druid = GetCaster();
        Unit* target = GetHitUnit();
        if (!druid || !target || !druid->HasAura(214508))
            return;

        SpellEffectInfo const* area = sSpellMgr->GetSpellInfo(191034)->GetEffect(EFFECT_0);
        float radius = area ? area->CalcRadius(druid) : 0.0f;
        if (radius <= 0.0f)
            return;

        std::list<Unit*> enemies;
        druid->GetAttackableUnitListInRange(enemies, druid->GetDistance(target) + radius);
        enemies.remove_if([target, radius](Unit* enemy)
        {
            return enemy == target || !enemy->IsWithinDist(target, radius);
        });

        if (enemies.empty())
            return;

        druid->CastSpell(Trinity::Containers::SelectRandomContainerElement(enemies), 226104u, true);
        ArtifactTraitTest::MarkGenRan(214508);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dru_echoing_stars::HandleAfterHit);
    }
};

// ===============================================================================================================
// Hunter (round 9)
// ===============================================================================================================

// Call of the Hunter (191048): "When you Marked Shot, Thas'dorah has a chance to call forth a barrage of wind arrows
// to strike all Vulnerable targets."
// Earlier blocker: the arrow spell is named nowhere. SimC names it: 191070 "Call of the Hunter" (client data: physical
// damage, AP 3). The chance is the trait's PPM entry 57 (1 per minute, SpellProcsPerMinute), which the core's proc
// system applies to the trait aura natively; SimC also rolls once per Marked Shot cast (185901). Targets: every
// enemy with this hunter's Vulnerable (187131) within Marked Shot's own search radius (185901 effect 0, same as
// spell_hun_marked_shot). SimC fires 191070 twice per target, the client text/data give no count - here once per
// target. Round LCF2 R23: LegionCore-7.3.5 settles the count - spell_target_filter 191323 keeps only targets with
// Vulnerable 187131, spell_dummy_trigger 191323 -> 191061 "Wind Arrow Barrage" (client: TRIGGER_MISSILE -> 191070)
// once per target. So the arrow is now sent through 191061 (missile visual of the client) instead of 191070 directly.
// (LegionCore additionally delays the barrage by 500 ms, spell_trigger option 45 - a server value, not taken over.)
class gen_arti_hun_call_of_the_hunter : public AuraScript
{
    PrepareAuraScript(gen_arti_hun_call_of_the_hunter);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 191048, 191061, 191070, 185901, 187131 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->Id == 185901;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* hunter = eventInfo.GetActor();
        if (!hunter)
            return;

        SpellEffectInfo const* area = sSpellMgr->GetSpellInfo(185901)->GetEffect(EFFECT_0);
        float radius = area ? area->CalcRadius(hunter) : 0.0f;
        if (radius <= 0.0f)
            return;

        std::list<Unit*> enemies;
        hunter->GetAttackableUnitListInRange(enemies, radius);

        bool fired = false;
        for (Unit* enemy : enemies)
        {
            if (!enemy->HasAura(187131, hunter->GetGUID()))
                continue;

            hunter->CastSpell(enemy, 191061u, true); // Wind Arrow Barrage -> missile 191070
            fired = true;
        }

        if (fired)
            ArtifactTraitTest::MarkGenRan(191048);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_hun_call_of_the_hunter::CheckProc);
        OnProc += AuraProcFn(gen_arti_hun_call_of_the_hunter::HandleProc);
    }
};

// ===============================================================================================================
// Round 10: lateral external research (report section "Runde 7 (laterale externe Recherche)")
//
// Sources used in this round, all fetched and checked (URLs and quotes in the report):
//  - archived Wowhead spell pages from the Legion era (web.archive.org, 2017/2018 snapshots),
//  - archived Legion beta forum thread "Theorycrafting Questions" (us.battle.net topic 20743504316, answers of the
//    Blizzard game designers, web.archive.org snapshots of 2016).
// Only ONE number in this block comes from outside the client data: the Crystalline Swords counter (9), stated by a
// Blizzard game designer in the beta; that trait is therefore marked "unsicher". All other traits below were solved
// because the Legion-era pages pointed at helper spells that DO exist in build 26972 but had been missed (earlier
// rounds searched by trait id only, these helpers are linked by name / "$@spelldesc<trait>"). Their numbers are the
// client values of those helpers and of the trait auras.
// ===============================================================================================================

// Crystalline Swords (189186) - round LCF2 R24: counted as proven (Blizzard designer post + SimC 7.3.5 agree; only
// LegionCore uses a plain chance model instead): "Your melee attacks have a chance to create icy copies of Icebringer and
// Frostreaper, which will then stab and pierce your foes."
// Client: proc mask 20 (melee), ProcChance 45; damage spells 205164/205165 ("$@spelldesc189186", 1.2 AP each).
// Missing before: when the swords actually strike. Blizzard game designer Celestalon, Legion beta forum, 25.04.2016:
// "Under the hood, it's a 45% proc chance, but when it procs it increments a counter. [...] the gameplay element
// happens on 9, dealing 2x 1*AP Frost damage, then resetting the counter." and "Multiple targets on AoEs such as
// Frostscythe will not give multiple chances." The 45% of that post is still the ProcChance of the 7.3.5 client
// (and of the archived Wowhead page of 05.04.2018), SimC 7.3.5 still uses the 9 - but the post is a beta statement,
// so the counter stays an external number (status "unsicher").
class gen_arti_dk_crystalline_swords : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_crystalline_swords);

    static constexpr uint32 COUNTER_MAX = 9; // Blizzard beta forum post, see above

    uint32 _counter = 0;
    ObjectGuid _lastCastId;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 189186, 205164, 205165 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!eventInfo.GetActionTarget())
            return false;

        // one chance per cast, not per target hit by an AoE
        if (Spell const* spell = eventInfo.GetProcSpell())
        {
            if (!spell->m_castId.IsEmpty() && spell->m_castId == _lastCastId)
                return false;
            _lastCastId = spell->m_castId;
        }
        return true;
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* dk = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!dk || !victim)
            return;

        if (++_counter < COUNTER_MAX)
            return;

        _counter = 0;
        dk->CastSpell(victim, 205164, true);
        dk->CastSpell(victim, 205165, true);
        ArtifactTraitTest::MarkGenRan(189186);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dk_crystalline_swords::CheckProc);
        OnProc += AuraProcFn(gen_arti_dk_crystalline_swords::HandleProc);
    }
};

// Shroud of Mist (199365): "Reduces all damage you take by $s1% while you are channeling Soothing Mist."
// Carrier: 214478 "Shroud of Mist" ($@spelldesc199365, MOD_DAMAGE_PERCENT_TAKEN -3, all schools, infinite).
// Soothing Mist (115175) is the channel aura on the healed target, cast by the monk (or by his statue - the statue
// has no trait and is skipped). The carrier is put on the monk while the channel aura exists.
class gen_arti_monk_shroud_of_mist : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_shroud_of_mist);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 199365, 214478 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* monk = GetCaster();
        if (!monk || !monk->IsPlayer())
            return;

        int32 pct = TraitValue(monk, 199365);
        if (pct >= 0)
            return;

        monk->CastCustomSpell(214478, SPELLVALUE_BASE_POINT0, pct, monk, true);
        ArtifactTraitTest::MarkGenRan(199365);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* monk = GetCaster())
            monk->RemoveAurasDueToSpell(214478, monk->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_monk_shroud_of_mist::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_monk_shroud_of_mist::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Defender of Truth (238097): "Ardent Defender grants an absorb shield for $s1% of maximum health when it fades."
// Carrier: 240059 "Defender of Truth" (SCHOOL_ABSORB, all schools, 10 s). Amount = $s1 (12) % of max health.
// "fades" = every end of Ardent Defender (31850) while the paladin lives; that includes the end through its own
// cheat-death heal (the core script removes the aura there).
class gen_arti_pal_defender_of_truth : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_defender_of_truth);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238097, 240059 });
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* paladin = GetTarget();
        if (!paladin || !paladin->IsAlive() || GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_DEATH)
            return;

        int32 pct = TraitValue(paladin, 238097);
        if (pct <= 0)
            return;

        int32 amount = int32(paladin->CountPctFromMaxHealth(pct));
        paladin->CastCustomSpell(240059, SPELLVALUE_BASE_POINT0, amount, paladin, true);
        ArtifactTraitTest::MarkGenRan(238097);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pal_defender_of_truth::HandleRemove, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL);
    }
};

// Mimiron's Shell (197160): "Aspect of the Turtle also heals you for $s1% of your maximum health over its duration."
// Carrier: 197161 "Mimiron's Shell" ($@spelldesc197160, PERIODIC_HEAL every 2 s, 8 s). Total = $s1 (10) % of max
// health, split evenly over the ticks of 197161 (duration / period, both from the client data).
class gen_arti_hun_mimirons_shell : public AuraScript
{
    PrepareAuraScript(gen_arti_hun_mimirons_shell);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197160, 197161 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* hunter = GetTarget();
        int32 pct = TraitValue(hunter, 197160);
        if (!hunter || pct <= 0)
            return;

        SpellInfo const* heal = sSpellMgr->GetSpellInfo(197161);
        SpellEffectInfo const* effect = heal ? heal->GetEffect(EFFECT_0) : nullptr;
        if (!effect || !effect->ApplyAuraPeriod || heal->GetMaxDuration() <= 0)
            return;

        int32 ticks = heal->GetMaxDuration() / int32(effect->ApplyAuraPeriod);
        if (ticks <= 0)
            return;

        int32 perTick = int32(hunter->CountPctFromMaxHealth(pct)) / ticks;
        if (perTick <= 0)
            return;

        hunter->CastCustomSpell(197161, SPELLVALUE_BASE_POINT0, perTick, hunter, true);
        ArtifactTraitTest::MarkGenRan(197160);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_hun_mimirons_shell::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Borrowed Time (197762): "Applying Atonement to a target reduces the cast time of your next Smite or Light's Wrath
// by $s1%, or causes your next Penance to channel $s1% faster."
// Carrier: 197763 "Borrowed Time" ($@spelldesc197762): four ADD_PCT_MODIFIER effects on the Smite / Light's Wrath /
// Penance masks, 1 proc charge (used up by the next of these spells), 12 s. Base points = trait $s1 (-5).
class gen_arti_pri_borrowed_time : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_borrowed_time);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 197762, 197763 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* priest = GetCaster();
        int32 pct = TraitValue(priest, 197762);
        if (!priest || pct >= 0)
            return;

        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, pct);
        values.AddSpellMod(SPELLVALUE_BASE_POINT1, pct);
        values.AddSpellMod(SPELLVALUE_BASE_POINT2, pct);
        values.AddSpellMod(SPELLVALUE_BASE_POINT3, pct);
        priest->CastCustomSpell(197763, values, priest, TRIGGERED_FULL_MASK);
        ArtifactTraitTest::MarkGenRan(197762);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_pri_borrowed_time::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Adaptive Fur (200850): "Taking elemental damage has a chance to grant you $200945s1% damage reduction for
// $200945d against the elements that caused the damage."
// Earlier blocker: only the Holy variant 200945 was known. The client has one "Adaptive Fur" spell per school, each
// MOD_DAMAGE_PERCENT_TAKEN -10 % for 8 s on its own school mask: 200944 Fire (4), 200943 Nature (8), 200942 Frost
// (16), 200941 Shadow (32), 200940 Arcane (64), 200945 Holy (2). Chance (25) and proc mask come from the trait.
class gen_arti_dru_adaptive_fur : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_adaptive_fur);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 200940, 200941, 200942, 200943, 200944, 200945 });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return damage && damage->GetDamage() > 0 && (damage->GetSchoolMask() & ~SPELL_SCHOOL_MASK_NORMAL);
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* druid = GetTarget();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!druid || !damage)
            return;

        static std::pair<SpellSchoolMask, uint32> const schools[] =
        {
            { SPELL_SCHOOL_MASK_HOLY,   200945 },
            { SPELL_SCHOOL_MASK_FIRE,   200944 },
            { SPELL_SCHOOL_MASK_NATURE, 200943 },
            { SPELL_SCHOOL_MASK_FROST,  200942 },
            { SPELL_SCHOOL_MASK_SHADOW, 200941 },
            { SPELL_SCHOOL_MASK_ARCANE, 200940 },
        };

        bool cast = false;
        for (auto const& school : schools)
            if (damage->GetSchoolMask() & school.first)
            {
                druid->CastSpell(druid, school.second, true);
                cast = true;
            }

        if (cast)
            ArtifactTraitTest::MarkGenRan(200850);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dru_adaptive_fur::CheckProc);
        OnProc += AuraProcFn(gen_arti_dru_adaptive_fur::HandleProc);
    }
};

// Blessing of the Ashbringer (238098): "When both of your Greater Blessings are active, gain Blessing of the
// Ashbringer. Blessing of the Ashbringer grants $242981s1% Strength."
// Carrier: 242981 "Blessing of the Ashbringer" (MOD_TOTAL_STAT_PERCENTAGE Strength +4 %, infinite). The two Greater
// Blessings of the Retribution paladin in 7.3.5 are Kings (203538) and Wisdom (203539). The script counts, per
// paladin, how many of each blessing he has active on anyone and keeps 242981 on him while both counts are > 0.
class gen_arti_pal_blessing_of_the_ashbringer : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_blessing_of_the_ashbringer);

    static std::unordered_map<ObjectGuid, std::pair<int32, int32>>& Counts()
    {
        static std::unordered_map<ObjectGuid, std::pair<int32, int32>> counts;
        return counts;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 238098, 242981, 203538, 203539 });
    }

    void Update(int32 delta)
    {
        ObjectGuid casterGuid = GetCasterGUID();
        if (casterGuid.IsEmpty())
            return;

        std::pair<int32, int32>& count = Counts()[casterGuid];
        int32& slot = GetId() == 203538 ? count.first : count.second;
        slot = std::max(0, slot + delta);
        bool both = count.first > 0 && count.second > 0;
        if (count.first == 0 && count.second == 0)
            Counts().erase(casterGuid);

        Player* paladin = ObjectAccessor::GetPlayer(*GetTarget(), casterGuid);
        if (!paladin)
            return;

        if (both && paladin->HasAura(238098))
        {
            if (!paladin->HasAura(242981))
            {
                paladin->CastSpell(paladin, 242981, true);
                ArtifactTraitTest::MarkGenRan(238098);
            }
        }
        else
            paladin->RemoveAurasDueToSpell(242981);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Update(+1);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Update(-1);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_pal_blessing_of_the_ashbringer::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pal_blessing_of_the_ashbringer::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ===============================================================================================================
// Round 11 (report section "Runde 9 (24.09.2026)"): carriers found in rounds 7/8, hooks re-checked in the core
// ===============================================================================================================

// Mental Fortitude (194018): "Healing from Vampiric Touch when you are at maximum health will shield you for the same
// amount. Shield cannot exceed ${$MHP*$s1/100} damage absorbed."
// Carrier: 194022 "Mental Fortitude" ($@spelldesc194018, SCHOOL_ABSORB, "Absorbs $w1 damage.").
// Cap: $s1 of the trait = 4 (client, rank value via TraitValue) -> 4 % of max health (SimC's 8 % is NOT used).
// Earlier blocker ("no hook with the real heal amount") was wrong: AuraEffect::HandlePeriodicHealthLeechAuraTick
// (Vampiric Touch E1 = PERIODIC_LEECH, aura 53) calls ProcSkillsAndAuras(PROC_FLAG_DONE_PERIODIC, PROC_SPELL_TYPE_HEAL,
// &healInfo) AFTER HealBySpell - the trait's own client proc mask is exactly 0x40000 (DONE_PERIODIC).
// "healing when you are at maximum health" = the part of the tick that did not heal (heal - effective heal); a tick
// at full health is fully converted, the tick that tops the priest up only with its excess.
class gen_arti_pri_mental_fortitude : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_mental_fortitude);

    static constexpr uint32 SPELL_VAMPIRIC_TOUCH = 34914;
    static constexpr uint32 SPELL_MENTAL_FORTITUDE_SHIELD = 194022;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 194018, SPELL_VAMPIRIC_TOUCH, SPELL_MENTAL_FORTITUDE_SHIELD });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        HealInfo* heal = eventInfo.GetHealInfo();
        if (!heal || !heal->GetSpellInfo() || heal->GetSpellInfo()->Id != SPELL_VAMPIRIC_TOUCH)
            return false;
        return heal->GetTarget() == GetTarget() && heal->GetHeal() > heal->GetEffectiveHeal();
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* priest = GetTarget();
        HealInfo* heal = eventInfo.GetHealInfo();
        if (!priest || !heal)
            return;

        int32 pct = TraitValue(priest, 194018);
        if (pct <= 0)
            return;

        int32 cap = int32(priest->CountPctFromMaxHealth(pct));
        int32 overheal = int32(heal->GetHeal() - heal->GetEffectiveHeal());
        if (cap <= 0 || overheal <= 0)
            return;

        if (AuraEffect* shield = priest->GetAuraEffect(SPELL_MENTAL_FORTITUDE_SHIELD, EFFECT_0, priest->GetGUID()))
        {
            shield->ChangeAmount(std::min(shield->GetAmount() + overheal, cap));
            shield->GetBase()->RefreshDuration();
        }
        else
            priest->CastCustomSpell(SPELL_MENTAL_FORTITUDE_SHIELD, SPELLVALUE_BASE_POINT0, std::min(overheal, cap), priest, true);

        ArtifactTraitTest::MarkGenRan(194018);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pri_mental_fortitude::CheckProc);
        OnProc += AuraProcFn(gen_arti_pri_mental_fortitude::HandleProc);
    }
};

// Souldrinker (238114): "Overhealing done by Death Strike and Consumption increases your maximum health by $s1% of the
// overheal amount, stacking up to a maximum of $s2% of your health."
// Carrier: 240558 "Souldrinker" ($@spelldesc238114, E0 MOD_INCREASE_HEALTH_PERCENT "Maximum health increased by $w1%",
// 15 s). Values: trait E0 = 50 (% of the overheal), E1 = 30 (cap, % of health) - client.
// Conversion absolute -> percent (derived from the core, not assumed): aura 133 is applied by
// HandleAuraModIncreaseHealthPercent as UNIT_MOD_HEALTH TOTAL_PCT, i.e. p % of the max health WITHOUT this aura. An
// absolute gain X therefore is X / (maxHealth / (1 + p/100)) * 100 percent, and the cap "$s2% of your health" is
// directly p <= 30. The exact (fractional) percent is kept per DK, the aura carries the integer part (aura amounts are
// int32 in this core); it resets when 240558 is gone. "stacking" = the gains add up, each gain refreshes the 15 s.
// Hook (round 13 rework): the trait's client proc mask (0x4000, DONE_SPELL_MAGIC_DMG_CLASS_POS) never matches the Death
// Strike heal 45470 or the Consumption heal 205224 in this core (no SpellCategories row -> DmgClass NONE -> 0x400).
// Round 11 therefore scripted the heal spell 45470 itself (SpellScript, AfterHit). That could never work: in AfterHit
// GetHitHeal() returns m_healing, which Spell::DoAllEffectOnTarget has already overwritten with the EFFECTIVE heal
// (Spell.cpp: m_healing = healInfo.GetEffectiveHeal()), so "heal - health gained" was always ~0 and the buff was never
// applied. Now it is an AuraScript on the trait (238114) with a spell_proc row adding 0x400 (SpellTypeMask HEAL,
// SpellPhaseMask HIT): the HealInfo passed to the proc carries both the raw heal (incl. crit, minus heal absorbs) and the
// effective heal, exactly like gen_arti_pri_mental_fortitude. Consumption heals since round 13 (spell_dk_consumption).
class gen_arti_dk_souldrinker : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_souldrinker);

    static constexpr uint32 SPELL_SOULDRINKER_TRAIT = 238114;
    static constexpr uint32 SPELL_SOULDRINKER_BUFF = 240558;
    static constexpr uint32 SPELL_DEATH_STRIKE_HEAL = 45470;
    static constexpr uint32 SPELL_CONSUMPTION_HEAL = 205224;

    struct Stack
    {
        double Pct = 0.0;       // exact accumulated percent
        uint32 LastGainMs = 0;  // getMSTime() of the last gain
    };

    static std::unordered_map<ObjectGuid, Stack>& Stacks()
    {
        static std::unordered_map<ObjectGuid, Stack> stacks;
        return stacks;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SOULDRINKER_TRAIT, SPELL_SOULDRINKER_BUFF, SPELL_DEATH_STRIKE_HEAL, SPELL_CONSUMPTION_HEAL });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        HealInfo* heal = eventInfo.GetHealInfo();
        if (!heal || !heal->GetSpellInfo())
            return false;

        uint32 spellId = heal->GetSpellInfo()->Id;
        if (spellId != SPELL_DEATH_STRIKE_HEAL && spellId != SPELL_CONSUMPTION_HEAL)
            return false;

        return heal->GetHealer() == GetTarget() && heal->GetTarget() == GetTarget() && heal->GetHeal() > heal->GetEffectiveHeal();
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* dk = GetTarget();
        HealInfo* heal = eventInfo.GetHealInfo();
        if (!dk || !heal)
            return;

        int64 overheal = int64(heal->GetHeal()) - int64(heal->GetEffectiveHeal());
        if (overheal <= 0)
            return;

        int32 pctOfOverheal = TraitValue(dk, SPELL_SOULDRINKER_TRAIT, EFFECT_0);
        int32 capPct = TraitValue(dk, SPELL_SOULDRINKER_TRAIT, EFFECT_1);
        if (pctOfOverheal <= 0 || capPct <= 0)
            return;

        SpellInfo const* buffInfo = sSpellMgr->GetSpellInfo(SPELL_SOULDRINKER_BUFF);
        uint32 window = buffInfo && buffInfo->GetMaxDuration() > 0 ? uint32(buffInfo->GetMaxDuration()) : 0;

        AuraEffect* buff = dk->GetAuraEffect(SPELL_SOULDRINKER_BUFF, EFFECT_0, dk->GetGUID());
        Stack& stack = Stacks()[dk->GetGUID()];
        // the stack ends with the buff; a gain below 1 % (no buff yet) is kept for the buff's duration
        if (!buff && (stack.Pct >= 1.0 || GetMSTimeDiffToNow(stack.LastGainMs) > window))
            stack.Pct = 0.0;

        int32 currentPct = buff ? buff->GetAmount() : 0;
        double maxHealthWithoutBuff = double(dk->GetMaxHealth()) / (1.0 + currentPct / 100.0);
        if (maxHealthWithoutBuff <= 0.0)
            return;

        double gain = double(overheal) * pctOfOverheal / 100.0;
        stack.Pct = std::min(stack.Pct + gain / maxHealthWithoutBuff * 100.0, double(capPct));
        stack.LastGainMs = getMSTime();
        int32 newPct = int32(stack.Pct);
        if (newPct <= 0)
            return;

        if (buff)
        {
            if (newPct != buff->GetAmount())
                buff->ChangeAmount(newPct);
            buff->GetBase()->RefreshDuration();
        }
        else
            dk->CastCustomSpell(SPELL_SOULDRINKER_BUFF, SPELLVALUE_BASE_POINT0, newPct, dk, true);

        ArtifactTraitTest::MarkGenRan(SPELL_SOULDRINKER_TRAIT);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dk_souldrinker::CheckProc);
        OnProc += AuraProcFn(gen_arti_dk_souldrinker::HandleProc);
    }
};

// Touch of the Moon (203018): "When you take damage, you have a $h% chance to heal yourself for ${$213672s1*$s1}.
// Cannot occur more than once every $proccooldown sec."
// Earlier reason "Zahlen unvollstaendig" was wrong: everything is in the client data - $h = ProcChance 20,
// $proccooldown = ProcCategoryRecovery 20000, the heal 213672 is E1 (PROC_TRIGGER_SPELL) of the trait, so the core
// already casts it on proc. What the core does NOT do is the "*$s1": $s1 is the trait E0 (DUMMY, BasePoints 1,
// AuraPointsOverride 1..8 = rank value in artifactpowerrank.csv). The heal of 213672 is multiplied by that value.
class gen_arti_dru_touch_of_the_moon : public SpellScript
{
    PrepareSpellScript(gen_arti_dru_touch_of_the_moon);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 203018 });
    }

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        int32 mult = TraitValue(GetCaster(), 203018);
        if (mult <= 1)
            return;

        SetHitHeal(GetHitHeal() * mult);
        ArtifactTraitTest::MarkGenRan(203018);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_dru_touch_of_the_moon::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// Critical Focus (191328): "$?s214579[Sidewinders generates][Arcane Shot critical strikes generate]
// $?s214579[$s2][$s1] additional Focus."
// E1 (ADD_FLAT_MODIFIER on the Sidewinders mask) covers the Sidewinders branch in the core already. The Arcane Shot
// branch is E0 (DUMMY, $s1 = 5) with the trait's client proc mask 0x100 (DONE_SPELL_RANGED_DMG_CLASS; Arcane Shot
// 185358 has DefenseType 3 = ranged in spellcategories.csv). Focus is given for critical Arcane Shot hits only while
// Sidewinders (214579) is not known (Sidewinders replaces that branch, see the tooltip condition).
class gen_arti_hun_critical_focus : public AuraScript
{
    PrepareAuraScript(gen_arti_hun_critical_focus);

    static constexpr uint32 SPELL_ARCANE_SHOT = 185358;
    static constexpr uint32 SPELL_SIDEWINDERS = 214579;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 191328, SPELL_ARCANE_SHOT });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!eventInfo.GetSpellInfo() || eventInfo.GetSpellInfo()->Id != SPELL_ARCANE_SHOT)
            return false;
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;
        Player* hunter = GetTarget()->ToPlayer();
        return hunter && !hunter->HasSpell(SPELL_SIDEWINDERS);
    }

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* hunter = GetTarget();
        int32 focus = TraitValue(hunter, 191328);
        if (focus <= 0)
            return;

        hunter->EnergizeBySpell(hunter, 191328, focus, POWER_FOCUS);
        ArtifactTraitTest::MarkGenRan(191328);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_hun_critical_focus::CheckProc);
        OnProc += AuraProcFn(gen_arti_hun_critical_focus::HandleProc);
    }
};

// ===============================================================================================================
// Round 12 (report section "Runde 10 (24.09.2026, abends)"): the four core blockers of round 11
// ===============================================================================================================

// Faith's Armor (209225, artifact power 1129): "Increases armor by $s2% while you are below $s1% health."
// Mechanism (all client data): trait E0 aura 468 below 40 % -> 211903 (E0 MOD_RESISTANCE_PCT armor, "Armor increased by
// $w1%"; E1 aura 468 above 40 % -> 211905 = REMOVE_AURA_2 of 211903). Aura 468 is now handled by the core for exactly
// these spells (AuraEffect::IsTriggerSpellOnHealthPctEnabled).
// The 50-vs-10 conflict resolved from artifactpowerrank.csv: power 1129 has EIGHT rank spells (209225, 211912-211916,
// 239295, 239296) whose E1 ($s2) is 10/20/30/40/50/60/70/80, and all eight trigger the SAME buff 211903 with a static
// E0 of 50 - i.e. exactly the rank-5 value. The buff text uses $w1 (the applied amount), not $s1. A fixed 50 would make
// seven of eight rank tooltips wrong; the only reading that fits all client rows is "the buff takes $s2 of the rank
// that triggered it". This script does exactly that, no number is invented.
class gen_arti_pal_faiths_armor : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_faiths_armor);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 209225, 211903 });
    }

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        static uint32 const ranks[] = { 239296, 239295, 211916, 211915, 211914, 211913, 211912, 209225 }; // highest first
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;

        for (uint32 rankSpell : ranks)
        {
            if (!owner->HasAura(rankSpell))
                continue;

            if (int32 pct = TraitValue(owner, rankSpell, EFFECT_1))
            {
                amount = pct;
                ArtifactTraitTest::MarkGenRan(209225);
            }
            return;
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_pal_faiths_armor::CalcAmount, EFFECT_0, SPELL_AURA_MOD_RESISTANCE_PCT);
    }
};

// Forbearant Faithful (209376): "Each active Forbearance you have caused increases the cooldown recovery rate of all
// your Forbearance causing abilities by $s2%."
// Client: E0 aura 143 (TrinityCore master: MOD_RECOVERY_RATE_BY_SPELL_LABEL) with MiscValue 177 = SpellLabel 177
// (spelllabel.csv: 642 Divine Shield, 633 Lay on Hands, 204018 Blessing of Spellwarding, 229854 "Dark Energy" - no
// paladin spell); E1 DUMMY 50 = $s2; E2 aura 148 (TC master: MOD_CHARGE_RECOVERY_RATE) with MiscValue 1392 = charge
// category of 1022 Blessing of Protection (spellcategories.csv). E0/E2 carry BP 0 - the value is dynamic.
// Why a script and not the core: this core has no SpellLabel store (no DB2 store, no hotfix table) and label 16 alone
// has 5,379 spells, so a generic aura 143 is out of scope; aura 148 is still declared as the obsolete
// SPELL_AURA_RETAIN_COMBO_POINTS here (only 152173 Serenity and this trait use 148 in 7.3.5, report section 13.3).
// Rate: TC semantics, recovery rate = 100 / (100 + amount), amount = $s2 x count. Applied at cooldown / recharge start
// (PlayerScript hooks) and to running cooldowns whenever the count changes (SpellHistory::ScaleRemaining*).
namespace ForbearantFaithful
{
    static constexpr uint32 SPELL_TRAIT = 209376;
    static constexpr uint32 SPELL_FORBEARANCE = 25771;
    static constexpr uint32 SPELL_BLESSING_OF_PROTECTION = 1022;
    static uint32 const LabelSpells[] = { 642, 633, 204018 };     // SpellLabel 177, paladin spells only

    // caster -> units that currently carry a Forbearance from that caster
    static std::unordered_map<ObjectGuid, std::unordered_set<ObjectGuid>> Active;

    uint32 Count(ObjectGuid const& caster)
    {
        auto itr = Active.find(caster);
        return itr == Active.end() ? 0 : uint32(itr->second.size());
    }

    // 100 / (100 + $s2 * count); 1.0 when the trait is missing
    float Rate(Player* paladin, uint32 count)
    {
        int32 pct = TraitValue(paladin, SPELL_TRAIT, EFFECT_1);
        if (pct <= 0 || !count)
            return 1.0f;
        return 100.0f / (100.0f + float(pct) * float(count));
    }

    uint32 ChargeCategory()
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_TRAIT))
            if (SpellEffectInfo const* effect = info->GetEffect(EFFECT_2))
                return uint32(effect->MiscValue); // 1392
        return 0;
    }

    bool IsLabelSpell(uint32 spellId)
    {
        for (uint32 id : LabelSpells)
            if (id == spellId)
                return true;
        return false;
    }

    void OnCountChanged(Player* paladin, uint32 oldCount, uint32 newCount)
    {
        if (!paladin || oldCount == newCount || !paladin->HasAura(SPELL_TRAIT))
            return;

        // remaining time x (100 + pct*old) / (100 + pct*new)
        float modChange = Rate(paladin, newCount) / Rate(paladin, oldCount);
        for (uint32 spellId : LabelSpells)
            paladin->GetSpellHistory()->ScaleRemainingCooldown(spellId, modChange);
        if (uint32 category = ChargeCategory())
            paladin->GetSpellHistory()->ScaleRemainingChargeRecovery(category, modChange);

        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }
}

// bound to 25771 Forbearance: keeps the per-caster count
class gen_arti_pal_forbearant_faithful : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_forbearant_faithful);

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        ObjectGuid casterGuid = GetCasterGUID();
        Unit* target = GetTarget();
        if (casterGuid.IsEmpty() || !target)
            return;

        uint32 oldCount = ForbearantFaithful::Count(casterGuid);
        ForbearantFaithful::Active[casterGuid].insert(target->GetGUID());
        uint32 newCount = ForbearantFaithful::Count(casterGuid);

        if (Unit* caster = GetCaster())
            ForbearantFaithful::OnCountChanged(caster->ToPlayer(), oldCount, newCount);
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        ObjectGuid casterGuid = GetCasterGUID();
        Unit* target = GetTarget();
        if (casterGuid.IsEmpty() || !target)
            return;

        uint32 oldCount = ForbearantFaithful::Count(casterGuid);
        auto itr = ForbearantFaithful::Active.find(casterGuid);
        if (itr != ForbearantFaithful::Active.end())
        {
            itr->second.erase(target->GetGUID());
            if (itr->second.empty())
                ForbearantFaithful::Active.erase(itr);
        }
        uint32 newCount = ForbearantFaithful::Count(casterGuid);

        if (Unit* caster = GetCaster())
            ForbearantFaithful::OnCountChanged(caster->ToPlayer(), oldCount, newCount);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_pal_forbearant_faithful::AfterApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pal_forbearant_faithful::AfterRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// cooldown / recharge start: apply the recovery rate of the forbearances that are active at that moment
class gen_arti_pal_forbearant_faithful_cooldowns : public PlayerScript
{
public:
    gen_arti_pal_forbearant_faithful_cooldowns() : PlayerScript("gen_arti_pal_forbearant_faithful_cooldowns") { }

    void OnCooldownStart(Player* player, SpellInfo const* spellInfo, uint32 /*itemId*/, int32& cooldown, uint32& /*categoryId*/, int32& categoryCooldown) override
    {
        if (!ForbearantFaithful::IsLabelSpell(spellInfo->Id) || !player->HasAura(ForbearantFaithful::SPELL_TRAIT))
            return;

        float rate = ForbearantFaithful::Rate(player, ForbearantFaithful::Count(player->GetGUID()));
        if (rate >= 1.0f)
            return;

        if (cooldown > 0)
            cooldown = int32(cooldown * rate);
        if (categoryCooldown > 0)
            categoryCooldown = int32(categoryCooldown * rate);
        ArtifactTraitTest::MarkGenRan(ForbearantFaithful::SPELL_TRAIT);
    }

    void OnChargeRecoveryTimeStart(Player* player, uint32 chargeCategoryId, int32& chargeRecoveryTime) override
    {
        if (chargeCategoryId != ForbearantFaithful::ChargeCategory() || !player->HasAura(ForbearantFaithful::SPELL_TRAIT))
            return;

        float rate = ForbearantFaithful::Rate(player, ForbearantFaithful::Count(player->GetGUID()));
        if (rate < 1.0f && chargeRecoveryTime > 0)
        {
            chargeRecoveryTime = int32(chargeRecoveryTime * rate);
            ArtifactTraitTest::MarkGenRan(ForbearantFaithful::SPELL_TRAIT);
        }
    }
};

// Double Doom (191741): "Sudden Doom can now accumulate ${$m1+1} charges and will occur $s2% more frequently."
// E0 ADD_FLAT_MODIFIER op 37 (SPELLMOD_STACK_AMOUNT2) +1 on 81340 - already applied by the core in
// Aura::GetMaxStackAmount, so 81340 can stack to 2.
// E1 is a DUMMY, but it carries MiscValue 18 (= SPELLMOD_CHANCE_OF_SUCCESS) and class mask [3]=2 (= 49530 Sudden Doom,
// the RPPM passive, RPPM 130 = 2.875). The core already applies SPELLMOD_CHANCE_OF_SUCCESS to the RPPM chance
// (Aura::CalcProcChance, after CalcPPMProcChance). The script only turns the DUMMY into that modifier (pattern of
// spell_pri_improved_power_word_shield), op, sign, value and mask are the client's. No new core hook needed.
class gen_arti_dk_double_doom : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_double_doom);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 191741, 49530, 81340 });
    }

    void CalcSpellMod(AuraEffect const* aurEff, SpellModifier*& spellMod)
    {
        if (!spellMod)
        {
            spellMod = new SpellModifier(GetAura());
            spellMod->op = SpellModOp(aurEff->GetMiscValue());          // 18, from the client
            spellMod->type = SPELLMOD_PCT;
            spellMod->spellId = GetId();
            spellMod->mask = GetSpellInfo()->GetEffect(aurEff->GetEffIndex())->SpellClassMask;
        }
        spellMod->value = aurEff->GetAmount();                            // 15
    }

    void Register() override
    {
        DoEffectCalcSpellMod += AuraEffectCalcSpellModFn(gen_arti_dk_double_doom::CalcSpellMod, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Double Doom, second half: 81340 has ProcCharges 1, so the first Death Coil would remove BOTH stacks. With 2 stacks a
// consuming proc takes one stack instead of the charge ("accumulate 2 charges" = the E0 stack modifier).
class gen_arti_dk_double_doom_charges : public AuraScript
{
    PrepareAuraScript(gen_arti_dk_double_doom_charges);

    void PrepareProc(ProcEventInfo& /*eventInfo*/)
    {
        if (GetStackAmount() <= 1)
            return;

        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(191741))
            return;

        PreventDefaultAction();                                          // keep the charge
        GetAura()->ModStackAmount(-1, AURA_REMOVE_BY_DEFAULT, false, false);
        ArtifactTraitTest::MarkGenRan(191741);
    }

    void Register() override
    {
        DoPrepareProc += AuraProcFn(gen_arti_dk_double_doom_charges::PrepareProc);
    }
};

// Divine Tempest (186773): "Ashbringer projects Divine Storm forward $s1 yds, damaging all enemies in its path.
// Also increases damage dealt by Divine Storm by $s2%."  (E1 = ADD_PCT_MODIFIER, already native)
// Carrier found by name search: 186775 "Divine Tempest" ($@spelldesc186773) = CREATE_AREATRIGGER misc 4366, 1750 ms.
// World DB (read): spell_areatrigger 4366 -> areatrigger_template 9110 = cylinder radius 8 / height 6 (same 8 yd as
// Divine Storm's own radius index 14), TimeToTarget 1429 ms, VerifiedBuild 26365 (sniffed) - but no spline rows.
// The script casts 186775 when Divine Storm is used with the trait and moves the trigger $s1 (20) yd forward over the
// sniffed TimeToTarget. Every enemy the trigger touches takes Divine Storm damage (224239) once; enemies already hit by
// the normal 8-yd Divine Storm are skipped (reading "projects forward" as extending the storm, not doubling it).
namespace DivineTempest
{
    static constexpr uint32 SPELL_TRAIT = 186773;
    static constexpr uint32 SPELL_TEMPEST_AT = 186775;
    static constexpr uint32 SPELL_DIVINE_STORM_DAMAGE = 224239;

    // caster -> enemies hit by the current Divine Storm (normal area + tempest)
    static std::unordered_map<ObjectGuid, GuidUnorderedSet> HitByStorm;
}

class gen_arti_pal_divine_tempest : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_divine_tempest);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ DivineTempest::SPELL_TRAIT, DivineTempest::SPELL_TEMPEST_AT, DivineTempest::SPELL_DIVINE_STORM_DAMAGE });
    }

    void CollectTargets(std::list<WorldObject*>& targets)
    {
        GuidUnorderedSet& hit = DivineTempest::HitByStorm[GetCaster()->GetGUID()];
        hit.clear();
        for (WorldObject* target : targets)
            hit.insert(target->GetGUID());
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster->HasAura(DivineTempest::SPELL_TRAIT))
            return;

        caster->CastSpell(caster->GetPosition(), DivineTempest::SPELL_TEMPEST_AT, true);
        ArtifactTraitTest::MarkGenRan(DivineTempest::SPELL_TRAIT);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(gen_arti_pal_divine_tempest::CollectTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        AfterCast += SpellCastFn(gen_arti_pal_divine_tempest::HandleAfterCast);
    }
};

// areatrigger_template 9110 (spell misc 4366, spell 186775)
struct at_arti_pal_divine_tempest : AreaTriggerAI
{
    at_arti_pal_divine_tempest(AreaTrigger* areatrigger) : AreaTriggerAI(areatrigger) { }

    void OnInitialize() override
    {
        Unit* caster = at->GetCaster();
        if (!caster)
            return;

        int32 distance = TraitValue(caster, DivineTempest::SPELL_TRAIT, EFFECT_0); // 20
        uint32 timeToTarget = at->GetMiscTemplate() ? at->GetMiscTemplate()->TimeToTarget : 0; // 1429 (sniffed)
        if (distance <= 0 || !timeToTarget)
            return;

        float o = caster->GetOrientation();
        std::vector<G3D::Vector3> splinePoints;
        splinePoints.push_back(G3D::Vector3(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ()));
        splinePoints.push_back(G3D::Vector3(caster->GetPositionX() + distance * std::cos(o), caster->GetPositionY() + distance * std::sin(o), caster->GetPositionZ()));
        at->InitSplines(splinePoints, timeToTarget);
    }

    void OnUnitEnter(Unit* unit) override
    {
        Unit* caster = at->GetCaster();
        if (!caster || !unit->IsAlive() || !caster->IsValidAttackTarget(unit))
            return;

        GuidUnorderedSet& hit = DivineTempest::HitByStorm[caster->GetGUID()];
        if (!hit.insert(unit->GetGUID()).second)
            return;

        caster->CastSpell(unit, DivineTempest::SPELL_DIVINE_STORM_DAMAGE, true);
    }
};

// Barrier for the Devoted (197815): "Increases healing done by Atonement by $s1% on targets inside your Power Word:
// Barrier."  $s1 = 100 (client, single rank). Earlier reason "state/hook missing" no longer holds: the core's
// at_pri_power_word_barrier (spell_priest.cpp) puts 81782 "Power Word: Barrier" on every ally inside the dome with the
// priest as caster, and spell_pri_atonement casts the Atonement heal 81751 per target - both are observable here.
class gen_arti_pri_barrier_for_the_devoted : public SpellScript
{
    PrepareSpellScript(gen_arti_pri_barrier_for_the_devoted);

    static constexpr uint32 SPELL_TRAIT = 197815;
    static constexpr uint32 SPELL_POWER_WORD_BARRIER_BUFF = 81782;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_POWER_WORD_BARRIER_BUFF });
    }

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !target->HasAura(SPELL_POWER_WORD_BARRIER_BUFF, caster->GetGUID()))
            return;

        int32 pct = TraitValue(caster, SPELL_TRAIT);
        if (pct <= 0)
            return;

        SetHitHeal(int32(GetHitHeal() * (100.0f + pct) / 100.0f));
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_pri_barrier_for_the_devoted::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// ===============================================================================================================
// Round 13 (autonomous round, 24.09.2026; report section "Runde 11 (24.09.2026, spaet)")
// ===============================================================================================================

// Soul Skin (218567), open assumption of round 12 resolved: "When your health drops below $s1%, Soul Link increases to
// split $s3% of all damage you take with your demon pet." The trait chain itself runs via aura 468 (218565 below 35 %,
// 218566 removes it above 35 %). Two things were missing, both checked in the core:
// 1. Soul Link was never active: 108415 (Demonology spec spell, specializationspells.csv SpecID 266, passive, E0 DUMMY)
//    had no consumer, SPELL_WARLOCK_SOUL_LINK_DUMMY_AURA 108446 is declared in spell_warlock.cpp but never used.
//    108446 is SPELL_EFFECT_APPLY_AREA_AURA_OWNER (effect 143) with SPLIT_DAMAGE_PCT 20 % - an aura the PET casts onto its
//    owner (split damage goes to the aura caster, Unit.cpp). The core mechanism for "passive dummy of the player ->
//    pet casts aura X" is spell_pet_auras (SpellEffects.cpp EffectDummy -> Player::AddPetAura -> Pet::CastPetAura);
//    SQL candidate: spell_pet_auras (108415, 0, 0, 108446).
// 2. 218565 is ADD_PCT_MODIFIER ALL_EFFECTS +100 % on 108446 (class mask [2] 16384). Pet-cast spells use the owner's
//    spell mods (Unit::GetSpellModOwner), so a NEW 108446 is right, but AuraEffect::ApplySpellMod only recalculates
//    auras whose caster is the player itself - the pet-cast Soul Link on the warlock is skipped. This script does that one
//    recalculation for 108446 only (no core change), on apply and on remove of 218565.
class gen_arti_lock_soul_skin : public AuraScript
{
    PrepareAuraScript(gen_arti_lock_soul_skin);

    static constexpr uint32 SPELL_SOUL_LINK_AURA = 108446;
    static constexpr uint32 SPELL_SOUL_SKIN_TRAIT = 218567;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SOUL_LINK_AURA, SPELL_SOUL_SKIN_TRAIT });
    }

    void Recalculate()
    {
        Unit* warlock = GetTarget();
        if (!warlock)
            return;

        std::vector<Aura*> soulLinks;
        for (AuraEffect* eff : warlock->GetAuraEffectsByType(SPELL_AURA_SPLIT_DAMAGE_PCT))
            if (eff->GetId() == SPELL_SOUL_LINK_AURA)
                soulLinks.push_back(eff->GetBase());

        for (Aura* soulLink : soulLinks)
            soulLink->RecalculateAmountOfEffects();

        if (!soulLinks.empty())
            ArtifactTraitTest::MarkGenRan(SPELL_SOUL_SKIN_TRAIT);
    }

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Recalculate();
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Recalculate();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_lock_soul_skin::AfterApply, EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_lock_soul_skin::AfterRemove, EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Tyranny of Pain (238099): "Pain Suppression also heals the target for $s1% of damage taken during Pain Suppression."
// Everything is in the client (round 7 reason "not observable without a proc entry" is resolved by the data itself):
// - Pain Suppression 33206 carries its own ProcTypeMask 664232 (all TAKEN damage flags: melee/ranged auto attack,
//   melee/ranged/none/magic negative spells, periodic) with ProcChance 100 - Blizzard observes the damage on the aura.
// - 33206 E1 is SPELL_AURA_PERIODIC_DUMMY (226) with a 500 ms period and no value: the carrier tick.
// - 242094 "Tyranny of Pain" ($@spelldesc238099) is a HEAL (BP 1) on TARGET_UNIT_TARGET_ANY.
// - trait E0 = 33 ($s1), one rank (artifactpowerrank.csv ArtifactPower 1566, no override).
// The core creates no proc entry for 33206 (aura 87/226 are no trigger auras), so a spell_proc row with the client mask
// is needed (gen_priest.sql, round 13). Damage is the damage actually taken (DamageInfo after absorbs); it is collected
// per aura and healed on each 500 ms tick, the rest when Pain Suppression ends (so nothing taken "during" it is lost).
// Only when the Pain Suppression caster (the priest) has the trait.
class gen_arti_pri_tyranny_of_pain : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_tyranny_of_pain);

    static constexpr uint32 SPELL_TRAIT = 238099;
    static constexpr uint32 SPELL_TYRANNY_OF_PAIN_HEAL = 242094;

    uint64 _damageTaken = 0;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_TYRANNY_OF_PAIN_HEAL });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage() || damage->GetVictim() != GetTarget())
            return false;

        Unit* priest = GetCaster();
        return priest && priest->HasAura(SPELL_TRAIT);
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        if (DamageInfo* damage = eventInfo.GetDamageInfo())
            _damageTaken += damage->GetDamage();
    }

    void Flush()
    {
        if (!_damageTaken)
            return;

        uint64 taken = _damageTaken;
        _damageTaken = 0;

        Unit* priest = GetCaster();
        Unit* target = GetTarget();
        if (!priest || !target || !target->IsAlive())
            return;

        int32 pct = TraitValue(priest, SPELL_TRAIT);
        if (pct <= 0)
            return;

        uint64 heal = CalculatePct(taken, pct);
        if (!heal)
            return;

        priest->CastCustomSpell(SPELL_TYRANNY_OF_PAIN_HEAL, SPELLVALUE_BASE_POINT0, int32(std::min<uint64>(heal, uint64(std::numeric_limits<int32>::max()))), target, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Flush();
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Flush();
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pri_tyranny_of_pain::CheckProc);
        OnProc += AuraProcFn(gen_arti_pri_tyranny_of_pain::HandleProc);
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_pri_tyranny_of_pain::HandleTick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pri_tyranny_of_pain::AfterRemove, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Terms of Engagement (203754): "The remaining cooldown on Harpoon is reset when you kill an enemy."
// Harpoon 190925 has no RecoveryTime but ChargeCategory 1635 ("Class - Hunter - Harpoon", MaxCharges 1, 30 s), so its
// "cooldown" is the charge recovery -> SpellHistory::ResetCharges(1635) (category read from the SpellInfo, not hard-coded).
// "you kill an enemy" is read literally: the hunter lands the killing blow on a hostile creature or player (the core's
// PlayerScript kill hooks, Unit::Kill -> ScriptMgr::OnCreatureKill / OnPVPKill). Pet kills do not count here.
class gen_arti_hun_terms_of_engagement : public PlayerScript
{
public:
    gen_arti_hun_terms_of_engagement() : PlayerScript("gen_arti_hun_terms_of_engagement") { }

    static constexpr uint32 SPELL_TRAIT = 203754;
    static constexpr uint32 SPELL_HARPOON = 190925;

    static void Handle(Player* killer, Unit* killed)
    {
        if (!killer || !killed || killer == killed || !killer->HasAura(SPELL_TRAIT) || killer->IsFriendlyTo(killed))
            return;

        SpellInfo const* harpoon = sSpellMgr->GetSpellInfo(SPELL_HARPOON);
        if (!harpoon)
            return;

        if (harpoon->ChargeCategoryId)
            killer->GetSpellHistory()->ResetCharges(harpoon->ChargeCategoryId);
        killer->GetSpellHistory()->ResetCooldown(SPELL_HARPOON, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void OnCreatureKill(Player* killer, Creature* killed) override { Handle(killer, killed); }
    void OnPVPKill(Player* killer, Player* killed) override { Handle(killer, killed); }
};

// Death Art (195266): "Reduces the remaining cooldown on Touch of Death by $s1% if the target dies while under the
// effects of Touch of Death or from Touch of Death." $s1 = rank value 10..80 (artifactpowerrank.csv ArtifactPower 822).
// Touch of Death 115080 (E0 PERIODIC_DUMMY) deals its damage 229980 from its last tick while the aura is still applied
// (spell_monk_touch_of_death::OnTick), so both cases end with the aura removed by AURA_REMOVE_BY_DEATH on the target.
// Only the caster's own Touch of Death counts (the aura's caster is the monk).
class gen_arti_monk_death_art : public AuraScript
{
    PrepareAuraScript(gen_arti_monk_death_art);

    static constexpr uint32 SPELL_TRAIT = 195266;
    static constexpr uint32 SPELL_TOUCH_OF_DEATH = 115080;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_TOUCH_OF_DEATH });
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;

        Unit* monk = GetCaster();
        if (!monk || !monk->HasAura(SPELL_TRAIT))
            return;

        int32 pct = TraitValue(monk, SPELL_TRAIT);
        SpellInfo const* touchOfDeath = sSpellMgr->GetSpellInfo(SPELL_TOUCH_OF_DEATH);
        if (pct <= 0 || !touchOfDeath)
            return;

        uint32 remaining = monk->GetSpellHistory()->GetRemainingCooldown(touchOfDeath);
        int32 reduction = int32(CalculatePct(uint64(remaining), std::min(pct, 100)));
        if (reduction <= 0)
            return;

        monk->GetSpellHistory()->ModifyCooldown(SPELL_TOUCH_OF_DEATH, -reduction);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_monk_death_art::AfterRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Unbreakable Will (182234): "When unable to act in combat for more than $s1 seconds, Ashbringer will break you free. Can
// occur only once every $182496d." All pieces are client carriers named "Unbreakable Will" ($@spelldesc182234):
// - trait 182234: DUMMY auras (E0 $s1 = 2), ProcTypeMask 698912 = every TAKEN spell flag (melee/ranged/none/magic, pos
//   and neg, periodic), ProcChance 101 -> checked whenever a spell lands on the paladin (the CC itself included);
// - 182497 "You are crowd controlled!": DUMMY, duration index 39 = 2000 ms (= $s1 seconds) - the tracking debuff;
// - 182531 "Immune to Crowd Control effects.": aura 77 MECHANIC_IMMUNITY for mechanics charm, disoriented, fear, freeze,
//   horror, knockout, polymorph, sapped, sleep, stun (its MiscValues), 2000 ms - the break-free;
// - 182496 "The Ashbringer recently broke you out of a crowd control effect.": duration index 4 = 120 s - the lockout.
// "Unable to act" = an aura with one of 182531's mechanics (the set is read from 182531, not hard-coded). The break frees
// only when a CC aura of that set has lasted at least $s1 seconds when the tracker expires.
namespace UnbreakableWill
{
    static constexpr uint32 SPELL_TRAIT = 182234;
    static constexpr uint32 SPELL_TRACKER = 182497;
    static constexpr uint32 SPELL_IMMUNITY = 182531;
    static constexpr uint32 SPELL_LOCKOUT = 182496;

    static uint32 MechanicMask()
    {
        uint32 mask = 0;
        if (SpellInfo const* immunity = sSpellMgr->GetSpellInfo(SPELL_IMMUNITY))
            for (SpellEffectInfo const* effect : immunity->GetEffectsForDifficulty(DIFFICULTY_NONE))
                if (effect && effect->ApplyAuraName == SPELL_AURA_MECHANIC_IMMUNITY && effect->MiscValue > 0 && effect->MiscValue < MAX_MECHANIC)
                    mask |= 1u << effect->MiscValue;
        return mask;
    }

    static bool IsControlledFor(Unit* unit, uint32 mechanicMask, int32 minMs)
    {
        for (auto const& app : unit->GetAppliedAuras())
        {
            Aura const* aura = app.second->GetBase();
            if (!(aura->GetSpellInfo()->GetAllEffectsMechanicMask() & mechanicMask))
                continue;

            int64 elapsedMs = aura->GetMaxDuration() > 0
                ? int64(aura->GetMaxDuration()) - int64(aura->GetDuration())
                : int64(time(nullptr) - aura->GetApplyTime()) * IN_MILLISECONDS;
            if (elapsedMs >= minMs)
                return true;
        }
        return false;
    }
}

class gen_arti_pal_unbreakable_will : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_unbreakable_will);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ UnbreakableWill::SPELL_TRAIT, UnbreakableWill::SPELL_TRACKER, UnbreakableWill::SPELL_IMMUNITY, UnbreakableWill::SPELL_LOCKOUT });
    }

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* paladin = GetTarget();
        if (!paladin->IsInCombat() || paladin->HasAura(UnbreakableWill::SPELL_LOCKOUT) || paladin->HasAura(UnbreakableWill::SPELL_TRACKER))
            return false;

        uint32 mask = UnbreakableWill::MechanicMask();
        return mask && paladin->HasAuraWithMechanic(mask);
    }

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        GetTarget()->CastSpell(GetTarget(), UnbreakableWill::SPELL_TRACKER, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pal_unbreakable_will::CheckProc);
        OnProc += AuraProcFn(gen_arti_pal_unbreakable_will::HandleProc);
    }
};

class gen_arti_pal_unbreakable_will_tracker : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_unbreakable_will_tracker);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ UnbreakableWill::SPELL_TRAIT, UnbreakableWill::SPELL_IMMUNITY, UnbreakableWill::SPELL_LOCKOUT });
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* paladin = GetTarget();
        if (!paladin->IsAlive() || !paladin->IsInCombat() || !paladin->HasAura(UnbreakableWill::SPELL_TRAIT) || paladin->HasAura(UnbreakableWill::SPELL_LOCKOUT))
            return;

        int32 seconds = TraitValue(paladin, UnbreakableWill::SPELL_TRAIT);
        uint32 mask = UnbreakableWill::MechanicMask();
        if (seconds <= 0 || !mask || !UnbreakableWill::IsControlledFor(paladin, mask, seconds * IN_MILLISECONDS))
            return;

        paladin->RemoveAurasWithMechanic(mask, AURA_REMOVE_BY_ENEMY_SPELL);
        paladin->CastSpell(paladin, UnbreakableWill::SPELL_IMMUNITY, true);
        paladin->CastSpell(paladin, UnbreakableWill::SPELL_LOCKOUT, true);
        ArtifactTraitTest::MarkGenRan(UnbreakableWill::SPELL_TRAIT);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_pal_unbreakable_will_tracker::AfterRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Round 14 (report section "Runde 12 (24.09.2026)"): Second Sunrise (200482, The Silver Hand, ArtifactPower 1186,
// ranks 5..40 %): "Light of Dawn has a $s1% chance to cast a second time for no additional mana cost."
// The trait aura is PROC_TRIGGER_SPELL with EffectTriggerSpell 0 (found by a scan of all ArtifactPowerRank spells for
// triggerless proc auras - it was never in the DUMMY-based candidate list). The recast is done once per player cast of
// Light of Dawn 85222 (SpellScript, not per healed target), as a triggered cast (= no mana cost); the live-bound
// spell_pal_light_of_dawn then sends the second wave 185984. The empty proc of the aura itself is suppressed.
class gen_arti_pal_second_sunrise : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_second_sunrise);

    static constexpr uint32 SPELL_TRAIT = 200482;

    void HandleAfterCast()
    {
        if (GetSpell()->IsTriggered())
            return;

        Unit* caster = GetCaster();
        AuraEffect const* trait = caster ? caster->GetAuraEffect(SPELL_TRAIT, EFFECT_0) : nullptr;
        if (!trait || !roll_chance_i(trait->GetAmount()))
            return;

        caster->CastSpell(caster, GetSpellInfo()->Id, TRIGGERED_FULL_MASK);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_pal_second_sunrise::HandleAfterCast);
    }
};

class gen_arti_pal_second_sunrise_aura : public AuraScript
{
    PrepareAuraScript(gen_arti_pal_second_sunrise_aura);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return false;   // no trigger spell in the client data; the effect lives in gen_arti_pal_second_sunrise
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pal_second_sunrise_aura::CheckProc);
    }
};

// Round 15 (24.09.2026): Holy Mending (196779, T'uure, ArtifactPower 840). E0 PROC_TRIGGER_SPELL with trigger 0 and
// ProcTypeMask 0x4400 (every positive spell the priest casts) - the core would log "Could not trigger spell 0" on each
// of them. The effect ("When Prayer of Mending jumps to a target affected by your Renew ... $196781s1") is implemented
// in spell_pri_prayer_of_mending_legion_jump (spell_priest.cpp), which is the only place a jump is known as a jump.
class gen_arti_pri_holy_mending : public AuraScript
{
    PrepareAuraScript(gen_arti_pri_holy_mending);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_pri_holy_mending::CheckProc);
    }
};

// ===============================================================================================================
// Round 18 (25.09.2026; report section "Runde 15 (Nutzerzaehlung), 25.09.2026"). Files with prefix r18_.
// Overflow is in spell_monk.cpp (live-bound Gift of the Ox PlayerScript). All numbers below are client data of
// build 26972; the two traits marked UNSICHER carry one reading of the tooltip that is named in the comment.
// ===============================================================================================================

// Bag of Tricks (192657, Kingslayers, ArtifactPower rank value 25): "Envenom and Rupture have a ${$s1/10}.1% chance
// per combo point spent to smash a vial of poison at the target's location, creating a pool of acidic death that deals
// ${$192660s1*6} Nature damage over $192661d to all enemies within it."
//   - chance = $s1/10 % x combo points spent (the tooltip formula itself)
//   - 192661 = CREATE_AREATRIGGER misc 4928 at TARGET_DEST_TARGET_ENEMY, 3 s (SpellDuration 27); the world DB has the
//     sniffed spell_areatrigger row 4928 -> AT 9645 (cylinder r 6, VerifiedBuild 26654)
//   - damage 192660 (AP 1.2, area enemies radius 6 = SpellRadius 29); "x6 over $192661d" = 6 equal pulses, one every
//     duration/6 = 500 ms (derived from the tooltip, not from SimC)
// Earlier blocker (report Runde 7): "AreaTrigger row could not be checked (DB offline)" - it exists.
class gen_arti_rog_bag_of_tricks : public SpellScript
{
    PrepareSpellScript(gen_arti_rog_bag_of_tricks);

    static constexpr uint32 SPELL_TRAIT = 192657;
    static constexpr uint32 SPELL_VIAL  = 192661;

    int32 _comboPoints = 0;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_VIAL });
    }

    void HandleBeforeCast()
    {
        _comboPoints = 0;
        if (SpellPowerCost const* cost = GetSpell()->GetPowerCost(POWER_COMBO_POINTS))
            _comboPoints = cost->Amount;
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target || _comboPoints <= 0)
            return;

        int32 value = TraitValue(caster, SPELL_TRAIT);
        if (value <= 0)
            return;

        if (!roll_chance_f(value / 10.0f * _comboPoints))
            return;

        caster->CastSpell(target, SPELL_VIAL, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(gen_arti_rog_bag_of_tricks::HandleBeforeCast);
        AfterCast += SpellCastFn(gen_arti_rog_bag_of_tricks::HandleAfterCast);
    }
};

// areatrigger_template 9645 (spell misc 4928, spell 192661)
struct at_arti_rog_bag_of_tricks : AreaTriggerAI
{
    static constexpr uint32 SPELL_VIAL        = 192661;
    static constexpr uint32 SPELL_POISON_BOMB = 192660;
    static constexpr uint32 PULSES            = 6; // "${$192660s1*6} ... over $192661d"

    uint32 _pulsesDone = 0;

    at_arti_rog_bag_of_tricks(AreaTrigger* areatrigger) : AreaTriggerAI(areatrigger) { }

    uint32 PulseInterval() const
    {
        SpellInfo const* vial = sSpellMgr->GetSpellInfo(SPELL_VIAL);
        int32 duration = vial ? vial->GetMaxDuration() : 0;
        return duration > 0 ? uint32(duration) / PULSES : 0;
    }

    void DoDuePulses()
    {
        uint32 interval = PulseInterval();
        Unit* caster = at->GetCaster();
        if (!interval || !caster)
            return;

        while (_pulsesDone < PULSES && at->GetTimeSinceCreated() >= (_pulsesDone + 1) * interval)
        {
            ++_pulsesDone;
            caster->CastSpell(at->GetPositionX(), at->GetPositionY(), at->GetPositionZ(), SPELL_POISON_BOMB, true);
        }
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        DoDuePulses();
    }

    // the last pulse falls on the expiry tick, which removes the AT before OnUpdate runs
    void OnRemove() override
    {
        DoDuePulses();
    }
};

// Hardened Roots (210638, Fangs of Ashamane, one rank): "Your Bleed damage will no longer cancel Entangling Roots early."
// Entangling Roots 339 (Aura 455 MOD_ROOT_2, ProcTypeMask 664232 = taken damage) breaks in this core through
// AuraEffect::HandleBreakableCCAuraProc (damage budget 10 % max health, TC semantics). The trait removes the druid's
// own bleed damage (mechanic 15 on the spell or an effect, SpellInfo::GetAllEffectsMechanicMask) from that budget:
// Rip 1079, Rake 155722, Thrash (cat) 106830, Thrash (bear) bleed 192090 carry mechanic 15 in the client.
// Only 339 is bound (the druid class skill, skilllineability 798); Mass Entanglement is not named by the tooltip.
class gen_arti_dru_hardened_roots : public AuraScript
{
    PrepareAuraScript(gen_arti_dru_hardened_roots);

    static constexpr uint32 SPELL_TRAIT = 210638;

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* druid = GetCaster();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!druid || !damage || eventInfo.GetActor() != druid || !druid->HasAura(SPELL_TRAIT))
            return true;

        SpellInfo const* source = damage->GetSpellInfo();
        if (!source || !(source->GetAllEffectsMechanicMask() & (1 << MECHANIC_BLEED)))
            return true;

        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
        return false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(gen_arti_dru_hardened_roots::CheckProc);
    }
};

// Flaming Soul (238118, Aldrachi Warblades, one rank, E0 = 500): "Damage from Soul Carver and Immolation Aura extends the
// duration of your Fiery Brand by ${$s1/1000}.1 seconds."
// Damage spells (client): Immolation Aura 187727 (initial burst) and 178741 (the 1 s pulses of 178740), Soul Carver
// 207407 (E1 main-hand hit, E0 periodic damage) and 214743 (off-hand hit). "Your Fiery Brand" = 207744 (without Burning
// Alive) or 207771 (with it) on the damaged target, cast by this demon hunter.
// UNSICHER: the tooltip names no upper limit and none is in the client (207744/207771 Duration = MaxDuration = 8000).
// SimulationCraft caps the brand at 10 s (MAX_FIERY_BRAND_DURATION, no source given); a PTR analysis of 02.03.2017
// (kaylriene.com) computed without a cap. Implemented literally without a cap.
// Round LCF2 R23: LegionCore-7.3.5 spell_dh.cpp (spell_dh_flaming_soul, proc on Immolation Aura / Soul Carver damage)
// also extends without any cap -> second independent implementation without a cap; no longer "unsicher".
namespace FlamingSoul
{
    static constexpr uint32 SPELL_TRAIT = 238118;

    void Extend(Unit* dh, Unit* target)
    {
        if (!dh || !target || dh == target)
            return;

        int32 ms = TraitValue(dh, SPELL_TRAIT);
        if (ms <= 0)
            return;

        bool extended = false;
        for (uint32 brandId : { 207744u, 207771u })
        {
            if (Aura* brand = target->GetAura(brandId, dh->GetGUID()))
            {
                int32 duration = brand->GetDuration() + ms;
                if (duration > brand->GetMaxDuration())
                    brand->SetMaxDuration(duration);
                brand->SetDuration(duration);
                extended = true;

                // Round LCF2 R23 (LegionCore spell_dh_flaming_soul): the Fiery Demise debuff 212818 that comes with the
                // brand is kept in step with it
                if (Aura* demise = target->GetAura(212818, dh->GetGUID()))
                {
                    if (duration > demise->GetMaxDuration())
                        demise->SetMaxDuration(duration);
                    demise->SetDuration(duration);
                }
            }
        }

        if (extended)
            ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }
}

class gen_arti_dh_flaming_soul : public SpellScript
{
    PrepareSpellScript(gen_arti_dh_flaming_soul);

    void HandleAfterHit()
    {
        if (GetHitDamage() > 0)
            FlamingSoul::Extend(GetCaster(), GetHitUnit());
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dh_flaming_soul::HandleAfterHit);
    }
};

class gen_arti_dh_flaming_soul_dot : public AuraScript
{
    PrepareAuraScript(gen_arti_dh_flaming_soul_dot);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        FlamingSoul::Extend(GetCaster(), GetTarget());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(gen_arti_dh_flaming_soul_dot::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Death and Glory (238148, Warswords of the Valarjar, one rank): "Odyn's Fury is empowered by either Odyn or Helya. Odyn
// empowers Odyn's Fury with $243228s1 Fire damage and generates ${$243228s2/10} Rage. Helya empowers Odyn's Fury with
// $243223s1 Shadow damage which heals you for ${$243223e1*100}% of the damage it deals."
// 243228 (SCHOOL_DAMAGE AP 3 + ENERGIZE 200 rage) and 243223 (HEALTH_LEECH AP 3, amplitude 1) use the same targeting as
// Odyn's Fury 205546/205547 (TARGET_DEST_CASTER + area enemies, radius index 61), so they are cast by the warrior after
// each Odyn's Fury.
// Round LCF2 R38 (2026-09-26), final closure round - upgraded from UNSICHER to belegt (a): which of the two empowers
// a given cast is not in any client data field, but two independent Legion-era sources now confirm a plain random
// (not alternating, not conditional) choice:
//  - SimulationCraft (github.com/simulationcraft/simc, engine/class_modules/sc_warrior.cpp, odyns_fury_t::execute()):
//    "if (odyn && rng().roll(0.5)) ... else if (helya) ..." with the developer comment "// Seems to have a coin flip
//    chance to either get odyn or helya... test more later." - i.e. the SimC authors themselves empirically tested
//    this in Legion and modeled it as an unweighted 50/50 coin flip.
//  - mmo-champion.com thread "Death and Glory Gold Trait Not Working?" (Legion-era): a player asked whether the two
//    effects alternate; another player answered "It doesn't alternate, which you get is random."
// Both are independent of our own prior assumption and agree with each other and with the client tooltip's neutral
// "either" wording, so the even random choice below is now treated as sourced (a), not merely assumed (b).
class gen_arti_war_death_and_glory : public SpellScript
{
    PrepareSpellScript(gen_arti_war_death_and_glory);

    static constexpr uint32 SPELL_TRAIT       = 238148;
    static constexpr uint32 SPELL_ODYNS_GLORY = 243228;
    static constexpr uint32 SPELL_HELYAS_SCORN = 243223;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_ODYNS_GLORY, SPELL_HELYAS_SCORN });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_TRAIT))
            return;

        caster->CastSpell(caster, urand(0, 1) ? SPELL_ODYNS_GLORY : SPELL_HELYAS_SCORN, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_war_death_and_glory::HandleAfterCast);
    }
};

// ===============================================================================================================
// Round LCF (25.09.2026): open traits checked against LegionCore-7.3.5 (github.com/The-Legion-Preservation-Project/
// LegionCore-7.3.5, derived from the UWOW 2020 leak - used with the explicit permission of the server owner) and its
// world database (schema legioncore_full). Only the MECHANIC was taken over; every number still comes from our own
// 7.3.5.26972 client data (trait aura amount, EffectTriggerSpell, base points of the named helper spells). Where
// LegionCore relies on something the client does not state, the script says so ("UNSICHER").
// ===============================================================================================================

namespace
{
    // the player a trait belongs to: summons and pets cast many of the involved spells
    Unit* TraitOwner(Unit* unit)
    {
        return unit ? unit->GetCharmerOrOwnerOrSelf() : nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Pattern 11 (LegionCore table spell_aura_dummy, option 4 = SPELL_DUMMY_MOD_EFFECT_MASK, SpellAuras.cpp
// CalculateEffMaskFromDummy / Spell.cpp): the listed effects of a spell exist ONLY while the owner has the trait. The
// client carries the finished effect on the spell itself and names the trait in the tooltip ("$?a179546[...]");
// without the trait the effect must not happen at all. Used uniformly for every trait LegionCore handles this way:
//   Death's Harbinger 238080  -> Apocalypse 220143 E3 (ENERGIZE 2 runes)
//   Aluneth's Avarice 238090  -> Mark of Aluneth 211076 E1 (ENERGIZE_PCT 20 % mana)
//   Embrace of the Nightmare 200855 -> Rage of the Sleeper 200851 E2/E3/E4 (loss-of-control immunity, leech, damage)
//   Ashes to Ashes 179546     -> Wake of Ashes 205273 E2 (the Radiant DoT; the Holy Power part is gen_arti_pal_ashes_to_ashes)
// ---------------------------------------------------------------------------------------------------------------
#define GEN_TRAIT_GATED_EFFECTS(scriptName, traitId, effMask)                                   \
class scriptName : public SpellScript                                                           \
{                                                                                               \
    PrepareSpellScript(scriptName);                                                             \
                                                                                                \
    bool Validate(SpellInfo const* /*spellInfo*/) override                                      \
    {                                                                                           \
        return ValidateSpellInfo({ traitId });                                                  \
    }                                                                                           \
                                                                                                \
    void HandleGate(SpellEffIndex effIndex)                                                     \
    {                                                                                           \
        Unit* caster = GetOriginalCaster() ? GetOriginalCaster() : GetCaster();                 \
        Unit* owner = TraitOwner(caster);                                                       \
        if (owner && owner->HasAura(traitId))                                                   \
        {                                                                                       \
            ArtifactTraitTest::MarkGenRan(traitId);                                             \
            return;                                                                             \
        }                                                                                       \
        PreventHitDefaultEffect(effIndex);                                                      \
    }                                                                                           \
                                                                                                \
    void Register() override                                                                    \
    {                                                                                           \
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)                                           \
            if ((effMask) & (1u << i))                                                          \
                OnEffectHitTarget += SpellEffectFn(scriptName::HandleGate, i, SPELL_EFFECT_ANY); \
    }                                                                                           \
};

GEN_TRAIT_GATED_EFFECTS(gen_arti_dk_deaths_harbinger, 238080, 0x08)          // 220143 Apocalypse, E3
GEN_TRAIT_GATED_EFFECTS(gen_arti_mage_aluneths_avarice, 238090, 0x02)        // 211076 Mark of Aluneth (detonation), E1
GEN_TRAIT_GATED_EFFECTS(gen_arti_dru_embrace_of_the_nightmare, 200855, 0x1C) // 200851 Rage of the Sleeper, E2-E4
GEN_TRAIT_GATED_EFFECTS(gen_arti_pal_ashes_to_ashes_dot, 179546, 0x04)       // 205273 Wake of Ashes, E2

// Focus in Chaos (200871): "Your auto attacks have no penalty to hit from dual-wielding during Enrage."
// LegionCore: spell_linked_spell 184362 -> 200876 (hastalent 200871). 200876 carries aura 458
// SPELL_AURA_IGNORE_DUAL_WIELD_HIT_PENALTY - the only client spell with that aura; the core now honours it in
// Unit::MeleeSpellMissChance (the +19 % dual-wield miss chance). The helper lives exactly as long as Enrage.
class gen_arti_war_focus_in_chaos : public AuraScript
{
    PrepareAuraScript(gen_arti_war_focus_in_chaos);

    static constexpr uint32 SPELL_TRAIT  = 200871;
    static constexpr uint32 SPELL_HELPER = 200876;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_HELPER });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(SPELL_TRAIT))
            return;

        target->CastSpell(target, SPELL_HELPER, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_HELPER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_war_focus_in_chaos::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(gen_arti_war_focus_in_chaos::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Black Claws (238116): "While under the effect of Dark Transformation, your ghoul's Claw has a $s1% chance to burst a
// Festering Wound." LegionCore spell_dk_black_claws (spell_dk.cpp) on 91778 Sweeping Claws, the Claw of the
// transformed ghoul. The burst follows OUR core's burst convention (spell_dk_scourge_strike: 194311 on the target, one
// stack less), not LegionCore's extra 195757 - so all bursts behave the same on this server.
class gen_arti_dk_black_claws : public SpellScript
{
    PrepareSpellScript(gen_arti_dk_black_claws);

    static constexpr uint32 SPELL_TRAIT            = 238116;
    static constexpr uint32 SPELL_FESTERING_WOUND  = 194310;
    static constexpr uint32 SPELL_WOUND_BURST      = 194311;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_FESTERING_WOUND, SPELL_WOUND_BURST });
    }

    void HandleAfterHit()
    {
        Unit* owner = TraitOwner(GetCaster());
        Unit* target = GetHitUnit();
        if (!owner || !target || owner == GetCaster())
            return;

        int32 chance = TraitValue(owner, SPELL_TRAIT);
        if (chance <= 0 || !roll_chance_i(chance))
            return;

        if (Aura* wound = target->GetAura(SPELL_FESTERING_WOUND, owner->GetGUID()))
        {
            owner->CastSpell(target, SPELL_WOUND_BURST, true);
            wound->ModStackAmount(-1);
            ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_dk_black_claws::HandleAfterHit);
    }
};

// The Light Saves (200421): "When the health of your Beacon of Light target falls below $s1%, your next Holy Light or
// Flash of Light on your Beacon target will heal for an additional $200423s1%. Can occur only once every $211426d."
// LegionCore (spell_pal_the_light_saves_aura) polls the beacon targets and hands out 200423 + the 211426 lockout.
// Here the same rule is checked when the Holy Light / Flash of Light lands on a beacon target of the paladin (health
// below the trait value, no 211426 lockout) - no timer needed. Numbers: trait $s1 (50), 200423 E0 (100), 211426 duration.
class gen_arti_pal_the_light_saves : public SpellScript
{
    PrepareSpellScript(gen_arti_pal_the_light_saves);

    static constexpr uint32 SPELL_TRAIT   = 200421;
    static constexpr uint32 SPELL_BONUS   = 200423;
    static constexpr uint32 SPELL_LOCKOUT = 211426;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_BONUS, SPELL_LOCKOUT });
    }

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_TRAIT) || caster->HasAura(SPELL_LOCKOUT))
            return;

        bool beacon = false;
        for (uint32 beaconId : { 53563u, 156910u, 200025u })   // Beacon of Light / Faith / Virtue
            if (target->HasAura(beaconId, caster->GetGUID()))
                beacon = true;
        if (!beacon || target->GetHealthPct() >= float(TraitValue(caster, SPELL_TRAIT)))
            return;

        int32 heal = GetHitHeal();
        AddPct(heal, SpellEffectValue(SPELL_BONUS, EFFECT_0));
        SetHitHeal(heal);
        caster->CastSpell(caster, SPELL_LOCKOUT, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_pal_the_light_saves::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// Siphon Power (218910): "Empower Wards increases your Agility by up to $s1% for $218561d, based on magic damage you
// take while it is active." LegionCore spell_dh_empower_wards: E1 of Empower Wards (SCHOOL_ABSORB, magic schools) is
// only a listener - the magic damage taken is summed up in the client tracker 218713, and 218561 (MOD_TOTAL_STAT_PCT)
// is refreshed with (sum * 100 / max health) %, capped at the trait value. Without the trait E1 keeps its client value.
class gen_arti_dh_siphon_power : public AuraScript
{
    PrepareAuraScript(gen_arti_dh_siphon_power);

    static constexpr uint32 SPELL_TRAIT   = 218910;
    static constexpr uint32 SPELL_TRACKER = 218713;
    static constexpr uint32 SPELL_AGILITY = 218561;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_TRACKER, SPELL_AGILITY });
    }

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (Unit* caster = GetCaster())
            if (caster->HasAura(SPELL_TRAIT))
                amount = -1;
    }

    void HandleAbsorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* target = GetTarget();
        int32 cap = TraitValue(target, SPELL_TRAIT);
        if (cap <= 0)
            return;                                           // no trait: normal client absorb

        absorbAmount = 0;
        int64 sum = int64(dmgInfo.GetDamage());
        if (AuraEffect const* tracker = target->GetAuraEffect(SPELL_TRACKER, EFFECT_0))
            sum += tracker->GetAmount();
        if (sum <= 0 || !target->GetMaxHealth())
            return;

        int32 tracked = int32(std::min<int64>(sum, int64(0x7FFFFFFF)));
        int32 pct = int32(std::min<int64>(sum * 100 / int64(target->GetMaxHealth()), int64(cap)));
        target->CastCustomSpell(SPELL_TRACKER, SPELLVALUE_BASE_POINT0, tracked, target, true);
        if (pct > 0)
            target->CastCustomSpell(SPELL_AGILITY, SPELLVALUE_BASE_POINT0, pct, target, true);
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(gen_arti_dh_siphon_power::CalcAmount, EFFECT_1, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(gen_arti_dh_siphon_power::HandleAbsorb, EFFECT_1);
    }
};

// Sharpened Dreadfangs (211123): "Increases the critical strike chance of Dreadstalkers by $s1%."
// LegionCore: spell_pet_auras (98035 -> 215111, aura 211123) + spell_warl_sharpened_dreadfangs (amount = trait value).
// 215111 is MOD_CRIT_PCT on the Dreadstalker itself; our Call Dreadstalkers summons them synchronously (193331/193332,
// creature 98035), so after the cast each new Dreadstalker of the warlock gets 215111 with the trait value.
class gen_arti_lock_sharpened_dreadfangs : public SpellScript
{
    PrepareSpellScript(gen_arti_lock_sharpened_dreadfangs);

    static constexpr uint32 SPELL_TRAIT       = 211123;
    static constexpr uint32 SPELL_CRIT        = 215111;
    static constexpr uint32 NPC_DREADSTALKER  = 98035;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_CRIT });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        int32 value = TraitValue(caster, SPELL_TRAIT);
        if (value <= 0)
            return;

        std::vector<Unit*> stalkers;
        for (Unit* controlled : caster->m_Controlled)
            if (controlled->GetEntry() == NPC_DREADSTALKER && !controlled->HasAura(SPELL_CRIT))
                stalkers.push_back(controlled);

        for (Unit* stalker : stalkers)
        {
            stalker->CastCustomSpell(SPELL_CRIT, SPELLVALUE_BASE_POINT0, value, stalker, true);
            ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(gen_arti_lock_sharpened_dreadfangs::HandleAfterCast);
    }
};

// Jaws of Shadow (238109): "Dreadbite increases damage taken from your Wild Imps' Fel Firebolt by $s1%."
// LegionCore spell_warl_dreadbite: the Dreadstalker puts 242922 (dummy, E0 "Fel Firebolt +$s1%") on its target with the
// trait value, the warlock as original caster. Part 1 (Dreadbite 205196) applies it, part 2 (Fel Firebolt 104318, cast
// by the Wild Imp with the warlock as original caster, spell_warlock.cpp npc AI) reads it.
class gen_arti_lock_jaws_of_shadow : public SpellScript
{
    PrepareSpellScript(gen_arti_lock_jaws_of_shadow);

    static constexpr uint32 SPELL_TRAIT = 238109;
    static constexpr uint32 SPELL_DEBUFF = 242922;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TRAIT, SPELL_DEBUFF });
    }

    void HandleAfterHit()
    {
        Unit* stalker = GetCaster();
        Unit* owner = TraitOwner(stalker);
        Unit* target = GetHitUnit();
        if (!stalker || !owner || owner == stalker || !target)
            return;

        int32 value = TraitValue(owner, SPELL_TRAIT);
        if (value <= 0)
            return;

        stalker->CastCustomSpell(SPELL_DEBUFF, SPELLVALUE_BASE_POINT0, value, target, true, nullptr, nullptr, owner->GetGUID());
        ArtifactTraitTest::MarkGenRan(SPELL_TRAIT);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(gen_arti_lock_jaws_of_shadow::HandleAfterHit);
    }
};

class gen_arti_lock_jaws_of_shadow_firebolt : public SpellScript
{
    PrepareSpellScript(gen_arti_lock_jaws_of_shadow_firebolt);

    static constexpr uint32 SPELL_DEBUFF = 242922;

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* owner = GetOriginalCaster() ? GetOriginalCaster() : TraitOwner(GetCaster());
        Unit* target = GetHitUnit();
        if (!owner || !target)
            return;

        if (AuraEffect const* debuff = target->GetAuraEffect(SPELL_DEBUFF, EFFECT_0, owner->GetGUID()))
        {
            int32 damage = GetHitDamage();
            AddPct(damage, debuff->GetAmount());
            SetHitDamage(damage);
            ArtifactTraitTest::MarkGenRan(238109);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(gen_arti_lock_jaws_of_shadow_firebolt::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// ===============================================================================================================
// Underlight Angler (fishing artifact, ArtifactID 73) - round LCF2 R28 (2026-09-25)
// ===============================================================================================================

// Luremaster (201946, ArtifactPower 1031): "Increases duration of Legion lures and baits by $s1% for every trait
// purchased." Client 26972: SpellEffect 201946 E0 APPLY_AURA/DUMMY BasePoints 8 (the earlier note "BP 0" was wrong,
// R27); Wowhead tooltip shows 8 %. ArtifactPower 1031 has MaxPurchasableRank 0 and flag 0x08
// ARTIFACT_POWER_FLAG_SCALES_WITH_NUM_POWERS - the engine's count for such powers is the total of purchased ranks of the
// artifact (Item::LoadArtifactData / HandleArtifactAddPower), so "every trait purchased" = sum of PurchasedRank.
// (Counting purchased ranks vs. distinct traits is not stated anywhere explicitly; the flag semantics decide it here.)
// Hooked lures/baits = every timed APPLY_AURA use spell of the Legion bait items 133701-133725 and 133795 (Ravenous Fly)
// plus the Legion lures 138956 (Hypermagnetic Lure) and 139175 (Arcane Lure) - list from ItemEffect/SpellEffect 26972.
// Summon/teleport baits (Drowned Thistleleaf, Swollen Murloc Egg, ...) have no aura duration and are not bound.
class gen_arti_fish_luremaster : public AuraScript
{
    PrepareAuraScript(gen_arti_fish_luremaster);

    static constexpr uint32 SPELL_LUREMASTER = 201946;
    static constexpr uint8 ARTIFACT_UNDERLIGHT_ANGLER = 73;

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        AuraEffect const* luremaster = player->GetAuraEffect(SPELL_LUREMASTER, EFFECT_0);
        if (!luremaster || luremaster->GetAmount() <= 0)
            return;

        Item* rod = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!rod || rod->GetTemplate()->GetArtifactID() != ARTIFACT_UNDERLIGHT_ANGLER)
            return;

        int32 purchased = 0;
        for (ItemDynamicFieldArtifactPowers const& power : rod->GetArtifactPowers())
            purchased += power.PurchasedRank;
        if (purchased <= 0)
            return;

        Aura* aura = GetAura();
        int32 maxDuration = aura->GetMaxDuration();
        if (maxDuration <= 0)
            return;

        int32 newDuration = maxDuration + CalculatePct(maxDuration, luremaster->GetAmount() * purchased);
        aura->SetMaxDuration(newDuration);
        aura->SetDuration(newDuration);
        ArtifactTraitTest::MarkGenRan(SPELL_LUREMASTER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(gen_arti_fish_luremaster::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddSC_artifact_trait_gen_spell_scripts()
{
    // Underlight Angler (round LCF2 R28)
    RegisterAuraScript(gen_arti_fish_luremaster);

    // Priest
    RegisterSpellScript(gen_arti_pri_vestments_of_discipline);
    RegisterSpellScript(gen_arti_pri_speed_of_the_pious);
    RegisterSpellScript(gen_arti_pri_mind_quickening);
    RegisterAuraScript(gen_arti_pri_shield_of_faith);
    RegisterSpellScript(gen_arti_pri_share_in_the_light);
    RegisterAuraScript(gen_arti_pri_sins_of_the_many);

    // Paladin
    RegisterSpellScript(gen_arti_pal_ashes_to_ashes);
    RegisterSpellScript(gen_arti_pal_scatter_the_shadows);
    RegisterSpellScript(gen_arti_pal_healing_storm);
    RegisterSpellScript(gen_arti_pal_bulwark_of_order);

    // Shaman
    RegisterSpellScript(gen_arti_sha_elemental_healing);
    RegisterSpellScript(gen_arti_sha_sense_of_urgency);
    RegisterSpellScript(gen_arti_sha_queens_decree);
    RegisterSpellScript(gen_arti_sha_cumulative_upkeep);
    RegisterSpellScript(gen_arti_sha_gathering_storms);
    RegisterSpellScript(gen_arti_sha_winds_of_change);
    RegisterSpellScript(gen_arti_sha_deep_waters);

    // Druid
    RegisterSpellScript(gen_arti_dru_natures_essence);
    RegisterSpellScript(gen_arti_dru_rapid_innervation);
    RegisterSpellScript(gen_arti_dru_ashamanes_energy);
    RegisterSpellScript(gen_arti_dru_fury_of_ashamane);
    RegisterSpellScript(gen_arti_dru_feral_instinct);
    RegisterSpellScript(gen_arti_dru_pawsitive_outlook);

    // Warrior
    RegisterSpellScript(gen_arti_war_juggernaut);
    RegisterSpellScript(gen_arti_war_tactical_advance);
    RegisterSpellScript(gen_arti_war_oathblood);
    RegisterSpellScript(gen_arti_war_one_against_many);
    RegisterSpellScript(gen_arti_war_void_cleave);

    // Mage
    RegisterSpellScript(gen_arti_mage_scorched_earth);
    RegisterSpellScript(gen_arti_mage_warmth_of_the_phoenix);

    // Monk
    RegisterSpellScript(gen_arti_monk_healing_winds);
    RegisterSpellScript(gen_arti_monk_transfer_the_power);
    RegisterSpellScript(gen_arti_monk_stave_off);

    // Rogue
    RegisterSpellScript(gen_arti_rog_dense_concoction);
    RegisterSpellScript(gen_arti_rog_surge_of_toxins);
    RegisterSpellScript(gen_arti_rog_shadow_swiftness);
    RegisterSpellScript(gen_arti_rog_shadows_whisper);
    RegisterSpellScript(gen_arti_rog_fortunes_bite);

    // Hunter
    RegisterSpellScript(gen_arti_hun_survival_of_the_fittest);
    RegisterSpellScript(gen_arti_hun_bird_of_prey);
    RegisterSpellScript(gen_arti_hun_furious_swipes);
    RegisterSpellScript(gen_arti_hun_mark_of_the_windrunner);
    RegisterSpellScript(gen_arti_hun_hunters_advantage);
    RegisterSpellScript(gen_arti_hun_jaws_of_thunder);

    // Death Knight
    RegisterSpellScript(gen_arti_dk_blood_feast);
    RegisterSpellScript(gen_arti_dk_gravitational_pull);
    RegisterSpellScript(gen_arti_dk_over_powered);
    RegisterSpellScript(gen_arti_dk_scourge_the_unbeliever);

    // Demon Hunter
    RegisterSpellScript(gen_arti_dh_chaotic_onslaught);

    // Round 2: proc based and critical strike chance traits
    RegisterAuraScript(gen_arti_mage_pyretic_incantation);
    RegisterAuraScript(gen_arti_war_pulse_of_battle);
    RegisterAuraScript(gen_arti_pri_unleash_the_shadows);
    RegisterAuraScript(gen_arti_sha_queen_ascendant);
    RegisterAuraScript(gen_arti_dru_circadian_invocation);
    RegisterSpellScript(gen_arti_hun_marked_for_death);
    RegisterSpellScript(gen_arti_war_precise_strikes);
    RegisterSpellScript(gen_arti_dru_roar_of_the_crowd);
    RegisterSpellScript(gen_arti_dru_scent_of_blood);
    RegisterSpellScript(gen_arti_rog_from_the_shadows);
    RegisterAuraScript(gen_arti_dru_open_wounds);
    RegisterSpellScript(gen_arti_lock_cry_havoc);
    RegisterSpellScript(gen_arti_war_bloodcraze);

    // Round 6: traits recovered from the client data of the spells their descriptions refer to
    RegisterSpellScript(gen_arti_hun_spirit_bond);
    RegisterAuraScript(gen_arti_hun_talon_strike);
    RegisterAuraScript(gen_arti_hun_hunters_bounty);
    RegisterAuraScript(gen_arti_dk_hypothermia);
    RegisterAuraScript(gen_arti_dk_thronebreaker);
    RegisterSpellScript(gen_arti_dk_vampiric_aura);
    RegisterAuraScript(gen_arti_rog_embrace_of_darkness);
    RegisterSpellScript(gen_arti_monk_mists_of_life);
    RegisterAuraScript(gen_arti_monk_light_on_your_feet);
    RegisterSpellScript(gen_arti_monk_face_palm);
    RegisterSpellScript(gen_arti_monk_quick_sip);
    RegisterAuraScript(gen_arti_dru_deep_rooted);
    RegisterSpellScript(gen_arti_dru_dreamwalker);
    RegisterAuraScript(gen_arti_pri_thrive_in_the_shadows);
    RegisterSpellScript(gen_arti_pri_taming_the_shadows);
    RegisterAuraScript(gen_arti_sha_ghost_wolf_traits);
    RegisterAuraScript(gen_arti_sha_spirit_of_the_maelstrom);
    RegisterAuraScript(gen_arti_sha_ghost_in_the_mist);
    RegisterSpellScript(gen_arti_mage_aftershocks);

    // Round 7
    RegisterSpellScript(gen_arti_hun_hellcarver_carve);
    RegisterSpellScript(gen_arti_hun_hellcarver_butchery);
    RegisterSpellScript(gen_arti_pri_lights_wrath);
    RegisterAuraScript(gen_arti_pri_aegis_of_wrath);
    RegisterSpellScript(gen_arti_monk_dragonfire_brew);
    RegisterSpellScript(gen_arti_dh_painbringer);
    RegisterSpellScript(gen_arti_dh_feast_on_the_souls);
    RegisterSpellScript(gen_arti_war_might_of_the_vrykul);
    RegisterSpellScript(gen_arti_sha_stormflurry);
    RegisterSpellScript(gen_arti_sha_stormflurry_damage);

    // Round 8
    RegisterAuraScript(gen_arti_dru_mark_of_shifting_form);
    RegisterAuraScript(gen_arti_dru_mark_of_shifting_travel);
    RegisterAuraScript(gen_arti_dru_mark_of_shifting_tick);
    RegisterAuraScript(gen_arti_dru_protection_of_ashamane);
    RegisterSpellScript(gen_arti_dru_light_of_the_sun);
    RegisterAuraScript(gen_arti_mage_phoenix_reborn);
    RegisterSpellScript(gen_arti_rog_sinister_circulation);

    // Round 9: SimulationCraft cross-check
    RegisterSpellScript(gen_arti_dk_runic_chills);
    RegisterSpellScript(gen_arti_dk_frozen_soul_tick);
    RegisterAuraScript(gen_arti_dk_frozen_soul);
    RegisterSpellScript(gen_arti_dk_frozen_soul_damage);
    RegisterSpellScript(gen_arti_dh_erupting_souls);
    RegisterSpellScript(gen_arti_monk_thunderfist);
    RegisterAuraScript(gen_arti_monk_thunderfist_discharge);
    RegisterSpellScript(gen_arti_monk_tornado_kicks);
    RegisterSpellScript(gen_arti_monk_dancing_mists);
    RegisterAuraScript(gen_arti_monk_strength_of_xuen);
    RegisterAuraScript(gen_arti_monk_strength_of_xuen_trait);
    RegisterAuraScript(gen_arti_pal_righteous_verdict);
    RegisterSpellScript(gen_arti_pal_echo_of_the_highlord);
    RegisterAuraScript(gen_arti_pri_mass_hysteria_swp);
    RegisterAuraScript(gen_arti_pri_mass_hysteria_vt);
    RegisterAuraScript(gen_arti_pri_sphere_of_insanity_voidform);
    RegisterAuraScript(gen_arti_pri_sphere_of_insanity);
    RegisterSpellScript(gen_arti_sha_wind_strikes);
    RegisterSpellScript(gen_arti_rog_finality_eviscerate);
    RegisterSpellScript(gen_arti_rog_finality_nightblade);
    RegisterAuraScript(gen_arti_rog_finality_nightblade_dot);
    RegisterAuraScript(gen_arti_rog_greed);
    RegisterSpellScript(gen_arti_rog_greed_heal);
    RegisterSpellScript(gen_arti_rog_blunderbuss);
    RegisterSpellScript(gen_arti_rog_blunderbuss_fired);
    RegisterAuraScript(gen_arti_war_battle_scars);
    RegisterAuraScript(gen_arti_war_reflective_plating);
    RegisterSpellScript(gen_arti_mage_rule_of_threes);
    RegisterAuraScript(gen_arti_mage_rule_of_threes_end);
    RegisterSpellScript(gen_arti_mage_time_and_space);
    RegisterAuraScript(gen_arti_mage_aegwynns_ascendance);
    RegisterSpellScript(gen_arti_mage_aegwynns_ascendance_damage);
    RegisterAuraScript(gen_arti_dru_ashamanes_bite);
    RegisterSpellScript(gen_arti_dru_echoing_stars);
    RegisterAuraScript(gen_arti_hun_call_of_the_hunter);

    // Round 10: lateral external research
    RegisterAuraScript(gen_arti_dk_crystalline_swords);
    RegisterAuraScript(gen_arti_monk_shroud_of_mist);
    RegisterAuraScript(gen_arti_pal_defender_of_truth);
    RegisterAuraScript(gen_arti_hun_mimirons_shell);
    RegisterAuraScript(gen_arti_pri_borrowed_time);
    RegisterAuraScript(gen_arti_dru_adaptive_fur);
    RegisterAuraScript(gen_arti_pal_blessing_of_the_ashbringer);

    // Round 11: Mental Fortitude / Souldrinker (report section "Runde 9 (24.09.2026)")
    RegisterAuraScript(gen_arti_pri_mental_fortitude);
    RegisterAuraScript(gen_arti_dk_souldrinker); // round 13: AuraScript on 238114 (was SpellScript on 45470)
    RegisterSpellScript(gen_arti_dru_touch_of_the_moon);
    RegisterAuraScript(gen_arti_hun_critical_focus);

    // Round 12: the four core blockers (report section "Runde 10 (24.09.2026, abends)")
    RegisterAuraScript(gen_arti_pal_faiths_armor);
    RegisterAuraScript(gen_arti_pal_forbearant_faithful);
    new gen_arti_pal_forbearant_faithful_cooldowns();
    RegisterAuraScript(gen_arti_dk_double_doom);
    RegisterAuraScript(gen_arti_dk_double_doom_charges);
    RegisterSpellScript(gen_arti_pal_divine_tempest);
    RegisterAreaTriggerAI(at_arti_pal_divine_tempest);
    RegisterSpellScript(gen_arti_pri_barrier_for_the_devoted);

    // Round 13 (report section "Runde 11 (24.09.2026, spaet)")
    RegisterAuraScript(gen_arti_lock_soul_skin);
    RegisterAuraScript(gen_arti_pri_tyranny_of_pain);
    new gen_arti_hun_terms_of_engagement();
    RegisterAuraScript(gen_arti_monk_death_art);
    RegisterAuraScript(gen_arti_pal_unbreakable_will);
    RegisterAuraScript(gen_arti_pal_unbreakable_will_tracker);

    // Round 14 (report section "Runde 12 (24.09.2026)")
    RegisterSpellScript(gen_arti_pal_second_sunrise);
    RegisterAuraScript(gen_arti_pal_second_sunrise_aura);
    // Round 15 (report section "Runde 13 (24.09.2026)")
    RegisterAuraScript(gen_arti_pri_holy_mending);
    // Round 18 (report section "Runde 15 (Nutzerzaehlung), 25.09.2026")
    RegisterSpellScript(gen_arti_rog_bag_of_tricks);
    RegisterAreaTriggerAI(at_arti_rog_bag_of_tricks);
    RegisterAuraScript(gen_arti_dru_hardened_roots);
    RegisterSpellScript(gen_arti_dh_flaming_soul);
    RegisterAuraScript(gen_arti_dh_flaming_soul_dot);
    RegisterSpellScript(gen_arti_war_death_and_glory);
    // Round LCF (report section "Runde LCF (25.09.2026)") - mechanics checked against LegionCore-7.3.5
    RegisterSpellScript(gen_arti_dk_deaths_harbinger);
    RegisterSpellScript(gen_arti_mage_aluneths_avarice);
    RegisterSpellScript(gen_arti_dru_embrace_of_the_nightmare);
    RegisterSpellScript(gen_arti_pal_ashes_to_ashes_dot);
    RegisterAuraScript(gen_arti_war_focus_in_chaos);
    RegisterSpellScript(gen_arti_dk_black_claws);
    RegisterSpellScript(gen_arti_pal_the_light_saves);
    RegisterAuraScript(gen_arti_dh_siphon_power);
    RegisterSpellScript(gen_arti_lock_sharpened_dreadfangs);
    RegisterSpellScript(gen_arti_lock_jaws_of_shadow);
    RegisterSpellScript(gen_arti_lock_jaws_of_shadow_firebolt);
}
