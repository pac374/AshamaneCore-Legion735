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
Name: bot_dungeonclear_control_chat
Comment: Erlaubt einem echten Spieler, den Dungeon-Clear-Modus (siehe BotMgr::SetDungeonClearMode()) fuer
         ALLE Bot-Mitglieder der eigenen Gruppe OHNE GM-Rechte umzuschalten - zwei gleichwertige Wege, siehe
         README Abschnitt e)/mod-dungeon-clear-addon-Ideenreferenz:
           1. Party-/Raid-Chat-Schluesselwort "!dc on"/"!dc off"/"!dc status" (dieses Script, OnChat()-
              Group-Ueberladung - liefert die Gruppe direkt, kein Umweg ueber GetGroup() noetig).
           2. Eine an einen Bot GERICHTETE Addon-Whisper-Nachricht (siehe bot_dungeonclear_control_addon
              unten) - fuer ein echtes WoW-Addon (Lua, siehe README/Ideenreferenz mod-dungeon-clear-addon).
Name: bot_dungeonclear_control_addon
Comment: Serverseitiger Handler fuer den Addon-Kanal. WICHTIG (per Recherche bestaetigt, siehe
         BotMgr.h-Kommentar bei SetDungeonClearModeForPlayerGroup()): PlayerScript::OnChat() bekommt bei
         einer Addon-Nachricht NUR den Text + lang==LANG_ADDON, NICHT den eigentlichen Addon-"Prefix"
         (der wird von Player::WhisperAddon() nicht an sScriptMgr->OnPlayerChat() durchgereicht, siehe
         Player.cpp). Deshalb betten wir stattdessen eine einfache Text-Markierung direkt in die
         Nachricht selbst ein ("ASHDC:CMD:..." fuer Client->Server-Befehle). Der Client schickt diese
         Nachricht per SendAddonMessage(prefix, "ASHDC:CMD:...", "WHISPER", <Bot-Name>) AN EINEN DER
         EIGENEN BOTS - Player::WhisperAddon() liefert dadurch Sender (der echte Spieler) UND Empfaenger
         (der Bot) direkt an diesen Hook, ganz ohne Umweg ueber eine Selbst-Whisper-Technik. Die Antwort
         geht bewusst als NORMALE (sichtbare) Whisper mit einem festen Text-Praefix "[AshDC]" zurueck
         (kein echtes CHAT_MSG_ADDON-Client-Event) - robuster als sich auf ungeklaerte Client-seitige
         Sonderbehandlung von lang==LANG_ADDON ohne echten Prefix zu verlassen; ein Addon kann diese
         Zeile trotzdem per eigenem OnEvent("CHAT_MSG_WHISPER")-Hook erkennen und in der eigenen UI
         anzeigen (siehe DungeonClear.lua).
Category: custom
EndScriptData */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "BotMgr.h"
#include "SharedDefines.h"
#include "Chat.h"
#include "Log.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    // Liest ein einfaches "!dc <on|off|status>"-Kommando aus einer Party-/Raid-Chatzeile. Bewusst kein
    // generisches Tokenizing/keine Gross-/Kleinschreibungs-Empfindlichkeit - ein einzelnes, robust
    // erkennbares Schluesselwort reicht fuer diesen Zweck.
    bool ParseChatKeywordCommand(std::string const& msg, bool& outEnable, bool& outIsStatusQuery)
    {
        std::string lower = msg;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

        if (lower == "!dc on")
        {
            outEnable = true;
            outIsStatusQuery = false;
            return true;
        }
        if (lower == "!dc off")
        {
            outEnable = false;
            outIsStatusQuery = false;
            return true;
        }
        if (lower == "!dc status")
        {
            outIsStatusQuery = true;
            return true;
        }
        return false;
    }

    // Gemeinsame Antwort-Formatierung fuer beide Steuerwege (Chat-Schluesselwort UND Addon-Kanal) - siehe
    // Kopfkommentar oben zum "[AshDC]"-Text-Praefix.
    std::string BuildStatusReplyText(Player* requester)
    {
        uint32 activeCount = sBotMgr->CountActiveDungeonClearBotsInGroup(requester);
        std::ostringstream reply;
        reply << "[AshDC] Dungeon-Clear aktiv bei " << activeCount << " Bot(s) in deiner Gruppe.";
        return reply.str();
    }
}

