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

#include "OllamaChatMgr.h"
#include "OllamaHttpClient.h"
#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    // Bereinigt eine Modellantwort fuer den Whisper (OI-018): Zeilenumbrueche/Tabs -> Leerzeichen,
    // gerade und typografische Anfuehrungszeichen entfernen, Mehrfach-Leerzeichen zusammenfassen,
    // Anfang/Ende trimmen, Laenge auf ~200 Bytes begrenzen (nie mitten in einem UTF-8-Zeichen).
    std::string SanitizeReplyText(std::string in)
    {
        auto eraseAll = [](std::string& s, std::string const& what)
        {
            for (std::string::size_type pos = s.find(what); pos != std::string::npos; pos = s.find(what, pos))
                s.erase(pos, what.size());
        };
        // typografische Anfuehrungszeichen (UTF-8): „ “ ” ‚ ‘ ’ « »
        for (char const* q : { "\xE2\x80\x9E", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x9A", "\xE2\x80\x98",
                               "\xE2\x80\x99", "\xC2\xAB", "\xC2\xBB" })
            eraseAll(in, q);
        std::string out;
        out.reserve(in.size());
        bool lastSpace = true;
        for (char c : in)
        {
            if (c == '"')
                continue;
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
            if (c == ' ')
            {
                if (lastSpace)
                    continue;
                lastSpace = true;
            }
            else
                lastSpace = false;
            out += c;
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();

        std::size_t const maxBytes = 200;
        if (out.size() > maxBytes)
        {
            std::size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
        }
        return out;
    }

    // Minimaler JSON-String-Escaper - siehe OllamaHttpClient.h-Kopfkommentar fuer die Begruendung,
    // warum hier bewusst kein voller JSON-Objektmodell/keine Bibliothek verwendet wird. Deckt alle
    // Faelle ab, die in einem Chat-Text/Prompt realistisch vorkommen (Anfuehrungszeichen, Backslash,
    // Zeilenumbrueche); sonstige Steuerzeichen werden verworfen statt \u-escaped (fuer Chat-Text
    // irrelevant).
    std::string JsonEscape(std::string const& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        continue;
                    out += c;
            }
        }
        return out;
    }

    // Sucht "fieldName":"..." im rohen JSON-Text und liefert den (Backslash-entschaerften) String-Wert.
    // KEIN vollstaendiger JSON-Parser (keine \uXXXX-Unicode-Escapes, kein Ueberspringen verschachtelter
    // Objekte/Arrays vor dem Treffer) - bewusst ausreichend fuer Ollamas bekanntes, flaches
    // /api/generate-Antwortformat ({"model":...,"response":"...","done":true,...}), siehe
    // OllamaHttpClient.h-Kopfkommentar fuer die volle Begruendung.
    bool ExtractJsonStringField(std::string const& json, std::string const& fieldName, std::string& out)
    {
        std::string const needle = "\"" + fieldName + "\"";
        std::string::size_type pos = json.find(needle);
        if (pos == std::string::npos)
            return false;

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return false;
        ++pos;

        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
            ++pos;

        if (pos >= json.size() || json[pos] != '"')
            return false;
        ++pos;

        std::string value;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                ++pos;
                switch (json[pos])
                {
                    case 'n':  value += '\n'; break;
                    case 'r':  value += '\r'; break;
                    case 't':  value += '\t'; break;
                    case '"':  value += '"';  break;
                    case '\\': value += '\\'; break;
                    default:   value += json[pos]; break;
                }
            }
            else
                value += json[pos];
            ++pos;
        }

        out = value;
        return true;
    }
}

OllamaChatMgr* OllamaChatMgr::instance()
{
    static OllamaChatMgr instance;
    return &instance;
}

