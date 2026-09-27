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

// Legion-Server round "LCF2 R23" (2026-09-25): rest list of round LCF2.
// Mechanics checked against an external reference implementation. The code is
// our own; numbers come from our client 7.3.5.26972 unless a comment names the reference source of a value.
// Binding SQL: C:\LegionServer\fixes\lcf2r23_2026-09-25_scripts.sql - without it every script here is inert.

#include "AreaTrigger.h"
#include "AreaTriggerAI.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

namespace
{
    enum R23Spells : uint32
    {
        SPELL_WINDBURST             = 204147,
        SPELL_WINDBURST_TRAIL       = 204475, // client: CREATE_AREATRIGGER misc 6026 -> areatrigger_template 10713 (polygon, sniffed)
        SPELL_WINDBURST_SPEED       = 204477, // client: +50 % run speed, ally
        SPELL_CYCLONIC_BURST_TRAIT  = 238124,
        SPELL_CYCLONIC_BURST        = 242712, // client: -50 % speed + periodic damage every 1 s, 5 s

        SPELL_FLAME_RIFT_AURA       = 243045, // client: dummy, permanent (the rift's look)
        SPELL_SEARING_BOLTS         = 243046, // client: 10 s (no period in the client)
        SPELL_SEARING_BOLT          = 243050, // client: Fire damage + stacking DoT

        SPELL_PRECISE_STRIKES_TRAIT = 248579,
        SPELL_PRECISE_STRIKES_BUFF  = 248195
    };
}

// ============================================================================================================
// Hunter (Marksmanship): Windburst trail (204147 -> 204475) + Cyclonic Burst (238124).
// Our core never created the trail: Windburst 204147 has only damage effects, the trail spell 204475 was not cast
// by anything, so the "trail of wind that increases the movement speed of allies" (base ability text) and Cyclonic
// Burst ("The trail of wind left by Windburst deals ... to enemies within") were missing.
// the reference core spell_hunter.cpp spell_hun_windburst (AfterCast: cast 204475) + areatrigger_hun_windburst (AT of 204475):
//   friendly unit enters -> 204477 with the AT's remaining duration; enemy enters and the hunter has 238124 ->
//   242712 with the AT's remaining duration; leaving the AT removes both.
// Differences to the reference core (on purpose): the reference core resizes the polygon to the distance hunter -> target; we keep
// our sniffed polygon of template 10713 (-3..+29 yd x 4 yd) and only turn it so it points from the target back to
// the hunter (204475 targets TARGET_DEST_TARGET_ENEMY, so the AT stands at the target). the reference core's extra ground
// visuals 223114/226872 every 5 yd are not cast.
// ============================================================================================================
class spell_r23_hun_windburst_trail : public SpellScript
{
    PrepareSpellScript(spell_r23_hun_windburst_trail);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_WINDBURST_TRAIL, SPELL_WINDBURST_SPEED, SPELL_CYCLONIC_BURST });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (caster && target && target != caster)
            caster->CastSpell(target, SPELL_WINDBURST_TRAIL, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r23_hun_windburst_trail::HandleAfterCast);
    }
};

struct at_r23_hun_windburst : AreaTriggerAI
{
    at_r23_hun_windburst(AreaTrigger* areatrigger) : AreaTriggerAI(areatrigger) { }

    bool _cyclonicBurst = false;

    void OnInitialize() override
    {
        // runs before the AreaTrigger is added to the map, so the client gets the turned polygon right away
        if (Unit* caster = at->GetCaster())
        {
            if (caster->GetGUID() != ObjectGuid::Empty && !caster->IsWithinDist2d(at, 0.5f))
                at->SetOrientation(at->GetAngle(caster));
            _cyclonicBurst = caster->HasAura(SPELL_CYCLONIC_BURST_TRAIT);
        }
    }

    void OnUnitEnter(Unit* unit) override
    {
        Unit* caster = at->GetCaster();
        if (!caster || unit->IsTotem())
            return;

        int32 duration = at->GetDuration();
        if (duration <= 0)
            return;

        if (caster->IsFriendlyTo(unit))
            ApplyWithDuration(caster, unit, SPELL_WINDBURST_SPEED, duration);
        else if (_cyclonicBurst && caster->IsValidAttackTarget(unit))
            ApplyWithDuration(caster, unit, SPELL_CYCLONIC_BURST, duration);
    }

    void OnUnitExit(Unit* unit) override
    {
        ObjectGuid casterGuid = at->GetCasterGuid();
        unit->RemoveAurasDueToSpell(SPELL_WINDBURST_SPEED, casterGuid);
        unit->RemoveAurasDueToSpell(SPELL_CYCLONIC_BURST, casterGuid);
    }

