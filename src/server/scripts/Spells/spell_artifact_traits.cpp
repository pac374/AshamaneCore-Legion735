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

// Artifact trait scripts for Demon Hunter and Warlock artifact weapons.
// Ported from the master branch of AshamaneProject/AshamaneCore (GPL), where they were
// written by the AshamaneCore community, and adapted to this 7.3.5 branch.

#include "AreaTrigger.h"
#include "AreaTriggerAI.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include <G3D/Vector3.h>
#include <map>

// Number of times the proc handler of a trait ran, read by the .arttest command
namespace ArtifactTraitTest
{
    std::map<uint32, uint32> ProcCount;
    uint32 ThalkielTicks = 0;
}

namespace
{
enum ArtifactTraitSpells
{
    SPELL_DH_ANGUISH_DAMAGE = 202446,
    SPELL_DH_CHARRED_WARBLADES_HEAL = 213011,
    SPELL_DH_DECEIVERS_FURY_ENERGIZE = 202120,
    SPELL_DH_DEMON_SPEED = 201469,
    SPELL_DH_FEL_RUSH = 195072,
    SPELL_DH_FIERY_DEMISE_DEBUFF = 212818,
    SPELL_DH_INNER_DEMONS_DAMAGE = 202388,
    SPELL_DH_LESSER_SOUL_SHARD = 203795,
    SPELL_DH_LESSER_SOUL_SHARD_HEAL = 203794,
    SPELL_DH_METAMORPHOSIS_VENGEANCE = 187827,
    SPELL_DH_OVERWHELMING_POWER = 201464,
    SPELL_DH_SHATTERED_SOULS_MISSILE = 209651,
    SPELL_DH_SOUL_FRAGMENT_HEAL_VENGEANCE = 210042,
    SPELL_WARLOCK_DEVOURER_OF_LIFE_PROC = 215165,
    SPELL_WARLOCK_DIMENSIONAL_RIFT = 196586,
    SPELL_WARLOCK_ETERNAL_STRUGGLE_PROC = 196304,
    SPELL_WARLOCK_SOUL_FLAME_PROC = 199581,
    SPELL_WARLOCK_SOULSNATCHER_PROC = 196234,
    SPELL_WARLOCK_THALKIES_DISCORD_DAMAGE = 211727,
    SPELL_WARLOCK_THE_EXPANDABLES_BUFF = 211218,
    SPELL_WARLOCK_WRATH_OF_CONSUMPTION_PROC = 199646,
};
}
// 201469 - Demon Speed
// Called by Blur (212800) and Netherwalk (196555)
class spell_dh_artifact_demon_speed : public SpellScriptLoader
{
    public:
        spell_dh_artifact_demon_speed() : SpellScriptLoader("spell_dh_artifact_demon_speed") { }

        class spell_dh_artifact_demon_speed_SpellScript : public SpellScript
        {
            PrepareSpellScript(spell_dh_artifact_demon_speed_SpellScript);

            void HandleCast()
            {
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                if (AuraEffect* aurEff = caster->GetAuraEffect(SPELL_DH_DEMON_SPEED, EFFECT_0))
                    for (uint8 i = 0; i < aurEff->GetAmount(); ++i)
                        caster->GetSpellHistory()->RestoreCharge(sSpellMgr->GetSpellInfo(SPELL_DH_FEL_RUSH)->ChargeCategoryId);
            }

            void Register()
            {
                OnCast += SpellCastFn(spell_dh_artifact_demon_speed_SpellScript::HandleCast);
            }
        };

        SpellScript* GetSpellScript() const
        {
            return new spell_dh_artifact_demon_speed_SpellScript();
        }
};

// 201463 - Deceiver's Fury
class spell_dh_artifact_deceivers_fury : public SpellScriptLoader
{
    public:
        spell_dh_artifact_deceivers_fury() : SpellScriptLoader("spell_dh_artifact_deceivers_fury") { }

        class spell_dh_artifact_deceivers_fury_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_deceivers_fury_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                caster->CastCustomSpell(SPELL_DH_DECEIVERS_FURY_ENERGIZE, SPELLVALUE_BASE_POINT0, aurEff->GetAmount(), caster, true);
            }

            void Register()
            {
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_deceivers_fury_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_deceivers_fury_AuraScript();
        }
};

// 202443 - Anguish
class spell_dh_artifact_anguish : public SpellScriptLoader
{
    public:
        spell_dh_artifact_anguish() : SpellScriptLoader("spell_dh_artifact_anguish") { }

