/*
 * Legion-Server 2026-09-24 (Runde 14): server-side Netherlight Crucible. See NetherlightCrucible.h for the design and the
 * source of every rule. Nothing here is hard-coded game data except the talent layout (client UI source) and the six
 * artifact-level thresholds (Blizzard preview article); pools, weights, ranks, values and item-level bonus lists are read
 * from RelicTalent.db2 / ArtifactPower(.Rank).db2 / ItemBonus.db2.
 */

#include "NetherlightCrucible.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <atomic>

namespace
{
    // Blizzard_ArtifactRelicForgeUI.lua (7.3.5), TALENTS_LAYOUT, converted to 0-based indices:
    //   [1] row 1 neutral            [2] row 2 void, links {1}     [3] row 2 light, links {1}
    //   [4] row 3 void, links {2}    [5] row 3 neutral, links {2,3} [6] row 3 light, links {3}
    struct TalentLayoutEntry
    {
        uint8 Row;
        uint8 LinkMask;     // talent is reachable when any of these indices is chosen (0 = root)
    };

    constexpr TalentLayoutEntry TalentLayout[MAX_RELIC_TALENT_OPTIONS] =
    {
        { 1, 0x00 },
        { 2, 0x01 },
        { 2, 0x01 },
        { 3, 0x02 },
        { 3, 0x06 },
        { 3, 0x04 },
    };

    // Artifact level (total purchased ranks) needed per relic slot for tier 2 / tier 3.
    // Blizzard "Patch 7.3 Preview: Netherlight Crucible" (60 / 69, "fully unlocked once you reach Artifact level 75"),
    // per-slot values identical on MMO-Champion (content 6942) and Blizzard Watch (13.09.2017).
    constexpr uint32 TierRequirement[3][2] =
    {
        { 60, 69 },
        { 63, 72 },
        { 66, 75 },
    };

    std::atomic<int> TableAvailable{ -1 };

    RelicTalentEntry const* GetOption(ItemRelicTalentData const& data, uint8 index)
    {
        return data.Options[index] ? sRelicTalentStore.LookupEntry(data.Options[index]) : nullptr;
    }

    bool HasRelic(Item const* artifact, uint8 slot)
    {
        ItemDynamicFieldGems const* gem = artifact->GetGem(slot);
        return gem && gem->ItemId;
    }

    void SaveSlot(Item const* artifact, uint8 slot)
    {
        ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
        if (!data.HasOptions())
        {
            CharacterDatabase.PExecute("DELETE FROM item_instance_relic_talents WHERE itemGuid = %u AND slot = %u",
                artifact->GetGUID().GetCounter(), uint32(slot));
            return;
        }

        CharacterDatabase.PExecute("REPLACE INTO item_instance_relic_talents (itemGuid, slot, relicItemId, option1, option2, option3, option4, option5, option6, chosenMask) "
            "VALUES (%u, %u, %u, %u, %u, %u, %u, %u, %u, %u)",
            artifact->GetGUID().GetCounter(), uint32(slot), data.RelicItemId,
            data.Options[0], data.Options[1], data.Options[2], data.Options[3], data.Options[4], data.Options[5], uint32(data.ChosenMask));
    }

    // data-only change of CurrentRankWithBonus; callers wrap equipped items in _ApplyItemMods(false/true)
    void ChangeTraitRank(Item* artifact, uint32 artifactPowerId, int32 delta)
    {
        ItemDynamicFieldArtifactPowers const* power = artifact->GetArtifactPower(artifactPowerId);
        if (!power)
            return;

        ItemDynamicFieldArtifactPowers newPower = *power;
        int32 rank = int32(newPower.CurrentRankWithBonus) + delta;
        newPower.CurrentRankWithBonus = uint8(std::max(rank, 0));
        artifact->SetArtifactPower(&newPower);
    }

    RelicTalentEntry const* GetFortificationTalent()
    {
        for (RelicTalentEntry const* talent : sRelicTalentStore)
            if (talent->Type == NetherlightCrucible::RELIC_TALENT_TYPE_FORTIFICATION)
                return talent;
        return nullptr;
    }

