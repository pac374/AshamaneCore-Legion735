/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "BotPopulationMgr.h"
#include "AccountMgr.h"
#include "BotMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace
{
    // Zustaende des Fabrik-Automaten
    enum JobState { JOB_NEW = 0, JOB_WAIT_CHAR, JOB_WAIT_LOGIN, JOB_SETTLE, JOB_PROVISION, JOB_PLACE, JOB_FINISH, JOB_DONE, JOB_FAILED };

    uint32 WeightedPick(std::vector<uint32> const& weights)
    {
        uint64 sum = 0;
        for (uint32 w : weights)
            sum += w;
        if (!sum)
            return uint32(-1);
        uint64 r = urand(0, uint32(std::min<uint64>(sum - 1, 0x7FFFFFFF)));
        for (uint32 i = 0; i < weights.size(); ++i)
        {
            if (r < weights[i])
                return i;
            r -= weights[i];
        }
        return uint32(weights.size() - 1);
    }

    std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, sep))
        {
            tok.erase(std::remove_if(tok.begin(), tok.end(), [](unsigned char c) { return std::isspace(c); }), tok.end());
            if (!tok.empty())
                out.push_back(tok);
        }
        return out;
    }
}

BotPopulationMgr* BotPopulationMgr::instance()
{
    static BotPopulationMgr instance;
    return &instance;
}

