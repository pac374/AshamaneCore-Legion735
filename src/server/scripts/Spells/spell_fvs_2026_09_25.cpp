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

// Legion-Server "freie Vollsystem-Suche" (FVS, 2026-09-25): item/enchant procs that could never fire.
//
// Found by a new scan of every Legion item spell (ItemEffect ON_EQUIP of ExpansionID 6 items, ItemSetSpell of Legion
// sets, SpellItemEnchantment EQUIP_SPELL/COMBAT_SPELL): the equip aura is a DUMMY (aura 4) with a proc mask and PPM in
// SpellAuraOptions, but its effect carries no trigger spell and nothing in the core/scripts handles it. The core still
// generates a proc entry for DUMMY auras (SpellMgr isTriggerAura[SPELL_AURA_DUMMY]), so the proc rolls - and does
// nothing. The scripts below only add the missing "what happens on proc"; chance/PPM/mask stay the client values.
//
// Proc -> effect links come from the client itself:
//  - Mark of the Claw 190888 -> 190909: the enchanting spell 190892 reads "increase Critical Strike and Haste by
//    $190909s1 for $190909d".
//  - Mark of the Heavy Hide 228398 -> 228399: the enchanting spell 228402 reads "increase armor by $228399s1 for $228399d".
//  - Screams of the Dead 214798 (Memento of Angerboda) -> one random of 214802/214803/214807, whose descriptions are
//    "$@spelldesc214798" and whose amount is "$w1" (runtime value). The only scaled amount is 214798 E1 (aura 189 with
//    misc 0, SpellEffect.Coefficient 2.2977815, SpellMisc Attributes[11] 0x4 = scales with item level), so the buff
//    gets that amount.
//  - Huntmaster's Infusion 224169 -> 224170/224172/224173 (crit/haste/mastery, "$@spelldesc224169", "$w1"), "by $s2
//    ..., whichever is highest" -> amount = 224169 E1, buff = the rating the player has most of.
// Not done (value not in the client): Mark of the Hidden Satyr / Distant Army (damage 191259 BP -1 / 191380 BP 1,
// the real coefficients are server side), Caged Horror "Dark Blast" (controller 214399 is a plain DUMMY).
// Binding SQL: C:\LegionServer\fixes\fvs_2026-09-25_item_enchant_procs.sql - without it the scripts are inert.

#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

namespace
{
    Item* FvsCastItem(Aura const* aura, Unit* caster)
    {
        if (Player* player = caster ? caster->ToPlayer() : nullptr)
            return player->GetItemByGuid(aura->GetCastItemGUID());
        return nullptr;
    }
}

// 190888 Mark of the Claw, 228398 Mark of the Heavy Hide
class spell_fvs_enchant_proc_buff : public AuraScript
{
    PrepareAuraScript(spell_fvs_enchant_proc_buff);

    static uint32 BuffFor(uint32 auraId)
    {
        switch (auraId)
        {
            case 190888: return 190909;
            case 228398: return 228399;
            default:     return 0;
        }
    }

    bool Validate(SpellInfo const* spellInfo) override
    {
        uint32 buff = BuffFor(spellInfo->Id);
        return buff && ValidateSpellInfo({ buff });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        caster->CastSpell(caster, BuffFor(GetId()), true, FvsCastItem(GetAura(), caster), aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_fvs_enchant_proc_buff::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 214798 Screams of the Dead (Memento of Angerboda), 224169 Huntmaster's Infusion
class spell_fvs_trinket_rating_proc : public AuraScript
{
    PrepareAuraScript(spell_fvs_trinket_rating_proc);

    enum : uint32
    {
        SPELL_SCREAMS_OF_THE_DEAD   = 214798,
        SPELL_HOWL_OF_INGVAR        = 214802, // crit
        SPELL_WAIL_OF_SVALA         = 214803, // haste
        SPELL_DIRGE_OF_ANGERBODA    = 214807, // mastery
        SPELL_HUNTMASTERS_INFUSION  = 224169,
        SPELL_HUNTMASTER_CRIT       = 224170,
        SPELL_HUNTMASTER_HASTE      = 224172,
        SPELL_HUNTMASTER_MASTERY    = 224173
    };

    bool Validate(SpellInfo const* spellInfo) override
    {
        if (!spellInfo->GetEffect(EFFECT_1))
            return false;
        if (spellInfo->Id == SPELL_SCREAMS_OF_THE_DEAD)
            return ValidateSpellInfo({ SPELL_HOWL_OF_INGVAR, SPELL_WAIL_OF_SVALA, SPELL_DIRGE_OF_ANGERBODA });
        if (spellInfo->Id == SPELL_HUNTMASTERS_INFUSION)
            return ValidateSpellInfo({ SPELL_HUNTMASTER_CRIT, SPELL_HUNTMASTER_HASTE, SPELL_HUNTMASTER_MASTERY });
        return false;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        AuraEffect const* amountEff = GetAura()->GetEffect(EFFECT_1);
        int32 amount = amountEff ? amountEff->GetAmount() : 0;
        if (amount <= 0)
            return;

        uint32 buff = 0;
        if (GetId() == SPELL_SCREAMS_OF_THE_DEAD)
        {
            static uint32 const buffs[] = { SPELL_HOWL_OF_INGVAR, SPELL_WAIL_OF_SVALA, SPELL_DIRGE_OF_ANGERBODA };
            buff = buffs[urand(0, 2)];
        }
        else
        {
            Player* player = caster->ToPlayer();
            if (!player)
                return;
            uint32 crit = player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_CRIT_MELEE);
            uint32 haste = player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_HASTE_MELEE);
            uint32 mastery = player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_MASTERY);
            buff = SPELL_HUNTMASTER_CRIT;
            uint32 best = crit;
            if (haste > best) { best = haste; buff = SPELL_HUNTMASTER_HASTE; }
            if (mastery > best) { buff = SPELL_HUNTMASTER_MASTERY; }
        }

        caster->CastCustomSpell(buff, SPELLVALUE_BASE_POINT0, amount, caster, true, FvsCastItem(GetAura(), caster), aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_fvs_trinket_rating_proc::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddSC_fvs_2026_09_25_spell_scripts()
{
    RegisterAuraScript(spell_fvs_enchant_proc_buff);
    RegisterAuraScript(spell_fvs_trinket_rating_proc);
}
