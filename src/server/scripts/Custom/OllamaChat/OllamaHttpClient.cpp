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
#include <sstream>

bool OllamaHttpClient::PostJson(std::string const& host, uint16_t port, std::string const& path,
    std::string const& jsonBody, std::string& outResponseBody)
{
    try
    {
        boost::asio::io_context ioContext;
        boost::asio::ip::tcp::resolver resolver(ioContext);
        boost::asio::ip::tcp::socket socket(ioContext);

        boost::asio::connect(socket, resolver.resolve(host, std::to_string(port)));

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

        boost::asio::streambuf responseBuf;
        boost::system::error_code ec;
        // read() bis EOF (Connection: close oben) - ec==eof ist hier der ERWARTETE, erfolgreiche
        // Abschluss, kein Fehlerfall.
        boost::asio::read(socket, responseBuf, ec);
        if (ec && ec != boost::asio::error::eof)
            return false;

        std::string const response(boost::asio::buffers_begin(responseBuf.data()),
            boost::asio::buffers_end(responseBuf.data()));

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