        class spell_dh_artifact_anguish_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_anguish_AuraScript);

            void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                Unit* caster = GetCaster();
                Unit* target = GetUnitOwner();
                if (!caster || !target)
                    return;

                caster->CastCustomSpell(SPELL_DH_ANGUISH_DAMAGE, SPELLVALUE_AURA_STACK, GetStackAmount(), target, true);
            }

            void Register()
            {
                OnEffectRemove += AuraEffectRemoveFn(spell_dh_artifact_anguish_AuraScript::OnRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_anguish_AuraScript();
        }
};

// 202446 - Anguish damage
class spell_dh_artifact_anguish_damage : public SpellScriptLoader
{
    public:
        spell_dh_artifact_anguish_damage() : SpellScriptLoader("spell_dh_artifact_anguish_damage") { }

        class spell_dh_artifact_anguish_damage_SpellScript : public SpellScript
        {
            PrepareSpellScript(spell_dh_artifact_anguish_damage_SpellScript);

            void HandleHit(SpellEffIndex /*effIndex*/)
            {
                int32 stacks = GetSpellValue()->AuraStackAmount;
                SetHitDamage(GetHitDamage() * stacks);
            }

            void Register()
            {
                OnEffectHitTarget += SpellEffectFn(spell_dh_artifact_anguish_damage_SpellScript::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
            }
        };

        SpellScript* GetSpellScript() const
        {
            return new spell_dh_artifact_anguish_damage_SpellScript();
        }
};

// 201471 - Inner Demons
class spell_dh_artifact_inner_demons : public SpellScriptLoader
{
    public:
        spell_dh_artifact_inner_demons() : SpellScriptLoader("spell_dh_artifact_inner_demons") { }

        class spell_dh_artifact_inner_demons_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_inner_demons_AuraScript);

            void OnProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                Unit* target = eventInfo.GetActionTarget();
                if (!caster || !target)
                    return;

                caster->Variables.Set("Spells.InnerDemonsTarget", target->GetGUID());
            }

            void Register()
            {
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_inner_demons_AuraScript::OnProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_inner_demons_AuraScript();
        }
};

// 201472 - Rage of the Illidari
class spell_dh_artifact_rage_of_the_illidari : public SpellScriptLoader
{
    public:
        spell_dh_artifact_rage_of_the_illidari() : SpellScriptLoader("spell_dh_artifact_rage_of_the_illidari") { }

        class spell_dh_artifact_rage_of_the_illidari_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_rage_of_the_illidari_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                if (!caster || !eventInfo.GetDamageInfo())
                    return;

                int32 damage = CalculatePct(eventInfo.GetDamageInfo()->GetDamage(), aurEff->GetSpellEffectInfo()->BasePoints);
                if (!damage)
                    return;

                damage += caster->Variables.GetValue<int32>("Spells.RageOfTheIllidariDamage");

                caster->Variables.Set("Spells.RageOfTheIllidariDamage", damage);
            }

            void Register()
            {
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_rage_of_the_illidari_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_rage_of_the_illidari_AuraScript();
        }
};

// 201464 - Overwhelming Power
// Called by 179057 - Chaos Nova
class spell_dh_artifact_overwhelming_power : public SpellScriptLoader
{
    public:
        spell_dh_artifact_overwhelming_power() : SpellScriptLoader("spell_dh_artifact_overwhelming_power") { }

        class spell_dh_artifact_overwhelming_power_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_overwhelming_power_AuraScript);

            void OnApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                AuraEffect const* traitEffect = caster->GetAuraEffect(SPELL_DH_OVERWHELMING_POWER, EFFECT_0);
                if (traitEffect && roll_chance_i(traitEffect->GetAmount()))
                    caster->CastCustomSpell(SPELL_DH_SHATTERED_SOULS_MISSILE, SPELLVALUE_BASE_POINT0, SPELL_DH_LESSER_SOUL_SHARD, caster, true);
            }

            void Register()
            {
                OnEffectApply += AuraEffectApplyFn(spell_dh_artifact_overwhelming_power_AuraScript::OnApply, EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_overwhelming_power_AuraScript();
        }
};

// 207407 - Soul Carver
class spell_dh_artifact_soul_carver : public SpellScriptLoader
{
    public:
        spell_dh_artifact_soul_carver() : SpellScriptLoader("spell_dh_artifact_soul_carver") { }

