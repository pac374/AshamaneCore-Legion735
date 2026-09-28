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

#include "WorldSession.h"
#include "ArtifactPackets.h"
#include "ConditionMgr.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameTables.h"
#include "Item.h"
#include "Log.h"
#include "NetherlightCrucible.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellPackets.h"

void WorldSession::HandleArtifactAddPower(WorldPackets::Artifact::ArtifactAddPower& artifactAddPower)
{
    if (!_player->GetGameObjectIfCanInteractWith(artifactAddPower.ForgeGUID, GAMEOBJECT_TYPE_ARTIFACT_FORGE))
        return;

    Item* artifact = _player->GetItemByGuid(artifactAddPower.ArtifactGUID);
    if (!artifact)
        return;

    uint32 currentArtifactTier = artifact->GetModifier(ITEM_MODIFIER_ARTIFACT_TIER);

    uint64 xpCost = 0;
    if (GtArtifactLevelXPEntry const* cost = sArtifactLevelXPGameTable.GetRow(artifact->GetTotalPurchasedArtifactPowers() + 1))
        xpCost = uint64(currentArtifactTier == MAX_ARTIFACT_TIER ? cost->XP2 : cost->XP);

    if (xpCost > artifact->GetUInt64Value(ITEM_FIELD_ARTIFACT_XP))
        return;

    if (artifactAddPower.PowerChoices.empty())
        return;

    ItemDynamicFieldArtifactPowers const* artifactPower = artifact->GetArtifactPower(artifactAddPower.PowerChoices[0].ArtifactPowerID);
    if (!artifactPower)
        return;

    ArtifactPowerEntry const* artifactPowerEntry = sArtifactPowerStore.LookupEntry(artifactPower->ArtifactPowerId);
    if (!artifactPowerEntry)
        return;

    if (artifactPowerEntry->Tier > currentArtifactTier)
        return;

    uint32 maxRank = artifactPowerEntry->MaxPurchasableRank;
    if (artifactPowerEntry->Tier < currentArtifactTier)
    {
        if (artifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_FINAL)
            maxRank = 1;
        else if (artifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_MAX_RANK_WITH_TIER)
            maxRank += currentArtifactTier - artifactPowerEntry->Tier;
    }

    if (artifactAddPower.PowerChoices[0].Rank != artifactPower->PurchasedRank + 1 ||
        artifactAddPower.PowerChoices[0].Rank > maxRank)
        return;

    if (!(artifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_NO_LINK_REQUIRED))
    {
        if (std::unordered_set<uint32> const* artifactPowerLinks = sDB2Manager.GetArtifactPowerLinks(artifactPower->ArtifactPowerId))
        {
            bool hasAnyLink = false;
            for (uint32 artifactPowerLinkId : *artifactPowerLinks)
            {
                ArtifactPowerEntry const* artifactPowerLink = sArtifactPowerStore.LookupEntry(artifactPowerLinkId);
                if (!artifactPowerLink)
                    continue;

                ItemDynamicFieldArtifactPowers const* artifactPowerLinkLearned = artifact->GetArtifactPower(artifactPowerLinkId);
                if (!artifactPowerLinkLearned)
                    continue;

                if (artifactPowerLinkLearned->PurchasedRank >= artifactPowerLink->MaxPurchasableRank)
                {
                    hasAnyLink = true;
                    break;
                }
            }

            if (!hasAnyLink)
                return;
        }
    }

    ArtifactPowerRankEntry const* artifactPowerRank = sDB2Manager.GetArtifactPowerRank(artifactPower->ArtifactPowerId, artifactPower->CurrentRankWithBonus + 1 - 1); // need data for next rank, but -1 because of how db2 data is structured
    if (!artifactPowerRank)
        return;

    ItemDynamicFieldArtifactPowers newPower = *artifactPower;
    ++newPower.PurchasedRank;
    ++newPower.CurrentRankWithBonus;
    artifact->SetArtifactPower(&newPower);

    if (artifact->IsEquipped())
    {
        _player->ApplyArtifactPowerRank(artifact, artifactPowerRank, true);

        for (ItemDynamicFieldArtifactPowers const& power : artifact->GetArtifactPowers())
        {
            ArtifactPowerEntry const* scaledArtifactPowerEntry = sArtifactPowerStore.AssertEntry(power.ArtifactPowerId);
            if (!(scaledArtifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_SCALES_WITH_NUM_POWERS))
                continue;

            ArtifactPowerRankEntry const* scaledArtifactPowerRank = sDB2Manager.GetArtifactPowerRank(scaledArtifactPowerEntry->ID, 0);
            if (!scaledArtifactPowerRank)
                continue;

            ItemDynamicFieldArtifactPowers newScaledPower = power;
            ++newScaledPower.CurrentRankWithBonus;
            artifact->SetArtifactPower(&newScaledPower);

            _player->ApplyArtifactPowerRank(artifact, scaledArtifactPowerRank, false);
            _player->ApplyArtifactPowerRank(artifact, scaledArtifactPowerRank, true);
        }
    }

    artifact->SetUInt64Value(ITEM_FIELD_ARTIFACT_XP, artifact->GetUInt64Value(ITEM_FIELD_ARTIFACT_XP) - xpCost);
    artifact->SetState(ITEM_CHANGED, _player);

    uint32 totalPurchasedArtifactPower = artifact->GetTotalPurchasedArtifactPowers();
    uint32 artifactTier = 0;

    for (ArtifactTierEntry const* tier : sArtifactTierStore)
    {
        if (artifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_FINAL && artifactPowerEntry->Tier < MAX_ARTIFACT_TIER)
        {
            artifactTier = artifactPowerEntry->Tier + 1;
            break;
        }

        if (totalPurchasedArtifactPower < tier->MaxNumTraits)
        {
            artifactTier = tier->ArtifactTier;
            break;
        }
    }

    artifactTier = std::max(artifactTier, currentArtifactTier);

    for (uint32 i = currentArtifactTier; i <= artifactTier; ++i)
        artifact->InitArtifactPowers(artifact->GetTemplate()->GetArtifactID(), uint8(i));

    artifact->SetModifier(ITEM_MODIFIER_ARTIFACT_TIER, artifactTier);

    // Legion-Server 2026-09-24: CRITERIA_TYPE_ARTIFACT_TRAITS_UNLOCKED (191) = purchased ranks of this artifact
    // (TC master: CriteriaType::AnyArtifactPowerRankPurchased, same call). Unblocks the class artifact
    // tutorial quests (39192 The Forge of Odyn, ...) and 46744.
    _player->UpdateCriteria(CRITERIA_TYPE_ARTIFACT_TRAITS_UNLOCKED, totalPurchasedArtifactPower);
}

