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

#include "OllamaHttpClient.h"
#include <boost/asio.hpp>
#include <algorithm>
#include <chrono>
#include <sstream>

bool OllamaHttpClient::PostJson(std::string const& host, uint16_t port, std::string const& path,
    std::string const& jsonBody, std::string& outResponseBody, uint32_t timeoutSeconds)
{
    try
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

        boost::asio::io_context ioContext;
        boost::asio::ip::tcp::resolver resolver(ioContext);
        boost::asio::ip::tcp::socket socket(ioContext);

        boost::asio::connect(socket, resolver.resolve(host, std::to_string(port)));

        // Sende-/Empfangs-Timeout pro Socket-Aufruf (Obergrenze fuer ein einzelnes Blockieren); die
        // Gesamtdauer prueft die Leseschleife unten gegen "deadline".
#ifdef _WIN32
        DWORD const sockTimeoutMs = DWORD(std::min<uint32_t>(timeoutSeconds, 3600u) * 1000u);
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const*>(&sockTimeoutMs), sizeof(sockTimeoutMs));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&sockTimeoutMs), sizeof(sockTimeoutMs));
#else
        timeval sockTimeout;
        sockTimeout.tv_sec = long(timeoutSeconds);
        sockTimeout.tv_usec = 0;
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &sockTimeout, sizeof(sockTimeout));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &sockTimeout, sizeof(sockTimeout));
#endif

        // Bewusst ein minimaler, selbst gebauter HTTP/1.1-Request statt einer Bibliothek - siehe
        // Kopfkommentar in OllamaHttpClient.h fuer die Begruendung. "Connection: close" spart uns
        // Keep-Alive-Handling: der Server schliesst die Verbindung nach der Antwort selbst, wir lesen
        // dadurch einfach "bis EOF" (siehe unten).
        std::ostringstream request;
        request << "POST " << path << " HTTP/1.1\r\n"
                << "Host: " << host << "\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << jsonBody.size() << "\r\n"
                << "Connection: close\r\n"
                << "\r\n"
                << jsonBody;

        std::string const requestStr = request.str();
        boost::asio::write(socket, boost::asio::buffer(requestStr));

        // Lesen bis EOF (Connection: close oben) in Stuecken, damit die Gesamtdauer begrenzt werden kann.
        // ec==eof ist der ERWARTETE, erfolgreiche Abschluss, kein Fehlerfall; ein Socket-Timeout
        // (SO_RCVTIMEO) oder das Ueberschreiten der Gesamtfrist bricht mit false ab.
        std::string response;
        char chunk[4096];
        boost::system::error_code ec;
        for (;;)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::size_t const n = socket.read_some(boost::asio::buffer(chunk), ec);
            response.append(chunk, n);
            if (ec == boost::asio::error::eof)
                break;
            if (ec)
                return false;
        }

        // HTTP-Header vom Body trennen (Leerzeile-Trenner "\r\n\r\n") - bewusst KEIN vollstaendiger
        // HTTP-Parser (kein Chunked-Transfer-Encoding, siehe Header-Kommentar): Ollama antwortet bei
        // "stream": false mit einem einzelnen, vollstaendigen Content-Length-Body.
        std::string::size_type const headerEnd = response.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
            return false;

        outResponseBody = response.substr(headerEnd + 4);
        return !outResponseBody.empty();
    }
    catch (std::exception const&)
    {
        // Verbindungsaufbau/-abbruch (Ollama-Server nicht erreichbar, falscher Host/Port, etc.) - kein
        // Absturz, nur ein Fehlschlag dieser einen Anfrage. Aufrufer (OllamaChatMgr) protokolliert.
        return false;
    }
}
