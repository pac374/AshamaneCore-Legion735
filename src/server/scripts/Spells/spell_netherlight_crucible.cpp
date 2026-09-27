/*
 * Legion-Server 2026-09-24 (Runde 14): Netherlight Crucible (7.3) and Concordance of the Legionfall (7.2).
 *
 * - go_netherlight_crucible: gossip front end for the server-side crucible rules in game/Entities/Item/NetherlightCrucible.
 *   The client relic forge window cannot be used (its protocol is undocumented, see NetherlightCrucible.h).
 * - spell_crucible_*: the ten Light/Shadow powers with proc effects. All values come from the power aura amount, which
 *   NetherlightCrucible::ApplyPowers sets to the ArtifactPowerRank.db2 value of the current rank (x1.5 with Insignia of
 *   the Grand Army). Proc masks and RPPM come from the client (SpellAuraOptions / SpellProcsPerMinute), the core generates
 *   the proc entries for these DUMMY auras itself. Triggered spells are named in the client descriptions
 *   ($252208d, $252907d, $252921d, $253022A1, $253072d, $253216d) or share name and school with the power.
 * - spell_arti_concordance_of_the_legionfall: 239042 is PROC_TRIGGER_SPELL without a trigger spell on all 36 artifacts;
 *   the buff is picked by the spec's stat negation aura exactly as in the tooltip
 *   ($?a162700[Versatility]?a162702[Versatility]?a162697[Agility]?a162698[Strength]?a162699[Intellect]?a162701[Intellect][primary stat]).
 *
 * SQL bindings: C:\LegionServer\fixes\r14_2026-09-24_netherlight_crucible_world.sql and
 *               C:\LegionServer\fixes\r14_2026-09-24_concordance_of_the_legionfall.sql
 */

#include "Chat.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MiscPackets.h"
#include "NetherlightCrucible.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "WorldSession.h"
#include <sstream>

namespace
{
    // total amount of a periodic spell spread over its ticks (duration / period from the client data)
    int32 PerTick(uint32 spellId, int32 total)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->GetEffect(EFFECT_0) || !info->GetEffect(EFFECT_0)->ApplyAuraPeriod)
            return total;

        int32 ticks = info->GetMaxDuration() / int32(info->GetEffect(EFFECT_0)->ApplyAuraPeriod);
        return ticks > 0 ? total / ticks : total;
    }

    bool IsHostileDamageProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage())
            return false;

        Unit* actor = eventInfo.GetActor();
        Unit* victim = damage->GetVictim();
        return actor && victim && victim != actor && victim->IsAlive() && actor->IsValidAttackTarget(victim);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Gossip front end
// ---------------------------------------------------------------------------------------------------------------------

enum NetherlightCrucibleGossip
{
    CRUCIBLE_ACTION_OVERVIEW    = 1,
    CRUCIBLE_ACTION_SLOT        = 100,  // + slot
    CRUCIBLE_ACTION_CHOOSE      = 200   // + slot * 10 + index
};

class go_netherlight_crucible : public GameObjectScript
{
public:
    go_netherlight_crucible() : GameObjectScript("go_netherlight_crucible") { }

    static Item* GetArtifact(Player* player)
    {
        Aura const* artifactAura = player->GetAura(ARTIFACTS_ALL_WEAPONS_GENERAL_WEAPON_EQUIPPED_PASSIVE);
        return artifactAura ? player->GetItemByGuid(artifactAura->GetCastItemGUID()) : nullptr;
    }

    static std::string SpellName(Player* player, uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->SpellName)
            return std::to_string(spellId);

