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
  * [e) Roadmap](#e-roadmap---naechste-bausteine-inspiriert-von-aber-neu-gebaut-gegenueber-3.3.5-community-modulen)
* [Setup / Build](#setup--build)
* [Befehlsuebersicht](README-Commands.md) - alle "."-Befehle der Custom-Module (Playerbots, Dungeon-Clear, AhBot, OllamaChat)
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

Fuer den Eigenbedarf (Geraetewechsel, Ausfallsicherheit) liegen vollstaendige Datenbank-Dumps (`world`,
`characters`, `hotfixes`, `auth`) zusaetzlich lokal sowie in einem **privaten** GitHub-Repository - bewusst
privat, aus genau demselben Copyright-Grund wie oben.

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
  ungetestet und deaktiviert. Siehe Kommentar in `src/server/worldserver/worldserver.conf.dist`. Alle
  guenstigen Recherchewege (Code-/Websuche, offizielle Lua-API, bekannte Leak-Forks - allesamt Kopien derselben
  unbestaetigten Quelle) sind ausgeschoepft; eine Client-Binary-Analyse wuerde vermutlich neue Erkenntnisse
  liefern, ist aber bewusst zurueckgestellt.
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

Code unter `src/server/scripts/Custom/Bots/` (`BotMgr.h/.cpp`, `bot_commandscript.cpp`) plus kleine, additive
Aenderungen in `src/server/game/Server/WorldSession.h/.cpp`. Ziel: ein eigenes Playerbots-Modul nach Vorbild von
AzerothCores `mod-playerbots` - bot-gesteuerte Spielercharaktere ("Session-Faking": ein echtes `Player`-Objekt
auf einer socketlosen `WorldSession`, kein `Creature`-basierter NPC-Bot), die kaempfen, sich ausruesten, der
eigenen Gruppe/LFR beitreten und questen sollen. Anders als die Punkte unter (c) ist das hier **kein Blocker und
keine Recherchefrage**, sondern ein laufendes Bauprojekt mit einem funktionierenden, aber bewusst unvollstaendigen
Zwischenstand - deshalb ein eigener Abschnitt statt (b) oder (c).

**Was funktioniert (live am Produktivserver mehrfach bestaetigt, ueber GM-Testbefehle `.bottest ...`):**

- **Login/Logout/Shutdown**: Bot-Account/-Charakter loggt ohne echten Client ein/aus (`.bottest login|logout`),
  Online-Flags korrekt, kein Absturz bei kontrolliertem Server-Shutdown mit aktivem Bot.
- **Bewegung**: geradlinig (`.bottest move`), Pendeln (`.bottest patrol <n>`), echtes Navmesh-Pathfinding
  (`.bottest movepath`, Recast/Detour).
- **Kampf**: ein Bot kann ein Ziel angreifen und toeten (`Unit::ApplySpellImmune`-basierter Invuln-Testmodus
  `.bottest invuln`, damit unbewaffnete Test-Charaktere nicht am Gegenschlag sterben). Bekannte Einschraenkung:
  bewegliche/Kreaturen-Ziele (`type=8`) werden derzeit noch verloren, weil `StartBotAttack()` kein
  `MotionMaster::MoveChase()` nachfuehrt (offener Punkt, dokumentiert statt verschwiegen).
- **Tod/Wiederbelebung**: vollstaendiger Zyklus (`HandleBotDeath()`/`ReviveBotAtGraveyard()`, `.bottest
  release|revive`), inkl. korrektem Friedhofs-Teleport.
- **Ausruestung**: Bots tragen ab Erstellung levelgerechte, zufaellig aus einem Qualitaets-Pool gewuerfelte
  Ausruestung (`EquipBotItem()`/`EquipBotFromPool()`, `.bottest equip|equipfrompool`). Der Pool
  (`world.bot_equipment_pool`, 58.705 Eintraege ueber 6 Levelbaender x 4 Qualitaetsstufen, aus den lokalen
  Client-DB2-Daten generiert) und die pro Bot einmalig und dauerhaft gewuerfelte Qualitaetsstufe
  (`characters.bot_gear_tier`, pyramidenfoermige Verteilung: die meisten Bots eher schlecht/mittel ausgeruestet,
  wenige episch) sind bewusst so gewaehlt, dass kein aktives Loot-/Spielverhalten noetig ist - Bots sind reine
  Gruppen-/LFR-Fuellung, kein eigenstaendiges Progressionsziel.
- **Gruppe/LFR (Stufe 1)**: ein Bot kann der Gruppe des Spielers beitreten/sie verlassen
  (`Group::AddMember()` direkt aufgerufen, `.bottest groupinvite|groupleave`) und ihr per echtem
  `MotionMaster::MoveFollow()` folgen (`.bottest follow|followstop`), mehrfach ueber laengere Zeitraeume
  verifiziert.

**Zwischenzeitlich ergaenzt (seit dem letzten Stand dieses Abschnitts):**

- **LFG-Pool-Matchmaking**: Bots stehen jetzt solo im regulaeren `LFGMgr`-Warteschlangensystem und werden per
  Lazy-Nachfuell-Trigger passend zu wartenden echten Spielern eingereiht (`BotMgr::TriggerLfgPoolFillOnce()`),
  kein separater Bot-Direktpfad.
- **Kampf-KI**: alle 36 Legion-Spezialisierungen haben eine datengetriebene Rotationstabelle
  (`g_BotSpecRotations`, `BotMgr::ProcessBotCombatAI()`) - Faehigkeiten-Namen/-Reihenfolgen sind Patch-7.3.5-
  recherchiert, numerische Spell-IDs werden NIE hartkodiert, sondern zur Laufzeit gegen das auf diesem Server
  geladene `Spell.db2` aufgeloest (siehe Kommentar bei `BotMgr::ResolveSpellIdByName()`).
- **Quest-KI, Teil 1**: Annahme/Abgabe ueber direkte `Player`-Methodenaufrufe (`BotAcceptQuest()`/
  `BotTurnInQuest()`), Toetungs-Kill-Credit laeuft automatisch ueber die normale Core-Logik mit.
- **Teil-Loesung fuer "kein autonomer Zustandsautomat"**: ein Bot in einer Gruppe engagiert jetzt automatisch
  dasselbe Kampfziel wie ein bereits kaempfendes Gruppenmitglied (`SelectBotCombatTarget()`).
- **Aktive Debug-Werkzeuge** (Runde 6, Antwort auf "suche aktiv nach Fehlern/fehlenden Werten"):
  `.bottest findnpc <namePart>` findet die DB-Spawn-Id/Position eines NPCs anhand seines Namens (loest das
  wiederkehrende "ich kenne die Spawn-Id nicht"-Problem bei `attack`/`loot`/`questaccept`/`questturnin` ohne
  manuelle SQL-Abfrage); `.bottest diagspells` erzwingt die Aufloesung ALLER 36 Kampf-KI-Rotationen auf
  einen Schlag und meldet jeden gegen das aktuelle `Spell.db2` nicht aufloesbaren Faehigkeits-/Aura-/
  Interrupt-/Dispel-Namen. Siehe [README-Commands.md](README-Commands.md) fuer die vollstaendige
  Befehlsuebersicht aller Custom-Module.

**Was (noch) nicht existiert:**

- **Voll autonomer Zustandsautomat**: die obige Mit-Kampf-Automatik deckt nur das Ziel-Engagement ab - eigene
  Entscheidungsfindung ("was tue ich als naechstes ohne GM-Befehl") fehlt noch weitgehend ausserhalb von Kampf
  und LFG. Siehe Abschnitt e) fuer den geplanten naechsten Schritt (autonomer Dungeon-Clear-Modus).
- **Artefaktwaffen fuer Bots** (Zuweisung + levelgerechtes Skillen) ist in Arbeit.
- **Quest-KI, Teil 2** (autonome Quest-Auswahl + Mehr-Zonen-Navigation zum Questgeber/-ziel) ist noch nicht
  begonnen.
- Das Loot-System fuer Bots wurde **bewusst nicht gebaut** (Entscheidung): Bots erhalten ihre Ausruestung
  ausschliesslich ueber den Equipment-Pool, aktives Looten waere fuer reine Gruppen-/LFR-Fuellbots unnoetiger
  Aufwand ohne Nutzen. (Ausnahme: der neue Dungeon-Clear-Modus in Abschnitt e) loest nach jedem Kill automatisch
  `BotLootTarget()` aus - das ist weiterhin kein "Spieler entscheidet, was er behaelt"-Loot-System, sondern reine
  Bewegungsfreigabe fuer den naechsten Kampf.)

