# Befehlsuebersicht - Custom-Module

Diese Datei listet **alle serverseitigen "."-Befehle**, die zu den Custom-Modulen dieses Forks gehoeren
(Playerbots, Dungeon-Clear, AhBot, OllamaChat), mit Syntax und Wirkung. Reine Stock-TrinityCore-Befehle
(`.account`, `.character`, `.gm`, `.npc add` usw.) sind NICHT Teil dieser Liste - die Standard-TrinityCore-
Befehlsreferenz deckt die bereits ab.

Nicht-"."-Steuerwege (Party-/Raid-Chat-Schluesselwort, Client-Addon) sind der Vollstaendigkeit halber unter
"Modul Dungeon-Clear" mit aufgefuehrt, da sie denselben Funktionsumfang wie der zugehoerige "."-Befehl
abdecken.

---

## Server (allgemeine Hinweise, gelten fuer alle unten aufgefuehrten "."-Befehle)

- **Berechtigungs-Gate**: alle `.bottest`-Unterbefehle sind ueber `RBAC_PERM_COMMAND_ACCOUNT_CREATE`
  geschuetzt (dieselbe Berechtigungsstufe wie `.account create`, da `createaccount` intern genau das
  aufruft) - ein Account/eine Sicherheitsstufe ohne dieses Recht sieht/nutzt keinen dieser Befehle.
- **Konsole/RA vs. echter GM-Client**: einige Befehle (`groupinvite`, `follow`, `findnpc`) nutzen ohne
  expliziten Parameter automatisch den Charakter des AUSFUEHRENDEN echten Spielers als Bezugspunkt
  (Gruppenleiter, Folge-Ziel, Suchzentrum). Von der Server-Konsole/RA-Fernkonsole aus gibt es keinen
  In-World-Charakter - dort muessen die optionalen `accountId`-Parameter (eines bereits eingeloggten
  Bot-Accounts) explizit angegeben werden.
- **Asynchrone Befehle**: `createchar` und `login` stossen mehrstufige, mehrere Ticks dauernde
  Opcode-Handler-Ketten an (derselbe Pfad wie ein echter Client-Login) - ihr Ergebnis ist NICHT sofort
  sichtbar, sondern muss per `.bottest status <accountId>` abgefragt werden. Alle anderen Befehle sind
  synchron (Ergebnis steht sofort in der Server-Antwort).
- **DB-Spawn-Ids**: `attack`, `loot`, `questaccept` und `questturnin` erwarten `creature.guid` (die
  DB-Spawn-Id, NICHT die laufzeit-volle `ObjectGuid`) als Ziel-Identifikator. Diese Id vorher per
  `.bottest findnpc` ermitteln (siehe unten) statt sie manuell per SQL nachzuschlagen.
- **Master-Schalter `Playerbots.Enable`** (neu, `worldserver.conf`, Default `1`): schaltet den
  `BotMgr::Tick()`-Heartbeat global ab, ohne eingeloggte Bots auszuloggen (sie frieren einfach ein) -
  per `.reload config` zur Laufzeit anwendbar, kein Serverneustart noetig. `.bottest ...`-Befehle
  funktionieren unabhaengig vom Schalter weiter (rufen BotMgr-Methoden direkt auf).

---

## Modul Playerbots (`.bottest ...`, Datei `bot_commandscript.cpp`)

### Account-/Charakter-Lebenszyklus

| Befehl | Wirkung |
|---|---|
| `.bottest createaccount <accountName> <password>` | Legt (idempotent) einen Bot-Account an (`BotMgr::CreateBotAccount`, nutzt `AccountMgr::CreateAccount()`). |
| `.bottest createchar <accountId> <charName> [race=1] [class=1] [sex=0]` | Erstellt fuer diesen Account einen Charakter (asynchron, wie ein echter Client-Charaktererstellungs-Screen). |
| `.bottest login <accountId>` | Loggt den Bot ein (asynchron - CharEnum + Login-Opcode-Kette, wie ein echter Client-Login). |
| `.bottest logout <accountId>` | Loggt den Bot sauber aus, waehrend der Server normal weiterlaeuft (synchron). |
| `.bottest status <accountId>` | Zeigt Session-Zustand, Position, Lebend-/Geist-Status, LFG-Zustand, Kampf-KI-Rolle, Dungeon-Clear-Status und laufende Patrol/Follow-Zustaende auf einen Blick. |