void BotPopulationMgr::LoadConfig()
{
    _managerEnabled = sConfigMgr->GetBoolDefault("Playerbots.Pop.Enable", false);
    _accountPrefix = sConfigMgr->GetStringDefault("Playerbots.Pop.AccountPrefix", "rndbot");
    _minOnline = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbots.Pop.MinOnline", 0)));
    _maxOnline = uint32(std::max<int32>(int32(_minOnline), sConfigMgr->GetIntDefault("Playerbots.Pop.MaxOnline", 0)));
    _updateIntervalMs = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("Playerbots.Pop.UpdateIntervalSeconds", 20))) * 1000;
    _perInterval = uint32(std::max<int32>(1, std::min<int32>(100, sConfigMgr->GetIntDefault("Playerbots.Pop.BotsPerInterval", 5))));
    _targetChangeMinS = uint32(std::max<int32>(30, sConfigMgr->GetIntDefault("Playerbots.Pop.TargetChangeMinSeconds", 1800)));
    _targetChangeMaxS = uint32(std::max<int32>(int32(_targetChangeMinS), sConfigMgr->GetIntDefault("Playerbots.Pop.TargetChangeMaxSeconds", 7200)));
    _maxParallelJobs = uint32(std::max<int32>(1, std::min<int32>(10, sConfigMgr->GetIntDefault("Playerbots.Factory.Parallel", 2))));
    _keepOnlineAfterCreate = sConfigMgr->GetBoolDefault("Playerbots.Factory.KeepOnline", false);
    _allianceRatio = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbots.AllianceRatio", 50)));
    _hordeRatio = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbots.HordeRatio", 50)));
    _levelMin = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("Playerbots.Level.Min", 1)));
    _levelMax = uint32(std::min<int32>(110, std::max<int32>(int32(_levelMin), sConfigMgr->GetIntDefault("Playerbots.Level.Max", 110))));
    _minLevelChance = std::max(0.0f, std::min(1.0f, sConfigMgr->GetFloatDefault("Playerbots.Level.MinChance", 0.05f)));
    _maxLevelChance = std::max(0.0f, std::min(1.0f, sConfigMgr->GetFloatDefault("Playerbots.Level.MaxChance", 0.10f)));
    _targetIlvlAt110 = uint16(std::max<int32>(0, std::min<int32>(2000, sConfigMgr->GetIntDefault("Playerbots.Gear.TargetIlvlAtMaxLevel", 0))));

    // Stufenbaender "von-bis:Gewicht,..."
    _bands.clear();
    std::string const bands = sConfigMgr->GetStringDefault("Playerbots.Level.Bands", "1-9:10,10-29:20,30-59:20,60-89:20,90-100:15,101-110:15");
    for (std::string const& b : Split(bands, ','))
    {
        std::string::size_type dash = b.find('-'), colon = b.find(':');
        if (dash == std::string::npos || colon == std::string::npos || colon < dash)
            continue;
        Band band;
        band.Min = uint8(std::min<unsigned long>(110, std::strtoul(b.substr(0, dash).c_str(), nullptr, 10)));
        band.Max = uint8(std::min<unsigned long>(110, std::strtoul(b.substr(dash + 1, colon - dash - 1).c_str(), nullptr, 10)));
        band.Weight = uint32(std::strtoul(b.substr(colon + 1).c_str(), nullptr, 10));
        if (band.Min >= 1 && band.Max >= band.Min && band.Weight > 0)
            _bands.push_back(band);
    }

    // Klassen-/Volksgewichte (Standard 1 = gleichverteilt unter den im Core gueltigen Paaren), Spec-Gewichte je Klasse
    for (uint32 c = 1; c <= 12; ++c)
        _classWeight[c] = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault(("Playerbots.ClassWeight." + std::to_string(c)).c_str(), 1)));
    for (uint32 r = 1; r < 40; ++r)
        _raceWeight[r] = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault(("Playerbots.RaceWeight." + std::to_string(r)).c_str(), 1)));
    for (uint32 c = 1; c <= 12; ++c)
        for (uint32 s = 0; s < 8; ++s)
            _specWeight[c][s] = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault(("Playerbots.SpecWeight." + std::to_string(c) + "." + std::to_string(s)).c_str(), 1)));

    // Rollenmischung "tank=10,heal=15,dps=75" (leer = nur Spec-Gewichte)
    _roleMix.clear();
    for (std::string const& p : Split(sConfigMgr->GetStringDefault("Playerbots.RoleMix", "tank=10,heal=15,dps=75"), ','))
    {
        std::string::size_type eq = p.find('=');
        if (eq == std::string::npos)
            continue;
        std::string name = p.substr(0, eq);
        uint32 w = uint32(std::strtoul(p.substr(eq + 1).c_str(), nullptr, 10));
        uint8 role = name == "tank" ? 1 : (name == "heal" ? 2 : (name == "dps" ? 3 : 0));
        if (role && w)
            _roleMix.push_back({ role, w });
    }

    TC_LOG_INFO("scripts.bots", "BotPopulationMgr::LoadConfig: Manager=%d, Praefix=%s, Online %u-%u, Stufen %u-%u (%u Baender), Fraktion %u:%u, Rollenmix-Eintraege %u.",
        _managerEnabled, _accountPrefix.c_str(), _minOnline, _maxOnline, _levelMin, _levelMax, uint32(_bands.size()),
        _allianceRatio, _hordeRatio, uint32(_roleMix.size()));
}

uint8 BotPopulationMgr::RollLevel() const
{
    float const r = float(rand_chance()) / 100.0f;
    if (r < _minLevelChance)
        return uint8(_levelMin);
    if (r < _minLevelChance + _maxLevelChance)
        return uint8(_levelMax);
    if (_bands.empty())
        return uint8(urand(_levelMin, _levelMax));
    std::vector<uint32> weights;
    for (Band const& b : _bands)
        weights.push_back(b.Weight);
    uint32 idx = WeightedPick(weights);
    if (idx == uint32(-1))
        return uint8(urand(_levelMin, _levelMax));
    uint32 lo = std::max<uint32>(_bands[idx].Min, _levelMin), hi = std::min<uint32>(_bands[idx].Max, _levelMax);
    if (hi < lo)
        return uint8(urand(_levelMin, _levelMax));
    return uint8(urand(lo, hi));
}

