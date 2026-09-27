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

#include "ScenarioMgr.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "InstanceScenario.h"
#include "Log.h"
#include "Map.h"
#include "ScenarioPackets.h"
#include "CriteriaHandler.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include <set>

ScenarioMgr* ScenarioMgr::Instance()
{
    static ScenarioMgr instance;
    return &instance;
}

// Legion-Server round 16: several scenario maps host more than one scenario (LFGDungeons.db2 lists e.g. Shield's Rest
// 1495 with 909/1068/1082, Niskara 1489 with five, Ulduar 1579 with 1125/1148, Black Temple 1621 with 1124/1131/1170).
// The `scenarios` table can hold only one per map and difficulty, so those maps have no row. The quests that need one
// of these scenarios name it themselves: their quest objective (type CRITERIA_TREE) contains a criterion of type 152
// COMPLETE_SCENARIO whose asset is the ScenarioID. When the creator of the instance carries exactly one incomplete quest
// whose required scenario is listed for this map and difficulty in LFGDungeons, that scenario is started. Nothing is
// chosen when there is no such quest or more than one candidate.
uint32 ScenarioMgr::FindQuestRequiredScenario(Map const* map, Player const* creator) const
{
    if (!creator)
        return 0;

    std::set<uint32> mapScenarios;
    for (LFGDungeonsEntry const* dungeon : sLFGDungeonsStore)
        if (dungeon->MapID == int32(map->GetId()) && dungeon->DifficultyID == map->GetDifficultyID() && dungeon->ScenarioID)
            mapScenarios.insert(dungeon->ScenarioID);

    if (mapScenarios.empty())
        return 0;

    std::set<uint32> required;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = creator->GetQuestSlotQuestId(slot);
        if (!questId || creator->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (QuestObjective const& objective : quest->GetObjectives())
        {
            if (objective.Type != QUEST_OBJECTIVE_CRITERIA_TREE)
                continue;

            CriteriaTree const* tree = sCriteriaMgr->GetCriteriaTree(objective.ObjectID);
            if (!tree)
                continue;

            CriteriaMgr::WalkCriteriaTree(tree, [&](CriteriaTree const* node)
            {
                if (node->Criteria && node->Criteria->Entry && node->Criteria->Entry->Type == CRITERIA_TYPE_COMPLETE_SCENARIO)
                    if (mapScenarios.count(node->Criteria->Entry->Asset.ID))
                        required.insert(node->Criteria->Entry->Asset.ID);
            });
        }
    }

    if (required.size() != 1)
        return 0;

    return *required.begin();
}

InstanceScenario* ScenarioMgr::CreateInstanceScenario(Map const* map, TeamId team, Player const* creator /*= nullptr*/) const
{
    auto dbDataItr = _scenarioDBData.find(std::make_pair(map->GetId(), map->GetDifficultyID()));
    // No scenario registered for this map and difficulty in the database
    if (dbDataItr == _scenarioDBData.end())
    {
        // We then search for global scenario
        dbDataItr = _scenarioDBData.find(std::make_pair(map->GetId(), 0));

        // No more luck: try the scenario that the creator's quest requires (see FindQuestRequiredScenario)
        if (dbDataItr == _scenarioDBData.end())
        {
            uint32 questScenarioId = FindQuestRequiredScenario(map, creator);
            if (!questScenarioId)
                return nullptr;

            auto questItr = _scenarioData.find(questScenarioId);
            if (questItr == _scenarioData.end())
                return nullptr;

            TC_LOG_DEBUG("scenario", "ScenarioMgr::CreateInstanceScenario: map %u difficulty %u - scenario %u chosen from the quest log of %s.",
                map->GetId(), map->GetDifficultyID(), questScenarioId, creator->GetName().c_str());
            return new InstanceScenario(map, &questItr->second);
        }
    }

    uint32 scenarioID = 0;
    switch (team)
    {
        case TEAM_ALLIANCE:
            scenarioID = dbDataItr->second.Scenario_A;
            break;
        case TEAM_HORDE:
            scenarioID = dbDataItr->second.Scenario_H;
            break;
        default:
            break;
    }

    auto itr = _scenarioData.find(scenarioID);
    if (itr == _scenarioData.end())
    {
        TC_LOG_ERROR("scenario", "Table `scenarios` contained data linking scenario (Id: %u) to map (Id: %u), difficulty (Id: %u) but no scenario data was found related to that scenario Id.", scenarioID, map->GetId(), map->GetDifficultyID());
        return nullptr;
    }

    return new InstanceScenario(map, &itr->second);
}