**Vier unabhaengige, strukturelle Fehlerursachen gefunden und behoben** (nicht nur symptomatisch umschifft -
interessant fuer andere TrinityCore-Entwickler, die Aehnliches versuchen):

1. **`groups` ist seit MySQL 8.0.2 ein reserviertes Schluesselwort**: `GroupMgr::LoadGroups()` referenzierte die
   Tabelle an zwei Stellen unquotiert (`SELECT guid FROM groups`), was beim erstmaligen Vorhandensein echter
   Gruppendaten (ausgeloest durch das neue Gruppen-Beitritts-Feature) zu einem reproduzierbaren Absturz beim
   Serverstart fuehrte. Fix: beide Stellen mit Backticks quotiert (`` FROM `groups` ``) - alle anderen Stellen
   in derselben Datei waren bereits korrekt quotiert, es handelte sich um zwei isolierte Zeilen.

2. **Nullpointer bei Socket-Idle-Check**: `WorldSession::Update()` ruft bei abgelaufenem Idle-Timeout
   unbedingt `m_Socket[CONNECTION_TYPE_REALM]->CloseSocket()` auf - fuer eine Session ohne echten Socket
   (`socket == nullptr`, wie sie ein socketloser Bot zwangslaeufig hat) ist das ein garantierter
   Nullpointer-Zugriff, der den gesamten `worldserver`-Prozess abstuerzen laesst, sobald diese Session
   ueberhaupt einmal durch den regulaeren Update-Zyklus laeuft. Fix: ein `IsBotSession()`-Guard uebergeht den
   Idle-Kick fuer Bot-Sessions vollstaendig (rein additiv, kein Verhaltensunterschied fuer echte Spieler).