class bot_dungeonclear_control_chat : public PlayerScript
{
    public:
        bot_dungeonclear_control_chat() : PlayerScript("bot_dungeonclear_control_chat") { }

        void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* /*group*/) override
        {
            if (type != CHAT_MSG_PARTY && type != CHAT_MSG_RAID)
                return;

            // Fuehrung/Pull-Modus: "!dc lead <BotName>", "!dc mode normal|pack|leeroy|combo", "!dc info"
            {
                std::string lowerMsg = msg;
                std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string reply;
                if (lowerMsg.compare(0, 9, "!dc lead ") == 0 && msg.size() > 9)
                    reply = sBotMgr->SetGroupLead(player, msg.substr(9));
                else if (lowerMsg.compare(0, 9, "!dc mode ") == 0 && msg.size() > 9)
                    reply = sBotMgr->SetGroupMode(player, lowerMsg.substr(9));
                else if (lowerMsg == "!dc info")
                    reply = sBotMgr->GroupLeadStatus(player);
                if (!reply.empty())
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("%s", reply.c_str());
                    return;
                }
            }

            bool enable = false;
            bool isStatusQuery = false;
            if (!ParseChatKeywordCommand(msg, enable, isStatusQuery))
                return;

            if (isStatusQuery)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", BuildStatusReplyText(player).c_str());
                return;
            }

            uint32 toggledCount = sBotMgr->SetDungeonClearModeForPlayerGroup(player, enable);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "[AshDC] Dungeon-Clear-Modus fuer %u Bot(s) deiner Gruppe %s.", toggledCount,
                enable ? "aktiviert" : "deaktiviert");
        }
};

class bot_dungeonclear_control_addon : public PlayerScript
{
    public:
        bot_dungeonclear_control_addon() : PlayerScript("bot_dungeonclear_control_addon") { }

        void OnChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
        {
            // Siehe Kopfkommentar: NUR eine an einen EIGENEN BOT gerichtete Addon-Whisper-Nachricht mit
            // unserer Text-Markierung wird als Steuerbefehl behandelt - alles andere (normale Whispers,
            // Whispers an echte Spieler, Whispers OHNE die Markierung) wird ignoriert.
            if (lang != LANG_ADDON || !receiver || !sBotMgr->IsBotPlayerGuid(receiver->GetGUID()))
                return;

            static std::string const cmdPrefix = "ASHDC:CMD:";
            if (msg.compare(0, cmdPrefix.size(), cmdPrefix) != 0)
                return;

            std::string const command = msg.substr(cmdPrefix.size());

            if (command == "STATUS")
            {
                receiver->Whisper(BuildStatusReplyText(player), LANG_UNIVERSAL, player);
                return;
            }

            bool enable;
            if (command == "ON")
                enable = true;
            else if (command == "OFF")
                enable = false;
            else
            {
                TC_LOG_ERROR("scripts.bots", "bot_dungeonclear_control_addon::OnChat: unbekanntes Addon-"
                    "Kommando '%s' von Spieler %s.", command.c_str(), player->GetName().c_str());
                return;
            }

            uint32 toggledCount = sBotMgr->SetDungeonClearModeForPlayerGroup(player, enable);
            std::ostringstream reply;
            reply << "[AshDC] Dungeon-Clear-Modus fuer " << toggledCount << " Bot(s) deiner Gruppe "
                  << (enable ? "aktiviert." : "deaktiviert.");
            receiver->Whisper(reply.str(), LANG_UNIVERSAL, player);
        }
};

void AddSC_bot_dungeonclear_control()
{
    new bot_dungeonclear_control_chat();
    new bot_dungeonclear_control_addon();
}
