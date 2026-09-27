/*
 * Legion-Server LCF2 - Round 44.
 *
 * Ported from the reference core's OutdoorPVPArgusInvasion (invasion_point_argus.cpp, class body originally at LC source
 * line 4105-4890). This is a from-scratch port onto our own OutdoorPvP/Scenario base classes (extended in R43),
 * NOT a copy-paste - LC's own OutdoorPvP base is zone-indexed with signature (ObjectGuid, uint32) while ours is
 * (Player*, Area*); LC's Scenario/OutdoorPvP classes also carry extra virtuals (SetData(zone,step),
 * BroadcastPacketByZone, ApplyOnEveryPlayerInZone, HandleGameEventStart) that do not exist on our base and were
 * added additively in this round (see OutdoorPvP.h and Scenario.cpp/.h, R44 comments).
 *
 * Scope of this round (see reports\lcf2r44_2026-09-26_invasion_checkpoint.md for the full account):
 * the class below is structurally complete for all 6 invasion zones (registration, scenario lifecycle, kill
 * criteria, spell click, per-zone timers), but the per-step creature WAVE spawn tables (LC's
 * DataStep1_Zone_x / DataStep2_Zone_x coordinate arrays) are only ported for zone 9127 (Val'sharah / "Val"), chosen
 * as the pilot because it has the smallest data footprint (only a Step-1 table, no Step-2 table). The other 5
 * zones (9126/9100/9180/9102/9128) have their SetData() wave-spawn bodies left as explicit TODO stubs - they do
 * NOT crash or misbehave, they simply spawn nothing yet. Porting their spawn tables is follow-up work.
 */

#ifndef OutdoorPvPArgusInvasion_h__
#define OutdoorPvPArgusInvasion_h__

#include "OutdoorPvP.h"
#include <unordered_map>

class Scenario;

class OutdoorPVPArgusInvasion : public OutdoorPvP
{
public:
    OutdoorPVPArgusInvasion();
    ~OutdoorPVPArgusInvasion();

    bool SetupOutdoorPvP() override;

    void Initialize(uint32 zoneId) override;

    void HandlePlayerEnterZone(Player* player, Area* zone) override;
    void HandlePlayerLeaveZone(Player* player, Area* zone) override;

    void HandleKill(Player* killer, Unit* killed) override;

    void HandleSpellClick(Player* player, Unit* target) override;

    void BroadcastPacketByZone(WorldPacket const& data, uint32 zone) override;

    void ApplyOnEveryPlayerInZone(std::function<void(Player*)> function, uint32 zone) override;

    void SetData(uint32 zone, uint32 step) override;

    bool Update(uint32 diff) override;

    void HandleGameEventStart(uint32 eventId) override;

private:
    // Legion-Server R44: zone-indexed state, kept on the subclass only (see R43 checkpoint section 3 - our
    // OutdoorPvP base's m_players[2] stays team-indexed and untouched; NOT reused here to avoid any risk to the
    // 5 existing zones). Renamed m_playersByZone (LC calls it m_players) to avoid shadowing the base member.
    std::map<uint32, Scenario*> m_scenarios;
    std::map<uint32, GuidSet> m_playersByZone;
    std::map<uint32, std::vector<ObjectGuid>> m_creatures;
    std::map<uint32, int32> m_timersPerZone;
    std::map<uint32, bool> m_justInitialized;
    std::map<uint32, int32> m_timerSpell;
};

#endif // OutdoorPvPArgusInvasion_h__