### Bewegung

| Befehl | Wirkung |
|---|---|
| `.bottest move <accountId>` | Ein einzelner Bewegungsschritt (8 Yards geradeaus, KEINE Pfadfindung). |
| `.bottest movepath <accountId>` | Wie `move`, aber mit Navmesh-Pfadfindung (`generatePath=true`). |
| `.bottest patrol <accountId> <cycles>` | Pendelt `<cycles>` mal zwischen zwei Punkten (Start- und 8-Yards-Blickrichtungspunkt), 1-20 geklemmt. |
| `.bottest teleport <accountId> <mapId> <x> <y> <z> [orientation]` | Kartenwechsel-Teleport inkl. manuell nachgebildetem Worldport-Ack (da der Bot keinen echten Client hat). |
| `.bottest follow <botAccountId> [targetPlayerName]` | Bot folgt dem angegebenen Spieler (ohne Angabe: dem ausfuehrenden GM). |
| `.bottest followstop <botAccountId>` | Bricht das Folgen ab. |

### Kampf

| Befehl | Wirkung |
|---|---|
| `.bottest attack <accountId> <targetGuid>` | Startet Auto-Attack (`Player::Attack()`) gegen die Kreatur mit dieser DB-Spawn-Id. Reiner Melee-Auto-Attack, kein Spell-Cast. |
| `.bottest attackstop <accountId>` | Bricht den Angriff ab. |
| `.bottest invuln <accountId> <on\|off>` | Schaltet Test-Schadensimmunitaet fuer GENAU diesen Bot um (nur fuer Livetests, nie automatisch). |
| Automatische Kampf-KI | Laeuft OHNE eigenen Befehl, sobald ein Bot in Kombat ist - siehe `ProcessBotCombatAI()`: waehlt Ziel/Heilziel, castet nach der Rotationstabelle der jeweiligen Skillung (36/36 abgedeckt), weicht Bodeneffekten aus, unterbricht/dispelt wenn die Skillung das kann (siehe "Modul Dungeon-Clear" fuer den generischen Mechanik-Reaktionsteil, der auch ausserhalb des Dungeon-Clear-Modus aktiv ist). |

### Loot / Tod

| Befehl | Wirkung |
|---|---|
| `.bottest loot <accountId> <targetGuid>` | Loot-Zyklus gegen eine BEREITS TOTE Kreatur (DB-Spawn-Id). |
| `.bottest release <accountId>` | Simuliert den "Geist werden"-Dialog nach dem Tod (Bot muss bereits tot sein). |
| `.bottest revive <accountId>` | Volle Wiederbelebung inkl. Resurrection Sickness (Spirit-Healer-Aequivalent). |

### Ausruestung

| Befehl | Wirkung |
|---|---|
| `.bottest equip <accountId> <itemEntry>` | Legt EIN Test-Item ins Inventar und ruestet es in den passenden Slot. |
| `.bottest equipfrompool <accountId>` | Ruestet den Bot VOLLSTAENDIG aus dem levelabhaengigen Equipment-Pool aus (alle Slots automatisch). |
| `.bottest equipartifact <accountId>` | Ruestet die zu Klasse+Primaerspezialisierung passende Artefaktwaffe aus (36er-Zuordnungstabelle). |
| `.bottest skillartifact <accountId> [levelBudget]` | Vergibt Artefakt-Traits (ohne Angabe: Budget automatisch aus dem Bot-Level berechnet). |

### Gruppe

| Befehl | Wirkung |
|---|---|
| `.bottest groupinvite <botAccountId> [leaderAccountId]` | Nimmt den Bot in eine Gruppe auf (Leader = ausfuehrender GM, oder explizit angegebener Bot-Account). |
| `.bottest groupleave <botAccountId>` | Entfernt den Bot aus seiner aktuellen Gruppe. |

