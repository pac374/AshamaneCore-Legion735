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

#include "ClassHall.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GarrisonMgr.h"
#include "Log.h"
#include "MapManager.h"
#include "ObjectMgr.h"

ClassHall::ClassHall(Player* owner) : Garrison(owner)
{
    _garrisonType = GARRISON_TYPE_CLASS_HALL;
}

bool ClassHall::LoadFromDB()
{
    if (!Garrison::LoadFromDB())
        return false;

    // OI-030: Talente laden. Fertige Forschungen (Zeit abgelaufen) werden sofort als READY markiert.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARACTER_GARRISON_TALENTS);
    stmt->setUInt64(0, _owner->GetGUID().GetCounter());
    stmt->setUInt8(1, _garrisonType);
    _talents.clear();
    if (PreparedQueryResult talentsResult = CharacterDatabase.Query(stmt))
    {
        do
        {
            Field* fields = talentsResult->Fetch();
            if (!sGarrTalentStore.LookupEntry(fields[0].GetUInt32()))
            {
                TC_LOG_ERROR("garrison", "ClassHall::LoadFromDB: Talent %u (Spieler %s) existiert nicht in GarrTalent.db2 - uebersprungen.",
                    fields[0].GetUInt32(), _owner->GetGUID().ToString().c_str());
                continue;
            }

            WorldPackets::Garrison::GarrisonTalent talent;
            talent.GarrTalentID = int32(fields[0].GetUInt32());
            talent.ResearchStartTime = time_t(fields[1].GetInt64());
            talent.Flags = fields[2].GetInt32();
            _talents.push_back(talent);
        } while (talentsResult->NextRow());
    }

    for (WorldPackets::Garrison::GarrisonTalent& talent : _talents)
        if (talent.Flags != CLASS_HALL_TALENT_READY)
            FinishTalent(talent); // setzt nur READY, wenn die Zeit abgelaufen ist

    return true;
}

void ClassHall::SaveToDB(CharacterDatabaseTransaction& trans)
{
    Garrison::SaveToDB(trans); // loescht zuerst alle Zeilen dieser Garnison inkl. Talente (DeleteFromDB)

    for (WorldPackets::Garrison::GarrisonTalent const& talent : _talents)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_GARRISON_TALENTS);
        stmt->setUInt64(0, _owner->GetGUID().GetCounter());
        stmt->setUInt8(1, _garrisonType);
        stmt->setUInt32(2, uint32(talent.GarrTalentID));
        stmt->setInt64(3, int64(talent.ResearchStartTime));
        stmt->setInt32(4, talent.Flags);
        trans->Append(stmt);
    }
}

void ClassHall::Update(uint32 const diff)
{
    Garrison::Update(diff);

    // Forschungen einmal pro Sekunde auf Fertigstellung pruefen
    _talentUpdateAccumMs += diff;
    if (_talentUpdateAccumMs < 1000)
        return;
    _talentUpdateAccumMs = 0;

    for (WorldPackets::Garrison::GarrisonTalent& talent : _talents)
        if (talent.Flags != CLASS_HALL_TALENT_READY)
            FinishTalent(talent);
}

void ClassHall::Enter()
{
    Garrison::Enter();
    ApplyTalentPerks();
}

void ClassHall::SendResearchResult(int32 result, uint32 talentId, uint32 researchTime, uint32 flags) const
{
    WorldPackets::Garrison::GarrisonResearchTalent packet;
    packet.Result = result;
    packet.GarrTypeID = GARRISON_TYPE_CLASS_HALL;
    packet.TalentID = talentId;
    packet.ResearchTime = researchTime;
    packet.Flags = flags;
    _owner->SendDirectMessage(packet.Write());
}

void ClassHall::FinishTalent(WorldPackets::Garrison::GarrisonTalent& talent)
{
    GarrTalentEntry const* entry = sGarrTalentStore.LookupEntry(uint32(talent.GarrTalentID));
    if (!entry)
        return;

    int64 const duration = (talent.Flags == CLASS_HALL_TALENT_CHANGE) ? entry->RespecDurationSecs : entry->ResearchDurationSecs;
    if (time(nullptr) < int64(talent.ResearchStartTime) + duration)
        return;

    talent.Flags = CLASS_HALL_TALENT_READY;
    if (entry->PerkSpellID && _owner->IsInWorld() && !_owner->HasAura(uint32(entry->PerkSpellID)))
        _owner->CastSpell(_owner, uint32(entry->PerkSpellID), TRIGGERED_FULL_MASK);
}

void ClassHall::ApplyTalentPerks() const
{
    for (WorldPackets::Garrison::GarrisonTalent const& talent : _talents)
    {
        if (talent.Flags != CLASS_HALL_TALENT_READY)
            continue;
        GarrTalentEntry const* entry = sGarrTalentStore.LookupEntry(uint32(talent.GarrTalentID));
        if (entry && entry->PerkSpellID && !_owner->HasAura(uint32(entry->PerkSpellID)))
            _owner->CastSpell(_owner, uint32(entry->PerkSpellID), TRIGGERED_FULL_MASK);
    }
}