3. **DB-Zugriff im `WorldSession`-Destruktor nach bereits geschlossenem DB-Pool (Fund 1)**: laeuft der
   Destruktor einer noch eingeloggten Bot-Session sehr spaet (z.B. im `atexit`-Zeitfenster eines
   Singleton-Managers, NACH `main()`s Rueckkehr), sind die DB-Pools (`CharacterDatabase` u.a.) bereits ueber
   `StopDB()` geschlossen und ihr interner `boost::asio`-Executor bereits zerstoert - ein darin ausgeloester
   `LogoutPlayer()->SaveToDB()`-Aufruf dereferenziert dann einen bereits auf `nullptr` zurueckgesetzten
   Executor-Zeiger. Fix: der eigentliche, saubere Logout (inkl. DB-Schreibzugriff) wird VORHER, synchron
   innerhalb von `main()` und garantiert vor jedem DB-Pool-Teardown ausgefuehrt (`WorldScript::OnShutdown()`
   -> `BotMgr::LogoutAllBots()`); der Destruktor selbst macht fuer Bot-Sessions ab diesem Fix grundsaetzlich
   keinen DB-Zugriff mehr.
4. **DB-Zugriff im `WorldSession`-Destruktor nach bereits geschlossenem DB-Pool (Fund 2, unabhaengig)**: derselbe
   Destruktor enthielt eine zweite, von der ersten unabhaengige unbedingte Datenbankschreiboperation
   (`LoginDatabase.PExecute("UPDATE account SET online = 0 ...")`), die fuer JEDE Session lief, unabhaengig vom
   Spielerzustand - im selben spaeten Zeitfenster ebenfalls ein Zugriff auf einen bereits geschlossenen
   DB-Pool. Fix nach demselben Muster: dieser Online-Flag-Reset erfolgt jetzt ebenfalls vorher, waehrend der
   `LoginDatabase`-Pool noch garantiert lebt; der Destruktor bleibt fuer Bot-Sessions vollstaendig DB-frei.