### LFG (Dungeon-Finder)

| Befehl | Wirkung |
|---|---|
| `.bottest lfgfill` | Stoesst manuell GENAU EINEN Nachfuell-Versuch der lazy-nachfuellenden LFG-Pool-Logik an (normalerweise ein automatischer ~10s-Timer, siehe `BotMgr::ProcessLfgPoolFillTick()`). Reiht bei Bedarf einen passenden Bot ueber die echte `LFGMgr::JoinLfg()`-Queue ein. |

### Quest-KI

| Befehl | Wirkung |
|---|---|
| `.bottest questaccept <accountId> <questGiverSpawnGuid> <questId>` | Nimmt eine Quest an (ruft dieselben `Player`-Methoden wie der echte Opcode-Handler auf). |
| `.bottest questturnin <accountId> <questGiverSpawnGuid> <questId> [rewardItemEntry]` | Gibt eine abgeschlossene Quest ab. `rewardItemEntry` ist der ECHTE Item-Entry (`item_template.entry`) der gewuenschten Auswahl-Belohnung, KEIN 0-basierter Index - 0/leer nur bei Quests ohne Auswahl-Belohnung. |
| `.bottest queststatus <accountId> <questId>` | Zeigt den rohen `QuestStatus`-Wert. |

### Aktive Debug-/Diagnose-Werkzeuge (neu, Runde 6)

| Befehl | Wirkung |
|---|---|
| `.bottest findnpc <namePart> [radius=100] [accountId]` | Durchsucht die aktuell geladenen Kreaturen um den ausfuehrenden GM (oder um den angegebenen Bot) nach einem Namens-Teilstring und listet **DB-Spawn-Id (creature.guid), Entry, Lebend/Tot, Distanz und Position** je Treffer - loest das Problem "ich kenne die Spawn-Id/Position des Ziel-NPCs nicht" ohne manuelle SQL-Abfrage. |
| `.bottest diagspells` | Erzwingt die Aufloesung ALLER 36 Kampf-KI-Rotationen (`g_BotSpecRotations`) gegen das aktuell geladene `Spell.db2` auf einen Schlag und meldet jeden Faehigkeits-/Aura-/Interrupt-/Dispel-Namen, der NICHT gefunden wurde - deckt z. B. falsch geratene Aura-Namen (Brewmaster-Stagger) oder unsichere Recherche-Zeilen (Demonology Warlock) auf, ohne dafuer 36 verschieden geskillte Bots anlegen zu muessen. |

---

## Modul Dungeon-Clear

### "."-Befehl (GM-Test)

| Befehl | Wirkung |
|---|---|
| `.bottest dungeonclear <accountId> <on\|off>` | Schaltet den autonomen Dungeon-Clear-Modus fuer GENAU DIESEN Bot um. Nur auf einer Dungeon-Karte aktivierbar. Navigiert per Navmesh zum naechsten lebenden Dungeon-Boss, engagiert Trash auf dem Weg, lootet automatisch, deaktiviert sich selbst sobald kein lebender Boss mehr gefunden wird. |

### Spieler-Steuerung ohne GM-Rechte (neu, Runde 6 - `bot_dungeonclear_control.cpp`)

Diese zwei Wege schalten den Modus fuer **ALLE Bot-Mitglieder der eigenen Gruppe gleichzeitig** um
(dieselbe Berechtigungsgrenze wie Party-Invite/-Kick, kein GM-Recht noetig):

| Weg | Syntax | Wirkung |
|---|---|---|
| Party-/Raid-Chat-Schluesselwort | `!dc on` / `!dc off` / `!dc status` (in Party- oder Raid-Chat tippen) | Schaltet den Modus fuer die eigene Gruppe um bzw. fragt den Status ab. Antwort kommt als sichtbare Systemnachricht. |
| Addon-Whisper (siehe `tools/addons/AshDC_DungeonClear/`) | Client-Addon-Slash-Befehl `/ashdc on\|off\|status` (nutzt intern denselben Chat-Weg) oder `/ashdc whisper on\|off\|status <BotName>` (echte Addon-Nachricht an einen bestimmten Bot) | Wie oben; die gezielte Addon-Variante antwortet als sichtbare Whisper-Nachricht des angesprochenen Bots mit Praefix `[AshDC]`. |