    // Netherlight Fortification: rank = number of relics with talent 1 chosen, bonus list from ArtifactPowerRank.db2
    // (1739: 3593/3594/3595 = +5/+10/+15 item level, ItemBonus.db2 Type 1). Applied to the artifact and to its child item,
    // like Item::CopyArtifactDataFromParent keeps the relic item level of both halves equal.
    void UpdateFortificationOn(Item* item, uint32 rank)
    {
        RelicTalentEntry const* fortification = GetFortificationTalent();
        if (!fortification)
            return;

        uint32 wanted = 0;
        for (uint8 r = 0; r < NetherlightCrucible::MAX_RELIC_TALENT_RANK; ++r)
        {
            ArtifactPowerRankEntry const* rankEntry = sDB2Manager.GetArtifactPowerRank(fortification->ArtifactPowerID, r);
            if (!rankEntry || !rankEntry->ItemBonusListID)
                continue;

            if (rank && r == rank - 1)
                wanted = rankEntry->ItemBonusListID;
            else
                item->RemoveItemLevelBonusList(rankEntry->ItemBonusListID);
        }

        if (wanted)
            item->AddBonuses(wanted);
    }

    uint32 GetFortificationRank(Item const* artifact)
    {
        uint32 rank = 0;
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SOCKETS; ++slot)
        {
            if (!HasRelic(artifact, slot))
                continue;

            ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
            if (RelicTalentEntry const* talent = GetOption(data, 0))
                if (data.IsChosen(0) && talent->Type == NetherlightCrucible::RELIC_TALENT_TYPE_FORTIFICATION)
                    ++rank;
        }
        return std::min<uint32>(rank, NetherlightCrucible::MAX_RELIC_TALENT_RANK);
    }

    void UpdateFortification(Player* owner, Item* artifact)
    {
        uint32 rank = GetFortificationRank(artifact);
        UpdateFortificationOn(artifact, rank);
        artifact->SetState(ITEM_CHANGED, owner);

        if (!artifact->GetChildItem().IsEmpty())
        {
            if (Item* child = owner->GetChildItemByGuid(artifact->GetChildItem()))
            {
                bool equipped = child->IsEquipped();
                if (equipped)
                    owner->_ApplyItemMods(child, child->GetSlot(), false);
                UpdateFortificationOn(child, rank);
                child->SetState(ITEM_CHANGED, owner);
                if (equipped)
                    owner->_ApplyItemMods(child, child->GetSlot(), true);
            }
        }
    }

    template<class Pred>
    RelicTalentEntry const* PickWeighted(Pred pred)
    {
        uint64 total = 0;
        for (RelicTalentEntry const* talent : sRelicTalentStore)
            if (pred(talent))
                total += std::max<uint32>(talent->PVal, 1);

        if (!total)
            return nullptr;

        uint64 roll = urand(0, uint32(total - 1));
        for (RelicTalentEntry const* talent : sRelicTalentStore)
        {
            if (!pred(talent))
                continue;

            uint64 weight = std::max<uint32>(talent->PVal, 1);
            if (roll < weight)
                return talent;
            roll -= weight;
        }
        return nullptr;
    }

    uint32 GetArtifactPowerIdByLabel(Item const* artifact, int32 label)
    {
        for (ArtifactPowerEntry const* power : sDB2Manager.GetArtifactPowers(artifact->GetTemplate()->GetArtifactID()))
            if (power->Label == label && artifact->GetArtifactPower(power->ID))
                return power->ID;
        return 0;
    }

    Item* GetEquippedArtifact(Player* owner)
    {
        Aura const* artifactAura = owner->GetAura(ARTIFACTS_ALL_WEAPONS_GENERAL_WEAPON_EQUIPPED_PASSIVE);
        return artifactAura ? owner->GetItemByGuid(artifactAura->GetCastItemGUID()) : nullptr;
    }
}