**Runde 6: ein Modul-Kollisions-Fund bei der modulweiten Durchsicht** (kein Core-Fund wie oben, sondern
zwei eigene Custom-Module, die sich gegenseitig ins Gehege kamen): `PlayerScript::OnChat()` (Whisper-
Ueberladung) wird fuer JEDE Whisper aufgerufen, ungeachtet ihrer `lang` - sowohl fuer normale sichtbare
Spieler-Whispers ALS AUCH fuer die neue Addon-Steuernachricht des Dungeon-Clear-Moduls
(`lang==LANG_ADDON`, siehe Abschnitt e)). Der Ollama-Chat-Hook (`ollamachat_scriptloader.cpp`) filterte
urspruenglich nicht nach `lang` - eine an einen Bot gerichtete Addon-Nachricht wie `"ASHDC:CMD:ON"` haette
dadurch versehentlich AUCH die Ollama-LLM ausgeloest und eine sinnlose In-Charakter-Antwort auf den
Steuertext erzeugt. Fix: der Ollama-Hook ignoriert jetzt explizit `lang==LANG_ADDON` (siehe README-
Commands.md, Abschnitt "Modul OllamaChat", fuer die dokumentierte Abgrenzung). Gefunden durch eine
systematische Durchsicht aller `PlayerScript`-Hook-Ueberladungen ueber alle Custom-Module hinweg, nicht
durch einen Livetest - noch nicht gegen einen laufenden Server verifiziert (siehe Test-Checkliste im PR).

### e) Roadmap - naechste Bausteine (inspiriert von, aber NEU gebaut gegenueber 3.3.5-Community-Modulen)

Drei Community-Module aus dem WotLK-3.3.5-Oekosystem (AzerothCore) dienen als **Ideen-/Zielreferenz** fuer die
naechsten Playerbots-Ausbaustufen - **nicht** als Code-Quelle: alle drei stehen unter AGPL-3.0 (netzwerk-
copyleft, staerker als unser GPL-2.0), ausserdem ist die AzerothCore-3.3.5-API (andere Core-Version, andere
Klassen-/Spell-/Instanz-Datenlage) technisch inkompatibel mit diesem TrinityCore-Legion-7.3.5-Fork. Uebernommen
wird ausschliesslich das **Feature-Konzept** (was soll das Modul koennen), die Implementierung ist in jedem Fall
eine eigenstaendige Neuentwicklung gegen unsere eigenen Core-APIs:

- **[mod-dungeon-clear](https://github.com/jrad7/mod-dungeon-clear)** (Referenz fuer: autonomer Dungeon-Clear-
  Modus) - **umgesetzt** (`BotMgr::SetDungeonClearMode()`/`ProcessDungeonClear()`). Kernidee, die
  uebernommen wird: Routen werden **live aus dem Navmesh generiert, keine
  handgepflegten Wegpunkte pro Dungeon** - das passt direkt zu unserer bereits bestaetigten
  `MotionMaster::MovePoint(generatePath=true)`-Navmesh-Bewegung (Runde U/`MoveBotTestStepPath()`). Unsere
  Variante navigiert autonom zum naechsten lebenden Dungeon-Boss auf der aktuellen Karte (ueber
  `Creature::IsDungeonBoss()` - dynamisch aus der `instance_encounters`-Tabelle gesetztes `flags_extra`-Bit,
  zuverlaessiger als `CreatureTemplate::rank`, keine Dungeon-spezifischen Daten unsererseits noetig), engagiert
  Trash automatisch ueber die
  bereits bestehende `SelectBotCombatTarget()`-Mit-Kampf-Logik und loest nach jedem Kill automatisch
  `BotLootTarget()` aus. **Zusaetzlich umgesetzt** (Runde 6, `BotMgr::ProcessBotMechanicReactions()`): eine
  GENERISCHE (nicht Boss-spezifische) Mechanik-Reaktionsebene, die JEDEM Encounter gemeinsam ist - gefaehrliche
  Bodeneffekte werden verlassen (`DynamicObject::GetSpellInfo()->IsPositive()==false` + Bot steht innerhalb
  `GetRadius()`), Skillungen mit Interrupt (siehe `g_BotSpecRotations`) unterbrechen automatisch castende
  Gegner, Skillungen mit Dispel entfernen automatisch entfernbare Debuffs (beides preemptiert die normale
  Rotation fuer den aktuellen Tick). Weiterhin bewusst NICHT uebernommen (braucht eine Boss-genaue
  Wissensbasis pro Encounter, die fuer Legion-Dungeons nicht recherchiert wurde): Boss-spezifisches
  Ausweich-Positionswissen, Soak-Mechaniken, Pull-Stile (Leeroy/Advanced/Dynamic), Encounter-Skripte (Hebel/
  Altare/Eskorten), Heiler-Positionierung, Tod-Wiederbelebungs-Choreographie.
