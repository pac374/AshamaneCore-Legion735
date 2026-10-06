/*
 * Copyright (C) 2017-2018 AshamaneProject <https://github.com/AshamaneProject>
 * Copyright (C) 2008-2017 TrinityCore <http://www.trinitycore.org/>
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

#ifndef ClassHall_h__
#define ClassHall_h__

#include "Player.h"
#include "Garrison.h"

class GameObject;
class Map;

// Flags eines Ordenshallen-Talents (Feld "Flags" im Wire-Format GarrisonTalent / in der Tabelle character_garrison_talents)
enum ClassHallTalentFlags : int32
{
    CLASS_HALL_TALENT_IN_RESEARCH = 0,
    CLASS_HALL_TALENT_READY       = 1,
    CLASS_HALL_TALENT_CHANGE      = 2
};

class TC_GAME_API ClassHall : public Garrison
{
public:

    explicit ClassHall(Player* owner);

    bool LoadFromDB() override;
    void SaveToDB(CharacterDatabaseTransaction& trans) override;
    void Update(uint32 const diff) override;
    void Enter() override;

    bool Create(uint32 garrSiteId) override;
    void Delete() override;

    bool IsAllowedArea(AreaTableEntry const* area) const override;

    // --- Talente (OI-030) ---
    std::vector<WorldPackets::Garrison::GarrisonTalent> const& GetTalents() const { return _talents; }
    // CMSG_GARRISON_RESEARCH_TALENT: prueft, zieht Kosten ab, startet die Forschung (oder den Wechsel, wenn dieselbe Tier-Stufe schon belegt ist)
    void ResearchTalent(uint32 talentId);
    // true, wenn das Talent erforscht und fertig ist
    bool HasTalent(uint32 talentId) const;

private:
    void SendResearchResult(int32 result, uint32 talentId, uint32 researchTime, uint32 flags) const;
    void FinishTalent(WorldPackets::Garrison::GarrisonTalent& talent);
    void ApplyTalentPerks() const;

    std::vector<WorldPackets::Garrison::GarrisonTalent> _talents;
    uint32 _talentUpdateAccumMs = 0;
};

#endif // ClassHall_h__