namespace NetherlightCrucible
{
bool IsAvailable()
{
    if (!sRelicTalentStore.GetNumRows())
        return false;

    int state = TableAvailable.load();
    if (state < 0)
    {
        QueryResult result = CharacterDatabase.Query("SHOW TABLES LIKE 'item_instance_relic_talents'");
        state = result ? 1 : 0;
        TableAvailable.store(state);
        if (!state)
            TC_LOG_ERROR("server.loading", "Netherlight Crucible: table characters.item_instance_relic_talents is missing "
                "(C:\\LegionServer\\fixes\\r14_2026-09-24_netherlight_crucible_characters.sql), crucible disabled");
    }
    return state == 1;
}

uint8 GetTalentTier(uint8 index)
{
    return index < MAX_RELIC_TALENT_OPTIONS ? TalentLayout[index].Row : 0;
}

uint32 GetRequiredArtifactLevel(uint8 slot, uint8 tier)
{
    if (slot >= 3 || tier < 2 || tier > 3)
        return 0;
    return TierRequirement[slot][tier - 2];
}

bool IsReachable(ItemRelicTalentData const& data, uint8 index)
{
    if (index >= MAX_RELIC_TALENT_OPTIONS)
        return false;

    uint8 links = TalentLayout[index].LinkMask;
    return !links || (data.ChosenMask & links) != 0;
}

bool IsRowTaken(ItemRelicTalentData const& data, uint8 index)
{
    for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
        if (i != index && TalentLayout[i].Row == TalentLayout[index].Row && data.IsChosen(i))
            return true;
    return false;
}

uint32 GetTalentArtifactPowerId(Item const* artifact, RelicTalentEntry const* talent)
{
    if (!talent)
        return 0;

    if (talent->Type == RELIC_TALENT_TYPE_TRAIT_RANK)
        return GetArtifactPowerIdByLabel(artifact, talent->ArtifactPowerLabel);

    return talent->ArtifactPowerID;
}

void LoadForPlayer(ObjectGuid::LowType playerGuid, PlayerRelicTalentData& out)
{
    if (!IsAvailable())
        return;

    //                                                 0         1     2            3        4        5        6        7        8        9
    QueryResult result = CharacterDatabase.PQuery("SELECT rt.itemGuid, rt.slot, rt.relicItemId, rt.option1, rt.option2, rt.option3, rt.option4, rt.option5, rt.option6, rt.chosenMask "
        "FROM item_instance_relic_talents rt INNER JOIN character_inventory ci ON ci.item = rt.itemGuid WHERE ci.guid = %u", playerGuid);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint8 slot = fields[1].GetUInt8();
        if (slot >= MAX_ITEM_PROTO_SOCKETS)
            continue;

        ItemRelicTalentData& data = out[ObjectGuid::Create<HighGuid::Item>(fields[0].GetUInt64())][slot];
        data.RelicItemId = fields[2].GetUInt32();
        for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
        {
            data.Options[i] = fields[3 + i].GetUInt32();
            if (data.Options[i] && !sRelicTalentStore.LookupEntry(data.Options[i]))
                data.Options[i] = 0;
        }
        data.ChosenMask = fields[9].GetUInt8() & ((1 << MAX_RELIC_TALENT_OPTIONS) - 1);
    } while (result->NextRow());
}

void OnArtifactLoaded(Player* owner, Item* artifact, PlayerRelicTalentData const& allData)
{
    if (!IsAvailable() || !artifact->GetTemplate()->GetArtifactID() || artifact->HasFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_CHILD))
        return;

    auto itr = allData.find(artifact->GetGUID());
    for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SOCKETS; ++slot)
    {
        ItemRelicTalentData& data = artifact->GetRelicTalentData(slot);
        data = ItemRelicTalentData();
        if (itr == allData.end())
            continue;

        ItemRelicTalentData const& stored = itr->second[slot];
        if (!stored.HasOptions())
            continue;

        // the relic was replaced outside of WorldSession::HandleSocketGems (GM command, DB edit): options are stale
        ItemDynamicFieldGems const* gem = artifact->GetGem(slot);
        if (!gem || gem->ItemId != stored.RelicItemId)
        {
            SaveSlot(artifact, slot);   // data is empty -> DELETE
            continue;
        }

        data = stored;

        for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
        {
            RelicTalentEntry const* talent = GetOption(data, i);
            if (data.IsChosen(i) && talent && talent->Type == RELIC_TALENT_TYPE_TRAIT_RANK)
                if (uint32 powerId = GetTalentArtifactPowerId(artifact, talent))
                    ChangeTraitRank(artifact, powerId, 1);
        }
    }

    // also removes a stale Fortification bonus list if no talent is chosen (anymore)
    uint32 rank = GetFortificationRank(artifact);
    UpdateFortificationOn(artifact, rank);

    UpdateClientField(artifact);    // round 19, no-op unless NetherlightCrucible.ClientUI = 1
}

