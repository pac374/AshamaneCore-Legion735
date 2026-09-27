/*
 * Legion-Server 2026-09-24 (Runde 14): server-side Netherlight Crucible (7.3 relic talents).
 *
 * The client protocol of the real Relic Forge UI (CMSG_ARTIFACT_ADD_RELIC_TALENT / ..._ATTUNE_*, the layout of
 * ITEM_DYNAMIC_FIELD_RELIC_TALENT_DATA) is not documented in any open source (TrinityCore 7.3.5 and AshamaneCore leave the
 * opcodes Handle_NULL, WowPacketParser only knows the opcode numbers). This module therefore implements the complete
 * *game rules* server-side and exposes them through a gossip menu on the Crucible game object (script
 * go_netherlight_crucible). The client relic forge window is not used.
 *
 * Sources of every rule (details: C:\LegionServer\reports\trait_candidates_report.md, "Runde 12"):
 *  - talent layout per relic (6 talents, rows 1/2/3, links 2,3<-1  4<-2  5<-2,3  6<-3): Blizzard 7.3.5 UI source
 *    Blizzard_ArtifactRelicForgeUI.lua (TALENTS_LAYOUT), Gethe/wow-ui-source tag 7.3.5
 *  - talent pool and weights: RelicTalent.db2 (Type 0 Fortification, 1 Shadow, 2 Light, 3 +1 rank by ArtifactPower.Label,
 *    PVal = weight)
 *  - rank of a power = number of relics that chose it (3 ranks in ArtifactPowerRank.db2; "they'll simply stack":
 *    peakofserenity.com 13.08.2017 and eyesofthebeast.com; SimulationCraft legion-dev uses the summed rank value)
 *  - artifact level per relic slot/tier (60/63/66, 69/72/75): Blizzard "Patch 7.3 Preview: Netherlight Crucible",
 *    MMO-Champion and Blizzard Watch (identical numbers)
 *  - tier 3 = three different random minor traits, never the relic's own trait: MMO-Champion, Blizzard Watch
 *  - Insignia of the Grand Army (251977, aura 218 on SpellLabel 332 = exactly the 12 Light/Shadow power spells):
 *    x(1 + amount%) on $s1/$s2, tooltip formula SpellDescriptionVariables 362
 */

#ifndef NetherlightCrucible_h__
#define NetherlightCrucible_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <string>
#include <unordered_map>

class Item;
class Player;
struct ItemRelicTalentData;
struct RelicTalentEntry;

namespace NetherlightCrucible
{
    enum : uint32
    {
        SPELL_LABEL_CRUCIBLE_POWERS         = 332,  // spelllabel.csv: 252088 252091 252191 252207 252799 252875 252888 252906 252922 253070 253093 253111
        RELIC_TALENT_TYPE_FORTIFICATION     = 0,
        RELIC_TALENT_TYPE_SHADOW            = 1,
        RELIC_TALENT_TYPE_LIGHT             = 2,
        RELIC_TALENT_TYPE_TRAIT_RANK        = 3,
        RELIC_TALENT_TIERS                  = 3,
        MAX_RELIC_TALENT_RANK               = 3     // ArtifactPowerRank.db2 has 3 ranks for every crucible power
    };

    enum class ChooseResult
    {
        Ok,
        Unavailable,
        NoRelic,
        NoOptions,
        AlreadyChosen,
        NotReachable,
        RowTaken,
        ArtifactLevel
    };

    using PlayerRelicTalentData = std::unordered_map<ObjectGuid, std::array<ItemRelicTalentData, 3>>;

    // RelicTalent.db2 loaded and characters.item_instance_relic_talents present (checked once)
    bool IsAvailable();

    uint8 GetTalentTier(uint8 index);                               // 1..3 for UI index 0..5
    uint32 GetRequiredArtifactLevel(uint8 slot, uint8 tier);
    bool IsReachable(ItemRelicTalentData const& data, uint8 index);
    bool IsRowTaken(ItemRelicTalentData const& data, uint8 index);
    uint32 GetTalentArtifactPowerId(Item const* artifact, RelicTalentEntry const* talent);

    // login: one query per character, then per artifact item right after Item::LoadArtifactData (item not equipped yet)
    void LoadForPlayer(ObjectGuid::LowType playerGuid, PlayerRelicTalentData& out);
    void OnArtifactLoaded(Player* owner, Item* artifact, PlayerRelicTalentData const& data);

    // gossip
    bool EnsureOptions(Player* owner, Item* artifact, uint8 slot);  // "attune": roll the 6 options once per relic
    ChooseResult ChooseTalent(Player* owner, Item* artifact, uint8 slot, uint8 index);

    // WorldSession::HandleSocketGems, between _ApplyItemMods(false) and (true): a new relic resets its slot
    void OnRelicSocketed(Player* owner, Item* artifact, uint8 slot);

    // Player::ApplyArtifactPowers: Light/Shadow power auras of the chosen talents
    void ApplyPowers(Player* owner, Item* artifact, bool apply);
    // aura 218 (label 332) changed: re-cast the power auras with the new multiplier
    void RefreshPowers(Player* owner);

    int32 GetPowerRank(Item const* artifact, uint32 artifactPowerId);
    int32 GetPowerValue(Player const* owner, uint32 artifactPowerId, uint8 rank, uint8 effIndex);

    // -----------------------------------------------------------------------------------------------------------------
    // Round 19 (25.09.2026): client Relic Forge window (CODE CANDIDATE, off by default).
    // worldserver.conf "NetherlightCrucible.ClientUI" (default 0). With 0 nothing below changes any behaviour: the field
    // is never written and the three CMSG handlers return at once; the gossip menu stays the front end.
    // With 1: go_netherlight_crucible lets GameObject::Use run, which sends SMSG_ARTIFACT_FORGE_OPENED; GO 273272 has
    // Data5 ForgeType = 1 ("Relic Forge", GameObjectData.h enum) so the client opens its relic forge window.
    //
    // Source of the wire format: the reference core (a community fork for build 26972),
    // ArtifactPackets.cpp / ArtifactHandler.cpp / Item.cpp (GetArtifactSockets, CreateSocketTalents,
    // AddOrRemoveSocketTalent). Single lineage (DestinyCore carries the same code), NOT confirmed by a sniff.
    // ITEM_DYNAMIC_FIELD_RELIC_TALENT_DATA, 6 uint32 per relic slot at offset slot * 6:
    //   [0] 1 (the reference core "unk1", always 1 when created)
    //   [1] socket index = relic (gem) slot + 2
    //   [2] (RelicTalent ID of talent 0 << 16) | chosen mask (bit i = talent index i chosen); the reference core creates 65536
    //       = ID 1 << 16 = the Type-0 row of RelicTalent.db2 (Netherlight Fortification) - consistent with the client data
    //   [3] (RelicTalent ID of talent 2 (light) << 16) | RelicTalent ID of talent 1 (shadow)
    //   [4] (RelicTalent ID of talent 4 << 16) | RelicTalent ID of talent 3
    //   [5] RelicTalent ID of talent 5
    // Talent index order = ItemRelicTalentData::Options order (Blizzard UI TALENTS_LAYOUT, 0-based) - the reference core uses the
    // same mapping (1 = shadow = low half of [3], 2 = light = high half, cross-checked against RelicTalent.db2 types).
    bool IsClientUIEnabled();
    void UpdateClientField(Item* artifact);
}

#endif // NetherlightCrucible_h__
