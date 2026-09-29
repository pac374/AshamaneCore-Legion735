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
        }

        void OnUpdate(uint32 diff) override
        {
            if (!sBotMgr->IsModuleEnabled())
                return; // Playerbots.Enable=0 - Heartbeat komplett pausiert, siehe BotMgr.h-Kommentar
            sBotMgr->Tick(diff);
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

void AddSC_bot_playerscript_hooks()
{
    new bot_playerscript_hooks();
    new bot_worldscript_tick();
}
