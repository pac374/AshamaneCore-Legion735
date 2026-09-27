/*
 * Copyright (C) 2017-2018 AshamaneProject <https://github.com/AshamaneProject>
 * Copyright (C) 2008-2017 TrinityCore <http://www.trinitycore.org/>
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

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "SceneMgr.h"

class npc_class_hall_druid_gatewarden : public CreatureScript
{
public:
    npc_class_hall_druid_gatewarden(const char* name, uint32 teleportSpell) : CreatureScript(name), _teleportSpell(teleportSpell) { }

    struct npc_class_hall_druid_gatewardenAI : public ScriptedAI
    {
        npc_class_hall_druid_gatewardenAI(Creature* creature, uint32 teleportSpell) : ScriptedAI(creature), _teleportSpell(teleportSpell)
        {
        }

        void MoveInLineOfSight(Unit* who) override
        {
            if (!who->ToPlayer() || !_teleportSpell)
                return;

            if (me->isInBackInMap(who, 12.0f))   // In my line of sight, "outdoors", and behind me
                who->CastSpell(who, _teleportSpell, true);
        }

        uint32 _teleportSpell;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_class_hall_druid_gatewardenAI(creature, _teleportSpell);
    }

    uint32 _teleportSpell;
};

/*
 * Emerald Dreamway gatewardens (map 1540). Each gate teleports the player out of the
 * Dreamway into the matching zone. The spell ids below are the "Teleport: <zone>" spells
 * of Spell.db2 build 7.3.5.26972; all of them share the same signature
 * (SPELL_EFFECT_TELEPORT_UNITS_WITH_VISUAL_LOADING_SCREEN on EFFECT_0, TARGET_UNIT_CASTER),
 * i.e. the destination has to come from `spell_target_position`.
 *
 * Known open points (see reports/class_halls_inventory.md):
 *  - Only 199549 has a `spell_target_position` row; the other gates will fail until the
 *    destinations are known. Do not invent coordinates.
 *  - The Grizzly Hills gate reuses 210066 ("Teleport: Hinterlands"). Spell.db2 7.3.5 has no
 *    "Teleport: Grizzly Hills" counterpart, so the correct id is unknown - left unchanged.
 *  - The Dreamway also has a Val'sharah gate in game; no gatewarden creature for it exists
 *    in the world database, so no script is registered for it.
 */
void AddSC_class_hall_druid()
{
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_dreamgrove",   199549); // Teleport: The Dreamgrove
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_hyjal",        206987); // Teleport: Mount Hyjal
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_feralas",      210073); // Teleport: Feralas
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_moonglade",    200082); // Teleport: Moonglade
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_duskwood",     205013); // Teleport: Duskwood
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_hinterlands",  210066); // Teleport: Hinterlands
    new npc_class_hall_druid_gatewarden("npc_class_hall_druid_gatewarden_grizzlyhills", 210066); // FIXME: wrong spell, see above
}