bool EnsureOptions(Player* owner, Item* artifact, uint8 slot)
{
    if (!IsAvailable() || slot >= MAX_ITEM_PROTO_SOCKETS)
        return false;

    ItemDynamicFieldGems const* gem = artifact->GetGem(slot);
    if (!gem || !gem->ItemId)
        return false;

    ItemRelicTalentData& data = artifact->GetRelicTalentData(slot);
    if (data.HasOptions() && data.RelicItemId == gem->ItemId)
        return true;

    ItemRelicTalentData fresh;
    fresh.RelicItemId = gem->ItemId;

    RelicTalentEntry const* fortification = GetFortificationTalent();
    RelicTalentEntry const* shadow = PickWeighted([](RelicTalentEntry const* t) { return t->Type == RELIC_TALENT_TYPE_SHADOW; });
    RelicTalentEntry const* light = PickWeighted([](RelicTalentEntry const* t) { return t->Type == RELIC_TALENT_TYPE_LIGHT; });
    if (!fortification || !shadow || !light)
        return false;

    fresh.Options[0] = fortification->ID;
    fresh.Options[1] = shadow->ID;
    fresh.Options[2] = light->ID;

    // tier 3: three different minor traits (+1 rank), never the relic's own trait (BonusData::GemRelicType = the label
    // the relic already improves, see Item::ApplyArtifactPowerEnchantmentBonuses), only labels the weapon has
    int32 ownLabel = artifact->GetBonus()->GemRelicType[slot];
    for (uint8 i = 3; i < MAX_RELIC_TALENT_OPTIONS; ++i)
    {
        RelicTalentEntry const* pick = PickWeighted([&](RelicTalentEntry const* t)
        {
            if (t->Type != RELIC_TALENT_TYPE_TRAIT_RANK || int32(t->ArtifactPowerLabel) == ownLabel)
                return false;
            for (uint8 j = 3; j < i; ++j)
                if (fresh.Options[j] == t->ID)
                    return false;
            return GetArtifactPowerIdByLabel(artifact, t->ArtifactPowerLabel) != 0;
        });
        if (!pick)
            return false;
        fresh.Options[i] = pick->ID;
    }

    data = fresh;
    SaveSlot(artifact, slot);
    TC_LOG_DEBUG("entities.player.items", "Netherlight Crucible: %s rolled relic slot %u of %s: %u %u %u %u %u %u",
        owner->GetGUID().ToString().c_str(), uint32(slot), artifact->GetGUID().ToString().c_str(),
        data.Options[0], data.Options[1], data.Options[2], data.Options[3], data.Options[4], data.Options[5]);
    UpdateClientField(artifact);    // round 19, no-op unless NetherlightCrucible.ClientUI = 1
    return true;
}

ChooseResult ChooseTalent(Player* owner, Item* artifact, uint8 slot, uint8 index)
{
    if (!IsAvailable() || slot >= MAX_ITEM_PROTO_SOCKETS || index >= MAX_RELIC_TALENT_OPTIONS)
        return ChooseResult::Unavailable;

    if (!HasRelic(artifact, slot))
        return ChooseResult::NoRelic;

    ItemRelicTalentData& data = artifact->GetRelicTalentData(slot);
    RelicTalentEntry const* talent = GetOption(data, index);
    if (!talent)
        return ChooseResult::NoOptions;

    if (data.IsChosen(index))
        return ChooseResult::AlreadyChosen;

    if (!IsReachable(data, index))
        return ChooseResult::NotReachable;

    if (IsRowTaken(data, index))
        return ChooseResult::RowTaken;

    if (artifact->GetTotalPurchasedArtifactPowers() < GetRequiredArtifactLevel(slot, GetTalentTier(index)))
        return ChooseResult::ArtifactLevel;

    bool equipped = artifact->IsEquipped();
    if (equipped)
        owner->_ApplyItemMods(artifact, artifact->GetSlot(), false);

    data.ChosenMask |= uint8(1 << index);

    uint32 powerId = GetTalentArtifactPowerId(artifact, talent);
    if (talent->Type == RELIC_TALENT_TYPE_TRAIT_RANK && powerId)
        ChangeTraitRank(artifact, powerId, 1);

    if (talent->Type == RELIC_TALENT_TYPE_FORTIFICATION)
        UpdateFortification(owner, artifact);

    if (equipped)
        owner->_ApplyItemMods(artifact, artifact->GetSlot(), true);

    artifact->SetState(ITEM_CHANGED, owner);
    SaveSlot(artifact, slot);
    UpdateClientField(artifact);    // round 19, no-op unless NetherlightCrucible.ClientUI = 1

    // CRITERIA_TYPE_RELIC_TALENT_UNLOCKED (211), asset = ArtifactPowerID (quest 49224: 1739 Netherlight Fortification)
    if (powerId)
        owner->UpdateCriteria(CRITERIA_TYPE_RELIC_TALENT_UNLOCKED, powerId);

    return ChooseResult::Ok;
}