void OllamaChatMgr::LoadConfig()
{
    _enabled = sConfigMgr->GetBoolDefault("OllamaChat.Enable", false);
    _host = sConfigMgr->GetStringDefault("OllamaChat.Host", "127.0.0.1");
    _port = uint16(sConfigMgr->GetIntDefault("OllamaChat.Port", 11434));
    _model = sConfigMgr->GetStringDefault("OllamaChat.Model", "llama3");
    _systemPrompt = sConfigMgr->GetStringDefault("OllamaChat.SystemPrompt", _systemPrompt);
    _maxConcurrentRequests = uint32(sConfigMgr->GetIntDefault("OllamaChat.MaxConcurrentRequests", 4));
    int32 const timeoutCfg = sConfigMgr->GetIntDefault("OllamaChat.TimeoutSeconds", 30);
    _requestTimeoutSeconds = uint32(timeoutCfg < 1 ? 1 : (timeoutCfg > 600 ? 600 : timeoutCfg));

    TC_LOG_INFO("scripts.ollamachat", "OllamaChatMgr::LoadConfig: Enable=%d, Host=%s, Port=%u, Model=%s, "
        "MaxConcurrentRequests=%u, TimeoutSeconds=%u.", _enabled, _host.c_str(), uint32(_port), _model.c_str(),
        _maxConcurrentRequests, _requestTimeoutSeconds);
}

std::string OllamaChatMgr::BuildRequestJson(std::string const& prompt) const
{
    std::ostringstream json;
    json << "{"
         << "\"model\":\"" << JsonEscape(_model) << "\","
         << "\"prompt\":\"" << JsonEscape(prompt) << "\","
         << "\"system\":\"" << JsonEscape(_systemPrompt) << "\","
         << "\"stream\":false"
         << "}";
    return json.str();
}

void OllamaChatMgr::RequestBotReply(Player* bot, Player* sender, std::string const& senderMessage)
{
    if (!_enabled || !bot || !sender)
        return;

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_activeRequestCount >= _maxConcurrentRequests)
        {
            TC_LOG_INFO("scripts.ollamachat", "OllamaChatMgr::RequestBotReply: %u/%u gleichzeitige "
                "Ollama-Anfragen bereits aktiv - Anfrage von '%s' an Bot '%s' wird uebersprungen "
                "(kein Backlog/Queueing in dieser ersten Runde).", _activeRequestCount,
                _maxConcurrentRequests, sender->GetName().c_str(), bot->GetName().c_str());
            return;
        }
        ++_activeRequestCount;
    }

    // Prompt-Aufbau: bewusst einfach gehalten (Name/Level des Bots, Name+Text des Absenders) - siehe
    // OllamaChatMgr.h-Kopfkommentar. Eine vollstaendige Kontext-Anreicherung (Klasse/Rasse/Fraktion/
    // Gilde/Gespraechsverlauf wie im Referenzmodul) ist bewusst NICHT Teil dieser ersten Runde.
    std::ostringstream prompt;
    // Deutscher Nutzer-Prompt (OI-018): passt zum deutschen System-Prompt; Anfuehrungszeichen im
    // Whisper-Text werden entschaerft, damit sie die Prompt-Struktur nicht aufbrechen.
    std::string quotedMessage = senderMessage;
    std::replace(quotedMessage.begin(), quotedMessage.end(), '"', '\'');
    prompt << "Du bist " << bot->GetName() << ", ein Charakter der Stufe " << uint32(bot->getLevel())
           << " in World of Warcraft. " << sender->GetName() << " hat dir gerade zugefluestert: \""
           << quotedMessage << "\". Antworte als " << bot->GetName() << " in einer kurzen Chatzeile auf Deutsch.";

    ObjectGuid const botGuid = bot->GetGUID();
    ObjectGuid const senderGuid = sender->GetGUID();
    std::string const senderName = sender->GetName();

    // Siehe Kopfkommentar in OllamaChatMgr.h: EIGENER, unabhaengiger Thread pro Anfrage, greift
    // ausschliesslich auf die hier per Wert uebergebenen GUIDs/Strings zu, NIEMALS auf Player/Map/
    // WorldSession direkt - das ist die zentrale Thread-Sicherheits-Invariante dieses Moduls.
    std::thread([this, botGuid, senderGuid, senderName, promptText = prompt.str()]()
    {
        WorkerThreadMain(botGuid, senderGuid, senderName, promptText);

        std::lock_guard<std::mutex> lock(_queueMutex);
        --_activeRequestCount;
    }).detach();
}

