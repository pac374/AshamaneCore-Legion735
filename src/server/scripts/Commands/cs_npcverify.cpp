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
Name: npcverify_commandscript
Comment: .npcverify <player> <entry> <spawn guid>: reports whether a creature spawn is loaded near the player and how high it stands above the ground
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include <sstream>

class npcverify_commandscript : public CommandScript
{
public:
    npcverify_commandscript() : CommandScript("npcverify_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> commandTable =
        {
            { "npcverify", rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleNpcVerify, "" },
        };
        return commandTable;
    }

    // .npcverify <player name> <creature entry> <spawn guid>
    // Writes one "[npcverify] ..." line to the server log (and the chat/console) that tools\auto_verify_npcs.ps1 reads:
    //   loaded=0  the spawn is not loaded on the player's map (grid not loaded, wrong map/difficulty or phase)
    //   loaded=1  dist = distance to the player, floor = floor height (terrain and WMO), terrain = terrain height only,
    //             ncr/ncrd/ncre = number/nearest distance/entry of other creatures within 2.5 yd, ngo/ngod/ngoe/ngot = the same for game objects within 3 yd (entry, type),
    //             losup = 1 when the line of sight 1.5 yd straight above the spawn is blocked (spawn inside or directly under geometry)
    static bool HandleNpcVerify(ChatHandler* handler, char const* args)
    {
        if (!*args)
            return false;

        std::istringstream in(args);
        std::string name;
        uint32 entry = 0;
        uint64 guid = 0;
        in >> name >> entry >> guid;
        if (name.empty() || !entry || !guid)
            return false;

        Player* player = ObjectAccessor::FindPlayerByName(name);
        if (!player)
        {
            handler->PSendSysMessage("[npcverify] player %s is not online", name.c_str());
            return true;
        }

        Map* map = player->GetMap();
        Creature* creature = nullptr;
        auto range = map->GetCreatureBySpawnIdStore().equal_range(guid);
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            if (itr->second && itr->second->GetEntry() == entry)
            {
                creature = itr->second;
                break;
            }
        }

        std::string line;
        char buf[768];
        if (!creature)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(guid);
            snprintf(buf, sizeof(buf), "[npcverify] guid=%llu entry=%u loaded=0 db=%d playermap=%u dbmap=%u playerpos=%.2f/%.2f/%.2f",
                (unsigned long long)guid, entry, data ? 1 : 0, player->GetMapId(), data ? uint32(data->mapid) : 0u,
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        }
        else
        {
            float const x = creature->GetPositionX();
            float const y = creature->GetPositionY();
            float const z = creature->GetPositionZ();
            float const floorZ = map->GetHeight(creature->GetPhaseShift(), x, y, z + 3.0f, true, 20.0f);
            float const terrainZ = map->GetHeight(creature->GetPhaseShift(), x, y, z + 3.0f, false, 20.0f);
            // The height search starts 2 yd above the given z. floor (z + 3) therefore starts 5 yd above the spawn and finds the ceiling
            // of a room or the roof of a building; floorlow starts 1 yd above the spawn and finds the floor the creature really stands on.
            float const floorLowZ = map->GetHeight(creature->GetPhaseShift(), x, y, z - 1.0f, true, 6.0f);
            std::list<Creature*> nearCreatures;
            creature->GetCreatureListWithEntryInGrid(nearCreatures, 0, 2.5f);
            uint32 nearCreatureCount = 0, nearCreatureEntry = 0;
            float nearCreatureDist = 99.0f;
            for (Creature* other : nearCreatures)
            {
                if (other == creature || !other->IsAlive())
                    continue;
                ++nearCreatureCount;
                float const d = creature->GetExactDist(other);
                if (d < nearCreatureDist)
                {
                    nearCreatureDist = d;
                    nearCreatureEntry = other->GetEntry();
                }
            }

            std::list<GameObject*> nearObjects;
            creature->GetGameObjectListWithEntryInGrid(nearObjects, 0, 3.0f);
            uint32 nearObjectCount = 0, nearObjectEntry = 0, nearObjectType = 0;
            float nearObjectDist = 99.0f;
            for (GameObject* object : nearObjects)
            {
                ++nearObjectCount;
                float const d = creature->GetExactDist(object);
                if (d < nearObjectDist)
                {
                    nearObjectDist = d;
                    nearObjectEntry = object->GetEntry();
                    nearObjectType = uint32(object->GetGoType());
                }
            }

            bool const losUp = !creature->IsWithinLOS(x, y, z + 1.5f);
            snprintf(buf, sizeof(buf), "[npcverify] guid=%llu entry=%u loaded=1 alive=%d dist=%.2f x=%.3f y=%.3f z=%.3f floor=%.3f floorlow=%.3f terrain=%.3f ncr=%u ncrd=%.2f ncre=%u ngo=%u ngod=%.2f ngoe=%u ngot=%u losup=%d",
                (unsigned long long)guid, entry, creature->IsAlive() ? 1 : 0, player->GetExactDist(creature), x, y, z, floorZ, floorLowZ, terrainZ,
                nearCreatureCount, nearCreatureDist, nearCreatureEntry, nearObjectCount, nearObjectDist, nearObjectEntry, nearObjectType, losUp ? 1 : 0);
        }

        line = buf;
        TC_LOG_INFO("server", "%s", line.c_str());
        handler->PSendSysMessage("%s", line.c_str());
        return true;
    }
};

void AddSC_npcverify_commandscript()
{
    new npcverify_commandscript();
}