uint8 BotPopulationMgr::RollRole(uint8 classId) const
{
    if (_roleMix.empty())
        return 0;
    // Rolle nur aus den Rollen waehlen, die die Klasse ueberhaupt hat
    bool has[4] = { false, false, false, false };
    for (uint32 i = 0; i < sChrSpecializationStore.GetNumRows(); ++i)
        if (ChrSpecializationEntry const* s = sChrSpecializationStore.LookupEntry(i))
            if (s->ClassID == int8(classId) && s->Role >= 0 && s->Role <= 2)
                has[s->Role + 1] = true;
    std::vector<uint32> weights;
    std::vector<uint8> roles;
    for (RoleW const& rw : _roleMix)
        if (has[rw.Role])
        {
            weights.push_back(rw.W);
            roles.push_back(rw.Role);
        }
    uint32 idx = WeightedPick(weights);
    return idx == uint32(-1) ? uint8(3) : roles[idx];
}

BotPopulationMgr::Roll BotPopulationMgr::RollBot() const
{
    Roll roll;
    // 1. Fraktion
    uint32 const total = _allianceRatio + _hordeRatio;
    bool alliance = total == 0 ? urand(0, 1) == 0 : urand(0, total - 1) < _allianceRatio;

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        // 2. Klasse nach Gewicht unter den Klassen, die in dieser Fraktion mindestens ein gueltiges Volk haben
        std::vector<uint32> classWeights(13, 0);
        for (uint32 c = 1; c <= 12; ++c)
        {
            if (!_classWeight[c])
                continue;
            for (uint32 r = 1; r < MAX_RACES; ++r)
                if (_raceWeight[r] && sObjectMgr->GetPlayerInfo(r, c) && ((Player::TeamForRace(r) == ALLIANCE) == alliance))
                {
                    classWeights[c] = _classWeight[c];
                    break;
                }
        }
        uint32 classId = WeightedPick(classWeights);
        if (classId == uint32(-1) || classId == 0)
        {
            alliance = !alliance; // Fraktion ohne gueltige Klasse: andere versuchen
            continue;
        }

        // 3. Volk nach Gewicht unter den gueltigen Paaren der Fraktion
        std::vector<uint32> raceWeights(MAX_RACES, 0);
        for (uint32 r = 1; r < MAX_RACES; ++r)
            if (_raceWeight[r] && sObjectMgr->GetPlayerInfo(r, classId) && ((Player::TeamForRace(r) == ALLIANCE) == alliance))
                raceWeights[r] = _raceWeight[r];
        uint32 raceId = WeightedPick(raceWeights);
        if (raceId == uint32(-1) || raceId == 0)
            continue;

        roll.Class = uint8(classId);
        roll.Race = uint8(raceId);
        break;
    }
    if (!roll.Class)
    {
        roll.Class = CLASS_WARRIOR;
        roll.Race = RACE_HUMAN;
    }
    roll.Sex = uint8(urand(0, 1));
    roll.Level = RollLevel();

    // 4. Rolle / Spezialisierung (ab Stufe 10; vorher gibt es keine Wahl)
    if (roll.Level >= 10)
    {
        roll.Role = RollRole(roll.Class);
        // Spec nach Gewicht (SpecWeight.<Klasse>.<Index>) unter den Specs der Rolle (bei Rolle 0: unter allen)
        std::vector<uint32> weights;
        std::vector<uint32> specIds;
        for (uint32 i = 0; i < sChrSpecializationStore.GetNumRows(); ++i)
            if (ChrSpecializationEntry const* s = sChrSpecializationStore.LookupEntry(i))
                if (s->ClassID == int8(roll.Class) && s->OrderIndex >= 0 && s->OrderIndex < 8 && (!roll.Role || s->Role == int8(roll.Role - 1)))
                {
                    weights.push_back(_specWeight[roll.Class][s->OrderIndex]);
                    specIds.push_back(s->ID);
                }
        uint32 idx = WeightedPick(weights);
        if (idx != uint32(-1))
            roll.SpecId = specIds[idx];
        else
            roll.SpecId = 0;
        if (roll.SpecId)
            roll.Role = 0; // Spec ist bestimmt, Rolle wird daraus abgeleitet
    }
    if (roll.Level >= 110)
        roll.TargetIlvl = _targetIlvlAt110;
    return roll;
}