void WorldSession::HandleArtifactSetAppearance(WorldPackets::Artifact::ArtifactSetAppearance& artifactSetAppearance)
{
    if (!_player->GetGameObjectIfCanInteractWith(artifactSetAppearance.ForgeGUID, GAMEOBJECT_TYPE_ARTIFACT_FORGE))
        return;

    ArtifactAppearanceEntry const* artifactAppearance = sArtifactAppearanceStore.LookupEntry(artifactSetAppearance.ArtifactAppearanceID);
    if (!artifactAppearance)
        return;

    Item* artifact = _player->GetItemByGuid(artifactSetAppearance.ArtifactGUID);
    if (!artifact)
        return;

    ArtifactAppearanceSetEntry const* artifactAppearanceSet = sArtifactAppearanceSetStore.LookupEntry(artifactAppearance->ArtifactAppearanceSetID);
    if (!artifactAppearanceSet || artifactAppearanceSet->ArtifactID != artifact->GetTemplate()->GetArtifactID())
        return;

    if (PlayerConditionEntry const* playerCondition = sPlayerConditionStore.LookupEntry(artifactAppearance->UnlockPlayerConditionID))
        if (!sConditionMgr->IsPlayerMeetingCondition(_player, playerCondition))
            return;

    artifact->SetAppearanceModId(artifactAppearance->ItemAppearanceModifierID);
    artifact->SetModifier(ITEM_MODIFIER_ARTIFACT_APPEARANCE_ID, artifactAppearance->ID);
    artifact->SetState(ITEM_CHANGED, _player);
    Item* childItem = _player->GetChildItemByGuid(artifact->GetChildItem());
    if (childItem)
    {
        childItem->SetAppearanceModId(artifactAppearance->ItemAppearanceModifierID);
        childItem->SetState(ITEM_CHANGED, _player);
    }

    if (artifact->IsEquipped())
    {
        // change weapon appearance
        _player->SetVisibleItemSlot(artifact->GetSlot(), artifact);
        if (childItem)
            _player->SetVisibleItemSlot(childItem->GetSlot(), childItem);

        // change druid form appearance
        if (artifactAppearance->OverrideShapeshiftDisplayID && artifactAppearance->OverrideShapeshiftFormID && _player->GetShapeshiftForm() == ShapeshiftForm(artifactAppearance->OverrideShapeshiftFormID))
            _player->RestoreDisplayId(_player->IsMounted());
    }
}