        class spell_dh_artifact_soul_carver_SpellScript : public SpellScript
        {
            PrepareSpellScript(spell_dh_artifact_soul_carver_SpellScript);

            void HandleHit(SpellEffIndex /*effIndex*/)
            {
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                uint32 soulsToShatter = GetEffectInfo(EFFECT_3)->BasePoints;
                for (uint32 i = 0; i < soulsToShatter; ++i)
                    caster->CastCustomSpell(SPELL_DH_SHATTERED_SOULS_MISSILE, SPELLVALUE_BASE_POINT0, SPELL_DH_LESSER_SOUL_SHARD, caster, true);
            }

            void Register()
            {
                OnEffectHitTarget += SpellEffectFn(spell_dh_artifact_soul_carver_SpellScript::HandleHit, EFFECT_2, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
            }
        };

        class spell_dh_artifact_soul_carver_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_soul_carver_AuraScript);

            void PeriodicTick(AuraEffect const* /*aurEff*/)
            {
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                caster->CastCustomSpell(SPELL_DH_SHATTERED_SOULS_MISSILE, SPELLVALUE_BASE_POINT0, SPELL_DH_LESSER_SOUL_SHARD, caster, true);
            }

            void Register()
            {
                OnEffectPeriodic += AuraEffectPeriodicFn(spell_dh_artifact_soul_carver_AuraScript::PeriodicTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_soul_carver_AuraScript();
        }

        SpellScript* GetSpellScript() const
        {
            return new spell_dh_artifact_soul_carver_SpellScript();
        }
};

// 213010 - Charred Warblades
class spell_dh_artifact_charred_warblades : public SpellScriptLoader
{
    public:
        spell_dh_artifact_charred_warblades() : SpellScriptLoader("spell_dh_artifact_charred_warblades") { }

        class spell_dh_artifact_charred_warblades_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_charred_warblades_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                if (!caster || !eventInfo.GetDamageInfo())
                    return;

                if (!eventInfo.GetDamageInfo()->GetDamage() || !(eventInfo.GetDamageInfo()->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE))
                    return;

                int32 heal = CalculatePct(eventInfo.GetDamageInfo()->GetDamage(), aurEff->GetAmount());
                caster->CastCustomSpell(SPELL_DH_CHARRED_WARBLADES_HEAL, SPELLVALUE_BASE_POINT0, heal, caster, true);
            }

            void Register()
            {
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_charred_warblades_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_charred_warblades_AuraScript();
        }
};

// 213017 - Fueled by Pain
class spell_dh_artifact_fueled_by_pain : public SpellScriptLoader
{
    public:
        spell_dh_artifact_fueled_by_pain() : SpellScriptLoader("spell_dh_artifact_fueled_by_pain") { }

        class spell_dh_artifact_fueled_by_pain_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_fueled_by_pain_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                int32 duration = aurEff->GetAmount() * IN_MILLISECONDS;
                if (Aura* aur = caster->AddAura(SPELL_DH_METAMORPHOSIS_VENGEANCE, caster))
                {
                    aur->SetMaxDuration(duration);
                    aur->RefreshDuration();
                }
            }

            bool CheckProc(ProcEventInfo& eventInfo)
            {
                return eventInfo.GetSpellInfo() && (eventInfo.GetSpellInfo()->Id == SPELL_DH_SOUL_FRAGMENT_HEAL_VENGEANCE || eventInfo.GetSpellInfo()->Id == SPELL_DH_LESSER_SOUL_SHARD_HEAL);
            }

            void Register()
            {
                DoCheckProc += AuraCheckProcFn(spell_dh_artifact_fueled_by_pain_AuraScript::CheckProc);
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_fueled_by_pain_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_fueled_by_pain_AuraScript();
        }
};

// 212817 - Fiery Demise
class spell_dh_artifact_fiery_demise : public SpellScriptLoader
{
    public:
        spell_dh_artifact_fiery_demise() : SpellScriptLoader("spell_dh_artifact_fiery_demise") { }

        class spell_dh_artifact_fiery_demise_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_dh_artifact_fiery_demise_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* caster = GetCaster();
                Unit* target = eventInfo.GetActionTarget();
                if (!caster || !target || !caster->IsValidAttackTarget(target))
                    return;