void ScenarioMgr::LoadDBData()
{
    _scenarioDBData.clear();

    uint32 oldMSTime = getMSTime();

    QueryResult result = WorldDatabase.Query("SELECT map, difficulty, scenario_A, scenario_H FROM scenarios");

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 scenarios. DB table `scenarios` is empty!");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        uint32 mapId = fields[0].GetUInt32();
        uint8 difficulty = fields[1].GetUInt8();

        uint32 scenarioAllianceId = fields[2].GetUInt32();
        if (scenarioAllianceId > 0 && _scenarioData.find(scenarioAllianceId) == _scenarioData.end())
        {
            TC_LOG_ERROR("sql.sql", "ScenarioMgr::LoadDBData: DB Table `scenarios`, column scenario_A contained an invalid scenario (Id: %u)!", scenarioAllianceId);
            continue;
        }

        uint32 scenarioHordeId = fields[3].GetUInt32();
        if (scenarioHordeId > 0 && _scenarioData.find(scenarioHordeId) == _scenarioData.end())
        {
            TC_LOG_ERROR("sql.sql", "ScenarioMgr::LoadDBData: DB Table `scenarios`, column scenario_H contained an invalid scenario (Id: %u)!", scenarioHordeId);
            continue;
        }

        if (scenarioHordeId == 0)
            scenarioHordeId = scenarioAllianceId;

        ScenarioDBData& data = _scenarioDBData[std::make_pair(mapId, difficulty)];
        data.MapID = mapId;
        data.DifficultyID = difficulty;
        data.Scenario_A = scenarioAllianceId;
        data.Scenario_H = scenarioHordeId;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded " SZFMTD " instance scenario entries in %u ms", _scenarioDBData.size(), GetMSTimeDiffToNow(oldMSTime));
}

void ScenarioMgr::LoadDB2Data()
{
    _scenarioData.clear();

    std::unordered_map<uint32, std::map<uint8, ScenarioStepEntry const*>> scenarioSteps;
    uint32 deepestCriteriaTreeSize = 0;

    for (ScenarioStepEntry const* step : sScenarioStepStore)
    {
        scenarioSteps[step->ScenarioID][step->OrderIndex] = step;
        if (CriteriaTree const* tree = sCriteriaMgr->GetCriteriaTree(step->Criteriatreeid))
        {
            uint32 criteriaTreeSize = 0;
            CriteriaMgr::WalkCriteriaTree(tree, [&criteriaTreeSize](CriteriaTree const* /*tree*/)
            {
                ++criteriaTreeSize;
            });
            deepestCriteriaTreeSize = std::max(deepestCriteriaTreeSize, criteriaTreeSize);
        }
    }

    ASSERT(deepestCriteriaTreeSize < MAX_ALLOWED_SCENARIO_POI_QUERY_SIZE, "MAX_ALLOWED_SCENARIO_POI_QUERY_SIZE must be at least %u", deepestCriteriaTreeSize + 1);

    for (ScenarioEntry const* scenario : sScenarioStore)
    {
        ScenarioData& data = _scenarioData[scenario->ID];
        data.Entry = scenario;
        data.Steps = std::move(scenarioSteps[scenario->ID]);
    }
}