bool ClassHall::HasTalent(uint32 talentId) const
{
    for (WorldPackets::Garrison::GarrisonTalent const& talent : _talents)
        if (uint32(talent.GarrTalentID) == talentId)
            return talent.Flags == CLASS_HALL_TALENT_READY;
    return false;
}

void ClassHall::ResearchTalent(uint32 talentId)
{
    GarrTalentEntry const* entry = sGarrTalentStore.LookupEntry(talentId);
    if (!entry)
    {
        SendResearchResult(GARRISON_ERROR_INVALID_TALENT, talentId, 0, 0);
        return;
    }

    // Das Talent muss zum Talentbaum der Klasse dieses Spielers (Ordenshalle, Typ 3) gehoeren.
    GarrTalentTreeEntry const* tree = sGarrTalentTreeStore.LookupEntry(uint32(entry->GarrTalentTreeID));
    if (!tree || tree->GarrTypeID != GARRISON_TYPE_CLASS_HALL || tree->ClassID != int32(_owner->getClass()))
    {
        SendResearchResult(GARRISON_ERROR_INVALID_TALENT, talentId, 0, 0);
        return;
    }

    // Immer nur eine laufende Forschung.
    int64 const now = time(nullptr);
    for (WorldPackets::Garrison::GarrisonTalent& other : _talents)
    {
        FinishTalent(other);
        if (other.Flags != CLASS_HALL_TALENT_READY)
        {
            SendResearchResult(GARRISON_ERROR_ALREADY_RESEARCHING_TALENT, talentId, 0, 0);
            return;
        }
        if (uint32(other.GarrTalentID) == talentId)
        {
            SendResearchResult(GARRISON_ERROR_INVALID_TALENT, talentId, 0, 0); // schon erforscht
            return;
        }
    }

    if (entry->PlayerConditionID)
    {
        PlayerConditionEntry const* condition = sPlayerConditionStore.LookupEntry(uint32(entry->PlayerConditionID));
        if (condition && !ConditionMgr::IsPlayerMeetingCondition(_owner, condition))
        {
            SendResearchResult(GARRISON_ERROR_INVALID_TALENT, talentId, 0, 0);
            return;
        }
    }

    // Gleiche Tier-Stufe schon belegt -> Wechsel (Respec-Kosten), das alte Talent wird ersetzt.
    auto toReplace = _talents.end();
    for (auto itr = _talents.begin(); itr != _talents.end(); ++itr)
    {
        GarrTalentEntry const* otherEntry = sGarrTalentStore.LookupEntry(uint32(itr->GarrTalentID));
        if (otherEntry && otherEntry->GarrTalentTreeID == entry->GarrTalentTreeID && otherEntry->Tier == entry->Tier)
        {
            toReplace = itr;
            break;
        }
    }

    bool const isChange = toReplace != _talents.end();
    int32 const currency = isChange ? entry->RespecCostCurrencyTypesID : entry->ResearchCostCurrencyTypesID;
    int32 const cost = isChange ? entry->RespecCost : entry->ResearchCost;
    int32 const gold = isChange ? entry->RespecGoldCost : entry->ResearchGoldCost;

    if (cost > 0 && currency > 0 && !_owner->HasCurrency(uint32(currency), uint32(cost)))
    {
        SendResearchResult(GARRISON_ERROR_NOT_ENOUGH_CURRENCY, talentId, 0, 0);
        return;
    }
    if (gold > 0 && !_owner->HasEnoughMoney(uint64(gold)))
    {
        SendResearchResult(GARRISON_ERROR_NOT_ENOUGH_GOLD, talentId, 0, 0);
        return;
    }

    if (cost > 0 && currency > 0)
        _owner->ModifyCurrency(uint32(currency), -cost, false, true);
    if (gold > 0)
        _owner->ModifyMoney(-int64(gold));

    if (isChange)
    {
        // Perk des ersetzten Talents entfernen
        if (GarrTalentEntry const* oldEntry = sGarrTalentStore.LookupEntry(uint32(toReplace->GarrTalentID)))
            if (oldEntry->PerkSpellID)
                _owner->RemoveAurasDueToSpell(uint32(oldEntry->PerkSpellID));
        _talents.erase(toReplace);
    }

    WorldPackets::Garrison::GarrisonTalent talent;
    talent.GarrTalentID = int32(talentId);
    talent.ResearchStartTime = time_t(now);
    talent.Flags = isChange ? CLASS_HALL_TALENT_CHANGE : CLASS_HALL_TALENT_IN_RESEARCH;
    _talents.push_back(talent);

    SendResearchResult(GARRISON_SUCCESS, talentId, uint32(now), 0);
    FinishTalent(_talents.back()); // Dauer 0 -> sofort fertig
}

bool ClassHall::Create(uint32 garrSiteId)
{
    if (!Garrison::Create(garrSiteId))
        return false;

    return true;
}

void ClassHall::Delete()
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    DeleteFromDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    Garrison::Delete();
}

bool ClassHall::IsAllowedArea(AreaTableEntry const* area) const
{
    if (!area)
        return false;

    // TODO : Find a better way to handle this
    return area->Flags[1] & AREA_FLAG_GARRISON && (area->ID >= 7638 && area->ID <= 8023);
}
