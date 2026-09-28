/*
 * VMapHeightSampler - standalone CLI tool
 *
 * Purpose: query VMap (WMO/model collision) based ground height for a given
 * map/x/y, WITHOUT running a full worldserver process. Complements
 * HeightSampler.exe (which only reads .map ADT terrain and therefore cannot
 * answer anything inside a WMO-only interior, e.g. class halls / buildings -
 * the .map grid simply has no data there).
 *
 * This links against the same "common" static library that vmap4assembler
 * and vmap4extractor already use, so it reuses the real VMapManager2 code -
 * the exact same class the live worldserver calls from
 * Map::GetHeight()/Unit::UpdateAllowedPositionZ().
 *
 * IMPORTANT - how to pick the search-start Z:
 * VMapManager2::getHeight() shoots a ray straight DOWN from the given Z and
 * returns the first surface it hits. A WMO's roof/upper floor is itself
 * collision geometry, so starting far above the building (e.g. Z=5000, "from
 * the sky") will report the ROOF height, not the interior floor - this is
 * not a bug, it is exactly how Map::GetHeight() behaves for a real player
 * too (the live server always starts the search from the unit's own last-
 * known Z, never from an arbitrary height). Always pass a searchZ close to
 * the expected/previously-recorded Z (a few yards above it is enough); do
 * not rely on the built-in high default for indoor queries.
 * Verified against a known-good control point: Innkeeper Allison
 * (world.creature guid 188629, map 0, -8867.79/673.673/97.9864, Stormwind
 * Trade District inn) - with searchZ=100 (close to the real Z) this tool
 * returns 97.9031, 0.083 yd off; with searchZ=150 ("from the sky") it
 * returns 114.801, i.e. the roof, confirming the above.
 *
 * Input (stdin), one query per line: "mapId;x;y;searchZ" or "mapId;x;y"
 *   - searchZ: Z to start the vertical search from. Optional; defaults to
 *     5000 (only useful for outdoor/roofless queries; see note above).
 * Output (stdout): "mapId;x;y;z" or "mapId;x;y;NaN" if no VMap hit found.
 *
 * Usage: VMapHeightSampler.exe <vmapsDir> [maxSearchDist] [--debug]
 *   vmapsDir defaults to C:\LegionServer\server\data\vmaps
 *   maxSearchDist defaults to 5000 (yards)
 *   --debug prints tile-load/tree diagnostics to stderr
 */

#include <iostream>
#include <sstream>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <filesystem>

#include "VMapManager2.h"
#include "MapTree.h"
#include "ModelInstance.h"

using namespace VMAP;

int main(int argc, char* argv[])
{
    std::string vmapsDir = R"(C:\LegionServer\server\data\vmaps)";
    float maxSearchDist = 5000.0f;
    bool debug = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--debug")
            debug = true;
        else if (i == 1)
            vmapsDir = arg;
        else if (i == 2)
            maxSearchDist = static_cast<float>(std::atof(arg.c_str()));
    }

    VMapManager2 vmgr;

    // mapId -> already brute-force-loaded (every .vmtile file for that map,
    // tried in both (A,B)/(B,A) tile-index argument order so the file-naming
    // convention of StaticMapTree::getTileFileName() - which does not match
    // the plain ADT gx/gy scheme HeightSampler.exe uses - can never cause a
    // missed tile). Cheap: a few hundred small file opens per map, once.
    std::unordered_map<uint32_t, bool> loadedMaps;

    std::string line;
    while (std::getline(std::cin, line))
    {
        // Strip a UTF-8 byte-order-mark (EF BB BF) if present. PowerShell's
        // pipe/echo/Out-File machinery routinely prepends one to the first
        // (sometimes every) line of text sent to a native process's stdin.
        // Left in place, it corrupts the first token (e.g. "\xEF\xBB\xBF0"
        // instead of "0"), which makes std::stoul() throw std::invalid_argument.
        // That uncaught exception is what previously showed up as an opaque
        // STATUS_STACK_BUFFER_OVERRUN (0xC0000409) crash: on Windows, __fastfail
        // (which the CRT uses to terminate on an unhandled exception) always
        // reports that same generic exception code regardless of the real
        // cause - it is not evidence of an actual stack/buffer overflow.
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);

        // Also strip a trailing '\r' in case of CRLF input read in binary/text
        // mode mismatches.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line.empty())
            continue;

        std::stringstream ss(line);
        std::string tok;
        std::vector<std::string> fields;
        while (std::getline(ss, tok, ';'))
            fields.push_back(tok);

        if (fields.size() < 3)
            continue;

        uint32_t mapId;
        double x, y;
        float searchZ = 5000.0f;
        try
        {
            mapId = static_cast<uint32_t>(std::stoul(fields[0]));
            x = std::stod(fields[1]);
            y = std::stod(fields[2]);
            if (fields.size() >= 4)
                searchZ = static_cast<float>(std::stod(fields[3]));
        }
        catch (std::exception const& e)
        {
            std::cerr << "[error] failed to parse input line \"" << line << "\": " << e.what() << std::endl;
            continue;
        }

        try
        {
            if (!loadedMaps[mapId])
            {
                char prefix[16];
                std::snprintf(prefix, sizeof(prefix), "%04u_", mapId);
                int loadedCount = 0;
                std::error_code ec;
                for (auto const& entry : std::filesystem::directory_iterator(vmapsDir, ec))
                {
                    std::string fname = entry.path().filename().string();
                    if (fname.size() < 12 || fname.rfind(prefix, 0) != 0 || fname.substr(fname.size() - 7) != ".vmtile")
                        continue;
                    std::string rest = fname.substr(5, fname.size() - 5 - 7); // strip "MMMM_" and ".vmtile"
                    size_t us = rest.find('_');
                    if (us == std::string::npos)
                        continue;
                    int a = std::atoi(rest.substr(0, us).c_str());
                    int b = std::atoi(rest.substr(us + 1).c_str());
                    vmgr.loadMap(vmapsDir.c_str(), mapId, a, b);
                    vmgr.loadMap(vmapsDir.c_str(), mapId, b, a);
                    ++loadedCount;
                }
                loadedMaps[mapId] = true;
                if (debug)
                    std::cerr << "[debug] loaded " << loadedCount << " vmap tile files (both orderings) for map " << mapId << std::endl;
            }

            float z = vmgr.getHeight(mapId, static_cast<float>(x), static_cast<float>(y), searchZ, maxSearchDist);

            if (debug)
                std::cerr << "[debug] map=" << mapId << " x=" << x << " y=" << y << " searchZ=" << searchZ << " -> raw=" << z << std::endl;

            std::cout << fields[0] << ";" << fields[1] << ";" << fields[2] << ";";
            if (z <= VMAP_INVALID_HEIGHT)
                std::cout << "NaN";
            else
                std::cout << z;
            std::cout << std::endl;
        }
        catch (std::exception const& e)
        {
            std::cerr << "[error] exception while processing line \"" << line << "\": " << e.what() << std::endl;
            std::cout << fields[0] << ";" << fields[1] << ";" << fields[2] << ";NaN" << std::endl;
            continue;
        }
    }

    return 0;
}