std::string BotPopulationMgr::RandomName() const
{
    static char const* const starts[] = { "Br", "Dr", "Gr", "Kal", "Mor", "Thal", "Vor", "Zar", "Fen", "Lor", "Sil", "Tor", "Ar", "El", "Ka", "Ma", "Ni", "Ra", "Sa", "Vel" };
    static char const* const mids[] = { "a", "e", "i", "o", "u", "ar", "en", "il", "or", "an", "ur", "ia", "ae" };
    static char const* const ends[] = { "dan", "ric", "wen", "mar", "thos", "lian", "nor", "vok", "rim", "ael", "gar", "iel", "dus", "mir", "ron", "sa", "la" };
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        std::string n = std::string(starts[urand(0, uint32(std::size(starts) - 1))]) + mids[urand(0, uint32(std::size(mids) - 1))]
            + ends[urand(0, uint32(std::size(ends) - 1))];
        n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
        if (ObjectMgr::CheckPlayerName(n, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
            continue;
        if (QueryResult r = CharacterDatabase.PQuery("SELECT 1 FROM characters WHERE name = '%s'", n.c_str()))
            continue;
        return n;
    }
    // Notfall: zufaellige Buchstabenfolge
    std::string n = "B";
    for (int i = 0; i < 7; ++i)
        n += char('a' + urand(0, 25));
    return n;
}

BotPopulationMgr::Roll BotPopulationMgr::RollFixed(uint8 level, uint8 role, bool alliance) const
{
    Roll roll;
    roll.Level = level;
    // Klassen, die die gewuenschte Rolle (1 Tank, 2 Heiler, 3 Schaden) ueberhaupt haben und in der Fraktion ein gueltiges Volk
    std::vector<uint32> classWeights(13, 0);
    for (uint32 c = 1; c <= 12; ++c)
    {
        bool hasRole = false;
        for (uint32 i = 0; i < sChrSpecializationStore.GetNumRows() && !hasRole; ++i)
            if (ChrSpecializationEntry const* s = sChrSpecializationStore.LookupEntry(i))
                if (s->ClassID == int8(c) && s->Role == int8(role - 1))
                    hasRole = true;
        if (!hasRole || !_classWeight[c])
            continue;
        for (uint32 r = 1; r < MAX_RACES; ++r)
            if (_raceWeight[r] && sObjectMgr->GetPlayerInfo(r, c) && ((Player::TeamForRace(r) == ALLIANCE) == alliance))
            {
                classWeights[c] = _classWeight[c];
                break;
            }
    }
    uint32 classId = WeightedPick(classWeights);
    if (classId == uint32(-1) || classId == 0)
        classId = CLASS_WARRIOR;
    std::vector<uint32> raceWeights(MAX_RACES, 0);
    for (uint32 r = 1; r < MAX_RACES; ++r)
        if (_raceWeight[r] && sObjectMgr->GetPlayerInfo(r, classId) && ((Player::TeamForRace(r) == ALLIANCE) == alliance))
            raceWeights[r] = _raceWeight[r];
    uint32 raceId = WeightedPick(raceWeights);
    roll.Class = uint8(classId);
    roll.Race = raceId == uint32(-1) || raceId == 0 ? uint8(alliance ? RACE_HUMAN : RACE_ORC) : uint8(raceId);
    roll.Sex = uint8(urand(0, 1));
    std::vector<uint32> weights, specIds;
    for (uint32 i = 0; i < sChrSpecializationStore.GetNumRows(); ++i)
        if (ChrSpecializationEntry const* s = sChrSpecializationStore.LookupEntry(i))
            if (s->ClassID == int8(roll.Class) && s->OrderIndex >= 0 && s->OrderIndex < 8 && s->Role == int8(role - 1))
            {
                weights.push_back(std::max<uint32>(1, _specWeight[roll.Class][s->OrderIndex]));
                specIds.push_back(s->ID);
            }
    uint32 idx = WeightedPick(weights);
    roll.SpecId = idx == uint32(-1) ? 0 : specIds[idx];
    return roll;
}

std::string BotPopulationMgr::QueueRaidPool(uint32 count, uint16 ilvl, bool alliance, std::string const& prefix, uint32 firstSlot)
{
    for (uint32 i = 0; i < count; ++i)
    {
        Job job;
        job.Index = _nextIndex++;
        job.Prefix = prefix;
        uint32 const slot = (i + firstSlot) % 25;
        uint8 const role = slot < 2 ? 1 : (slot < 7 ? 2 : 3);
        job.R = RollFixed(110, role, alliance);
        job.R.TargetIlvl = ilvl;
        _jobs.push_back(job);
    }
    std::ostringstream out;
    out << count << " Raid-Pool-Bots (Stufe 110, Ilvl >= " << ilvl << ", " << (alliance ? "Allianz" : "Horde") << ", Praefix " << prefix
        << ") eingereiht (" << _jobs.size() << " Auftraege in der Warteschlange)";
    return out.str();
}

std::string BotPopulationMgr::QueueFactory(uint32 count)
{
    for (uint32 i = 0; i < count; ++i)
    {
        Job job;
        job.Index = _nextIndex++;
        job.R = RollBot();
        _jobs.push_back(job);
    }
    std::ostringstream out;
    out << count << " Bots eingereiht (" << _jobs.size() << " Auftraege in der Warteschlange, parallel " << _maxParallelJobs << ")";
    return out.str();
}

void BotPopulationMgr::ProcessJob(Job& job, uint32 diff)
{
    job.StateMs += diff;
    switch (job.State)
    {
        case JOB_NEW:
        {
            // freien Konto-Namen finden
            std::string name;
            uint32 n = job.Index;
            for (int i = 0; i < 100000; ++i, ++n)
            {
                std::ostringstream ss;
                ss << (job.Prefix.empty() ? _accountPrefix : job.Prefix) << n;
                if (AccountMgr::GetId(ss.str()) == 0)
                {
                    name = ss.str();
                    break;
                }
            }
            job.AccountName = name;
            std::string password = "Bp" + std::to_string(urand(100000, 999999)) + "x";
            if (name.empty() || !sBotMgr->CreateBotAccount(name, password, job.AccountId) || !job.AccountId)
            {
                job.Result = "Konto konnte nicht angelegt werden";
                job.State = JOB_FAILED;
                return;
            }
            job.CharName = RandomName();
            if (!sBotMgr->RequestCreateBotCharacter(job.AccountId, job.CharName, job.R.Race, job.R.Class, job.R.Sex))
            {
                job.Result = "Charakteranlage abgelehnt (Name/Aussehen)";
                job.State = JOB_FAILED;
                return;
            }
            job.State = JOB_WAIT_CHAR;
            job.StateMs = 0;
            break;
        }
        case JOB_WAIT_CHAR:
            if (job.StateMs >= 4000)
            {
                sBotMgr->RequestBotLoginExistingAccount(job.AccountId);
                job.State = JOB_WAIT_LOGIN;
                job.StateMs = 0;
            }
            break;
        case JOB_WAIT_LOGIN:
        {
            Player* p = sBotMgr->GetBotPlayer(job.AccountId);
            if (p && p->IsInWorld())
            {
                job.State = JOB_SETTLE;
                job.StateMs = 0;
            }
            else if (job.StateMs >= 25000)
            {
                if (++job.Attempts >= 3)
                {
                    job.Result = "Login nicht abgeschlossen";
                    job.State = JOB_FAILED;
                    return;
                }
                sBotMgr->RequestBotLoginExistingAccount(job.AccountId);
                job.StateMs = 0;
            }
            break;
        }
        case JOB_SETTLE:
            if (job.StateMs >= 5000)
            {
                job.State = JOB_PROVISION;
                job.StateMs = 0;
                job.Attempts = 0;
            }
            break;
        case JOB_PROVISION:
        {
            std::string summary;
            // Manche Klassen/Voelker starten hoeher (Todesritter, Daemonenjaeger, Allied Races): nie unter die Startstufe zielen
            Player* current = sBotMgr->GetBotPlayer(job.AccountId);
            uint8 const targetLevel = current ? std::max<uint8>(job.R.Level, current->getLevel()) : job.R.Level;
            bool ok = sBotMgr->ProvisionBot(job.AccountId, targetLevel, job.R.SpecId, job.R.Role, job.R.TargetIlvl, summary);
            if (ok && summary.find("Slots 0/") == std::string::npos)
            {
                job.Result = summary;
                job.State = JOB_PLACE;
                job.StateMs = 0;
            }
            else if (++job.Attempts >= 4)
            {
                job.Result = ok ? summary : ("Ausbau fehlgeschlagen: " + summary);
                job.State = ok ? JOB_PLACE : JOB_FAILED;
            }
            else
                job.StateMs = 0; // naechster Versuch im naechsten Takt
            break;
        }
        case JOB_PLACE:
        {
            std::string place;
            sBotMgr->PlaceBotByLevel(job.AccountId, place);
            job.Result += " | " + place;
            job.State = JOB_FINISH;
            job.StateMs = 0;
            break;
        }
        case JOB_FINISH:
            if (job.StateMs >= 3000)
            {
                if (!_keepOnlineAfterCreate)
                    sBotMgr->LogoutBot(job.AccountId);
                job.State = JOB_DONE;
            }
            break;
        default:
            break;
    }
}

void BotPopulationMgr::RefreshBotAccounts()
{
    _botAccountIds.clear();
    std::string prefix = _accountPrefix;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::toupper(c); });
    LoginDatabase.EscapeString(prefix);
    if (QueryResult result = LoginDatabase.PQuery("SELECT id FROM account WHERE username LIKE '%s%%' ORDER BY id", prefix.c_str()))
    {
        do
            _botAccountIds.push_back((*result)[0].GetUInt32());
        while (result->NextRow());
    }
    _accountsLoaded = true;
}