void WorldSession::HandleConfirmArtifactRespec(WorldPackets::Artifact::ConfirmArtifactRespec& confirmArtifactRespec)
{
    if (!_player->GetNPCIfCanInteractWith(confirmArtifactRespec.NpcGUID, UNIT_NPC_FLAG_ARTIFACT_POWER_RESPEC))
        return;

    Item* artifact = _player->GetItemByGuid(confirmArtifactRespec.ArtifactGUID);
    if (!artifact)
        return;

    uint64 xpCost = 0;
    if (GtArtifactLevelXPEntry const* cost = sArtifactLevelXPGameTable.GetRow(artifact->GetTotalPurchasedArtifactPowers() + 1))
        xpCost = uint64(artifact->GetModifier(ITEM_MODIFIER_ARTIFACT_TIER) == 1 ? cost->XP2 : cost->XP);

    if (xpCost > artifact->GetUInt64Value(ITEM_FIELD_ARTIFACT_XP))
        return;

    uint64 newAmount = artifact->GetUInt64Value(ITEM_FIELD_ARTIFACT_XP) - xpCost;
    for (uint32 i = 0; i <= artifact->GetTotalPurchasedArtifactPowers(); ++i)
        if (GtArtifactLevelXPEntry const* cost = sArtifactLevelXPGameTable.GetRow(i))
            newAmount += uint64(artifact->GetModifier(ITEM_MODIFIER_ARTIFACT_TIER) == 1 ? cost->XP2 : cost->XP);

    for (ItemDynamicFieldArtifactPowers const& artifactPower : artifact->GetArtifactPowers())
    {
        uint8 oldPurchasedRank = artifactPower.PurchasedRank;
        if (!oldPurchasedRank)
            continue;

        ItemDynamicFieldArtifactPowers newPower = artifactPower;
        newPower.PurchasedRank -= oldPurchasedRank;
        newPower.CurrentRankWithBonus -= oldPurchasedRank;
        artifact->SetArtifactPower(&newPower);

        if (artifact->IsEquipped())
            if (ArtifactPowerRankEntry const* artifactPowerRank = sDB2Manager.GetArtifactPowerRank(artifactPower.ArtifactPowerId, 0))
                _player->ApplyArtifactPowerRank(artifact, artifactPowerRank, false);
    }

    for (ItemDynamicFieldArtifactPowers const& power : artifact->GetArtifactPowers())
    {
        ArtifactPowerEntry const* scaledArtifactPowerEntry = sArtifactPowerStore.AssertEntry(power.ArtifactPowerId);
        if (!(scaledArtifactPowerEntry->Flags & ARTIFACT_POWER_FLAG_SCALES_WITH_NUM_POWERS))
            continue;

        ArtifactPowerRankEntry const* scaledArtifactPowerRank = sDB2Manager.GetArtifactPowerRank(scaledArtifactPowerEntry->ID, 0);
        if (!scaledArtifactPowerRank)
            continue;

        ItemDynamicFieldArtifactPowers newScaledPower = power;
        newScaledPower.CurrentRankWithBonus = 0;
        artifact->SetArtifactPower(&newScaledPower);

        _player->ApplyArtifactPowerRank(artifact, scaledArtifactPowerRank, false);
    }

    artifact->SetUInt64Value(ITEM_FIELD_ARTIFACT_XP, newAmount);
    artifact->SetState(ITEM_CHANGED, _player);
}

