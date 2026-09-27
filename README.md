# AshamaneCore

**English version:** [README.en.md](README.en.md)

Ein privater [TrinityCore](https://www.trinitycore.org/)-Fork fuer **World of Warcraft: Legion (7.3.5, Build 26972)**.

Dieses Repository enthaelt den **C++-Quellcode**: eigene GM-Commands, SmartAI-Erweiterungen, Core-Erweiterungen
(z.B. das Invasionspunkte-OutdoorPvP-Framework) und Bugfixes auf Basis des urspruenglichen
[AshamaneCore/LegionEmulationProject](https://github.com/LegionEmulationProject/AshamaneCore)-Forks. Es ist
**kein fertig lauffaehiger Server** - dafuer fehlt bewusst ein privates SQL-Datenpaket (siehe
[Warum die SQL-Daten nicht hier liegen](#warum-die-sql-daten-nicht-hier-liegen)).

* [Ausgangsbasis & Lizenz](#ausgangsbasis--lizenz)
* [Warum die SQL-Daten nicht hier liegen](#warum-die-sql-daten-nicht-hier-liegen)
* [Was funktioniert - Detailaufschluesselung](#was-funktioniert---detailaufschluesselung)
  * [a) Vollstaendig geloest, blizzlike](#a-vollstaendig-geloest-blizzlike--korrekt)
  * [b) Funktioniert, aber eigene Interpretation](#b-funktioniert-aber-eigene-interpretation--nicht-100--blizzlike)
  * [c) Bewusst offen / ungeloest](#c-bewusst-offen--ungeloest-mit-grund)
  * [d) In aktiver Entwicklung](#d-in-aktiver-entwicklung)
* [Setup / Build](#setup--build)
* [Mitarbeit](#mitarbeit)

## Ausgangsbasis & Lizenz

- Fork-Basis: TrinityCore (GPL-2.0), ueber den Community-Fork **AshamaneCore/LegionEmulationProject**
  (Legion-7.3.5-Portierung von TrinityCore).
- Lizenz: **GPL-2.0**, siehe [`LICENSE`](LICENSE) bzw. [`COPYING`](COPYING) (identisch, `COPYING` ist die
  Datei aus dem urspruenglichen TrinityCore/AshamaneCore-Baum, `LICENSE` liegt zusaetzlich fuer GitHubs
  automatische Lizenzerkennung daneben).
- Dieser Fork ist ein **privates Weiterentwicklungsprojekt**, keine offizielle Fortsetzung des ursprünglichen
  AshamaneCore-Projekts. Ausgangs-Commit: `c250ffc` (Branch `legion`), danach ausschliesslich eigene Aenderungen.

## Warum die SQL-Daten nicht hier liegen

Ein grosser Teil der Spawn-Positionen, Kreatur-Platzierungen und Quest-Credit-Fixes in diesem Server basiert
auf der Auswertung mehrerer Quellen - u.a. offizieller TrinityCore-TDBs, Community-Addon-Datenbanken (Grail),
Wowhead-Recherche und dem **LegionCore-7.3.5-Leak**. Die Recherche in dieser letzten Quelle ist fuer den
Betrieb eines privaten Servers zulaessig, das **oeffentliche Weitergeben der daraus abgeleiteten Rohdaten**
(Koordinaten, Kreatur-Spawns etc.) waere aber ein eigenes rechtliches Risiko: die Daten selbst unterliegen
Blizzards Copyright an Spieldaten, unabhaengig davon, ob die Quelle im SQL-Kommentar genannt wird oder nicht.

Deshalb gilt fuer dieses Repository die bewusste Trennung:

| | oeffentlich (hier) | privat (nicht in diesem Repo) |
|---|---|---|
| **C++-Code** (`src/`) | ja - eigene Arbeit auf GPL-2.0-Basis | - |
| **SQL-Daten** (`sql/ashamane/world/*`, `fixes/*.sql`, `trait_candidates/*.sql`) | nein | ja, bleiben lokal |
| Datenbank-Dumps, `*.conf` mit echten Zugangsdaten, heruntergeladene Fremd-Repos | nein | nein (liegen ohnehin ausserhalb dieses Repo-Ordners, unter `C:\LegionServer\`) |

Wer diesen Code selbst betreiben moechte, braucht zusaetzlich ein **eigenes, nicht-oeffentliches SQL-Datenpaket**
(siehe [Setup / Build](#setup--build)). Ohne dieses Paket kompiliert und startet der Server, aber ein grosser
Teil der Legion-Inhalte (Class Halls, Weltquests, viele NPC-Spawns) fehlt oder ist unvollstaendig.

## Was funktioniert - Detailaufschluesselung

Diese Aufschluesselung basiert auf ueber 60 internen Arbeitsrunden ("LCF2-Runden", intern dokumentiert in
`reports/lcf2r*.md`, nicht Teil dieses Repos). Ziel: jeder, der diesen Code liest (auch der Autor selbst in
ein paar Monaten), soll auf einen Blick erkennen, was fertig ist, was eine Interpretation/Naeherung ist und
was bewusst offen liegt - und warum.

### a) Vollstaendig geloest, blizzlike / korrekt

Inhalte, die mit einer belastbaren, unabhaengig bestaetigten Quelle (Client-DB2-Daten, offizielle TrinityCore-
TDBs, mehrfach gegengepruefter Leak-Abgleich) umgesetzt und im Server verifiziert (Log-/DB-Zustandspruefung)
wurden:

- **Artefaktwaffen-Traits**: von anfangs 243 offenen Traits sind **206 mit belastbarer Quelle implementiert**
  (Client-DB2-Effektdaten, SimulationCraft-Quellcode-Abgleich, TrinityCore-Core-Semantik). Testinfrastruktur:
  eigener GM-Command `.arttest` (automatische Tests je Trait, inkl. verzoegerter/prozentbasierter Faelle).
- **Invasionspunkte (Karte 1779, "Legion Assault")**: komplette OutdoorPvP-Kampagne inkl. Core-Erweiterung
  (`OutdoorPvP`/`Scenario`/`ScenarioMgr` um zonen-parallele Szenarien erweitert, additiv, ohne bestehende Zonen
  zu veraendern), alle 6 Invasionszonen mit Wellenspawns, Game-Events und Szenario-Anbindung live.
- **Szenario 39746 "A Ring Unbroken"** (Karte 1572): vollstaendig spielbar, inkl. eines zuvor unbekannten
  strukturellen Zugangsblockers (ein `PlayerChoice`-Effekt wurde faelschlich als reiner Teleport angenommen -
  echte Ursache war ein fehlendes Teleport-Ziel, das aus der Quest-Choice-Logik gefixt wurde).
- **Server-Side Spells**: `serverside_spell`/`serverside_spell_effect`-Tabellen + `SpellMgr::LoadSpellInfoServerside()`
  aus einem TrinityCore-Upstream-Commit portiert (angepasst an unsere abweichende `SpellInfo`-Struktur).
- **NPC-Positionsverifikation**: mehrstufige automatische Pruefung (`.npcverify`, Terrain-Hoehenabgleich per
  eigenem `HeightSampler`-Tool) fuer hunderte abgeleitete Spawns, inkl. Korrektur von Ausreissern.
- **Diverse Quest-Credit-Ketten** (SmartAI-Portierungen mit klar nachvollziehbarer 1:1-Quelle), u.a. Anduin-
  Eskortquest, mehrere Class-Hall-Fragmente, Didi/Thalyssra/Farodin/Brann/Silgryn-NPC-Ketten.
- **Dargrul** (Karte 1458 "Neltharion's Lair") und mehrere Dungeon-Boden-NPCs (z.B. Rattlegore, Darkmaster
  Gandling in Scholomance): Map-Difficulty-Inkompatibilitaeten identifiziert und korrekt aufgeloest.

### b) Funktioniert, aber eigene Interpretation / nicht 100% blizzlike

Inhalte, die spielbar sind, aber auf einer Naeherung, einem Workaround oder einer plausiblen-aber-unbewiesenen
Annahme beruhen, weil die echte Blizzard-Logik nicht auffindbar war. Jeder Punkt mit kurzer Begruendung, warum
es eine Interpretation ist:

- **Naeherungs-Koordinaten aus Grail-/QuestPOI-Ableitung**: ein Teil der NPC-Spawns wurde nicht aus einem
  Sniff, sondern aus Community-Addon-Daten (Grail-NPC-Datenbank, Weltkarten-Prozentkoordinaten) und
  QuestPOI-Ableitung zurueckgerechnet, inkl. Terrain-Hoehe per eigenem Sampler-Tool. Validiert auf ca. 71-78%
  XY/Z-Genauigkeit gegen bekannte Referenzpunkte - **nicht** exakte Blizzard-Koordinaten.
- **Argentturnier-Teleport**: eine Teleport-Faehigkeit fuehrt zum naechstgelegenen Friedhof (Graveyard) statt
  zum exakten "Grounds"-Zielpunkt, weil das echte Zielkoordinatenpaar in keiner geprueften Quelle vorlag.
- **Underlight-Angler-Bonusfaenge ("X Angling"-Traits, 6 Stueck) und "Better Luck Next Time"**: die Client-
  Daten belegen nur eine reine Dummy-Aura ohne Zahlenwerte - Chance/Prozentsaetze (10%/Rang auf Bonusfische,
  5% auf ein Item) sind eine **dokumentierte Annahme**, keine belegte Blizzard-Zahl.
- **PlayerChoice-Workarounds**: mehrere Quest-Entscheidungsmomente (z.B. Szenario 39746) nutzen
  `PlayerScript::OnPlayerChoiceResponse`-Hooks statt einer nativen Client-Choice-Antwortlogik, weil unser
  Core PlayerChoice-Belohnungen ohne hinterlegte Spell-ID nicht automatisch verarbeitet.
- **Netherlight-Crucible-Interaktion laeuft ueber ein Gossip-Menue statt das echte Client-UI-Fenster**
  (Relikt-Schmiede). Das Netzwerkprotokoll fuer das native Fenster ist nur aus Leak-Quellen bekannt und wurde
  nicht unabhaengig bestaetigt - der C++-Code fuer die native Variante existiert bereits
  (`NetherlightCrucible.cpp/.h`, Config-Schalter `NetherlightCrucible.ClientUI`, Default aus), ist aber
  ungetestet und deaktiviert. Siehe Kommentar in `src/server/worldserver/worldserver.conf.dist`.
- **Einige Trait-Werte/Timings (Doom Wolves, Echoing Stars)** beruhen auf SimulationCraft-Community-Werten statt
  einer bestaetigten Blizzard-Quelle (SimC selbst markiert einen dieser Werte im eigenen Quellcode als
  vorlaeufigen Platzhalter).
- **Bestehende generische Platzhalter-Templates** (rund 2.200 NPC-Templates mit generischen Werten wie Level 1
  oder neutraler Fraktion) wurden dort belassen, wo auch die Vergleichsquelle nur denselben Platzhalter kennt -
  keine erfundenen, aber auch keine bestaetigten Werte.

### c) Bewusst offen / ungeloest, mit Grund

Diese Punkte sind **keine Bugs im herkoemmlichen Sinn**, sondern Stellen, an denen nach gruendlicher, mehrfach
wiederholter Recherche (oft ueber 5-7 unabhaengige Quellen) schlicht keine belastbare Quelle gefunden wurde.
Absichtlich so formuliert, dass ein Mitwirkender sofort weiss, wo er ansetzen kann:

- **Weltquest-Rotationssystem (Duration-Blocker)**: fuer 1.092 von 1.133 Weltquests konnte die Variable
  `world_quest.variable` aus Client-DB2-Bytecode rekonstruiert werden (100% Trefferquote gegen bekannte
  Referenzwerte) - die zweite noetige Spalte `duration` hat aber **in keiner gepruften Quelle** (Client-DB2,
  Leak-DB, eigene Tabellen) eine Entsprechung. Ein Teil-Deploy mit geratenem Wert wurde bewusst nicht gemacht.
  **Ansatzpunkt fuer Mitwirkende:** `duration` ueber 1:1-Geschwisterquests (identische Zone/Titel/QuestInfoID)
  ableiten, nicht raten.
- **38 von 42 Dungeon-Etagen-NPC-Kandidaten** ohne Spawn: das Z-Hoehen-Problem fuer Dungeon-Innenraeume ist
  ungeloest (das vorhandene `HeightSampler`-Tool berechnet nur Open-World-Terrain, kein WMO-Kollisionsmodell
  fuer Dungeoninnenraeume; das zweite Tool `VMapHeightSampler` stuerzt zuverlaessig ab).
- **301 verbliebene "Wowhead ohne Koordinaten"-IDs** (von urspruenglich 465-306): grosse Mehrheit ist
  konzeptionell gar nicht spawnbar (Kill-Credit-Trigger, unsichtbare "Bunnies", interne Marker, Szenario-Proxies
  ohne festen Ort) - kein Rateweg, aber auch keine abschliessende Einzelpruefung aller verbliebenen IDs.
- **539 von urspruenglich rund 562 Quest-Credits ohne auffindbare Quelle** (Stand nach diversen Runden: 25
  geloest / 537 weiterhin unloesbar): betroffene NPCs/Objekte existieren in keiner der sieben gepruften
  Quellen (eigene DB, LegionCore-Leak, offizielle TDBs, SkyFire, Draenor-Core-Fork, ADB-Dump, C++-Volltext) mit
  einem Spawn oder Beschwoerungsmechanismus. Drei prominente Faelle (Akama/Farondis/Reshad) sind nach
  wiederholter Tiefenpruefung als **quellenerschoepft** dokumentiert.
- **Skylord Tovra (Kreatur-ID 80005)**: 0 Treffer in allen gepruften Kreatur-Tabellen und UI-Map-Zuordnungen -
  endgueltig als unloesbar dokumentiert, keine weitere Pruefung noetig ausser bei neuer Leak-/Sniff-Quelle.
- **677 Teleport-Zielsprueche**: 141 der potenziell erreichbaren Ziele identifiziert, davon 106 bereits mit
  Zielposition, 35 fehlten - nur 6 mit vertretbarer Sicherheit deployt (Rest: genuine Datenluecke oder nur mit
  Raten schliessbar, u.a. wegen mehrdeutiger Namensfallen wie "Bloodmaul Slag Mines" vs. "Bloodmaul Slave Mines").
- **Objectives 108787/108788/108789** ("Armor Polish"/"Weapon Enchantment"/"Food Kill Credit", betrifft sowohl
  eine Allianz- als auch die analoge Horde-Quest): in keiner der sechs gepruften Quellen existiert ein Spawn
  oder Kredit-Mechanismus - formal als quellenerschoepft geschlossen.

### d) In aktiver Entwicklung

Code unter `src/server/scripts/Custom/Bots/` (`BotMgr.h/.cpp`, `BotCharacter.h/.cpp`, `bot_scriptloader.cpp`,
`bot_commandscript.cpp`) plus drei kleine, additive Aenderungen in `src/server/game/Server/WorldSession.h/.cpp`.
Ziel: ein eigenes Playerbots-Modul nach Vorbild von AzerothCores `mod-playerbots` - bot-gesteuerte
Spielercharaktere ("Session-Faking": ein echtes `Player`-Objekt auf einer socketlosen `WorldSession`, kein
`Creature`-basierter NPC-Bot). Anders als die Punkte unter (c) ist das hier **kein Blocker und keine
Recherchefrage**, sondern ein laufendes Bauprojekt mit einem funktionierenden, aber bewusst unvollstaendigen
Zwischenstand ("Phase 1") - deshalb ein eigener Abschnitt statt (b) oder (c).

**Was funktioniert (live am Produktivserver mehrfach bestaetigt, ueber GM-Testbefehle `.bottest ...`):**

- **Login**: ein Bot-Account/-Charakter kann sich ohne echten WoW-Client einloggen (`.bottest login`) - echtes
  `Player`-Objekt, echte Welt-Position, kein Fake-NPC.
- **Logout**: sauberes Ausloggen waehrend laufendem Betrieb (`.bottest logout`), Online-Flags in `auth.account`
  und `characters.characters` werden korrekt zurueckgesetzt.
- **Shutdown**: ein kontrollierter Server-Shutdown mit eingeloggtem, aktivem Bot laeuft ab, ohne den Bot-Zustand
  inkonsistent zu hinterlassen oder den Server zum Absturz zu bringen.
- **Einzelbewegung**: der Bot kann sich geradlinig bewegen (`.bottest move`, `MotionMaster::MovePoint()` ohne
  Pfadfindung).
- **Pendeln**: mehrere Bewegungszyklen zwischen zwei Punkten hintereinander (`.bottest patrol <n>`), inklusive
  Ankunftserkennung und sauberem Zyklusabschluss.
- **Pfadfindung**: Bewegung ueber das echte Navmesh (`.bottest movepath`, Recast/Detour-Pfadberechnung statt
  gerader Linie).

Alle sechs Mechanismen wurden einzeln UND gemeinsam in mehreren vollstaendigen Regressionsdurchlaeufen unter
der aktuellen Produktionsbinary (ohne Diagnose-Sonderflags) reproduzierbar bestaetigt - keine Simulation, echte
Live-Tests gegen den laufenden Server.

**Was (noch) nicht funktioniert / nicht existiert:**

- **Kein echtes Kampfverhalten**: kein Autoattack, kein Spell-Cast, kein Threat-Management, kein Umgang mit
  Tod/Ressurrection - bisher ungetesteter Risikobereich.
- **Kein Zustandsautomat/autonomes Verhalten**: der Bot tut nichts von sich aus. Jede Aktion (Login, Bewegung,
  Logout) muss einzeln per GM-Befehl ausgeloest werden - kein selbststaendiger Lebenszyklus ("loggt sich ein,
  patrouilliert eine Weile, loggt sich wieder aus").
- **Keine Gruppen-/KI-Logik**: kein Folgen, keine Rollenzuweisung (Tank/Heal/DD), keine Entscheidungslogik jeder
  Art - reine Test-Infrastruktur, kein Bot-Verhalten im eigentlichen Sinn.
- Die Test-Infrastruktur selbst (GM-Befehl `.bottest createaccount|createchar|login|logout|move|movepath|patrol|status`)
  ist bewusst manuell/schrittweise gehalten, nicht fuer den produktiven Einsatz gedacht.

**Drei unabhaengige, strukturelle Absturzursachen gefunden und behoben** (nicht nur symptomatisch umschifft -
interessant fuer andere TrinityCore-Entwickler, die aehnliches versuchen):

1. **Nullpointer bei Socket-Idle-Check**: `WorldSession::Update()` ruft bei abgelaufenem Idle-Timeout
   unbedingt `m_Socket[CONNECTION_TYPE_REALM]->CloseSocket()` auf - fuer eine Session ohne echten Socket
   (`socket == nullptr`, wie sie ein socketloser Bot zwangslaeufig hat) ist das ein garantierter
   Nullpointer-Zugriff, der den gesamten `worldserver`-Prozess abstuerzen laesst, sobald diese Session
   ueberhaupt einmal durch den regulaeren Update-Zyklus laeuft. Fix: ein `IsBotSession()`-Guard uebergeht den
   Idle-Kick fuer Bot-Sessions vollstaendig (rein additiv, kein Verhaltensunterschied fuer echte Spieler).
2. **DB-Zugriff im `WorldSession`-Destruktor nach bereits geschlossenem DB-Pool (Fund 1)**: laeuft der
   Destruktor einer noch eingeloggten Bot-Session sehr spaet (z.B. im `atexit`-Zeitfenster eines
   Singleton-Managers, NACH `main()`s Rueckkehr), sind die DB-Pools (`CharacterDatabase` u.a.) bereits ueber
   `StopDB()` geschlossen und ihr interner `boost::asio`-Executor bereits zerstoert - ein darin ausgeloester
   `LogoutPlayer()->SaveToDB()`-Aufruf dereferenziert dann einen bereits auf `nullptr` zurueckgesetzten
   Executor-Zeiger. Fix: der eigentliche, saubere Logout (inkl. DB-Schreibzugriff) wird VORHER, synchron
   innerhalb von `main()` und garantiert vor jedem DB-Pool-Teardown ausgefuehrt (`WorldScript::OnShutdown()`
   -> `BotMgr::LogoutAllBots()`); der Destruktor selbst macht fuer Bot-Sessions ab diesem Fix grundsaetzlich
   keinen DB-Zugriff mehr.
3. **DB-Zugriff im `WorldSession`-Destruktor nach bereits geschlossenem DB-Pool (Fund 2, unabhaengig)**: derselbe
   Destruktor enthielt eine zweite, von der ersten unabhaengige unbedingte Datenbankschreiboperation
   (`LoginDatabase.PExecute("UPDATE account SET online = 0 ...")`), die fuer JEDE Session lief, unabhaengig vom
   Spielerzustand - im selben spaeten Zeitfenster ebenfalls ein Zugriff auf einen bereits geschlossenen
   DB-Pool. Fix nach demselben Muster: dieser Online-Flag-Reset erfolgt jetzt ebenfalls vorher, waehrend der
   `LoginDatabase`-Pool noch garantiert lebt; der Destruktor bleibt fuer Bot-Sessions vollstaendig DB-frei.

## Setup / Build

Dies ist ein reiner **Code-Build** - ohne das private SQL-Datenpaket (siehe oben) fehlen grosse Teile der
Legion-Inhalte.

1. **Build-Voraussetzungen**: wie bei TrinityCore ueblich - CMake, ein C++17-faehiger Compiler (getestet mit
   MSVC/Visual Studio unter Windows), Boost, OpenSSL, MySQL/MariaDB-Client-Bibliotheken. Details siehe die
   TrinityCore-Wiki-Verweise unten (dieser Fork weicht hier nicht wesentlich vom Standard ab).
2. **Datenbanken anlegen**: `auth`, `characters`, `world`, `hotfixes` (Standard-TrinityCore-Schema).
3. **Server-seitige Konfiguration**: `worldserver.conf.dist` / `bnetserver.conf.dist` nach `worldserver.conf` /
   `bnetserver.conf` kopieren und eigene Zugangsdaten eintragen (diese Dateien sind bewusst per `.gitignore`
   von der Versionierung ausgeschlossen).
4. **Welt-Datenbank befuellen**: hierfuer wird zusaetzlich zur offiziellen TrinityCore-Basis-DB das
   **eigene, nicht in diesem Repo enthaltene SQL-Datenpaket** benoetigt (`sql/ashamane/world/*`). Ohne dieses
   Paket startet der Server, aber mit deutlich weniger Legion-Inhalten (siehe Abschnitt oben). Wer eigene
   Daten aufbauen moechte: der Updater liest automatisch aus `sql/ashamane/world` (nicht aus dem TrinityCore-
   Standardpfad `sql/updates/world/master`).
5. **Client**: WoW 7.3.5, Build 26972.

Ausfuehrliche, allgemeine TrinityCore-Installationsanleitungen (die fuer diesen Fork grundsaetzlich gelten):
[TrinityCore-Wiki: Requirements](https://www.trinitycore.info/display/tc/Requirements) und
[Installation Guide](https://www.trinitycore.info/display/tc/Installation+Guide).

## Mitarbeit

C++-Fixes gerne als Pull Request. Fuer Kategorie (c) oben (bewusst offene Punkte) lohnt sich ein Blick in die
GitHub-Issues dieses Repos, falls dort passende Themen als Issue angelegt sind - dort steht jeweils der genaue
Rechercheansatz, der bereits ausprobiert (und fuer die aktuelle Quellenlage abgeschlossen) wurde, damit keine
Arbeit doppelt gemacht wird.

## Copyright

Lizenz: GPL-2.0. Siehe [`LICENSE`](LICENSE) / [`COPYING`](COPYING).

## Links

* [TrinityCore](https://www.trinitycore.org/)
* [Urspruenglicher Fork: LegionEmulationProject/AshamaneCore](https://github.com/LegionEmulationProject/AshamaneCore)