void BotPopulationMgr::UpdateManager(uint32 diff)
{
    _managerAccumMs += diff;
    if (_managerAccumMs < _updateIntervalMs)
        return;
    uint32 const elapsed = _managerAccumMs;
    _managerAccumMs = 0;

    if (!_accountsLoaded)
        RefreshBotAccounts();

    // Sollzahl alle Zufallsintervall neu wuerfeln
    _targetRerollMs -= elapsed;
    if (_targetRerollMs <= 0)
    {
        _currentTarget = _maxOnline > _minOnline ? urand(_minOnline, _maxOnline) : _minOnline;
        _targetRerollMs = int64(urand(_targetChangeMinS, _targetChangeMaxS)) * 1000;
        TC_LOG_INFO("scripts.bots", "BotPopulationMgr: neue Soll-Anzahl eingeloggter Bots: %u (Bereich %u-%u).", _currentTarget, _minOnline, _maxOnline);
    }

    std::vector<uint32> online, offline;
    for (uint32 id : _botAccountIds)
    {
        Player* p = sBotMgr->GetBotPlayer(id);
        if (p && p->IsInWorld())
            online.push_back(id);
        else if (_loginFails[id] < 3)
            offline.push_back(id);
    }
    // Konten mit laufendem Fabrik-Auftrag nicht anfassen
    auto inJob = [this](uint32 id) { for (Job const& j : _jobs) if (j.AccountId == id) return true; return false; };

    uint32 actions = 0;
    if (online.size() < _currentTarget)
    {
        while (!offline.empty() && actions < _perInterval && online.size() + actions < _currentTarget)
        {
            uint32 idx = urand(0, uint32(offline.size() - 1));
            uint32 id = offline[idx];
            offline.erase(offline.begin() + idx);
            if (inJob(id))
                continue;
            if (sBotMgr->RequestBotLoginExistingAccount(id))
                ++actions;
            else
                ++_loginFails[id];
        }
    }
    else if (online.size() > _currentTarget)
    {
        while (!online.empty() && actions < _perInterval && online.size() - actions > _currentTarget)
        {
            uint32 idx = urand(0, uint32(online.size() - 1));
            uint32 id = online[idx];
            online.erase(online.begin() + idx);
            Player* p = sBotMgr->GetBotPlayer(id);
            if (!p || p->GetGroup() || inJob(id))
                continue; // Gruppen-Bots nie ausloggen
            if (sBotMgr->LogoutBot(id))
                ++actions;
        }
    }
}

