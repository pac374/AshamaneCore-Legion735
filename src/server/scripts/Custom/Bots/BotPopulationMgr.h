/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef BOT_POPULATION_MGR_H
#define BOT_POPULATION_MGR_H

// ---------------------------------------------------------------------------------------------------
// OI-051 (04.10.2026): Bot-Population - eigene Umsetzung (kein Code aus mod-playerbots uebernommen, dessen
// Lizenz AGPL-3.0 mit unserem GPL-2.0-Projekt nicht kompatibel ist; uebernommen ist nur das Konzept).
//
// 1. Bot-FABRIK in C++ (ersetzt bot_factory.ps1): legt Bot-Konten und -Charaktere nach konfigurierbaren
//    Wahrscheinlichkeitsmatrizen an (Fraktionsverhaeltnis, Klassengewichte, Volksgewichte nur ueber im Core
//    gueltige Volk/Klasse-Paare, Rollenmischung bzw. Spec-Gewichte, Stufenverteilung ueber Baender plus
//    Min-/Max-Chance), loggt sie ein, baut sie aus (BotMgr::ProvisionBot), platziert sie nach Stufe
//    (BotMgr::PlaceBotByLevel) und loggt sie wieder aus. Ablauf als Zustandsautomat im Welt-Tick, begrenzt
//    parallel, damit der Server nicht stockt.
// 2. Autonomer MANAGER: haelt eine Sollzahl eingeloggter Fabrik-Bots (zufaellig zwischen Min und Max, in
//    Intervallen neu gewuerfelt), loggt gestaffelt ein/aus (Bots pro Takt begrenzt).
// Konfiguration: worldserver.conf, Schluessel Playerbots.Pop.*, .Level.*, .ClassWeight.*, .RaceWeight.*,
// .SpecWeight.*, .RoleMix, .AllianceRatio, .Gear.* (siehe worldserver.conf.dist).
// ---------------------------------------------------------------------------------------------------

#include "Define.h"
#include <map>
#include <string>
#include <vector>

class BotPopulationMgr
{
public:
    static BotPopulationMgr* instance();

    void LoadConfig();
    // Aus BotMgr/WorldScript im Welt-Tick aufrufen (nur wenn das Bot-Modul aktiv ist).
    void Update(uint32 diff);

    // Legt `count` neue Bots an (asynchron, Zustandsautomat). Rueckgabe: kurze Zusammenfassung.
    std::string QueueFactory(uint32 count);
    // Legt `count` Bots der Stufe 110 mit fester Fraktion und Ziel-Itemlevel an (Rollen im 25er-Raster 2 Tanks / 5 Heiler / 18 Schaden),
    // Praefix getrennt vom Zufalls-Pool (z. B. "lfrbot"), damit der Manager sie nicht ein-/ausloggt. Fuer vorgebaute LFR-Gruppen.
    std::string QueueRaidPool(uint32 count, uint16 ilvl, bool alliance, std::string const& prefix, uint32 firstSlot = 0);
    // Verteilungs-Selbsttest ohne Server-Eingriff: n Zufallsbots wuerfeln und Histogramme ausgeben.
    std::string SelfTest(uint32 samples);
    std::string Status() const;
    void SetManagerEnabled(bool enabled) { _managerEnabled = enabled; }
    void SetTarget(uint32 minOnline, uint32 maxOnline) { _minOnline = minOnline; _maxOnline = maxOnline; _targetRerollMs = 0; }

    struct Roll
    {
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Sex = 0;
        uint8 Level = 1;
        uint8 Role = 0; // 0 egal, 1 Tank, 2 Heiler, 3 Schaden
        uint32 SpecId = 0;
        uint16 TargetIlvl = 0;
    };
    Roll RollBot() const;

private:
    BotPopulationMgr() = default;

    struct Band { uint8 Min, Max; uint32 Weight; };
    struct Job
    {
        uint32 Index = 0;
        uint32 AccountId = 0;
        std::string AccountName;
        std::string Prefix;
        std::string CharName;
        Roll R;
        int State = 0;
        uint32 StateMs = 0;
        uint32 Attempts = 0;
        std::string Result;
    };

    Roll RollFixed(uint8 level, uint8 role, bool alliance) const;
    void ProcessJob(Job& job, uint32 diff);
    void UpdateManager(uint32 diff);
    void RefreshBotAccounts();
    std::string RandomName() const;
    uint8 RollLevel() const;
    uint8 RollRole(uint8 classId) const;

    // Konfiguration
    bool _managerEnabled = false;
    std::string _accountPrefix = "rndbot";
    uint32 _minOnline = 0, _maxOnline = 0;
    uint32 _updateIntervalMs = 20000, _perInterval = 5;
    uint32 _targetChangeMinS = 1800, _targetChangeMaxS = 7200;
    uint32 _maxParallelJobs = 2;
    bool _keepOnlineAfterCreate = false;
    uint32 _allianceRatio = 50, _hordeRatio = 50;
    uint32 _levelMin = 1, _levelMax = 110;
    float _minLevelChance = 0.05f, _maxLevelChance = 0.10f;
    std::vector<Band> _bands;
    uint32 _classWeight[16] = { };
    uint32 _raceWeight[40] = { };
    uint32 _specWeight[16][8] = { };
    bool _specWeightsExplicit = false;
    struct RoleW { uint8 Role; uint32 W; };
    std::vector<RoleW> _roleMix;
    uint16 _targetIlvlAt110 = 0;

    // Laufzeit
    std::vector<Job> _jobs;
    uint32 _nextIndex = 1;
    uint32 _jobsDone = 0, _jobsFailed = 0;
    uint32 _managerAccumMs = 0;
    int64 _targetRerollMs = 0;
    uint32 _currentTarget = 0;
    std::vector<uint32> _botAccountIds; // alle Konten mit dem Praefix
    std::map<uint32, uint32> _loginFails;
    bool _accountsLoaded = false;
};

#define sBotPop BotPopulationMgr::instance()

#endif // BOT_POPULATION_MGR_H