    static void ApplyWithDuration(Unit* caster, Unit* unit, uint32 spellId, int32 duration)
    {
        if (Aura* aura = caster->AddAura(spellId, unit))
        {
            if (duration < aura->GetMaxDuration())
            {
                aura->SetMaxDuration(duration);
                aura->SetDuration(duration);
            }
        }
    }
};

// ============================================================================================================
// Warlock (Destruction): Flame Rift (238146) "Dimensional Rift can now summon a powerful Flame Rift."
// The 4th choice of Dimensional Rift is in spell_artifact.cpp (spell_arti_warl_dimensional_rift). This is the rift.
// the reference core: spell_pet_auras 121643 -> 31366 (root), 243045 (look), 243046 (Searing Bolts); SpellMgr.cpp turns
// 243046 into a PERIODIC_DUMMY with a 500 ms period (the reference core value - the client has no period), and
// spell_warlock.cpp spell_warl_searing_bolts casts 243050 at the rift's target on every tick with the warlock as
// original caster. Number of bolts = 243046 duration (client 10 s) / 500 ms = 20. The rift itself lives for the
// client duration of 242983 (40 s).
// ============================================================================================================
struct npc_r23_warl_flame_rift : public ScriptedAI
{
    npc_r23_warl_flame_rift(Creature* creature) : ScriptedAI(creature) { }

    static constexpr uint32 BOLT_PERIOD = 500; // the reference core SpellMgr.cpp ApplySpellFix 243046

    uint32 _timer = 0;
    uint32 _boltsLeft = 0;
    bool _started = false;

    void InitializeAI() override
    {
        me->SetReactState(REACT_PASSIVE);
        me->AddUnitState(UNIT_STATE_ROOT);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!_started)
        {
            _started = true;
            me->CastSpell(me, SPELL_FLAME_RIFT_AURA, true);
            SpellInfo const* bolts = sSpellMgr->GetSpellInfo(SPELL_SEARING_BOLTS);
            int32 duration = bolts ? bolts->GetMaxDuration() : 0;
            _boltsLeft = duration > 0 ? uint32(duration) / BOLT_PERIOD : 0;
            return;
        }

        if (!_boltsLeft)
            return;

        _timer += diff;
        if (_timer < BOLT_PERIOD)
            return;
        _timer -= BOLT_PERIOD;
        --_boltsLeft;

        Unit* owner = ObjectAccessor::GetUnit(*me, me->GetOwnerGUID());
        Unit* target = ObjectAccessor::GetUnit(*me, me->GetTarget());
        if (!owner || !target || !target->IsAlive() || !owner->IsValidAttackTarget(target))
        {
            _boltsLeft = 0;
            return;
        }

        me->CastSpell(target, SPELL_SEARING_BOLT, true, nullptr, nullptr, owner->GetGUID());
    }
};

// ============================================================================================================
// Warrior (Arms): Precise Strikes (248579) "Colossus Smash increases the critical strike chance of your next Mortal
// Strike or Execute by $s1%."  the reference core spell_trigger 248579 -> 248195, spell_proc_event 248579 = Colossus Smash
// (class mask [1] 0x40000000 = 167105) and Warbreaker ([3] 0x8000 = 209577). 248195 (client) carries the crit
// modifier for Mortal Strike/Execute; it is consumed by gen_arti_war_precise_strikes (spell_artifact_traits_gen.cpp).
// ============================================================================================================
class spell_r23_war_precise_strikes_apply : public SpellScript
{
    PrepareSpellScript(spell_r23_war_precise_strikes_apply);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PRECISE_STRIKES_TRAIT, SPELL_PRECISE_STRIKES_BUFF });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        AuraEffect const* trait = caster ? caster->GetAuraEffect(SPELL_PRECISE_STRIKES_TRAIT, EFFECT_0) : nullptr;
        if (!trait || trait->GetAmount() <= 0)
            return;

        caster->CastCustomSpell(SPELL_PRECISE_STRIKES_BUFF, SPELLVALUE_BASE_POINT0, trait->GetAmount(), caster, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_r23_war_precise_strikes_apply::HandleAfterCast);
    }
};

void AddSC_lcf2r23_2026_09_25_spell_scripts()
{
    RegisterSpellScript(spell_r23_hun_windburst_trail);
    RegisterAreaTriggerAI(at_r23_hun_windburst);
    RegisterCreatureAI(npc_r23_warl_flame_rift);
    RegisterSpellScript(spell_r23_war_precise_strikes_apply);
}