void BotPopulationMgr::Update(uint32 diff)
{
    // Fabrik: die ersten _maxParallelJobs Auftraege bearbeiten, Fertige entfernen
    uint32 active = 0;
    for (Job& job : _jobs)
    {
        if (active >= _maxParallelJobs)
            break;
        ++active;
        ProcessJob(job, diff);
    }
    for (auto itr = _jobs.begin(); itr != _jobs.end();)
    {
        if (itr->State == JOB_DONE)
        {
            ++_jobsDone;
            _accountsLoaded = false; // Kontenliste neu laden
            TC_LOG_INFO("scripts.bots", "BotPopulationMgr: Bot %s (Konto %u, Klasse %u, Volk %u) fertig: %s", itr->CharName.c_str(),
                itr->AccountId, uint32(itr->R.Class), uint32(itr->R.Race), itr->Result.c_str());
            itr = _jobs.erase(itr);
        }
        else if (itr->State == JOB_FAILED)
        {
            ++_jobsFailed;
            TC_LOG_ERROR("scripts.bots", "BotPopulationMgr: Auftrag %u (Konto %s %u) fehlgeschlagen: %s", itr->Index,
                itr->AccountName.c_str(), itr->AccountId, itr->Result.c_str());
            itr = _jobs.erase(itr);
        }
        else
            ++itr;
    }

    if (_managerEnabled)
        UpdateManager(diff);
}