void ScenarioMgr::LoadScenarioPOI()
{
    uint32 oldMSTime = getMSTime();

    _scenarioPOIStore.clear(); // need for reload case

    uint32 count = 0;

    //                                                      0            1        2     6          7           8       9       10         11               12
    QueryResult result = WorldDatabase.Query("SELECT CriteriaTreeID, BlobIndex, Idx1, MapID, WorldMapAreaId, Floor, Priority, Flags, WorldEffectID, PlayerConditionID FROM scenario_poi ORDER BY CriteriaTreeID, Idx1");
    if (!result)
    {
        TC_LOG_ERROR("server.loading", ">> Loaded 0 scenario POI definitions. DB table `scenario_poi` is empty.");
        return;
    }

    //                                                       0        1    2  3
    QueryResult points = WorldDatabase.Query("SELECT CriteriaTreeID, Idx1, X, Y FROM scenario_poi_points ORDER BY CriteriaTreeID DESC, Idx1, Idx2");

    std::vector<std::vector<std::vector<ScenarioPOIPoint>>> POIs;

    if (points)
    {
        // The first result should have the highest criteriaTreeId
        Field* fields = points->Fetch();
        uint32 criteriaTreeIdMax = fields[0].GetInt32();
        POIs.resize(criteriaTreeIdMax + 1);

        do
        {
            fields = points->Fetch();

            int32 CriteriaTreeID = fields[0].GetInt32();
            int32 Idx1 = fields[1].GetInt32();
            int32 X = fields[2].GetInt32();
            int32 Y = fields[3].GetInt32();

            if (int32(POIs[CriteriaTreeID].size()) <= Idx1 + 1)
                POIs[CriteriaTreeID].resize(Idx1 + 10);

            ScenarioPOIPoint point(X, Y);
            POIs[CriteriaTreeID][Idx1].push_back(point);
        } while (points->NextRow());
    }

    do
    {
        Field* fields = result->Fetch();

        int32 CriteriaTreeID = fields[0].GetInt32();
        int32 BlobIndex = fields[1].GetInt32();
        int32 Idx1 = fields[2].GetInt32();
        int32 MapID = fields[3].GetInt32();
        int32 WorldMapAreaId = fields[4].GetInt32();
        int32 Floor = fields[5].GetInt32();
        int32 Priority = fields[6].GetInt32();
        int32 Flags = fields[7].GetInt32();
        int32 WorldEffectID = fields[8].GetInt32();
        int32 PlayerConditionID = fields[9].GetInt32();

        if (!sCriteriaMgr->GetCriteriaTree(CriteriaTreeID))
            TC_LOG_ERROR("sql.sql", "`scenario_poi` CriteriaTreeID (%u) Idx1 (%u) does not correspond to a valid criteria tree", CriteriaTreeID, Idx1);

        if (CriteriaTreeID < int32(POIs.size()) && Idx1 < int32(POIs[CriteriaTreeID].size()))
            _scenarioPOIStore[CriteriaTreeID].emplace_back(BlobIndex, MapID, WorldMapAreaId, Floor, Priority, Flags, WorldEffectID, PlayerConditionID, POIs[CriteriaTreeID][Idx1]);
        else
            TC_LOG_ERROR("server.loading", "Table scenario_poi references unknown scenario poi points for criteria tree id %i POI id %i", CriteriaTreeID, BlobIndex);

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded %u scenario POI definitions in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
}

ScenarioPOIVector const* ScenarioMgr::GetScenarioPOIs(int32 criteriaTreeID) const
{
    auto itr = _scenarioPOIStore.find(criteriaTreeID);
    if (itr != _scenarioPOIStore.end())
        return &itr->second;

    return nullptr;
}

ScenarioData const* ScenarioMgr::GetScenarioData(uint32 scenarioId) const
{
    auto itr = _scenarioData.find(scenarioId);
    if (itr != _scenarioData.end())
        return &itr->second;

    return nullptr;
}