### Generische Boss-Mechanik-Reaktionen (kein eigener Befehl - laeuft automatisch)

Sobald ein Bot im Kampf ist (mit oder ohne aktiven Dungeon-Clear-Modus), prueft `ProcessBotCombatAI()`
VOR jeder normalen Rotation automatisch:
1. Steht der Bot in einem schaedlichen Bodeneffekt? -> ausweichen.
2. Hat die Skillung einen Interrupt und castet der Gegner gerade etwas Unterbrechbares? -> unterbrechen.
3. Hat die Skillung einen Dispel und gibt es eine entfernbare Debuff am Heilziel? -> dispellen.

Kein Ersatz fuer echtes Boss-Mechanik-Skripting (siehe README.md Abschnitt e) fuer die Grenzen).

---

## Modul AhBot (natives TrinityCore-System, `.ahbot ...`, Datei `src/server/scripts/Commands/cs_ahbot.cpp`)

Kein eigenes Custom-Modul (siehe README.md Abschnitt e) - dieser Fork bringt bereits ein vollstaendiges,
natives Auktionshaus-Bot-System mit; die Befehle sind Teil des Stock-TrinityCore-Codes, werden hier aber
mitdokumentiert, da sie zum Playerbots-Gesamtkonzept ("Bots beleben die Welt") gehoeren.

| Befehl | Wirkung |
|---|---|
| `.ahbot status` | Zeigt aktuellen Ratio-/Item-Konfigurationsstand des AH-Bots. |
| `.ahbot rebuild` | Baut den Item-Pool des AH-Bots komplett neu auf. |
| `.ahbot reload` | Laedt die AhBot-Konfiguration neu (`worldserver.conf`-Werte). |
| `.ahbot items [gray\|white\|green\|blue\|purple\|orange\|yellow] [Anzahl]` | Setzt/liest die Ziel-Itemanzahl je Qualitaetsstufe (ohne Parameter: Gesamtanzahl). |
| `.ahbot ratio [alliance\|horde\|neutral] [Prozent]` | Setzt/liest das Verkaufsverhaeltnis je Fraktions-Auktionshaus. |

**Aktivierung**: aktuell deaktiviert (`AuctionHouseBot.Seller.Enabled = 0` in `worldserver.conf.dist`) -
Inbetriebnahme ist reine Konfiguration (Bot-Account + `AuctionHouseBot.Account`/`.Seller.Enabled`/
`.Buyer.*.Enabled` setzen), kein Code noetig.

---

## Modul OllamaChat (`src/server/scripts/Custom/OllamaChat/`)

**Kein eigener "."-Befehl.** Das Modul reagiert passiv: whispert ein echter Spieler einen Bot an (normale,
sichtbare Whisper, KEINE Addon-Nachricht), generiert der Bot ueber eine lokale Ollama-HTTP-API eine
In-Charakter-Antwort und schickt sie als Whisper zurueck. Steuerung ausschliesslich ueber
`worldserver.conf`-Werte:

| Konfigurationsschluessel | Wirkung |
|---|---|
| `OllamaChat.Enable` | Modul an/aus (Standard: aus). |
| `OllamaChat.Host` / `OllamaChat.Port` | Adresse des lokalen Ollama-Servers. |
| `OllamaChat.Model` | Zu verwendendes Ollama-Modell. |
| `OllamaChat.SystemPrompt` | System-Prompt fuer jede Anfrage. |
| `OllamaChat.MaxConcurrentRequests` | Obergrenze gleichzeitiger HTTP-Anfragen (Hintergrund-Thread pro Anfrage). |

**Wichtige Abgrenzung (Runde 6, siehe Kollisions-Fix)**: das Modul reagiert NUR auf normale, sichtbare
Whispers (`lang != LANG_ADDON`) - eine an einen Bot gerichtete Addon-Steuerungsnachricht (siehe "Modul
Dungeon-Clear" oben) loest KEINE Ollama-Antwort aus.
