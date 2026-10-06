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

/* ScriptData
Name: bot_playerscript_hooks
Comment: Playerbots-Modul - Skeleton/Interface-Runde (27.09.2026). Registriert BotMgr an den
         bestehenden PlayerScript-Hooks (OnLogin/OnLogout/OnUpdate), damit der spaetere
         Session-Faking-Teil ohne weitere ScriptMgr-Aenderung andocken kann. Alle Hook-Bodies
         delegieren nur an BotMgr, dessen Methoden in dieser Runde bewusst leer/inert sind - kein
         echter Spieler wird durch diese Datei in irgendeiner Weise beeinflusst.
         Siehe Memory playerbots-module-idea.md und Bericht
         C:\LegionServer\reports\lcf2r62_2026-09-27_playerbots_aufbau.md.
Name: bot_worldscript_tick
Comment: Runde B (27.09.2026): taktet BotMgr::Tick() aus WorldScript::OnUpdate() (laeuft JEDEN
         World-Tick, unabhaengig davon ob echte Spieler online sind) - NICHT aus dem
         PlayerScript::OnUpdate-Hook oben, weil der nur fuer bereits eingeloggte echte Player-
         Objekte feuert und eine Bot-Session vor ihrem eigenen Login nie erreichen wuerde.
         BotMgr::Tick() ruft ausschliesslich WorldSession::ProcessQueryCallbacks() pro
         Bot-Session auf, niemals Update() (Absturzrisiko, siehe BotMgr.h Kopfkommentar Runde A).
         Runde N (27.09.2026): zusaetzlich WorldScript::OnShutdown()-Override, der
         BotMgr::LogoutAllBots() aufruft - Fix fuer den in Runde M gefundenen
         Shutdown-Absturz (BotMgr::instance()-atexit-Destruktor lief nach dem
         DB-Pool-Teardown, siehe BotMgr.h/.cpp Kopfkommentare und Bericht
         lcf2r76_2026-09-27_playerbots_runde_n.md). OnShutdown() laeuft laut
         Main.cpp:359 synchron noch innerhalb von main(), bevor main() zurueckkehrt
         und damit bevor StopDB() den DB-Pool schliesst.
Category: custom
EndScriptData */

#include "ScriptMgr.h"
#include "Player.h"
#include "BotMgr.h"
#include "BotPopulationMgr.h"

class bot_playerscript_hooks : public PlayerScript
{
    public:
        bot_playerscript_hooks() : PlayerScript("bot_playerscript_hooks") { }

        void OnLogin(Player* player, bool /*firstLogin*/) override
        {
            sBotMgr->OnPlayerLogin(player);
        }

        void OnLogout(Player* player) override
        {
            sBotMgr->OnPlayerLogout(player);
        }

        void OnUpdate(Player* player, uint32 diff) override
        {
            sBotMgr->OnPlayerUpdate(player, diff);
        }
};

class bot_worldscript_tick : public WorldScript
{
    public:
        bot_worldscript_tick() : WorldScript("bot_worldscript_tick") { }

        // Runde 8 (Kontrollzentrum-Anforderung "Playerbots an/aus schalten"): laedt/aktualisiert
        // Playerbots.Enable bei Start UND bei jedem '.reload config' - siehe BotMgr::LoadConfig().
        void OnConfigLoad(bool /*reload*/) override
        {
            sBotMgr->LoadConfig();
            sBotPop->LoadConfig();
        }

        void OnUpdate(uint32 diff) override
        {
            if (!sBotMgr->IsModuleEnabled())
                return; // Playerbots.Enable=0 - Heartbeat komplett pausiert, siehe BotMgr.h-Kommentar
            sBotMgr->Tick(diff);
            sBotPop->Update(diff);
        }

        // Runde N (27.09.2026): siehe Kopfkommentar oben / BotMgr::LogoutAllBots() -
        // loggt verbleibende Bot-Sessions sauber aus, waehrend main() noch laeuft
        // und der DB-Pool garantiert noch intakt ist (Fix fuer den
        // Runde-M-Shutdown-Absturz).
        void OnShutdown() override
        {
            sBotMgr->LogoutAllBots();
        }
};

// OI-023 Diagnose (05.10.2026): wer verursacht den Schaden an Bots auf Raidkarten? Summiert je (Angreifer-Entry, Zauber) den Schaden
// an Spieler-Bots und schreibt alle 10 s die fuenf groessten Quellen ins Log; Tode werden einzeln mit Killer protokolliert.
#include "Creature.h"
#include "SpellInfo.h"
#include "Map.h"
#include "Log.h"
#include "Timer.h"
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>

class bot_raid_damage_diag : public UnitScript, public PlayerScript
{
    public:
        bot_raid_damage_diag() : UnitScript("bot_raid_damage_diag"), PlayerScript("bot_raid_damage_diag_deaths") { }

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage, SpellInfo const* spell) override
        {
            if (!sBotMgr->IsCombatDebug() || !attacker || !victim || victim->GetTypeId() != TYPEID_PLAYER || !sBotMgr->IsBotPlayerGuid(victim->GetGUID()))
                return;
            Map* map = victim->GetMap();
            if (!map || !map->IsRaid() || attacker->GetTypeId() == TYPEID_PLAYER)
                return;
            std::lock_guard<std::mutex> lock(_mutex);
            Source& s = _sources[{ attacker->GetEntry(), spell ? spell->Id : 0u }];
            s.Total += damage;
            ++s.Hits;
            s.Max = std::max(s.Max, damage);
            s.MaxHealth = std::max<uint32>(s.MaxHealth, uint32(victim->GetMaxHealth()));
            uint32 const now = getMSTime();
            if (now - _lastLog < 10000)
                return;
            _lastLog = now;
            std::vector<std::pair<std::pair<uint32, uint32>, Source>> v(_sources.begin(), _sources.end());
            std::sort(v.begin(), v.end(), [](auto const& a, auto const& b) { return a.second.Total > b.second.Total; });
            for (size_t i = 0; i < v.size() && i < 5; ++i)
                TC_LOG_INFO("scripts.bots", "BotMgr::DamageDiag: Quelle Entry %u Zauber %u: %u Treffer, Summe %u, max %u (Bot-MaxHP bis %u).",
                    v[i].first.first, v[i].first.second, v[i].second.Hits, v[i].second.Total, v[i].second.Max, v[i].second.MaxHealth);
            _sources.clear();
        }

        void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
        {
            if (sBotMgr->IsCombatDebug() && killer && killed && sBotMgr->IsBotPlayerGuid(killed->GetGUID()) && killed->GetMap() && killed->GetMap()->IsRaid())
                TC_LOG_INFO("scripts.bots", "BotMgr::DamageDiag: Bot %s von '%s' (Entry %u) getoetet (HP-Max %u).", killed->GetName().c_str(), killer->GetName().c_str(), killer->GetEntry(), uint32(killed->GetMaxHealth()));
        }

    private:
        struct Source { uint64 Total = 0; uint32 Hits = 0, Max = 0, MaxHealth = 0; };
        std::mutex _mutex;
        std::map<std::pair<uint32, uint32>, Source> _sources;
        uint32 _lastLog = 0;
};

void AddSC_bot_playerscript_hooks()
{
    new bot_raid_damage_diag();
    new bot_playerscript_hooks();
    new bot_worldscript_tick();
}