std::string BotPopulationMgr::Status() const
{
    uint32 online = 0;
    for (uint32 id : _botAccountIds)
        if (Player* p = sBotMgr->GetBotPlayer(id))
            if (p->IsInWorld())
                ++online;
    std::ostringstream out;
    out << "Manager " << (_managerEnabled ? "AN" : "AUS") << ", Praefix '" << _accountPrefix << "', Konten " << _botAccountIds.size()
        << ", online " << online << ", Soll " << _currentTarget << " (Bereich " << _minOnline << "-" << _maxOnline << "), Fabrik: Warteschlange "
        << _jobs.size() << ", fertig " << _jobsDone << ", fehlgeschlagen " << _jobsFailed;
    return out.str();
}

std::string BotPopulationMgr::SelfTest(uint32 samples)
{
    samples = std::max<uint32>(10, std::min<uint32>(20000, samples));
    std::map<uint8, uint32> classes, races, roles, levels10;
    uint32 alliance = 0, specs = 0, noSpec = 0, ilvl = 0;
    for (uint32 i = 0; i < samples; ++i)
    {
        Roll r = RollBot();
        ++classes[r.Class];
        ++races[r.Race];
        {
            uint8 role = r.Role;
            if (r.SpecId)
                if (ChrSpecializationEntry const* s = sChrSpecializationStore.LookupEntry(r.SpecId))
                    role = uint8(s->Role + 1);
            ++roles[role];
        }
        ++levels10[uint8(r.Level / 10 * 10)];
        if (Player::TeamForRace(r.Race) == ALLIANCE)
            ++alliance;
        if (r.SpecId)
            ++specs;
        else
            ++noSpec;
        if (r.TargetIlvl)
            ++ilvl;
    }
    auto line = [samples](char const* title, std::map<uint8, uint32> const& m)
    {
        std::ostringstream o;
        o << title << ":";
        for (auto const& [k, v] : m)
            o << " " << uint32(k) << "=" << (v * 1000 / samples) / 10.0 << "%";
        return o.str();
    };
    std::ostringstream out;
    out << "Selbsttest " << samples << " Wuerfe: Allianz " << (alliance * 1000 / samples) / 10.0 << "% | mit Spec " << specs << ", ohne (Stufe<10) " << noSpec
        << ", mit Ziel-Ilvl " << ilvl << "\n" << line("Klassen", classes) << "\n" << line("Voelker", races) << "\n"
        << line("Rolle der Spec (0=keine Spec/unter 10, 1=Tank, 2=Heiler, 3=Schaden)", roles) << "\n" << line("Stufen-Zehner", levels10);
    return out.str();
}
