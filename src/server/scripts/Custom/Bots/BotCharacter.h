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

#ifndef BOT_CHARACTER_H
#define BOT_CHARACTER_H

// ---------------------------------------------------------------------------
// AshamaneCore Playerbots-Modul - Skeleton/Interface-Runde (Session-Faking
// bewusst NOCH NICHT implementiert, siehe Memory-Notiz playerbots-module-idea.md
// und Bericht C:\LegionServer\reports\lcf2r62_2026-09-27_playerbots_aufbau.md).
//
// IBotCharacter beschreibt, was ein spaeterer echter Bot-Charakter (echtes
// Player-Objekt auf einer socketlosen WorldSession, kein npcbot) einmal
// koennen soll. Aktuell existiert dafuer KEINE Implementierung, die einen
// echten Player erzeugt - das ist der riskante Kernteil ("Session-Faking",
// beruehrt WorldSession::Update/World::UpdateSessions/Legion-Doppelsocket)
// und bewusst einer eigenen, fokussierten Runde vorbehalten.
// ---------------------------------------------------------------------------

#include "Define.h"
#include "ObjectGuid.h"
#include <string>

class Player;

// Lebenszyklus-Zustand eines Bot-Charakters (rein deklarativ in dieser Runde).
enum class BotCharacterState : uint8
{
    STATE_UNINITIALIZED = 0,   // Objekt existiert, aber kein Player dahinter
    STATE_LOGGING_IN    = 1,   // Login-Query laeuft (spaeter: PlayerbotLoginQueryHolder-Analogon)
    STATE_IN_WORLD       = 2,  // Player existiert und ist auf einer Map
    STATE_LOGGING_OUT    = 3,
    STATE_ERROR           = 4
};

// --- Runde R (27.09.2026): rein vorbereitende Idle-Zustands-Grundstruktur ---
//
// Noch OHNE jede echte Movement-/MotionMaster-Logik (das ist explizit Runde S
// vorbehalten, siehe Empfehlung im Bericht lcf2r79_2026-09-27_playerbots_runde_q.md,
// Abschnitt 9 - MotionMaster/MovePoint-Enqueuing im Map-Update-Thread gilt seit der
// urspruenglichen Recherche als hoechstes Einzelrisiko fuer diesen naechsten Schritt
// und braucht einen eigenen, isolierten Testfall, nicht diese Runde). Dieser Enum
// beschreibt nur die zukuenftige "was macht der Bot gerade"-Grundzustandsmaschine
// fuer einen bereits eingeloggten Bot - aktuell hat er nur einen einzigen, bislang
// bedeutungslosen Wert (Idle) und wird von keinem Code ausgewertet oder in
// Entscheidungen einbezogen. Rein additiv, inert, kompiliert nur.
enum class BotState : uint8
{
    Idle = 0,       // Bot steht/agiert passiv in der Welt, keinerlei KI/Movement aktiv (Platzhalter)

    // --- Runde T (27.09.2026): reiner Diagnose-/Status-Wert, siehe BotMgr.h/.cpp
    // "Runde T" - kennzeichnet nur, dass gerade ein Patrol-Testlauf
    // (BotMgr::StartBotPatrol) aktiv ist. Keine eigene KI-Logik haengt an diesem
    // Wert, er wird ausschliesslich fuer Logging/'.bottest status' gesetzt.
    Patrolling = 1
};

// Reiner Interface-/Vertragsentwurf fuer spaetere Bot-Charaktere.
// Bewusst ohne Implementierung von Session-/Login-Logik.
class TC_GAME_API IBotCharacter
{
public:
    virtual ~IBotCharacter() = default;

    // Identitaet
    virtual ObjectGuid GetBotGuid() const = 0;
    virtual std::string const& GetBotName() const = 0;
    virtual uint32 GetOwnerAccountId() const = 0;      // Account, dem der Bot "gehoert" (Master-Bot-Modell)

    // Lebenszyklus (Implementierung folgt erst in der Session-Faking-Runde)
    virtual BotCharacterState GetState() const = 0;
    virtual bool RequestLogin() = 0;                    // socketlose Session aufbauen, Player laden
    virtual void RequestLogout() = 0;                   // sauber speichern/abmelden

    // Zugriff auf das dahinterliegende Player-Objekt, sobald STATE_IN_WORLD
    virtual Player* GetPlayer() const = 0;

    // Taktung: wird spaeter aus einem PlayerScript::OnUpdate-Hook aufgerufen
    // (siehe bot_scriptloader.cpp) - bewusst noch ohne jede Verhaltenslogik.
    virtual void UpdateBot(uint32 diff) = 0;
};

#endif // BOT_CHARACTER_H