                caster->CastCustomSpell(SPELL_DH_FIERY_DEMISE_DEBUFF, SPELLVALUE_BASE_POINT0, aurEff->GetAmount(), target, true);
            }

            void Register()
            {
                OnEffectProc += AuraEffectProcFn(spell_dh_artifact_fiery_demise_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const
        {
            return new spell_dh_artifact_fiery_demise_AuraScript();
        }
};

// 199471 - Soul Flame
class spell_warlock_artifact_soul_flame : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_soul_flame() : SpellScriptLoader("spell_warlock_artifact_soul_flame") { }

        class spell_warlock_artifact_soul_flame_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_soul_flame_AuraScript);

            void OnProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                Unit* target = eventInfo.GetActionTarget();
                Unit* caster = GetCaster();
                if (!caster || !target)
                    return;

                Position p = target->GetPosition();
                caster->GetScheduler().Schedule(300ms, [caster, p](TaskContext /*context*/)
                {
                    caster->CastSpell(p, SPELL_WARLOCK_SOUL_FLAME_PROC, true);
                });
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_soul_flame_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_soul_flame_AuraScript();
        }
};

// 199472 - Wrath of Consumption
class spell_warlock_artifact_wrath_of_consumption : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_wrath_of_consumption() : SpellScriptLoader("spell_warlock_artifact_wrath_of_consumption") { }

        class spell_warlock_artifact_wrath_of_consumption_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_wrath_of_consumption_AuraScript);

            void OnProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                if (Unit* caster = GetCaster())
                    caster->CastSpell(caster, SPELL_WARLOCK_WRATH_OF_CONSUMPTION_PROC, true);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_wrath_of_consumption_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_wrath_of_consumption_AuraScript();
        }
};

// 196305 - Eternal Struggle
class spell_warlock_artifact_eternal_struggle : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_eternal_struggle() : SpellScriptLoader("spell_warlock_artifact_eternal_struggle") { }

        class spell_warlock_artifact_eternal_struggle_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_eternal_struggle_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                PreventDefaultAction();
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                caster->CastCustomSpell(SPELL_WARLOCK_ETERNAL_STRUGGLE_PROC, SPELLVALUE_BASE_POINT0, aurEff->GetAmount(), caster, true);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_eternal_struggle_AuraScript::OnProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_eternal_struggle_AuraScript();
        }
};

// 196301 - Devourer of Life
class spell_warlock_artifact_devourer_of_life : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_devourer_of_life() : SpellScriptLoader("spell_warlock_artifact_devourer_of_life") { }

        class spell_warlock_artifact_devourer_of_life_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_devourer_of_life_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                PreventDefaultAction();
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                if (roll_chance_i(aurEff->GetAmount()))
                    caster->CastSpell(caster, SPELL_WARLOCK_DEVOURER_OF_LIFE_PROC, true);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_devourer_of_life_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_devourer_of_life_AuraScript();
        }
};

// 196236 - Soulsnatcher
class spell_warlock_artifact_soul_snatcher : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_soul_snatcher() : SpellScriptLoader("spell_warlock_artifact_soul_snatcher") { }

        class spell_warlock_artifact_soul_snatcher_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_soul_snatcher_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                PreventDefaultAction();
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                if (roll_chance_i(aurEff->GetAmount()))
                    caster->CastSpell(caster, SPELL_WARLOCK_SOULSNATCHER_PROC, true);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_soul_snatcher_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_soul_snatcher_AuraScript();
        }
};

// 219415 - Dimension Ripper
class spell_warlock_artifact_dimension_ripper : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_dimension_ripper() : SpellScriptLoader("spell_warlock_artifact_dimension_ripper") { }

        class spell_warlock_artifact_dimension_ripper_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_dimension_ripper_AuraScript);

            void OnProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                PreventDefaultAction();
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                caster->GetSpellHistory()->RestoreCharge(sSpellMgr->GetSpellInfo(SPELL_WARLOCK_DIMENSIONAL_RIFT)->ChargeCategoryId);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_dimension_ripper_AuraScript::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_dimension_ripper_AuraScript();
        }
};

// 211219 - The Expendables
class spell_warlock_artifact_the_expendables : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_the_expendables() : SpellScriptLoader("spell_warlock_artifact_the_expendables") { }

        class spell_warlock_artifact_the_expendables_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_the_expendables_AuraScript);

            void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                Unit* caster = GetCaster();
                if (!caster)
                    return;

                if (caster->ToPlayer())
                    return;

                Player* player = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
                if (!player)
                    return;

                for (Unit* unit : player->m_Controlled)
                    player->CastSpell(unit, SPELL_WARLOCK_THE_EXPANDABLES_BUFF, true);
            }

            void Register() override
            {
                OnEffectRemove += AuraEffectRemoveFn(spell_warlock_artifact_the_expendables_AuraScript::OnRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_the_expendables_AuraScript();
        }
};