void OnRelicSocketed(Player* owner, Item* artifact, uint8 slot)
{
    if (!IsAvailable() || slot >= MAX_ITEM_PROTO_SOCKETS || !artifact->GetTemplate()->GetArtifactID())
        return;

    ItemRelicTalentData& data = artifact->GetRelicTalentData(slot);
    if (!data.HasOptions())
        return;

    bool hadFortification = false;
    for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
    {
        RelicTalentEntry const* talent = GetOption(data, i);
        if (!data.IsChosen(i) || !talent)
            continue;

        if (talent->Type == RELIC_TALENT_TYPE_TRAIT_RANK)
            if (uint32 powerId = GetTalentArtifactPowerId(artifact, talent))
                ChangeTraitRank(artifact, powerId, -1);

        if (talent->Type == RELIC_TALENT_TYPE_FORTIFICATION)
            hadFortification = true;
    }

    // the new relic starts un-attuned: options are rolled the next time the crucible is used
    data = ItemRelicTalentData();
    SaveSlot(artifact, slot);

    if (hadFortification)
        UpdateFortification(owner, artifact);

    UpdateClientField(artifact);    // round 19, no-op unless NetherlightCrucible.ClientUI = 1
}

int32 GetPowerRank(Item const* artifact, uint32 artifactPowerId)
{
    int32 rank = 0;
    for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SOCKETS; ++slot)
    {
        if (!HasRelic(artifact, slot))
            continue;

        ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
        for (uint8 i = 0; i < MAX_RELIC_TALENT_OPTIONS; ++i)
        {
            RelicTalentEntry const* talent = GetOption(data, i);
            if (data.IsChosen(i) && talent && talent->Type != RELIC_TALENT_TYPE_TRAIT_RANK && talent->ArtifactPowerID == artifactPowerId)
                ++rank;
        }
    }
    return std::min<int32>(rank, MAX_RELIC_TALENT_RANK);
}

// AuraPointsOverride of the rank, times the aura-218 modifiers on SpellLabel 332 for that effect
// (Insignia of the Grand Army 251977: E0 SPELLMOD_EFFECT1 +50 %, E1 SPELLMOD_EFFECT2 +50 %; tooltip SDV 362
// "$insignia=$?a251977[${$s1*1.5}][${$s1}]").
int32 GetPowerValue(Player const* owner, uint32 artifactPowerId, uint8 rank, uint8 effIndex)
{
    if (!rank)
        return 0;

    ArtifactPowerRankEntry const* rankEntry = sDB2Manager.GetArtifactPowerRank(artifactPowerId, rank - 1);
    if (!rankEntry)
        return 0;

    static SpellModOp const effectOps[] = { SPELLMOD_EFFECT1, SPELLMOD_EFFECT2, SPELLMOD_EFFECT3, SPELLMOD_EFFECT4, SPELLMOD_EFFECT5 };
    int32 pct = 0;
    if (effIndex < std::extent<decltype(effectOps)>::value)
        for (AuraEffect const* aurEff : owner->GetAuraEffectsByType(SPELL_AURA_218))
            if (uint32(aurEff->GetMiscValueB()) == SPELL_LABEL_CRUCIBLE_POWERS && aurEff->GetMiscValue() == int32(effectOps[effIndex]))
                pct += aurEff->GetAmount();

    float value = rankEntry->AuraPointsOverride;
    if (pct)
        value = value * (100.0f + float(pct)) / 100.0f;

    return int32(value);
}

