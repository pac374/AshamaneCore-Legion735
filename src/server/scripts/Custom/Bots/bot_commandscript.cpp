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
Name: bot_commandscript
Comment: Playerbots-Modul, Runde B (27.09.2026). GM-/Konsolen-Befehle, um das Session-Faking-
         Kernstueck (BotMgr) SCHRITTWEISE anzustossen - bewusst drei getrennte Unterbefehle statt
         eines Rundumschlags, damit nach jedem Schritt Server.log/DBErrors.log geprueft werden kann
         (siehe Bericht lcf2r64_2026-09-27_playerbots_rundeb.md, Abschnitt 5/6). Diese Befehle wurden
         in Runde B NICHT gegen den laufenden Produktivserver ausgefuehrt (nur kompiliert) - siehe
         Stopp-Regel im Bericht. Gate: RBAC_PERM_COMMAND_ACCOUNT_CREATE (gleiche Guerteltier-Stufe
         wie ".account create", weil "createaccount" intern genau das aufruft).
Category: custom
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "BotMgr.h"
#include "SharedDefines.h"
#include "Player.h"
#include "RBAC.h"
#include <sstream>

class bot_commandscript : public CommandScript
{
public:
    bot_commandscript() : CommandScript("bot_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> botTestCommandTable =
        {
            { "createaccount", rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestCreateAccount, "" },
            { "createchar",    rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestCreateChar,    "" },
            { "login",         rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestLogin,         "" },
            { "logout",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestLogout,        "" },
            { "move",          rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestMove,          "" },
            { "movepath",      rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestMovePath,      "" },
            { "patrol",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestPatrol,        "" },
            { "status",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestStatus,        "" },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "bottest", rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, NULL, "", botTestCommandTable },
        };
        return commandTable;
    }

    // .bottest createaccount <accountName> <password>
    // Phase 1: legt (idempotent) den Bot-Account an. Siehe BotMgr::CreateBotAccount -
    // nutzt AccountMgr::CreateAccount(), keine eigene SQL/SRP6-Logik.
    static bool HandleBotTestCreateAccount(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest createaccount <accountName> <password>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        std::string accountName, password;
        iss >> accountName >> password;
        if (accountName.empty() || password.empty())
        {
            handler->SendSysMessage("Syntax: .bottest createaccount <accountName> <password>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = 0;
        bool ok = sBotMgr->CreateBotAccount(accountName, password, accountId);
        handler->PSendSysMessage("[bottest] createaccount('%s'): %s, accountId=%u", accountName.c_str(),
            ok ? "OK" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest createchar <accountId> <charName> [race] [class] [sex]
    // Phase 2: treibt HandleCharCreateOpcode() fuer die Bot-Session (asynchron -
    // Ergebnis erst nach mehreren Sekunden/Ticks in .bottest status sichtbar).
    static bool HandleBotTestCreateChar(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest createchar <accountId> <charName> [race=1 Human] [class=1 Warrior] [sex=0 Male]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0;
        std::string charName;
        uint32 race = RACE_HUMAN, charClass = CLASS_WARRIOR, sex = GENDER_MALE;
        iss >> accountId >> charName;
        if (iss >> race) { }
        if (iss >> charClass) { }
        if (iss >> sex) { }

        if (accountId == 0 || charName.empty())
        {
            handler->SendSysMessage("Syntax: .bottest createchar <accountId> <charName> [race] [class] [sex]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->RequestCreateBotCharacter(accountId, charName, uint8(race), uint8(charClass), uint8(sex));
        handler->PSendSysMessage("[bottest] createchar('%s', account %u): Opcode-Handler %s ausgeloest - "
            "Ergebnis ist ASYNCHRON, mit '.bottest status %u' und Server.log/DBErrors.log pruefen.",
            charName.c_str(), accountId, ok ? "wurde" : "konnte NICHT", accountId);
        return true;
    }

    // .bottest login <accountId>
    // Phase 3: CharEnum + HandlePlayerLoginOpcode + HandleContinuePlayerLogin.
    // Ergebnis ist asynchron (mehrere Ticks), mit '.bottest status' pruefen.
    static bool HandleBotTestLogin(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest login <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest login <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->RequestBotLogin(accountId);
        handler->PSendSysMessage("[bottest] login(account %u): %s - Ergebnis ist ASYNCHRON, "
            "mit '.bottest status %u' und Server.log/DBErrors.log pruefen, mehrere Minuten laufen lassen.",
            accountId, ok ? "ausgeloest" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest logout <accountId>
    // Runde R (27.09.2026): sauberes Logout WAEHREND laufendem Betrieb (kein
    // Server-Shutdown noetig) - Gegenstueck zu ".bottest login". Nutzt
    // BotMgr::LogoutBot(), denselben LogoutPlayer(true)-Pfad, der ueber
    // LogoutAllBots() beim Shutdown bereits zweifach live bestaetigt wurde
    // (siehe lcf2r79-Bericht Runde Q), diesmal aber im normalen
    // World-Update-Kontext statt WorldScript::OnShutdown(). Synchron - Ergebnis
    // steht sofort fest, kein Warten auf Tick()/Callbacks noetig.
    static bool HandleBotTestLogout(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest logout <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest logout <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->LogoutBot(accountId);
        handler->PSendSysMessage("[bottest] logout(account %u): %s - Server laeuft normal weiter, '.bottest status %u' "
            "und die DB (auth.account/characters.characters, online=0 erwartet) pruefen.",
            accountId, ok ? "OK" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest move <accountId>
    // Runde S (27.09.2026): EIN einziger, isolierter Bewegungstest - Bot bewegt sich
    // einmal 8 Yards geradeaus in aktueller Blickrichtung (keine Pfadfindung, kein
    // Patrouillieren). Siehe BotMgr::MoveBotTestStep() fuer den vollen Code-Review.
    // Synchron - MovePoint() gibt sofort zurueck, das eigentliche Bewegen laeuft ueber
    // den normalen MotionMaster-Update-Zyklus (mehrere Sekunden), mit '.bottest status'
    // pruefen.
    static bool HandleBotTestMove(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest move <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest move <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->MoveBotTestStep(accountId);
        handler->PSendSysMessage("[bottest] move(account %u): %s - '.bottest status %u' nach ein paar Sekunden "
            "pruefen (Position sollte sich veraendert haben).",
            accountId, ok ? "MovePoint ausgeloest" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest movepath <accountId>
    // Runde U (27.09.2026): EIN einziger, isolierter Navmesh-Pfadfindungstest - identisch zu
    // ".bottest move", aber mit generatePath=true (PathGenerator/Recast-Detour-Navmesh statt
    // gerader Linie). Siehe BotMgr::MoveBotTestStepPath() fuer den vollen Code-Review.
    static bool HandleBotTestMovePath(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest movepath <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest movepath <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->MoveBotTestStepPath(accountId);
        handler->PSendSysMessage("[bottest] movepath(account %u): %s - '.bottest status %u' nach ein paar Sekunden "
            "pruefen (Position sollte sich veraendert haben, generatePath=true/Navmesh).",
            accountId, ok ? "MovePoint(generatePath=true) ausgeloest" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest patrol <accountId> <cycles>
    // Runde T (27.09.2026): mehrfache Bewegungen / einfaches Pendeln zwischen zwei
    // Punkten (Punkt A = aktuelle Position beim Start, Punkt B = 8 Yards davor in
    // Blickrichtung), ueber <cycles> vollstaendige Rundlaeufe A->B->A. Weiterhin
    // generatePath=false. Siehe BotMgr::StartBotPatrol() fuer den vollen
    // Code-Review (Ankunftserkennung via movespline->Finalized(), Speicherleck-
    // Pruefung von MotionMaster::Mutate()). Synchron ausgeloest, der Fortschritt
    // laeuft danach tick-gesteuert - mit '.bottest status' verfolgen.
    static bool HandleBotTestPatrol(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest patrol <accountId> <cycles>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0, cycles = 0;
        iss >> accountId >> cycles;

        if (accountId == 0 || cycles == 0)
        {
            handler->SendSysMessage("Syntax: .bottest patrol <accountId> <cycles> (cycles wird auf 1-20 geklemmt)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->StartBotPatrol(accountId, cycles);
        handler->PSendSysMessage("[bottest] patrol(account %u, %u Zyklen): %s - Fortschritt mit "
            "'.bottest status %u' verfolgen (tick-gesteuert, laeuft mehrere Sekunden pro Zyklus).",
            accountId, cycles, ok ? "gestartet" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest status <accountId>
    static bool HandleBotTestStatus(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest status <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        BotCharacterState state = sBotMgr->GetBotSessionState(accountId);
        Player* player = sBotMgr->GetBotPlayer(accountId);

        char const* stateStr = "UNBEKANNT";
        switch (state)
        {
            case BotCharacterState::STATE_UNINITIALIZED: stateStr = "UNINITIALIZED (keine Session/kein Login angestossen)"; break;
            case BotCharacterState::STATE_LOGGING_IN:     stateStr = "LOGGING_IN (CharEnum/LoginQueryHolder laeuft noch)"; break;
            case BotCharacterState::STATE_IN_WORLD:       stateStr = "IN_WORLD (Player existiert, ist auf einer Map)"; break;
            case BotCharacterState::STATE_LOGGING_OUT:    stateStr = "LOGGING_OUT"; break;
            case BotCharacterState::STATE_ERROR:          stateStr = "ERROR"; break;
        }

        if (player)
        {
            handler->PSendSysMessage("[bottest] status(account %u): %s - Player '%s' (%s), Map %u, Position %s",
                accountId, stateStr, player->GetName().c_str(), player->GetGUID().ToString().c_str(),
                player->GetMapId(), player->GetPosition().ToString().c_str());

            if (sBotMgr->IsBotPatrolActive(accountId))
                handler->PSendSysMessage("[bottest] status(account %u): Patrol AKTIV - Zyklus %u/%u abgeschlossen.",
                    accountId, sBotMgr->GetBotPatrolCyclesCompleted(accountId), sBotMgr->GetBotPatrolCyclesTotal(accountId));
        }
        else
            handler->PSendSysMessage("[bottest] status(account %u): %s - kein Player-Objekt vorhanden.", accountId, stateStr);
        return true;
    }
};

void AddSC_bot_commandscript()
{
    new bot_commandscript();
}
