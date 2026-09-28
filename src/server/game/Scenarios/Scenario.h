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

#ifndef Scenario_h__
#define Scenario_h__

#include "CriteriaHandler.h"
#include <unordered_set>

struct ScenarioData;
struct ScenarioStepEntry;
class OutdoorPvP;

namespace WorldPackets
{
    namespace Achievement
    {
        struct CriteriaProgress;
    }

    namespace Scenario
    {
        struct BonusObjectiveData;
        class ScenarioState;
    }
}

enum ScenarioStepState
{
    SCENARIO_STEP_INVALID       = 0,
    SCENARIO_STEP_NOT_STARTED   = 1,
    SCENARIO_STEP_IN_PROGRESS   = 2,
    SCENARIO_STEP_DONE          = 3
};

class TC_GAME_API Scenario : public CriteriaHandler
{
    public:
        Scenario(ScenarioData const* scenarioData);

        // Legion-Server round 43: multi-zone OutdoorPvP invasion scenarios (e.g. invasion_point_argus) build their
        // own Scenario instances directly from a scenarioId, independent of the map+difficulty `scenarios` DB
        // table / ScenarioMgr::CreateInstanceScenario auto-assignment path. Caller must verify
        // sScenarioMgr->GetScenarioData(scenarioId) is non-null first (same convention as CreateInstanceScenario) -
        // this constructor still ASSERTs like the ScenarioData* overload if handed a bad id.
        Scenario(uint32 scenarioId);
        ~Scenario();

        void Reset() override;
        void SetStep(ScenarioStepEntry const* step);

        // Legion-Server round 43: thin adapters used by OutdoorPvP-owned multi-zone scenarios (ported from
        // LegionCore's OutdoorPVPArgusInvasion). Not used by the dungeon/InstanceScenario path.
        void SetOutdoorPvP(OutdoorPvP* outdoorPvP, uint32 zoneId) { _ownerOutdoorPvP = outdoorPvP; _ownerZoneId = zoneId; }
        void SetCurrentStep(uint8 stepIndex);
        void SendStepUpdate(Player* player, bool /*sendFull*/ = true) { SendScenarioState(player); }

        virtual void CompleteStep(ScenarioStepEntry const* step);
        virtual void CompleteScenario();

        virtual void OnPlayerEnter(Player* player);
        virtual void OnPlayerExit(Player* player);
        virtual void Update(uint32 /*diff*/) { }

        bool IsComplete();
        void SetStepState(ScenarioStepEntry const* step, ScenarioStepState state) { _stepStates[step] = state; }
        ScenarioStepState GetStepState(ScenarioStepEntry const* step);
        ScenarioStepEntry const* GetStep() const { return _currentstep; }
        ScenarioStepEntry const* GetFirstStep() const;

        void SendScenarioState(Player* player);
        void SendBootPlayer(Player* player);

        void SendScenarioEvent(Player* player, uint32 eventId);

    protected:
        GuidUnorderedSet _players;

        void SendCriteriaUpdate(Criteria const* criteria, CriteriaProgress const* progress, uint32 timeElapsed, bool timedCompleted) const override;
        void SendCriteriaProgressRemoved(uint32 /*criteriaId*/) override { }

        bool CanUpdateCriteriaTree(Criteria const* criteria, CriteriaTree const* tree, Player* referencePlayer) const override;
        bool CanCompleteCriteriaTree(CriteriaTree const* tree) override;
        void CompletedCriteriaTree(CriteriaTree const* tree, Player* referencePlayer) override;
        void AfterCriteriaTreeUpdate(CriteriaTree const* /*tree*/, Player* /*referencePlayer*/) override { }

        void SendPacket(WorldPacket const* data) const override;

        void SendAllData(Player const* /*receiver*/) const override { }

        // Legion-Server round 43: was pure virtual in CriteriaHandler and unimplemented on Scenario itself
        // (only InstanceScenario overrode it) - that made Scenario abstract and blocked LC's pattern of
        // instantiating a bare Scenario directly for OutdoorPvP-owned zone scenarios. Generic default here;
        // OutdoorPvP-owned scenarios get SetOutdoorPvP() called so this can identify the zone.
        std::string GetOwnerInfo() const override;

        void BuildScenarioState(WorldPackets::Scenario::ScenarioState* scenarioState);

        std::vector<WorldPackets::Scenario::BonusObjectiveData> GetBonusObjectivesData();
        std::vector<WorldPackets::Achievement::CriteriaProgress> GetCriteriasProgress();

        CriteriaList const& GetCriteriaByType(CriteriaTypes type, uint32 asset) const override;
        ScenarioData const* _data;
        Ashamane::AnyData Variables;

    private:
        ScenarioStepEntry const* _currentstep;
        std::map<ScenarioStepEntry const*, ScenarioStepState> _stepStates;

        // Legion-Server round 43: only set via SetOutdoorPvP() for OutdoorPvP-owned zone scenarios; nullptr/0 for
        // the normal dungeon/InstanceScenario path.
        OutdoorPvP* _ownerOutdoorPvP = nullptr;
        uint32 _ownerZoneId = 0;
};

#endif // Scenario_h__
