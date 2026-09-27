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

// Legion-Server round "LCF2 R58" (2026-09-27): access bridge into Scenario 39746 "A Ring Unbroken" (map 1572).
//
// Investigation finding: NPC Thrall (96527, quest 41335 / gossip 19365) invoker-casts spell 205790 ("Player Choice"),
// which is SPELL_EFFECT_LAUNCH_QUEST_CHOICE (effect 205) for PlayerChoice 266 ("Which weapon should we pursue
// first?"). That part already works with our existing PlayerChoice engine support (world.playerchoice / _response /
// _response_reward, already used by round 26's Kayn/Altruis choice 234).
//
// The Enhancement answer (ResponseId 587, "Travel with Thrall into Deepholm...") grants SpellID 205786 per the
// reference implementation's playerchoice_response_reward table. Spell 205786 has 4 effects; effect index 4 is
// SPELL_EFFECT_TELEPORT_UNITS. Checked both the reference DB's spell_target_position AND our own world.spell_target_position
// for spell 205786 (and its EffectTriggerSpell 211248, "Force Specialization") - 0 rows in BOTH databases. Without a
// TARGET_DEST_DB row, Spell::EffectTeleportUnits (SpellEffects.cpp) has no m_targets.HasDst() and silently does
// nothing (logs "does not have a destination" and returns) - so as shipped, this path leads nowhere, not just for us
// but apparently for the reference implementation's own DB export too (the real destination must be hardcoded in its
// compiled binary, same class of gap as R26's "quest credits only granted from compiled C++").
//
// Fix: same pattern as R26's PlayerScript_r26_kayn_altruis_choice (OnPlayerChoiceResponse hook) - on choosing the
// Enhancement response (587) of choice 266, directly teleport the player to map 1572, landing at the Maelstrom
// Pillar (GO 244431, coords from the reference implementation's gameobject table) where the scenario's opening SmartAI scene plays out.
// The other two responses (588 Elemental / 589 Restoration) are не Teil dieser Szenario-Bruecke (kein Ziel bekannt,
// nicht angefasst) - only 587/Enhancement is wired, matching the task scope (Scenario 39746 only).

#include "Player.h"
#include "ScriptMgr.h"

enum R58Scenario1572Data : uint32
{
    PLAYER_CHOICE_WEAPON_PATH          = 266,
    PLAYER_CHOICE_RESPONSE_ENHANCEMENT = 587,

    MAP_RING_UNBROKEN                  = 1572
};

// Landing spot: Maelstrom Pillar (gameobject 244431 on map 1572, reference DB gameobject guid 136903:
// 824.3 / 1094.42 / 49.3594 / 1.58136).
static Position const R58RingUnbrokenLanding = { 822.7f, 1092.8f, 49.36f, 1.58f };

class PlayerScript_r58_ring_unbroken_choice : public PlayerScript
{
public:
    PlayerScript_r58_ring_unbroken_choice() : PlayerScript("PlayerScript_r58_ring_unbroken_choice") { }

    void OnPlayerChoiceResponse(Player* player, uint32 choiceID, uint32 responseID) override
    {
        if (choiceID != PLAYER_CHOICE_WEAPON_PATH || responseID != PLAYER_CHOICE_RESPONSE_ENHANCEMENT)
            return;

        player->TeleportTo(MAP_RING_UNBROKEN, R58RingUnbrokenLanding.GetPositionX(), R58RingUnbrokenLanding.GetPositionY(),
            R58RingUnbrokenLanding.GetPositionZ(), R58RingUnbrokenLanding.GetOrientation());
    }
};

void AddSC_lcf2r58_2026_09_27_spell_scripts()
{
    new PlayerScript_r58_ring_unbroken_choice();
}