        LocaleConstant locale = player->GetSession()->GetSessionDbcLocale();
        char const* name = info->SpellName->Str[locale];
        if (!name || !*name)
            name = info->SpellName->Str[LOCALE_enUS];
        return name;
    }

    static std::string TalentText(Player* player, Item* artifact, uint8 slot, uint8 index)
    {
        ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
        RelicTalentEntry const* talent = data.Options[index] ? sRelicTalentStore.LookupEntry(data.Options[index]) : nullptr;
        if (!talent)
            return "?";

        uint32 powerId = NetherlightCrucible::GetTalentArtifactPowerId(artifact, talent);
        ArtifactPowerRankEntry const* firstRank = powerId ? sDB2Manager.GetArtifactPowerRank(powerId, 0) : nullptr;
        std::string name = firstRank ? SpellName(player, uint32(firstRank->SpellID)) : "?";

        std::ostringstream text;
        uint8 tier = NetherlightCrucible::GetTalentTier(index);
        text << "Tier " << uint32(tier) << " - ";
        switch (talent->Type)
        {
            case NetherlightCrucible::RELIC_TALENT_TYPE_FORTIFICATION: text << name << " (+5 item level)"; break;
            case NetherlightCrucible::RELIC_TALENT_TYPE_SHADOW:        text << "Shadow: " << name; break;
            case NetherlightCrucible::RELIC_TALENT_TYPE_LIGHT:         text << "Light: " << name; break;
            case NetherlightCrucible::RELIC_TALENT_TYPE_TRAIT_RANK:    text << "+1 rank: " << name; break;
            default: break;
        }

        if (data.IsChosen(index))
            text << "  [chosen]";
        else if (NetherlightCrucible::IsRowTaken(data, index))
            text << "  [not taken]";
        else if (!NetherlightCrucible::IsReachable(data, index))
            text << "  [not reachable]";
        else if (uint32 required = NetherlightCrucible::GetRequiredArtifactLevel(slot, tier))
        {
            if (artifact->GetTotalPurchasedArtifactPowers() < required)
                text << "  [requires artifact level " << required << "]";
        }
        return text.str();
    }

    static void ShowOverview(Player* player, GameObject* go, Item* artifact)
    {
        ClearGossipMenuFor(player);
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SOCKETS; ++slot)
        {
            std::ostringstream text;
            text << "Relic " << uint32(slot + 1) << ": ";
            ItemDynamicFieldGems const* gem = artifact->GetGem(slot);
            ItemTemplate const* relic = gem && gem->ItemId ? sObjectMgr->GetItemTemplate(gem->ItemId) : nullptr;
            if (!relic)
            {
                text << "(empty)";
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, text.str(), GOSSIP_SENDER_MAIN, CRUCIBLE_ACTION_OVERVIEW);
                continue;
            }

            ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
            uint32 chosen = 0;
            for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
                if (data.IsChosen(i))
                    ++chosen;

            text << relic->GetName(player->GetSession()->GetSessionDbcLocale()) << " (" << chosen << "/3)";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, text.str(), GOSSIP_SENDER_MAIN, CRUCIBLE_ACTION_SLOT + slot);
        }
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, go->GetGUID());
    }

    static void ShowSlot(Player* player, GameObject* go, Item* artifact, uint8 slot)
    {
        if (!NetherlightCrucible::EnsureOptions(player, artifact, slot))
        {
            ShowOverview(player, go, artifact);
            return;
        }

        ClearGossipMenuFor(player);
        ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
        for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
        {
            std::string text = TalentText(player, artifact, slot, i);
            bool selectable = !data.IsChosen(i) && NetherlightCrucible::IsReachable(data, i) && !NetherlightCrucible::IsRowTaken(data, i)
                && artifact->GetTotalPurchasedArtifactPowers() >= NetherlightCrucible::GetRequiredArtifactLevel(slot, NetherlightCrucible::GetTalentTier(i));
            if (selectable)
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, text, GOSSIP_SENDER_MAIN, CRUCIBLE_ACTION_CHOOSE + slot * 10 + i,
                    "Imbue this relic with " + text + "? This choice cannot be undone.", 0, false);
            else
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, CRUCIBLE_ACTION_SLOT + slot);
        }
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, CRUCIBLE_ACTION_OVERVIEW);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, go->GetGUID());
    }

    static bool CheckAccess(Player* player, GameObject* go, Item*& artifact)
    {
        if (uint32 condition = go->GetGOInfo()->artifactForge.conditionID1)
            if (!player->MeetPlayerCondition(condition))
                return false;

        artifact = GetArtifact(player);
        if (!artifact)
        {
            player->SendDirectMessage(WorldPackets::Misc::DisplayGameError(GameError::ERR_MUST_EQUIP_ARTIFACT).Write());
            return false;
        }

        if (!NetherlightCrucible::IsAvailable())
        {
            ChatHandler(player->GetSession()).SendSysMessage("The Netherlight Crucible is not available on this server (RelicTalent.db2 or characters.item_instance_relic_talents missing).");
            return false;
        }
        return true;
    }

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        // Round 19: with NetherlightCrucible.ClientUI = 1 the default GameObject::Use path runs instead (condition and
        // artifact checks, SMSG_ARTIFACT_FORGE_OPENED -> client relic forge window, handlers in ArtifactHandler.cpp)
        if (NetherlightCrucible::IsClientUIEnabled())
            return false;

        Item* artifact = nullptr;
        if (CheckAccess(player, go, artifact))
            ShowOverview(player, go, artifact);
        else
            CloseGossipMenuFor(player);
        return true;
    }

    bool OnGossipSelect(Player* player, GameObject* go, uint32 /*sender*/, uint32 action) override
    {
        Item* artifact = nullptr;
        if (!CheckAccess(player, go, artifact))
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (action >= CRUCIBLE_ACTION_CHOOSE)
        {
            uint8 slot = uint8((action - CRUCIBLE_ACTION_CHOOSE) / 10);
            uint8 index = uint8((action - CRUCIBLE_ACTION_CHOOSE) % 10);
            if (slot >= MAX_ITEM_PROTO_SOCKETS)
            {
                ShowOverview(player, go, artifact);
                return true;
            }

            switch (NetherlightCrucible::ChooseTalent(player, artifact, slot, index))
            {
                case NetherlightCrucible::ChooseResult::Ok:
                    break;
                case NetherlightCrucible::ChooseResult::ArtifactLevel:
                    ChatHandler(player->GetSession()).PSendSysMessage("Your artifact needs %u purchased traits for this tier.",
                        NetherlightCrucible::GetRequiredArtifactLevel(slot, NetherlightCrucible::GetTalentTier(index)));
                    break;
                default:
                    ChatHandler(player->GetSession()).SendSysMessage("This power cannot be chosen.");
                    break;
            }
            ShowSlot(player, go, artifact, slot);
            return true;
        }

        if (action >= CRUCIBLE_ACTION_SLOT && action < CRUCIBLE_ACTION_SLOT + MAX_ITEM_PROTO_SOCKETS)
        {
            ShowSlot(player, go, artifact, uint8(action - CRUCIBLE_ACTION_SLOT));
            return true;
        }

        ShowOverview(player, go, artifact);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Light / Shadow powers (Netherlight Crucible tier 2)
// ---------------------------------------------------------------------------------------------------------------------

// 252207 Refractive Shell: "Your spells and abilities have the chance to cause Refractive Shell, absorbing $<insignia>
// damage. Lasts $252208d." -> 252208 (SCHOOL_ABSORB on self, 10 s)
class spell_crucible_refractive_shell : public AuraScript
{
    PrepareAuraScript(spell_crucible_refractive_shell);

    static constexpr uint32 SPELL_REFRACTIVE_SHELL_ABSORB = 252208;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_REFRACTIVE_SHELL_ABSORB });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        target->CastCustomSpell(SPELL_REFRACTIVE_SHELL_ABSORB, SPELLVALUE_BASE_POINT0, aurEff->GetAmount(), target, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_crucible_refractive_shell::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Shadow damage procs: 252875 Shadowbind -> 252879 (HEALTH_LEECH, damage and heal $s1, amplitude 1),
// 252888 Chaotic Darkness -> 252896 damage + 252897 heal, "${$<insignia>} to ${$<insignia>*5}",
// 252906 Torment the Weak -> 252907 (15 s DoT, 3 s period, 3 stacks), "${$<insignia>*5} over $252907d",
// 252922 Dark Sorrows -> 252921 Sorrow (8 s marker), burst 253022 on expiry (spell_crucible_sorrow).
class spell_crucible_shadow_power : public AuraScript
{
    PrepareAuraScript(spell_crucible_shadow_power);

    enum
    {
        SPELL_SHADOWBIND            = 252875,
        SPELL_SHADOWBIND_DAMAGE     = 252879,
        SPELL_CHAOTIC_DARKNESS      = 252888,
        SPELL_CHAOTIC_DARKNESS_DMG  = 252896,
        SPELL_CHAOTIC_DARKNESS_HEAL = 252897,
        SPELL_TORMENT_THE_WEAK      = 252906,
        SPELL_TORMENT_THE_WEAK_DOT  = 252907,
        SPELL_DARK_SORROWS          = 252922,
        SPELL_SORROW                = 252921
    };

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SHADOWBIND_DAMAGE, SPELL_CHAOTIC_DARKNESS_DMG, SPELL_CHAOTIC_DARKNESS_HEAL,
            SPELL_TORMENT_THE_WEAK_DOT, SPELL_SORROW });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return IsHostileDamageProc(eventInfo);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetDamageInfo()->GetVictim();
        int32 value = aurEff->GetAmount();
        if (value <= 0)
            return;

        switch (GetId())
        {
            case SPELL_SHADOWBIND:
                caster->CastCustomSpell(SPELL_SHADOWBIND_DAMAGE, SPELLVALUE_BASE_POINT0, value, victim, true, nullptr, aurEff);
                break;
            case SPELL_CHAOTIC_DARKNESS:
            {
                // one roll for damage and heal (the tooltip gives the same range for both; a separate roll per part would be
                // equally consistent with the text - documented as open point in the report)
                int32 amount = irand(value, value * 5);
                caster->CastCustomSpell(SPELL_CHAOTIC_DARKNESS_DMG, SPELLVALUE_BASE_POINT0, amount, victim, true, nullptr, aurEff);
                caster->CastCustomSpell(SPELL_CHAOTIC_DARKNESS_HEAL, SPELLVALUE_BASE_POINT0, amount, caster, true, nullptr, aurEff);
                break;
            }
            case SPELL_TORMENT_THE_WEAK:
                caster->CastCustomSpell(SPELL_TORMENT_THE_WEAK_DOT, SPELLVALUE_BASE_POINT0, PerTick(SPELL_TORMENT_THE_WEAK_DOT, value * 5), victim, true, nullptr, aurEff);
                break;
            case SPELL_DARK_SORROWS:
                caster->CastCustomSpell(SPELL_SORROW, SPELLVALUE_BASE_POINT0, value, victim, true, nullptr, aurEff);
                break;
            default:
                break;
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_crucible_shadow_power::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_crucible_shadow_power::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 252921 Sorrow: "Deals $o1 Shadow damage after $d." -> on expiry 253022 ("Deals $s1 Shadow damage to enemies within
// $A1 yards", DEST_TARGET_ENEMY + UNIT_DEST_AREA_ENEMY, 8 yd). No burst when the target dies earlier (only EXPIRE).
class spell_crucible_sorrow : public AuraScript
{
    PrepareAuraScript(spell_crucible_sorrow);

    static constexpr uint32 SPELL_SORROW_BURST = 253022;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SORROW_BURST });
    }

    void AfterRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        if (Unit* caster = GetCaster())
            caster->CastCustomSpell(SPELL_SORROW_BURST, SPELLVALUE_BASE_POINT0, aurEff->GetAmount(), GetTarget(), true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_crucible_sorrow::AfterRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Light powers with a damage and a healing branch:
// 253070 Secure in the Light: damage -> 253073 (Holy damage $s1), heal -> 253072 Holy Bulwark (absorb, TARGET_UNIT_CASTER)
// 253093 Infusion of Light:  damage -> 253098 (Holy damage $s1), heal -> 253099 (heal, TARGET_UNIT_TARGET_ALLY: the healed ally)
class spell_crucible_light_power : public AuraScript
{
    PrepareAuraScript(spell_crucible_light_power);

    enum
    {
        SPELL_SECURE_IN_THE_LIGHT   = 253070,
        SPELL_SECURE_DAMAGE         = 253073,
        SPELL_HOLY_BULWARK          = 253072,
        SPELL_INFUSION_OF_LIGHT     = 253093,
        SPELL_INFUSION_DAMAGE       = 253098,
        SPELL_INFUSION_HEAL         = 253099
    };

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_SECURE_DAMAGE, SPELL_HOLY_BULWARK, SPELL_INFUSION_DAMAGE, SPELL_INFUSION_HEAL });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (HealInfo* heal = eventInfo.GetHealInfo())
            return heal->GetHeal() && heal->GetTarget() && heal->GetTarget()->IsAlive();
        return IsHostileDamageProc(eventInfo);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        int32 value = aurEff->GetAmount();
        if (value <= 0)
            return;

        bool secure = GetId() == SPELL_SECURE_IN_THE_LIGHT;
        if (HealInfo* heal = eventInfo.GetHealInfo())
        {
            if (secure)
                caster->CastCustomSpell(SPELL_HOLY_BULWARK, SPELLVALUE_BASE_POINT0, value, caster, true, nullptr, aurEff);
            else
                caster->CastCustomSpell(SPELL_INFUSION_HEAL, SPELLVALUE_BASE_POINT0, value, heal->GetTarget(), true, nullptr, aurEff);
            return;
        }

        if (DamageInfo* damage = eventInfo.GetDamageInfo())
            caster->CastCustomSpell(secure ? SPELL_SECURE_DAMAGE : SPELL_INFUSION_DAMAGE, SPELLVALUE_BASE_POINT0, value, damage->GetVictim(), true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_crucible_light_power::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_crucible_light_power::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 253111 Light's Embrace: "Taking damage has a chance to grant you Light's Embrace, restoring $<insignia> health over
// $253216d. Light's Embrace may stack up to 5 times." -> 253216 (PERIODIC_HEAL 6 s / 2 s, CumulativeAura 5)
class spell_crucible_lights_embrace : public AuraScript
{
    PrepareAuraScript(spell_crucible_lights_embrace);

    static constexpr uint32 SPELL_LIGHTS_EMBRACE_HOT = 253216;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_LIGHTS_EMBRACE_HOT });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return damage && damage->GetDamage() && damage->GetVictim() == GetTarget();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        target->CastCustomSpell(SPELL_LIGHTS_EMBRACE_HOT, SPELLVALUE_BASE_POINT0, PerTick(SPELL_LIGHTS_EMBRACE_HOT, aurEff->GetAmount()), target, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_crucible_lights_embrace::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_crucible_lights_embrace::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Concordance of the Legionfall (239042, artifact trait on all 36 artifacts, ArtifactPower 1484-1624 step 4, ranks 1-100)
// ---------------------------------------------------------------------------------------------------------------------

// "Your abilities have a chance to trigger Concordance of the Legionfall, increasing your [stat] by ${$s1*$<mult>} for
// $242583d." SDV: $mult=${1}. RPPM 2 (SpellProcsPerMinute 223), ProcTypeMask 2446608. E0 is PROC_TRIGGER_SPELL with
// EffectTriggerSpell 0, so without a script the core only logs an error. Buffs (10 s, bp 1000 placeholder):
// 242583 Strength, 242584 Agility, 242586 Intellect, 243096 Versatility.
// Crucible: Murderous Intent 252191 (+Versatility 252202) and Shocklight 252799 (+Critical Strike 252801) are active
// "while Concordance of the Legionfall is active" - they are applied together with the buff, for the same 10 s.
class spell_arti_concordance_of_the_legionfall : public AuraScript
{
    PrepareAuraScript(spell_arti_concordance_of_the_legionfall);

    enum
    {
        SPELL_CONCORDANCE_STRENGTH      = 242583,
        SPELL_CONCORDANCE_AGILITY       = 242584,
        SPELL_CONCORDANCE_INTELLECT     = 242586,
        SPELL_CONCORDANCE_VERSATILITY   = 243096,
        SPELL_NEGATION_AGILITY_DPS      = 162697,
        SPELL_NEGATION_STRENGTH_DPS     = 162698,
        SPELL_NEGATION_INTELLECT_DPS    = 162699,
        SPELL_NEGATION_AGILITY_TANK     = 162700,
        SPELL_NEGATION_INTELLECT_HEALER = 162701,
        SPELL_NEGATION_STRENGTH_TANK    = 162702,
        SPELL_MURDEROUS_INTENT          = 252191,
        SPELL_MURDEROUS_INTENT_BUFF     = 252202,
        SPELL_SHOCKLIGHT                = 252799,
        SPELL_SHOCKLIGHT_BUFF           = 252801
    };

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_CONCORDANCE_STRENGTH, SPELL_CONCORDANCE_AGILITY, SPELL_CONCORDANCE_INTELLECT,
            SPELL_CONCORDANCE_VERSATILITY, SPELL_MURDEROUS_INTENT_BUFF, SPELL_SHOCKLIGHT_BUFF });
    }

    uint32 SelectBuff(Unit* target) const
    {
        // same order as the tooltip condition chain
        if (target->HasAura(SPELL_NEGATION_AGILITY_TANK) || target->HasAura(SPELL_NEGATION_STRENGTH_TANK))
            return SPELL_CONCORDANCE_VERSATILITY;
        if (target->HasAura(SPELL_NEGATION_AGILITY_DPS))
            return SPELL_CONCORDANCE_AGILITY;
        if (target->HasAura(SPELL_NEGATION_STRENGTH_DPS))
            return SPELL_CONCORDANCE_STRENGTH;
        if (target->HasAura(SPELL_NEGATION_INTELLECT_DPS) || target->HasAura(SPELL_NEGATION_INTELLECT_HEALER))
            return SPELL_CONCORDANCE_INTELLECT;

        // "[primary stat]" branch: no stat negation aura (should not happen for a specialised character)
        float str = target->GetStat(STAT_STRENGTH);
        float agi = target->GetStat(STAT_AGILITY);
        float intel = target->GetStat(STAT_INTELLECT);
        if (str >= agi && str >= intel)
            return SPELL_CONCORDANCE_STRENGTH;
        return agi >= intel ? SPELL_CONCORDANCE_AGILITY : SPELL_CONCORDANCE_INTELLECT;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        int32 amount = aurEff->GetAmount();     // rank value from ArtifactPowerRank.db2 (4000 + 300 per rank), $mult = 1
        if (amount <= 0)
            return;

        target->CastCustomSpell(SelectBuff(target), SPELLVALUE_BASE_POINT0, amount, target, true, nullptr, aurEff);

        if (AuraEffect const* murderousIntent = target->GetAuraEffect(SPELL_MURDEROUS_INTENT, EFFECT_0))
            if (murderousIntent->GetAmount() > 0)
                target->CastCustomSpell(SPELL_MURDEROUS_INTENT_BUFF, SPELLVALUE_BASE_POINT0, murderousIntent->GetAmount(), target, true, nullptr, aurEff);

        if (AuraEffect const* shocklight = target->GetAuraEffect(SPELL_SHOCKLIGHT, EFFECT_0))
            if (shocklight->GetAmount() > 0)
                target->CastCustomSpell(SPELL_SHOCKLIGHT_BUFF, SPELLVALUE_BASE_POINT0, shocklight->GetAmount(), target, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_arti_concordance_of_the_legionfall::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

void AddSC_netherlight_crucible_scripts()
{
    new go_netherlight_crucible();
    RegisterAuraScript(spell_crucible_refractive_shell);
    RegisterAuraScript(spell_crucible_shadow_power);
    RegisterAuraScript(spell_crucible_sorrow);
    RegisterAuraScript(spell_crucible_light_power);
    RegisterAuraScript(spell_crucible_lights_embrace);
    RegisterAuraScript(spell_arti_concordance_of_the_legionfall);
}
