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

// This is where scripts' loading functions should be declared:
void AddSC_deathknight_spell_scripts();
void AddSC_demon_hunter_spell_scripts();
void AddSC_druid_spell_scripts();
void AddSC_generic_spell_scripts();
void AddSC_hunter_spell_scripts();
void AddSC_mage_spell_scripts();
void AddSC_monk_spell_scripts();
void AddSC_paladin_spell_scripts();
void AddSC_priest_spell_scripts();
void AddSC_rogue_spell_scripts();
void AddSC_shaman_spell_scripts();
void AddSC_npc_totem_scripts();
void AddSC_warlock_spell_scripts();
void AddSC_warrior_spell_scripts();
void AddSC_quest_spell_scripts();
void AddSC_item_spell_scripts();
void AddSC_toy_spell_scripts();
void AddSC_artifact_spell_scripts();
void AddSC_artifact_trait_spell_scripts();
void AddSC_artifact_trait_ext_spell_scripts();
void AddSC_artifact_trait_gen_spell_scripts();
void AddSC_mastery_spell_scripts();
void AddSC_netherlight_crucible_scripts();      // Legion-Server Runde 14
void AddSC_class_mechanics_r16_spell_scripts(); // Legion-Server Runde 16
void AddSC_fvs_2026_09_25_spell_scripts(); // Legion-Server freie Vollsystem-Suche 2026-09-25
void AddSC_lcf2_2026_09_25_spell_scripts(); // Legion-Server Runde LCF2 2026-09-25
void AddSC_lcf2r23_2026_09_25_spell_scripts(); // Legion-Server Runde LCF2 R23 2026-09-25
void AddSC_lcf2r24_2026_09_25_spell_scripts(); // Legion-Server Runde LCF2 R24 2026-09-25
void AddSC_lcf2r26_2026_09_25_spell_scripts(); // Legion-Server Runde LCF2 R26 2026-09-25
void AddSC_lcf2r58_2026_09_27_spell_scripts(); // Legion-Server Runde LCF2 R58 2026-09-27

// The name of this function should match:
// void Add${NameOfDirectory}Scripts()
void AddSpellsScripts()
{
    AddSC_deathknight_spell_scripts();
    AddSC_demon_hunter_spell_scripts();
    AddSC_druid_spell_scripts();
    AddSC_generic_spell_scripts();
    AddSC_hunter_spell_scripts();
    AddSC_mage_spell_scripts();
    AddSC_monk_spell_scripts();
    AddSC_paladin_spell_scripts();
    AddSC_priest_spell_scripts();
    AddSC_rogue_spell_scripts();
    AddSC_shaman_spell_scripts();
    AddSC_npc_totem_scripts();
    AddSC_warlock_spell_scripts();
    AddSC_warrior_spell_scripts();
    AddSC_quest_spell_scripts();
    AddSC_item_spell_scripts();
    AddSC_toy_spell_scripts();
    AddSC_artifact_spell_scripts();
    AddSC_artifact_trait_spell_scripts();
    AddSC_artifact_trait_ext_spell_scripts();
    AddSC_artifact_trait_gen_spell_scripts();
    AddSC_mastery_spell_scripts();
    AddSC_netherlight_crucible_scripts();
    AddSC_class_mechanics_r16_spell_scripts();
    AddSC_fvs_2026_09_25_spell_scripts();
    AddSC_lcf2_2026_09_25_spell_scripts();
    AddSC_lcf2r23_2026_09_25_spell_scripts();
    AddSC_lcf2r24_2026_09_25_spell_scripts();
    AddSC_lcf2r26_2026_09_25_spell_scripts();
    AddSC_lcf2r58_2026_09_27_spell_scripts();
}