void OllamaChatMgr::WorkerThreadMain(ObjectGuid botGuid, ObjectGuid senderGuid, std::string senderName,
    std::string prompt)
{
    std::string const requestJson = BuildRequestJson(prompt);
    std::string responseBody;

    // BLOCKIEREND - siehe OllamaHttpClient.h-Kopfkommentar. Laeuft auf diesem dedizierten
    // Hintergrund-Thread, NIEMALS auf dem World-Update-Thread.
    if (!OllamaHttpClient::PostJson(_host, _port, "/api/generate", requestJson, responseBody, _requestTimeoutSeconds))
    {
        TC_LOG_ERROR("scripts.ollamachat", "OllamaChatMgr::WorkerThreadMain: HTTP-Request an "
            "Ollama-Server %s:%u fehlgeschlagen (nicht erreichbar oder Timeout nach %u s?) - Antwort auf "
            "Whisper von '%s' entfaellt.", _host.c_str(), uint32(_port), _requestTimeoutSeconds,
            senderName.c_str());
        return;
    }

    std::string replyText;
    if (!ExtractJsonStringField(responseBody, "response", replyText) || replyText.empty())
    {
        TC_LOG_ERROR("scripts.ollamachat", "OllamaChatMgr::WorkerThreadMain: Ollama-Antwort enthielt "
            "kein auswertbares 'response'-Feld (Server-Fehler? Falsches Modell '%s' konfiguriert?) - "
            "Rohantwort (gekuerzt): %.200s", _model.c_str(), responseBody.c_str());
        return;
    }

    replyText = SanitizeReplyText(replyText);
    if (replyText.empty())
    {
        TC_LOG_ERROR("scripts.ollamachat", "OllamaChatMgr::WorkerThreadMain: Antwort war nach der "
            "Bereinigung leer - Antwort auf Whisper von '%s' entfaellt.", senderName.c_str());
        return;
    }

    PendingReply reply;
    reply.BotGuid = botGuid;
    reply.SenderGuid = senderGuid;
    reply.SenderName = std::move(senderName);
    reply.ReplyText = std::move(replyText);

    std::lock_guard<std::mutex> lock(_queueMutex);
    _pendingReplies.push(std::move(reply));
}

void OllamaChatMgr::DeliverPendingReplies()
{
    std::queue<PendingReply> toDeliver;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_pendingReplies.empty())
            return;
        toDeliver.swap(_pendingReplies);
    }

    // Ab hier laeuft alles auf dem World-Update-Thread (WorldScript::OnUpdate(), siehe
    // ollamachat_scriptloader.cpp) - Player::Whisper() darf NUR hier aufgerufen werden.
    while (!toDeliver.empty())
    {
        PendingReply const& reply = toDeliver.front();

        // GUIDs erst JETZT aufloesen - der Bot oder der anfragende Spieler koennte zwischenzeitlich
        // ausgeloggt sein (Ollama-Requests koennen mehrere Sekunden dauern), dann wird die Antwort
        // stillschweigend verworfen statt gegen einen toten Zeiger zu casten.
        Player* bot = ObjectAccessor::FindConnectedPlayer(reply.BotGuid);
        Player* sender = ObjectAccessor::FindConnectedPlayer(reply.SenderGuid);
        if (bot && sender)
        {
            bot->Whisper(reply.ReplyText, LANG_UNIVERSAL, sender);
            TC_LOG_INFO("scripts.ollamachat", "OllamaChatMgr::DeliverPendingReplies: Bot '%s' antwortet "
                "'%s' via Whisper: %.100s", bot->GetName().c_str(), reply.SenderName.c_str(),
                reply.ReplyText.c_str());
        }
        else
            TC_LOG_INFO("scripts.ollamachat", "OllamaChatMgr::DeliverPendingReplies: Bot oder Absender "
                "'%s' nicht mehr online - Antwort verworfen.", reply.SenderName.c_str());

        toDeliver.pop();
    }
}
