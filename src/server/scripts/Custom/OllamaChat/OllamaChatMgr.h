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

#ifndef OLLAMA_CHAT_MGR_H
#define OLLAMA_CHAT_MGR_H

// ---------------------------------------------------------------------------
// Ollama-Chat-Modul (Ideenreferenz mod-ollama-chat, siehe README Abschnitt e) fuer die
// Lizenz-/Kompatibilitaets-Begruendung und OllamaHttpClient.h fuer die Begruendung, warum keine
// Drittbibliothek vendored wurde). Kernidee (uebernommen): wenn ein echter Spieler einen Bot per
// Whisper anspricht, generiert der Bot seine Antwort ueber eine lokale Ollama-HTTP-API statt fest
// verdrahtet/zufaellig zu antworten.
//
// Threading-Modell (das ist der wichtigste Teil dieses Designs): ein HTTP-Request an einen LLM-Server
// kann mehrere Sekunden dauern - das NIEMALS auf dem World-Update-Thread ausfuehren (wuerde den
// gesamten Server fuer alle Spieler einfrieren, exakt die Warnung aus dem Referenzmodul-README: "this
// module ... can bog down your server due to the nature of running local LLM"). Deshalb:
//   1. RequestBotReply() (aufgerufen aus dem PlayerScript::OnChat-Hook, siehe
//      ollamachat_scriptloader.cpp, LAEUFT auf dem World-Update-Thread) startet nur einen NEUEN,
//      unabhaengigen std::thread (WorkerThreadMain()) und kehrt SOFORT zurueck - kein Blockieren.
//   2. WorkerThreadMain() (laeuft auf dem Hintergrund-Thread) fuehrt den blockierenden HTTP-Request aus
//      und legt das Ergebnis NUR als GUIDs+Strings (keine Player*/WorldSession*-Zeiger!) in eine
//      mutex-geschuetzte Queue - Zugriff auf Player/Map/WorldSession von einem Fremd-Thread aus waere
//      eine garantierte Race Condition mit dem World-Update-Thread.
//   3. DeliverPendingReplies() (aufgerufen aus WorldScript::OnUpdate(), LAEUFT auf dem World-Update-
//      Thread) leert die Queue und ruft dort - und NUR dort - Player::Whisper() auf. Loest dabei die
//      GUIDs erst in diesem Moment ueber ObjectAccessor auf (der Spieler/Bot koennte zwischenzeitlich
//      ausgeloggt sein - dann wird die Antwort verworfen, kein Fehler).
// ---------------------------------------------------------------------------

#include "Define.h"
#include "ObjectGuid.h"
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class Player;

class TC_GAME_API OllamaChatMgr
{
public:
    static OllamaChatMgr* instance();

    OllamaChatMgr(OllamaChatMgr const&) = delete;
    OllamaChatMgr(OllamaChatMgr&&) = delete;
    OllamaChatMgr& operator=(OllamaChatMgr const&) = delete;
    OllamaChatMgr& operator=(OllamaChatMgr&&) = delete;

    // Aus WorldScript::OnConfigLoad() aufgerufen (Erstladen UND '.reload config') - liest alle
    // OllamaChat.*-Konfigurationsschluessel neu ein (siehe worldserver.conf.dist).
    void LoadConfig();

    bool IsEnabled() const { return _enabled; }

    // Siehe Kopfkommentar oben - kehrt sofort zurueck, der eigentliche HTTP-Request laeuft
    // asynchron auf einem neuen std::thread. bot/sender muessen zum Aufrufzeitpunkt gueltige,
    // eingeloggte Player-Objekte sein (der Hook liefert das bereits so), werden aber NUR fuer den
    // Prompt-Aufbau gelesen (Name/Klasse/Level/Fraktion), nie ueber den Aufruf hinaus gehalten.
    void RequestBotReply(Player* bot, Player* sender, std::string const& senderMessage);

    // Aus WorldScript::OnUpdate() JEDEN Tick aufgerufen (billig, wenn die Queue leer ist - ein
    // Mutex-Lock + leere Pruefung). Liefert alle inzwischen fertigen Ollama-Antworten synchron auf
    // dem Hauptthread aus.
    void DeliverPendingReplies();

private:
    OllamaChatMgr() = default;
    ~OllamaChatMgr() = default;

    struct PendingReply
    {
        ObjectGuid BotGuid;
        ObjectGuid SenderGuid;
        std::string SenderName;
        std::string ReplyText;
    };

    // Laeuft auf einem EIGENEN std::thread (siehe RequestBotReply()) - fuehrt den blockierenden
    // Ollama-HTTP-Request aus. Greift NIE auf Player/Map/WorldSession zu, nur auf die hier per Wert
    // uebergebenen Strings/GUIDs - das ist die zentrale Thread-Sicherheits-Invariante dieses Moduls.
    void WorkerThreadMain(ObjectGuid botGuid, ObjectGuid senderGuid, std::string senderName,
        std::string prompt);

    // Baut den vollstaendigen Ollama-/api/generate-Request-JSON-Body (Modellname, Prompt inkl.
    // System-Prompt-Praefix, "stream": false) - reine String-Verarbeitung, kein JSON-Objektmodell
    // (siehe OllamaHttpClient.h-Kopfkommentar fuer die Begruendung).
    std::string BuildRequestJson(std::string const& prompt) const;

    bool _enabled = false;
    std::string _host = "127.0.0.1";
    uint16 _port = 11434;
    std::string _model = "llama3";
    std::string _systemPrompt =
        "You are a World of Warcraft player character in a Legion (patch 7.3.5) roleplay setting. "
        "Reply in-character, in a single short chat line (max ~200 characters), no markdown, no meta "
        "commentary about being an AI.";
    // Maximale gleichzeitig laufende Hintergrund-Threads (Sicherheitsnetz gegen einen Spam-Whisperer,
    // der beliebig viele parallele Ollama-Requests ausloest) - siehe RequestBotReply()-Implementierung.
    uint32 _maxConcurrentRequests = 4;

    mutable std::mutex _queueMutex;
    std::queue<PendingReply> _pendingReplies;
    uint32 _activeRequestCount = 0;
};

#define sOllamaChatMgr OllamaChatMgr::instance()

#endif // OLLAMA_CHAT_MGR_H