// 211720 - Thal'kiel's Discord
class spell_warlock_artifact_thalkiels_discord : public SpellScriptLoader
{
    public:
        spell_warlock_artifact_thalkiels_discord() : SpellScriptLoader("spell_warlock_artifact_thalkiels_discord") { }

        class spell_warlock_artifact_thalkiels_discord_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_warlock_artifact_thalkiels_discord_AuraScript);

            void OnProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                ++ArtifactTraitTest::ProcCount[GetSpellInfo()->Id];
                PreventDefaultAction();
                Unit* caster = GetCaster();
                Unit* target = eventInfo.GetActionTarget();
                if (!caster || !target)
                    return;

                if (!caster->IsValidAttackTarget(target))
                    return;

                caster->CastSpell(target, aurEff->GetSpellEffectInfo()->TriggerSpell, true);
            }

            void Register() override
            {
                OnEffectProc += AuraEffectProcFn(spell_warlock_artifact_thalkiels_discord_AuraScript::OnProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_warlock_artifact_thalkiels_discord_AuraScript();
        }
};

// 202387 - Inner Demons (AreaTrigger, misc id 5823 -> areatrigger template 10529)
struct at_dh_artifact_inner_demons : AreaTriggerAI
{
    at_dh_artifact_inner_demons(AreaTrigger* areatrigger) : AreaTriggerAI(areatrigger) { }

    void OnInitialize() override
    {
        Unit* caster = at->GetCaster();
        if (!caster)
            return;

        ObjectGuid const guid = caster->Variables.GetValue<ObjectGuid>("Spells.InnerDemonsTarget", ObjectGuid::Empty);
        if (Unit* target = ObjectAccessor::GetUnit(*caster, guid))
        {
            std::vector<G3D::Vector3> splinePoints;
            float orientation = caster->GetOrientation();
            float posX = caster->GetPositionX() - 7.f * cos(orientation);
            float posY = caster->GetPositionY() - 7.f * sin(orientation); // Start from behind the caster
            splinePoints.push_back(G3D::Vector3(posX, posY, caster->GetPositionZ()));
            splinePoints.push_back(G3D::Vector3(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()));

            at->InitSplines(splinePoints, 1000);
        }
    }

    void OnRemove() override
    {
        Unit* caster = at->GetCaster();
        if (!caster)
            return;

        caster->CastSpell(*at, SPELL_DH_INNER_DEMONS_DAMAGE, true);
    }
};

// 211729 - Thal'kiel's Discord (AreaTrigger, misc id 6913 -> areatrigger template 11417)
struct at_warlock_artifact_thalkiels_discord : AreaTriggerAI
{
    at_warlock_artifact_thalkiels_discord(AreaTrigger* areatrigger) : AreaTriggerAI(areatrigger) { }

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 1300)
            return;

        _timer = 0;
        if (Unit* caster = at->GetCaster())
            {
            ++ArtifactTraitTest::ThalkielTicks;
            caster->CastSpell(*at, SPELL_WARLOCK_THALKIES_DISCORD_DAMAGE, true);
        }
    }

private:
    uint32 _timer = 0;
};

void AddSC_artifact_trait_spell_scripts()
{
    new spell_dh_artifact_demon_speed();
    new spell_dh_artifact_deceivers_fury();
    new spell_dh_artifact_anguish();
    new spell_dh_artifact_anguish_damage();
    new spell_dh_artifact_inner_demons();
    new spell_dh_artifact_rage_of_the_illidari();
    new spell_dh_artifact_overwhelming_power();
    new spell_dh_artifact_soul_carver();
    new spell_dh_artifact_charred_warblades();
    new spell_dh_artifact_fueled_by_pain();
    new spell_dh_artifact_fiery_demise();
    new spell_warlock_artifact_soul_flame();
    new spell_warlock_artifact_wrath_of_consumption();
    new spell_warlock_artifact_eternal_struggle();
    new spell_warlock_artifact_devourer_of_life();
    new spell_warlock_artifact_soul_snatcher();
    new spell_warlock_artifact_dimension_ripper();
    new spell_warlock_artifact_the_expendables();
    new spell_warlock_artifact_thalkiels_discord();
    RegisterAreaTriggerAI(at_dh_artifact_inner_demons);
    RegisterAreaTriggerAI(at_warlock_artifact_thalkiels_discord);
}
