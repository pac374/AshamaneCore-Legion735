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
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "LFGMgr.h"
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
            { "teleport",      rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestTeleport,      "" },
            { "attack",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestAttack,        "" },
            { "attackstop",    rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestAttackStop,    "" },
            { "invuln",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestInvuln,        "" },
            { "loot",          rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestLoot,          "" },
            { "release",       rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestRelease,       "" },
            { "revive",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestRevive,        "" },
            { "equip",         rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestEquip,         "" },
            { "equipfrompool", rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestEquipFromPool, "" },
            { "groupinvite",   rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestGroupInvite,   "" },
            { "groupleave",    rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestGroupLeave,    "" },
            { "follow",        rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestFollow,        "" },
            { "followstop",    rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestFollowStop,    "" },
            { "equipartifact", rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestEquipArtifact, "" },
            { "skillartifact", rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestSkillArtifact, "" },
            { "lfgfill",       rbac::RBAC_PERM_COMMAND_ACCOUNT_CREATE, true, &HandleBotTestLfgFill,       "" },
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

    // .bottest teleport <accountId> <mapId> <x> <y> <z> [orientation]
    // Runde 93 (28.09.2026): EIN einziger Kartenwechsel-Live-Test. Siehe
    // BotMgr::TeleportBot() fuer den vollen Code-Review (lcf2r89/lcf2r90) - ruft
    // player->TeleportTo() auf, danach manuell botSession->HandleMoveWorldportAck()
    // (ersetzt den nie eintreffenden Client-Ack). Synchron - Ergebnis (inkl.
    // Ziel-Abgleich) steht sofort fest, zusaetzlich mit '.bottest status' pruefbar.
    static bool HandleBotTestTeleport(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest teleport <accountId> <mapId> <x> <y> <z> [orientation]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0, mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
        iss >> accountId >> mapId >> x >> y >> z;
        if (iss >> orientation) { }

        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest teleport <accountId> <mapId> <x> <y> <z> [orientation]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->TeleportBot(accountId, mapId, x, y, z, orientation);
        handler->PSendSysMessage("[bottest] teleport(account %u -> map %u, %.2f/%.2f/%.2f): %s - "
            "'.bottest status %u' pruefen, Server.log auf 'BotMgr::TeleportBot' Diagnose-Zeilen pruefen.",
            accountId, mapId, x, y, z, ok ? "ZIEL ERREICHT" : "FEHLER/ABWEICHUNG (siehe Server.log)", accountId);
        return true;
    }

    // .bottest attack <accountId> <targetGuid>
    // Runde 122 (28.09.2026): EIN einziger Kampf-Live-Test - siehe BotMgr::StartBotAttack() fuer den
    // vollen Code-Review (lcf2r121). targetGuid ist die DB-Spawn-Id aus der `creature`-Tabelle (Spalte
    // "guid"), NICHT die laufzeit-volle ObjectGuid. Ruft player->Attack(target, true) auf - reiner
    // Melee-Auto-Attack, KEIN Spell-Cast. Synchron ausgeloest, der eigentliche Schaden/die Pakete
    // laufen danach tick-gesteuert - NACH GENAU EINEM Autoattack-Zyklus sofort
    // '.bottest attackstop <accountId>' aufrufen (Auftragsvorgabe).
    static bool HandleBotTestAttack(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest attack <accountId> <targetGuid> (targetGuid = DB-Spawn-Id aus creature.guid)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0;
        uint64 targetGuid = 0;
        iss >> accountId >> targetGuid;

        if (accountId == 0 || targetGuid == 0)
        {
            handler->SendSysMessage("Syntax: .bottest attack <accountId> <targetGuid> (targetGuid = DB-Spawn-Id aus creature.guid)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->StartBotAttack(accountId, ObjectGuid::LowType(targetGuid));
        handler->PSendSysMessage("[bottest] attack(account %u -> targetGuid %llu): %s - NACH GENAU EINEM "
            "Autoattack-Zyklus (paar Sekunden warten, waffengeschwindigkeitsabhaengig) sofort "
            "'.bottest attackstop %u' aufrufen, danach Server.log auf 'BotMgr::StartBotAttack' pruefen.",
            accountId, (unsigned long long)targetGuid, ok ? "Attack() ausgeloest" : "FEHLER (siehe Server.log)", accountId);

        // Runde 122 Zusatz-Diagnose: der "scripts.bots"-Logkanal hat einen seit Runde 106/108 bekannten
        // Bug (literale "{}"-Platzhalter statt substituierter Werte, siehe playerbots-module-idea.md).
        // Bei einem FEHLER hier deshalb bewusst per PSendSysMessage (printf-Style, nachweislich
        // funktionierend) statt per TC_LOG diagnostizieren, damit ein Attack()-Fehlschlag trotzdem
        // auswertbar bleibt.
        if (!ok)
        {
            if (Player* player = sBotMgr->GetBotPlayer(accountId))
            {
                handler->PSendSysMessage("[bottest] attack-diag: player->IsAlive()=%u player->IsMounted()=%u "
                    "player->HasFlag(PACIFIED)=%u player->GetTypeId()==PLAYER=%u",
                    player->IsAlive(), player->IsMounted(),
                    player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED), player->GetTypeId() == TYPEID_PLAYER);
            }
        }
        return true;
    }

    // .bottest attackstop <accountId>
    // Runde 122 (28.09.2026): Gegenstueck zu '.bottest attack' - ruft player->AttackStop() auf. Siehe
    // BotMgr::StopBotAttack().
    static bool HandleBotTestAttackStop(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest attackstop <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest attackstop <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sBotMgr->StopBotAttack(accountId);
        handler->PSendSysMessage("[bottest] attackstop(account %u): AttackStop() ausgeloest - "
            "'.bottest status %u' pruefen.", accountId, accountId);
        return true;
    }

    // .bottest invuln <accountId> <on|off>
    // Runde 129 (28.09.2026): schaltet die Testbot-Damage-Immunitaet aus BotMgr::SetBotTestInvulnerable()
    // um (siehe dortigen vollen Code-Review) - NUR fuer den angegebenen Bot-Account, NIE global/
    // automatisch. Gedacht fuer Kampf-Livetests: vor dem Angriff 'on', direkt danach wieder 'off'.
    static bool HandleBotTestInvuln(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest invuln <accountId> <on|off>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0;
        std::string onOff;
        iss >> accountId >> onOff;

        if (accountId == 0 || (onOff != "on" && onOff != "off"))
        {
            handler->SendSysMessage("Syntax: .bottest invuln <accountId> <on|off>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool enable = (onOff == "on");
        bool ok = sBotMgr->SetBotTestInvulnerable(accountId, enable);
        handler->PSendSysMessage("[bottest] invuln(account %u, %s): %s - NUR fuer Test-Zwecke, nach dem Test "
            "'.bottest invuln %u off' nicht vergessen.",
            accountId, onOff.c_str(), ok ? "OK" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest loot <accountId> <targetGuid>
    // Runde 129 (28.09.2026): EIN einziger Loot-Live-Test - siehe BotMgr::BotLootTarget() fuer den
    // vollen Code-Review (Plan aus lcf2r128 Abschnitt 7). targetGuid ist die DB-Spawn-Id aus der
    // `creature`-Tabelle (Spalte "guid"), NICHT die laufzeit-volle ObjectGuid - dieselbe Konvention
    // wie '.bottest attack'. Das Ziel MUSS bereits tot sein (erst per '.bottest attack' toeten).
    // Synchron - Ergebnis (inkl. Item-Anzahl) steht sofort fest, zusaetzlich per Server.log pruefbar.
    static bool HandleBotTestLoot(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest loot <accountId> <targetGuid> (targetGuid = DB-Spawn-Id aus creature.guid, Ziel muss bereits tot sein)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0;
        uint64 targetGuid = 0;
        iss >> accountId >> targetGuid;

        if (accountId == 0 || targetGuid == 0)
        {
            handler->SendSysMessage("Syntax: .bottest loot <accountId> <targetGuid> (targetGuid = DB-Spawn-Id aus creature.guid, Ziel muss bereits tot sein)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->BotLootTarget(accountId, ObjectGuid::LowType(targetGuid));
        handler->PSendSysMessage("[bottest] loot(account %u -> targetGuid %llu): %s - Server.log auf "
            "'BotMgr::BotLootTarget' Diagnose-Zeilen pruefen (Item-Anzahl, target->loot.isLooted()).",
            accountId, (unsigned long long)targetGuid, ok ? "Loot-Zyklus durchlaufen" : "FEHLER (siehe Server.log)");
        return true;
    }

    // .bottest release <accountId>
    // Runde 132 (28.09.2026): EIN einziger Tod-Handling-Livetest - Gegenstueck zum Client-seitigen
    // "Geist werden"-Bestaetigungsdialog (CMSG_REPOP_REQUEST). Siehe BotMgr::HandleBotDeath() fuer
    // den vollen Code-Review. Der Bot muss vorher bereits tot sein (echter Kampf-Kill OHNE
    // '.bottest invuln on', z.B. per '.bottest attack'/'.bottest attackstop' gegen ein
    // ausreichend gefaehrliches, isoliertes Ziel). Synchron - Ergebnis (Geist-Flag, Position, Map)
    // steht sofort fest, zusaetzlich per Server.log/'bottest status' pruefbar.
    static bool HandleBotTestRelease(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest release <accountId> (Bot muss bereits tot sein, z.B. per echtem Kampf-Kill ohne 'invuln')");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest release <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->HandleBotDeath(accountId);
        handler->PSendSysMessage("[bottest] release(account %u): %s - '.bottest status %u' pruefen (sollte als "
            "Geist am naechsten Friedhof stehen), danach ggf. '.bottest revive %u'.",
            accountId, ok ? "OK (Release/Graveyard-Ablauf durchlaufen)" : "FEHLER (siehe Server.log)",
            accountId, accountId);
        return true;
    }

    // .bottest revive <accountId>
    // Runde 132 (28.09.2026): Gegenstueck zu '.bottest release' - volle Wiederbelebung MIT
    // Resurrection Sickness, entspricht dem Spirit-Healer-Rechtsklick (SendSpiritResurrect()).
    // Siehe BotMgr::ReviveBotAtGraveyard() fuer den vollen Code-Review. Bot muss vorher per
    // '.bottest release' als Geist markiert worden sein.
    static bool HandleBotTestRevive(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest revive <accountId> (Bot muss vorher per '.bottest release' Geist sein)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest revive <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->ReviveBotAtGraveyard(accountId);
        handler->PSendSysMessage("[bottest] revive(account %u): %s - '.bottest status %u' pruefen (sollte wieder "
            "leben, Resurrection-Sickness-Aura 15007 falls Level ausreichend).",
            accountId, ok ? "OK (SendSpiritResurrect() durchlaufen)" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest equip <accountId> <itemEntry>
    // Runde 133 (28.09.2026): EIN einziger Ausruesten-Livetest - siehe BotMgr::EquipBotItem() fuer den
    // vollen Code-Review. Legt ein Test-Item (itemEntry aus item_template) ueber denselben Pfad wie
    // ".additem" ins Bot-Inventar und ruestet es danach direkt in den passenden Ausruestungsslot aus.
    // AUSDRUECKLICH NICHT ueber Loot (Runde 131 gestrichen). Synchron - Ergebnis steht sofort fest,
    // zusaetzlich per '.bottest status' und characters.item_instance/characters.inventory pruefbar.
    static bool HandleBotTestEquip(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest equip <accountId> <itemEntry> (itemEntry aus item_template, Slot muss noch leer sein)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0, itemEntry = 0;
        iss >> accountId >> itemEntry;

        if (accountId == 0 || itemEntry == 0)
        {
            handler->SendSysMessage("Syntax: .bottest equip <accountId> <itemEntry> (itemEntry aus item_template, Slot muss noch leer sein)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->EquipBotItem(accountId, itemEntry);
        handler->PSendSysMessage("[bottest] equip(account %u, itemEntry %u): %s - '.bottest status %u' und "
            "characters.item_instance/characters.inventory (equip-Slot) pruefen.",
            accountId, itemEntry, ok ? "OK (Item im Ausruestungsslot bestaetigt)" : "FEHLER (siehe Server.log)",
            accountId);
        return true;
    }

    // .bottest equipfrompool <accountId>
    // Runde 135 (28.09.2026): rüstet einen Bot VOLLSTAENDIG aus dem neuen Equipment-Pool aus (Design
    // lcf2r134, Implementierung siehe BotMgr::EquipBotFromPool()) - wuerfelt/liest die dauerhaft fixe
    // Pool-Qualitaetsstufe des Bots und ruestet Slot fuer Slot ueber die bereits in Runde 133 live
    // bestaetigte EquipBotItem()-Logik aus. Kein itemEntry-Parameter noetig (im Gegensatz zu
    // '.bottest equip') - die Auswahl passiert automatisch nach Level/Klasse/Qualitaetsstufe.
    static bool HandleBotTestEquipFromPool(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest equipfrompool <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0;
        iss >> accountId;

        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest equipfrompool <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->EquipBotFromPool(accountId);
        handler->PSendSysMessage("[bottest] equipfrompool(account %u): %s - '.bottest status %u' und "
            "characters.character_inventory (Equip-Slots 0-18) pruefen. Details je Slot in Server.log "
            "(scripts.bots).", accountId, ok ? "OK (mindestens ein Slot befuellt)" : "FEHLER/0 Slots (siehe Server.log)",
            accountId);
        return true;
    }

    // .bottest groupinvite <botAccountId> [leaderAccountId]
    // Runde 137 (28.09.2026): EIN einziger Gruppen-Beitritts-Livetest - siehe BotMgr::InviteBotToGroup()
    // fuer den vollen Code-Review (Design lcf2r136). Urspruenglicher Entwurf (lcf2r136 Abschnitt 4):
    // nutzt AUTOMATISCH den ausfuehrenden GM-Charakter als Gruppenleiter, wenn der Befehl von einem
    // echten, eingeloggten Client-GM aus abgesetzt wird (handler->GetSession()->GetPlayer()).
    // ABWEICHUNG in dieser Runde (dokumentiert): dieser Livetest laeuft ueber die RA-Konsole
    // (ra_client.ps1), die KEINEN Player-Charakter an den ChatHandler bindet (reine
    // Account-Remote-Verwaltung, kein In-World-Charakter) - deshalb optionaler zweiter Parameter
    // <leaderAccountId>, der auf einen bereits per '.bottest login' eingeloggten (Bot-)Account
    // verweist und dessen Player als Leader nutzt. Ohne diesen Parameter bleibt das urspruengliche
    // Verhalten (echter GM-Client-Aufrufer) erhalten - vorwaertskompatibel fuer spaetere Runden, in
    // denen der Nutzer den Befehl selbst im Spiel eingibt.
    static bool HandleBotTestGroupInvite(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest groupinvite <botAccountId> [leaderAccountId]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 botAccountId = 0, leaderAccountId = 0;
        iss >> botAccountId;
        iss >> leaderAccountId;

        if (botAccountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest groupinvite <botAccountId> [leaderAccountId]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* leader = nullptr;
        if (leaderAccountId != 0)
            leader = sBotMgr->GetBotPlayer(leaderAccountId);
        else if (handler->GetSession() && handler->GetSession()->GetPlayer())
            leader = handler->GetSession()->GetPlayer();

        if (!leader)
        {
            handler->SendSysMessage("[bottest] groupinvite: kein Leader gefunden - entweder als echter GM-Client "
                "aufrufen (kein Konsolen-/RA-Aufruf moeglich) oder [leaderAccountId] eines bereits "
                "eingeloggten (Bot-)Accounts angeben.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->InviteBotToGroup(botAccountId, leader);
        handler->PSendSysMessage("[bottest] groupinvite(botAccount %u -> Leader '%s'): %s - '.bottest status %u' "
            "und characters.group_member pruefen.", botAccountId, leader->GetName().c_str(),
            ok ? "OK (Group::AddMember() erfolgreich)" : "FEHLER (siehe Server.log)", botAccountId);
        return true;
    }

    // .bottest groupleave <botAccountId>
    // Runde 137 (28.09.2026): Gegenstueck zu '.bottest groupinvite' - entfernt den Bot sauber aus
    // seiner aktuellen Gruppe. Siehe BotMgr::RemoveBotFromGroup().
    static bool HandleBotTestGroupLeave(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest groupleave <botAccountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 botAccountId = uint32(atoi(args));
        if (botAccountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest groupleave <botAccountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->RemoveBotFromGroup(botAccountId);
        handler->PSendSysMessage("[bottest] groupleave(botAccount %u): %s", botAccountId,
            ok ? "OK (aus Gruppe entfernt)" : "FEHLER (siehe Server.log)");
        return true;
    }

    // .bottest follow <botAccountId> [targetPlayerName]
    // Runde 137 (28.09.2026): EIN einziger Folgen-KI-Livetest - siehe BotMgr::StartBotFollow() fuer den
    // vollen Code-Review (Blocker-Check lcf2r136 Abschnitt 5, MotionMaster::MoveFollow()). targetPlayerName
    // ist optional - ohne Angabe folgt der Bot dem AUSFUEHRENDEN GM-Charakter selbst (der typische
    // Gruppenleiter-Fall). Synchron ausgeloest, das eigentliche Nachlaufen laeuft tick-gesteuert ueber
    // den bereits bestaetigten MotionMaster-Update-Zyklus - mit '.bottest status' ueber mehrere Sekunden
    // verfolgen.
    static bool HandleBotTestFollow(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest follow <botAccountId> [targetPlayerName] (ohne Name: der ausfuehrende GM selbst)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 botAccountId = 0;
        std::string targetName;
        iss >> botAccountId;
        iss >> targetName;

        if (botAccountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest follow <botAccountId> [targetPlayerName] (ohne Name: der ausfuehrende GM selbst)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = nullptr;
        if (!targetName.empty())
        {
            target = ObjectAccessor::FindPlayerByName(targetName);
            if (!target)
            {
                handler->PSendSysMessage("[bottest] follow: Ziel-Spieler '%s' nicht gefunden/nicht online.", targetName.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else if (handler->GetSession() && handler->GetSession()->GetPlayer())
            target = handler->GetSession()->GetPlayer();

        if (!target)
        {
            handler->SendSysMessage("[bottest] follow: kein Ziel angegeben und kein echter GM-Charakter als "
                "Aufrufer vorhanden (Konsole) - bitte targetPlayerName explizit angeben.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->StartBotFollow(botAccountId, target->GetGUID());
        handler->PSendSysMessage("[bottest] follow(botAccount %u -> Ziel '%s'): %s - '.bottest status %u' ueber "
            "mehrere Sekunden verfolgen (Position sollte sich dem Ziel annaehern).", botAccountId,
            target->GetName().c_str(), ok ? "MoveFollow() ausgeloest" : "FEHLER (siehe Server.log)", botAccountId);
        return true;
    }

    // .bottest followstop <botAccountId>
    // Runde 137 (28.09.2026): Notbremse zu '.bottest follow' - siehe BotMgr::StopBotFollow().
    static bool HandleBotTestFollowStop(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest followstop <botAccountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 botAccountId = uint32(atoi(args));
        if (botAccountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest followstop <botAccountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sBotMgr->StopBotFollow(botAccountId);
        handler->PSendSysMessage("[bottest] followstop(botAccount %u): Follow abgebrochen (MoveIdle()) - "
            "'.bottest status %u' pruefen.", botAccountId, botAccountId);
        return true;
    }

    // .bottest equipartifact <accountId>
    // Runde 143 (28.09.2026): EIN einziger Artefaktwaffen-Zuweisungs-Livetest - siehe
    // BotMgr::EquipBotArtifact() fuer den vollen Code-Review (Design lcf2r138 Teil B). Ermittelt
    // Klasse+Primaerspezialisierung des Bots automatisch, schlaegt das passende Artefakt-Item aus der
    // statischen 36er-Zuordnungstabelle nach und ruestet es ueber den bereits in Runde 133 live
    // bestaetigten EquipBotItem()-Pfad aus - kein itemEntry-Parameter noetig (im Gegensatz zu
    // '.bottest equip').
    static bool HandleBotTestEquipArtifact(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest equipartifact <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = uint32(atoi(args));
        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest equipartifact <accountId>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->EquipBotArtifact(accountId);
        handler->PSendSysMessage("[bottest] equipartifact(account %u): %s - '.bottest status %u' und "
            "characters.item_instance (Main-Hand-Slot, GetArtifactID()!=0 erwartet) pruefen.",
            accountId, ok ? "OK (Artefakt ausgeruestet)" : "FEHLER (siehe Server.log)", accountId);
        return true;
    }

    // .bottest skillartifact <accountId> [levelBudget]
    // Runde 143 (28.09.2026): EIN einziger Artefakt-Skillungs-Livetest - siehe BotMgr::SkillBotArtifact()
    // fuer den vollen Code-Review. Bot muss vorher per '.bottest equipartifact' eine Artefaktwaffe in
    // der Main-Hand haben. levelBudget optional - ohne Angabe (oder 0) wird das Rang-Budget automatisch
    // aus dem aktuellen Bot-Level berechnet (R138 B.3-Faustregel, siehe BotMgr.cpp-Kommentar).
    static bool HandleBotTestSkillArtifact(ChatHandler* handler, char const* args)
    {
        if (!*args)
        {
            handler->SendSysMessage("Syntax: .bottest skillartifact <accountId> [levelBudget] (0/leer = automatisch aus Level berechnet)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::istringstream iss(args);
        uint32 accountId = 0, levelBudget = 0;
        iss >> accountId;
        iss >> levelBudget;

        if (accountId == 0)
        {
            handler->SendSysMessage("Syntax: .bottest skillartifact <accountId> [levelBudget] (0/leer = automatisch aus Level berechnet)");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool ok = sBotMgr->SkillBotArtifact(accountId, levelBudget);
        handler->PSendSysMessage("[bottest] skillartifact(account %u, levelBudget %u): %s - Server.log "
            "(scripts.bots) auf 'BotMgr::SkillBotArtifact' Diagnose-Zeile pruefen (vergebene Raenge), "
            "zusaetzlich characters.item_instance Dynamic-Fields der Waffe.",
            accountId, levelBudget, ok ? "OK" : "FEHLER (siehe Server.log)");
        return true;
    }

    // .bottest lfgfill
    // Gruppe Stufe 2, Teil A: manueller Einzelschritt-Test (unabhaengig vom automatischen ~10s-Timer
    // in BotMgr::ProcessLfgPoolFillTick(), siehe BotMgr.h-Kopfkommentar) - stoesst GENAU EINEN
    // Nachfuell-Versuch ueber alle aktiven LFG-Queues beider Fraktionen an. Nuetzlich, um ohne
    // Wartezeit zu pruefen, ob ein wartender echter Spieler-Kandidat korrekt erkannt und ein
    // passender Bot per LFGMgr::JoinLfg() eingereiht wird - siehe BotMgr::TriggerLfgPoolFillOnce()
    // fuer den vollen Code-Review und Server.log (scripts.bots) fuer die Diagnose-Zeilen je Versuch.
    static bool HandleBotTestLfgFill(ChatHandler* handler, char const* /*args*/)
    {
        bool filled = sBotMgr->TriggerLfgPoolFillOnce();
        handler->PSendSysMessage("[bottest] lfgfill: %s - Server.log (scripts.bots) auf "
            "'BotMgr::TriggerLfgPoolFillOnce' Diagnose-Zeilen pruefen (Kandidat/Rolle/Dungeon-Auswahl).",
            filled ? "EIN Bot wurde eingereiht" : "kein Nachfuellbedarf erkannt ODER kein passender Bot verfuegbar");
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

            // Runde 132 (28.09.2026): Tod-Handling-Zustand zusaetzlich sichtbar machen (Auftragsvorgabe
            // "'.bottest status' muss den korrekten Zustand widerspiegeln") - IsAlive()/PLAYER_FLAGS_GHOST/
            // Resurrection-Sickness-Aura 15007, damit ein Release/Revive-Livetest ohne DB-Blick prüfbar ist.
            handler->PSendSysMessage("[bottest] status(account %u): IsAlive()=%u, HasFlag(PLAYER_FLAGS_GHOST)=%u, "
                "getDeathState()=%u, ResurrectionSickness(Aura 15007)=%u.", accountId, player->IsAlive(),
                player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST), uint32(player->getDeathState()),
                player->HasAura(15007));

            if (sBotMgr->IsBotPatrolActive(accountId))
                handler->PSendSysMessage("[bottest] status(account %u): Patrol AKTIV - Zyklus %u/%u abgeschlossen.",
                    accountId, sBotMgr->GetBotPatrolCyclesCompleted(accountId), sBotMgr->GetBotPatrolCyclesTotal(accountId));

            // Gruppe Stufe 2, Teil A: LFG-Zustand zusaetzlich sichtbar machen (dieselbe Begruendung wie
            // beim Tod-Handling-Zusatz aus Runde 132 oben) - sLFGMgr->GetState()/GetSelectedDungeons()
            // sind bereits oeffentliche, rein lesende LFGMgr-Methoden, kein neuer Core-Zugriff noetig.
            lfg::LfgState lfgState = sLFGMgr->GetState(player->GetGUID());
            handler->PSendSysMessage("[bottest] status(account %u): LFG-Zustand=%s (roh=%u).", accountId,
                lfg::GetStateString(lfgState).c_str(), uint32(lfgState));
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