void ApplyPowers(Player* owner, Item* artifact, bool apply)
{
    if (!IsAvailable() || !artifact->GetTemplate()->GetArtifactID() || artifact->HasFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_CHILD))
        return;

    for (RelicTalentEntry const* talent : sRelicTalentStore)
    {
        if (talent->Type != RELIC_TALENT_TYPE_SHADOW && talent->Type != RELIC_TALENT_TYPE_LIGHT)
            continue;

        ArtifactPowerRankEntry const* firstRank = sDB2Manager.GetArtifactPowerRank(talent->ArtifactPowerID, 0);
        if (!firstRank)
            continue;

        uint32 spellId = uint32(firstRank->SpellID);
        owner->RemoveAurasDueToItemSpell(spellId, artifact->GetGUID());

        if (!apply)
            continue;

        int32 rank = GetPowerRank(artifact, talent->ArtifactPowerID);
        if (!rank)
            continue;

        ArtifactPowerRankEntry const* rankEntry = sDB2Manager.GetArtifactPowerRank(talent->ArtifactPowerID, uint8(rank - 1));
        SpellInfo const* spellInfo = rankEntry ? sSpellMgr->GetSpellInfo(uint32(rankEntry->SpellID)) : nullptr;
        if (!spellInfo)
            continue;

        CustomSpellValues csv;
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->GetEffect(i))
                csv.AddSpellMod(SpellValueMod(SPELLVALUE_BASE_POINT0 + i), GetPowerValue(owner, talent->ArtifactPowerID, uint8(rank), uint8(i)));

        owner->CastCustomSpell(spellInfo->Id, csv, owner, TRIGGERED_FULL_MASK, artifact);
    }
}

void RefreshPowers(Player* owner)
{
    if (!IsAvailable())
        return;

    if (Item* artifact = GetEquippedArtifact(owner))
        ApplyPowers(owner, artifact, true);
}

bool IsClientUIEnabled()
{
    // read once (a missing key logs a warning on every read); changing it needs a restart
    static bool const enabled = sConfigMgr->GetBoolDefault("NetherlightCrucible.ClientUI", false);
    return enabled;
}

// Layout see NetherlightCrucible.h. Slots without rolled options stay 0 (as the gaps in LegionCore), so the client finds
// no talents for that socket and its UI calls AttuneSocketedRelic (Blizzard_ArtifactRelicForgeUI.lua RefreshTalents).
void UpdateClientField(Item* artifact)
{
    if (!artifact || !IsClientUIEnabled() || !IsAvailable() || !artifact->GetTemplate()->GetArtifactID()
        || artifact->HasFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_CHILD))
        return;

    std::array<uint32, MAX_ITEM_PROTO_SOCKETS * 6> values = { };
    uint32 used = 0;
    for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SOCKETS; ++slot)
    {
        ItemRelicTalentData const& data = artifact->GetRelicTalentData(slot);
        if (!data.HasOptions() || !HasRelic(artifact, slot))
            continue;

        uint32* v = &values[slot * 6];
        v[0] = 1;
        v[1] = uint32(slot) + 2;
        v[2] = ((data.Options[0] & 0xFFFF) << 16) | uint32(data.ChosenMask);
        v[3] = ((data.Options[2] & 0xFFFF) << 16) | (data.Options[1] & 0xFFFF);
        v[4] = ((data.Options[4] & 0xFFFF) << 16) | (data.Options[3] & 0xFFFF);
        v[5] = data.Options[5] & 0xFFFF;
        used = (slot + 1) * 6;
    }

    if (artifact->GetDynamicValues(ITEM_DYNAMIC_FIELD_RELIC_TALENT_DATA).size() > used)
        artifact->ClearDynamicValue(ITEM_DYNAMIC_FIELD_RELIC_TALENT_DATA);

    for (uint32 i = 0; i < used; ++i)
        artifact->SetDynamicValue(ITEM_DYNAMIC_FIELD_RELIC_TALENT_DATA, uint16(i), values[i]);
}
}