// ---------------------------------------------------------------------------------------------------------------------
// Round 19 (25.09.2026): Relic Forge / Netherlight Crucible client window - CODE CANDIDATE, inactive unless
// worldserver.conf NetherlightCrucible.ClientUI = 1. Wire format and slot numbering from LegionCore-7.3.5 (see
// NetherlightCrucible.h); all game rules stay in NetherlightCrucible (the same code the gossip menu uses), so a wrong
// packet can at worst be ignored, never grant anything the gossip path would not grant.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    enum : uint32
    {
        ARTIFACT_FORGE_TYPE_RELIC_FORGE = 1,    // GameObjectData.h artifactForge.ForgeType: enum { Artifact Forge, Relic Forge }
        RELIC_SOCKET_INDEX_OFFSET       = 2     // LegionCore: socket index in packets and in the update field = gem slot + 2
    };

    Item* GetRelicForgeArtifact(Player* player, ObjectGuid const& forgeGuid, ObjectGuid const& artifactGuid)
    {
        if (!NetherlightCrucible::IsClientUIEnabled() || !NetherlightCrucible::IsAvailable())
            return nullptr;

        GameObject* forge = player->GetGameObjectIfCanInteractWith(forgeGuid, GAMEOBJECT_TYPE_ARTIFACT_FORGE);
        if (!forge || forge->GetGOInfo()->artifactForge.ForgeType != ARTIFACT_FORGE_TYPE_RELIC_FORGE)
            return nullptr;

        if (uint32 condition = forge->GetGOInfo()->artifactForge.conditionID1)
            if (!player->MeetPlayerCondition(condition))
                return nullptr;

        Item* artifact = player->GetItemByGuid(artifactGuid);
        if (!artifact || !artifact->GetTemplate()->GetArtifactID() || artifact->HasFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_CHILD))
            return nullptr;

        return artifact;
    }

    bool GetRelicSlot(Player* player, uint32 socketIndex, char const* opcode, uint8& slot)
    {
        if (socketIndex < RELIC_SOCKET_INDEX_OFFSET || socketIndex - RELIC_SOCKET_INDEX_OFFSET >= MAX_ITEM_PROTO_SOCKETS)
        {
            // logged on purpose: the +2 numbering is the one part of the format that only LegionCore documents
            TC_LOG_INFO("network", "%s: %s sent socket index %u, expected %u..%u - packet ignored", opcode,
                player->GetGUID().ToString().c_str(), socketIndex, uint32(RELIC_SOCKET_INDEX_OFFSET),
                uint32(RELIC_SOCKET_INDEX_OFFSET + MAX_ITEM_PROTO_SOCKETS - 1));
            return false;
        }

        slot = uint8(socketIndex - RELIC_SOCKET_INDEX_OFFSET);
        return true;
    }
}

void WorldSession::HandleArtifactAttuneSocketedRelic(WorldPackets::Artifact::ArtifactAttuneSocketedRelic& packet)
{
    TC_LOG_DEBUG("network", "CMSG_ARTIFACT_ATTUNE_SOCKETED_RELIC: %s artifact %s forge %s socket %u", _player->GetGUID().ToString().c_str(),
        packet.ArtifactGUID.ToString().c_str(), packet.ForgeGUID.ToString().c_str(), packet.RelicSlotIndex);

    Item* artifact = GetRelicForgeArtifact(_player, packet.ForgeGUID, packet.ArtifactGUID);
    uint8 slot = 0;
    if (!artifact || !GetRelicSlot(_player, packet.RelicSlotIndex, "CMSG_ARTIFACT_ATTUNE_SOCKETED_RELIC", slot))
        return;

    // rolls the six options once per relic (same as opening the relic in the gossip menu), then (re)sends the field
    NetherlightCrucible::EnsureOptions(_player, artifact, slot);
    NetherlightCrucible::UpdateClientField(artifact);
}

void WorldSession::HandleArtifactAddRelicTalent(WorldPackets::Artifact::ArtifactAddRelicTalent& packet)
{
    TC_LOG_DEBUG("network", "CMSG_ARTIFACT_ADD_RELIC_TALENT: %s artifact %s forge %s socket %u talent %u", _player->GetGUID().ToString().c_str(),
        packet.ArtifactGUID.ToString().c_str(), packet.ForgeGUID.ToString().c_str(), packet.SlotIndex, uint32(packet.TalentIndex));

    Item* artifact = GetRelicForgeArtifact(_player, packet.ForgeGUID, packet.ArtifactGUID);
    uint8 slot = 0;
    if (!artifact || !GetRelicSlot(_player, packet.SlotIndex, "CMSG_ARTIFACT_ADD_RELIC_TALENT", slot))
        return;

    // all rules (options rolled, layout links, one talent per row, artifact level per slot/tier) are checked there
    NetherlightCrucible::ChooseResult result = NetherlightCrucible::ChooseTalent(_player, artifact, slot, packet.TalentIndex);
    if (result != NetherlightCrucible::ChooseResult::Ok)
        TC_LOG_DEBUG("network", "CMSG_ARTIFACT_ADD_RELIC_TALENT: rejected (result %u)", uint32(result));

    // resend the server state either way, so the window never shows a choice the server did not accept
    NetherlightCrucible::UpdateClientField(artifact);
}

void WorldSession::HandleArtifactAttunePreviewRelic(WorldPackets::Artifact::ArtifactAttunePreviewRelic& packet)
{
    // Not implemented: attuning a relic in the bags before socketing needs talent storage per relic item (LegionCore writes
    // the field on the relic item itself and copies it on socketing). Relics are attuned after socketing instead, which the
    // window supports (AttuneSocketedRelic). The packet is only logged.
    TC_LOG_DEBUG("network", "CMSG_ARTIFACT_ATTUNE_PREVIEW_RELIC: %s relic %s forge %s - preview attunement not supported, ignored",
        _player->GetGUID().ToString().c_str(), packet.RelicGUID.ToString().c_str(), packet.ForgeGUID.ToString().c_str());
}