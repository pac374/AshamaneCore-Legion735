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

#ifndef OLLAMA_HTTP_CLIENT_H
#define OLLAMA_HTTP_CLIENT_H

// ---------------------------------------------------------------------------
// Ollama-Chat-Modul (Ideenreferenz mod-ollama-chat, siehe README Abschnitt e) fuer die
// Lizenz-/Kompatibilitaets-Begruendung: das Referenzmodul ist AGPL-3.0 und zielt auf eine andere
// Core-Version - hier wird NICHTS daraus uebernommen, nur das Feature-Konzept "Bot-Chat-Antworten ueber
// eine lokale Ollama-HTTP-API generieren statt zufaellig wuerfeln").
//
// Bewusste Design-Entscheidung: KEINE Drittbibliothek vendored (das Referenzmodul nutzt cpp-httplib
// [~23.000 Zeilen] + nlohmann/json [~33.000 Zeilen], beide MIT-lizenziert und damit fuer sich genommen
// unproblematisch mit unserem GPL-2.0 zu kombinieren - das ist NICHT der Grund). Der eigentliche Grund:
// diese Server-Version kann in dieser Sitzung nicht kompiliert/getestet werden (kein Build-Environment
// verfuegbar, siehe alle vorherigen Runden-Berichte) - ~56.000 Zeilen zweier komplett ungetesteter
// Fremdbibliotheken zu vendoren waere fuer einen so kleinen, klar begrenzten Anwendungsfall (EIN
// JSON-POST-Request, EIN JSON-String-Feld aus der Antwort auslesen) ein unverhaeltnismaessiges Risiko.
// Stattdessen: ein minimaler, selbst geschriebener HTTP/1.1-Client auf Basis von boost::asio (bereits
// eine verlinkte, garantiert vorhandene Core-Abhaengigkeit dieses Forks) plus eine handgeschriebene
// String-basierte JSON-Konstruktion/-Extraktion in OllamaChatMgr.cpp - weniger Code, keine neue
// Bibliotheksabhaengigkeit, vollstaendig auditierbar.
//
// Dokumentierte Einschraenkungen dieser ersten Runde (bewusst, nicht versteckt):
//   - KEIN TLS/HTTPS - Ollama laeuft laut eigener Dokumentation standardmaessig ausschliesslich
//     unverschluesselt auf localhost; fuer einen entfernten/verschluesselten Ollama-Endpunkt muesste
//     diese Klasse erweitert werden (nicht Teil dieser Runde).
//   - KEIN Chunked-Transfer-Encoding-Support - es wird ein einzelner, vollstaendiger
//     Content-Length-Response-Body erwartet (das ist exakt, was Ollamas /api/generate mit "stream":false
//     liefert - siehe OllamaChatMgr.cpp).
//   - KEIN explizites Timeout in dieser ersten Runde - ein haengender Ollama-Server wuerde den
//     Hintergrund-Thread (siehe OllamaChatMgr::WorkerThreadMain()) unbegrenzt blockieren. Da JEDE Anfrage
//     einen EIGENEN, kurzlebigen std::thread bekommt (nie den World-Update-Thread), ist der Blast-Radius
//     auf diesen einen Thread begrenzt - kein Serverabsturz/-hänger, aber ein sauberes Timeout waere ein
//     sinnvoller naechster Ausbauschritt.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

class OllamaHttpClient
{
public:
    // Fuehrt einen BLOCKIERENDEN HTTP/1.1-POST-Request gegen host:port/path aus, mit jsonBody als Body
    // und "Content-Type: application/json". Gibt true zurueck und befuellt outResponseBody, wenn ein
    // vollstaendiger Response gelesen werden konnte (unabhaengig vom HTTP-Statuscode - Aufrufer muss bei
    // Bedarf selbst pruefen, ob outResponseBody eine Fehlermeldung statt der erwarteten JSON-Antwort
    // enthaelt). false bei jedem Verbindungs-/Netzwerkfehler.
    //
    // WICHTIG: blockierend - NIEMALS auf dem World-Update-Thread aufrufen, nur aus einem dedizierten
    // Hintergrund-Thread (siehe OllamaChatMgr::WorkerThreadMain()).
    static bool PostJson(std::string const& host, uint16_t port, std::string const& path,
        std::string const& jsonBody, std::string& outResponseBody);
};

#endif // OLLAMA_HTTP_CLIENT_H