- **[mod-ah-bot-plus](https://github.com/NathanHandley/mod-ah-bot-plus)** (Referenz fuer: Auktionshaus-Bot) -
  **Recherche-Ergebnis: braucht keine Neuentwicklung.** Dieser TrinityCore-Fork bringt unter
  `src/server/game/AuctionHouseBot/` (`AuctionHouseBot.*`, `AuctionHouseBotSeller.*`,
  `AuctionHouseBotBuyer.*`, GM-Befehle in `src/server/scripts/Commands/cs_ahbot.cpp`) bereits ein
  vollstaendiges, natives Seller-/Buyer-Auktionshaus-Bot-System mit - GPL-2.0 (TrinityCore-eigener Code,
  nicht das AGPL-Referenzmodul), funktional gleichwertig zum Kernkonzept von mod-ah-bot-plus (config-
  getriebene Preisbildung nach Kategorie/Qualitaet/Itemlevel, periodisches Listen/Kaufen ueber
  echte-aber-nie-eingeloggte Bot-Account-Charaktere, GM-Befehle `.ahbot reload/empty/update`). Aktuell
  **deaktiviert** (`AuctionHouseBot.Seller.Enabled = 0` in `worldserver.conf.dist`, Zeile ~3304) - der
  naechste Schritt ist reine Konfiguration/Inbetriebnahme (Bot-Account mit ein paar nie einzuloggenden
  Charakteren anlegen, `AuctionHouseBot.Account`/`.Seller.Enabled`/`.Buyer.*.Enabled` setzen), kein
  C++-Code noetig. Die "Plus"-Verbesserungen der Referenz (non-SQL-Kategorie-Konfiguration, erweiterte
  Preisformel-Tabelle, Mehrfach-Bot-Namen) waeren ein separates, kleineres Ausbauprojekt AUF dem bereits
  vorhandenen nativen System, keine Neuentwicklung von Grund auf.
- **[mod-ollama-chat](https://github.com/DustinHendrickson/mod-ollama-chat)** (Referenz fuer: LLM-gestuetzter
  Bot-Chat) - **umgesetzt** unter `src/server/scripts/Custom/OllamaChat/`. Kernidee: whispert ein echter
  Spieler einen Bot an, generiert der Bot seine Antwort ueber eine lokale Ollama-HTTP-API statt gar nicht/
  zufaellig zu antworten. Bewusst KEINE Drittbibliothek vendored (das Referenzmodul nutzt cpp-httplib +
  nlohmann/json, beide MIT-lizenziert und fuer sich unproblematisch mit GPL-2.0 kombinierbar - das war
  nicht der Hinderungsgrund): stattdessen ein minimaler, selbst geschriebener HTTP/1.1-Client auf
  `boost::asio`-Basis (bereits eine verlinkte Core-Abhaengigkeit, siehe `OllamaHttpClient.h/.cpp`) plus
  handgeschriebene String-basierte JSON-Konstruktion/-Extraktion (`OllamaChatMgr.cpp`) - fuer den engen
  Anwendungsfall (ein JSON-POST, ein JSON-Feld auslesen) angemessen und ohne ~56.000 Zeilen ungetesteten
  Fremdcode. Der eigentliche HTTP-Request laeuft auf einem dedizierten Hintergrund-Thread pro Anfrage
  (siehe `OllamaChatMgr.h`-Kopfkommentar fuer das volle Thread-Sicherheits-Modell) - blockiert also NICHT
  den World-Update-Thread. Dokumentierte Einschraenkungen: kein TLS, kein Chunked-Transfer-Encoding, kein
  explizites Timeout, keine Konversations-Historie/Persoenlichkeits-Profile wie im Referenzmodul.
- **[mod-dungeon-clear-addon](https://github.com/jrad7/mod-dungeon-clear-addon)** (Referenz fuer: Client-Addon
  zur Steuerung des Dungeon-Clear-Modus) - **umgesetzt** unter `tools/addons/AshDC_DungeonClear/` (Runde 6).
  Referenzmodul ist ein WotLK-3.3.5-Lua-Addon (`## Interface: 30300`, AGPL-3.0) - Struktur/Funktionsweise von
  WoW-Addons hat sich seit WotLK nicht grundlegend geaendert (Slash-Befehle, `CHAT_MSG_*`-Events, Addon-
  Nachrichtenkanal), trotzdem komplett neu fuer Client-Build 26972 (Legion 7.3.5, `## Interface: 70300`)
  geschrieben und strikt auf das beschraenkt, was `bot_dungeonclear_control.cpp` (neu, Runde 6) serverseitig
  tatsaechlich anbietet: zwei gleichwertige Steuerwege - ein Party-/Raid-Chat-Schluesselwort (`!dc on/off/
  status`, `PlayerScript::OnChat()`-Group-Ueberladung, braucht KEINEN Addon-Kanal) und eine an einen
  bestimmten Bot gerichtete Addon-Whisper-Nachricht (`SendAddonMessage(prefix, "ASHDC:CMD:...", "WHISPER",
  <BotName>)`). Wichtige, per Recherche bestaetigte Einschraenkung: `PlayerScript::OnChat()` bekommt bei einer
  Addon-Nachricht nur Text + `lang==LANG_ADDON`, NICHT den eigentlichen Addon-Prefix (`Player::WhisperAddon()`
  reicht ihn nicht an `sScriptMgr->OnPlayerChat()` durch) - geloest, indem die Server-Antwort als normale
  sichtbare System-/Whisper-Nachricht mit festem Text-Praefix `"[AshDC]"` zurueckkommt, die das Addon per
  eigenem `CHAT_MSG_SYSTEM`/`CHAT_MSG_WHISPER`-Hook erkennt und in einem kleinen Status-Fenster anzeigt -
  kein echtes `CHAT_MSG_ADDON`-Client-Event noetig. Bewusst NICHT uebernommen: Boss-Mechanik-Datenbank/
  Ausweich-UI, Pull-Stil-Auswahl, Konfigurations-Fenster/SavedVariables (passend zum aktuellen Server-
  Funktionsumfang, siehe oben).

Umsetzungsstand dieser Punkte: siehe Commit-Historie/PRs nach diesem README-Stand - wird hier bewusst nicht
laufend nachgepflegt, um Drift zwischen Code und Dokumentation zu vermeiden; der PR-Text der jeweiligen
Implementierungsrunde ist die verbindliche Quelle fuer den genauen Umfang/die Annahmen.

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
