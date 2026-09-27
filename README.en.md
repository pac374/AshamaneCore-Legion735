# AshamaneCore

**Deutsche Version:** [README.md](README.md)

A private [TrinityCore](https://www.trinitycore.org/) fork for **World of Warcraft: Legion (7.3.5, Build 26972)**.

This repository contains the **C++ source code**: custom GM commands, SmartAI extensions, core extensions
(e.g. the Invasion Points OutdoorPvP framework) and bug fixes, built on top of the original
[AshamaneCore/LegionEmulationProject](https://github.com/LegionEmulationProject/AshamaneCore) fork. It is
**not a ready-to-run server** - a private SQL data package is intentionally missing (see
[Why the SQL data isn't here](#why-the-sql-data-isnt-here)).

* [Base project & license](#base-project--license)
* [Why the SQL data isn't here](#why-the-sql-data-isnt-here)
* [What works - detailed breakdown](#what-works---detailed-breakdown)
  * [a) Fully solved, blizzlike](#a-fully-solved-blizzlike--correct)
  * [b) Working, but a custom interpretation](#b-working-but-a-custom-interpretation--not-100-blizzlike)
  * [c) Deliberately open / unsolved](#c-deliberately-open--unsolved-with-reason)
  * [d) Actively in development](#d-actively-in-development)
* [Setup / Build](#setup--build)
* [Contributing](#contributing)

## Base project & license

- Fork base: TrinityCore (GPL-2.0), via the community fork **AshamaneCore/LegionEmulationProject**
  (a Legion 7.3.5 port of TrinityCore).
- License: **GPL-2.0**, see [`LICENSE`](LICENSE) / [`COPYING`](COPYING) (identical; `COPYING` is the file
  from the original TrinityCore/AshamaneCore tree, `LICENSE` sits alongside it for GitHub's automatic
  license detection).
- This fork is a **private continuation project**, not an official continuation of the original AshamaneCore
  project. Starting commit: `c250ffc` (branch `legion`), with only original changes from that point on.

## Why the SQL data isn't here

A large part of the spawn positions, creature placements, and quest-credit fixes in this server is based on
cross-referencing several sources - among them official TrinityCore TDBs, community addon databases (Grail),
Wowhead research, and the **LegionCore 7.3.5 leak**. Researching that last source is legitimate for running a
private server, but **publicly redistributing the raw data derived from it** (coordinates, creature spawns,
etc.) would be its own legal risk: the data itself is subject to Blizzard's copyright on game data, regardless
of whether the source is named in a SQL comment or not.

That's why this repository draws a deliberate line:

| | public (here) | private (not in this repo) |
|---|---|---|
| **C++ code** (`src/`) | yes - original work on a GPL-2.0 base | - |
| **SQL data** (`sql/ashamane/world/*`, `fixes/*.sql`, `trait_candidates/*.sql`) | no | yes, stays local |
| Database dumps, `*.conf` files with real credentials, downloaded third-party repos | no | no (these live outside this repo folder anyway, under `C:\LegionServer\`) |

Anyone who wants to run this code themselves also needs their **own, non-public SQL data package**
(see [Setup / Build](#setup--build)). Without it the server compiles and starts, but a large part of the
Legion content (class halls, world quests, many NPC spawns) is missing or incomplete.

## What works - detailed breakdown

This breakdown is based on more than 60 internal work rounds ("LCF2 rounds", documented internally in
`reports/lcf2r*.md`, not part of this repo). The goal: anyone reading this code (including the author
themselves, a few months from now) should be able to tell at a glance what's finished, what's an
interpretation/approximation, and what's deliberately left open - and why.

### a) Fully solved, blizzlike / correct

Content implemented on a solid, independently confirmed source (client DB2 data, official TrinityCore TDBs,
leak data cross-checked multiple times) and verified in the server (log/DB state checks):

- **Artifact weapon traits**: of an initial 243 open traits, **206 have been implemented with a solid
  source** (client DB2 effect data, SimulationCraft source-code comparison, TrinityCore core semantics).
  Test infrastructure: a custom GM command `.arttest` (automated tests per trait, including delayed and
  percentage-based cases).
- **Invasion Points (map 1779, "Legion Assault")**: the complete OutdoorPvP campaign, including a core
  extension (`OutdoorPvP`/`Scenario`/`ScenarioMgr` extended to support zone-parallel scenarios, additively,
  without changing existing zones), all 6 invasion zones with wave spawns, game events, and scenario hookup
  live.
- **Scenario 39746 "A Ring Unbroken"** (map 1572): fully playable, including a previously unknown structural
  access blocker (a `PlayerChoice` effect had incorrectly been assumed to be a plain teleport - the real
  cause was a missing teleport target, fixed via the quest-choice logic).
- **Server-side spells**: the `serverside_spell`/`serverside_spell_effect` tables plus
  `SpellMgr::LoadSpellInfoServerside()` ported from an upstream TrinityCore commit (adapted to our diverging
  `SpellInfo` structure).
- **NPC position verification**: a multi-stage automated check (`.npcverify`, terrain height matching via a
  custom `HeightSampler` tool) for hundreds of derived spawns, including outlier correction.
- **Various quest-credit chains** (SmartAI ports with a clearly traceable 1:1 source), including the Anduin
  escort quest, several class-hall fragments, and the Didi/Thalyssra/Farodin/Brann/Silgryn NPC chains.
- **Dargrul** (map 1458 "Neltharion's Lair") and several dungeon-floor NPCs (e.g. Rattlegore, Darkmaster
  Gandling in Scholomance): map-difficulty incompatibilities identified and correctly resolved.

### b) Working, but a custom interpretation / not 100% blizzlike

Content that is playable but relies on an approximation, a workaround, or a plausible-but-unproven
assumption, because the real Blizzard logic couldn't be found. Each point comes with a short justification
for why it's an interpretation:

- **Approximate coordinates derived from Grail/QuestPOI**: some NPC spawns weren't taken from a sniff, but
  back-calculated from community addon data (the Grail NPC database, world-map percentage coordinates) and
  QuestPOI derivation, including terrain height via a custom sampler tool. Validated at roughly 71-78% XY/Z
  accuracy against known reference points - **not** exact Blizzard coordinates.
- **Argent Tournament teleport**: a teleport ability leads to the nearest graveyard instead of the exact
  "Grounds" target point, because the real target coordinate pair wasn't found in any verified source.
- **Underlight Angler bonus catches ("X Angling" traits, 6 of them) and "Better Luck Next Time"**: the client
  data only provides a plain dummy aura with no numeric values - the chance/percentages (10% per rank on
  bonus fish, 5% on an item) are a **documented assumption**, not a confirmed Blizzard number.
- **PlayerChoice workarounds**: several quest decision points (e.g. scenario 39746) use
  `PlayerScript::OnPlayerChoiceResponse` hooks instead of native client choice-response logic, because our
  core doesn't automatically process PlayerChoice rewards that have no spell ID attached.
- **The Netherlight Crucible interaction runs through a gossip menu instead of the real client UI window**
  (the relic forge). The network protocol for the native window is only known from leak sources and hasn't
  been independently confirmed - the C++ code for the native variant already exists
  (`NetherlightCrucible.cpp/.h`, config switch `NetherlightCrucible.ClientUI`, off by default), but is
  untested and disabled. See the comment in `src/server/worldserver/worldserver.conf.dist`.
- **Some trait values/timings (Doom Wolves, Echoing Stars)** are based on SimulationCraft community values
  rather than a confirmed Blizzard source (SimC itself flags one of these values in its own source code as a
  provisional placeholder).
- **Existing generic placeholder templates** (around 2,200 NPC templates with generic values such as level 1
  or a neutral faction) were left as-is wherever the comparison source also only has the same placeholder -
  not invented values, but not confirmed ones either.

### c) Deliberately open / unsolved, with reason

These points are **not bugs in the usual sense**, but spots where, after thorough and repeated research
(often across 5-7 independent sources), simply no solid source could be found. Deliberately phrased so that
a contributor immediately knows where they could pick up:

- **World quest rotation system (duration blocker)**: for 1,092 of 1,133 world quests, the
  `world_quest.variable` value could be reconstructed from client DB2 bytecode (a 100% hit rate against known
  reference values) - but the second required column, `duration`, has **no counterpart in any checked
  source** (client DB2, leak DB, our own tables). A partial deploy with a guessed value was deliberately not
  done. **Starting point for contributors:** derive `duration` from 1:1 sibling quests (identical
  zone/title/QuestInfoID), not by guessing.
- **38 of 42 dungeon-floor NPC candidates** without a spawn: the Z-height problem for dungeon interiors is
  unsolved (the existing `HeightSampler` tool only computes open-world terrain, not a WMO collision model for
  dungeon interiors; the second tool, `VMapHeightSampler`, reliably crashes).
- **301 remaining "Wowhead without coordinates" IDs** (down from an original 465-306): the large majority
  simply aren't conceptually spawnable (kill-credit triggers, invisible "bunnies", internal markers, scenario
  proxies with no fixed location) - not a case for guessing, but also not a completed individual review of
  every remaining ID.
- **539 of an original roughly 562 quest credits with no findable source** (as of the latest round: 25 solved
  / 537 still unsolvable): the NPCs/objects involved don't exist with a spawn or summoning mechanism in any
  of the seven checked sources (our own DB, the LegionCore leak, official TDBs, SkyFire, the Draenor core
  fork, an ADB dump, full-text C++ search). Three prominent cases (Akama/Farondis/Reshad) are documented as
  **source-exhausted** after repeated in-depth review.
- **Skylord Tovra (creature ID 80005)**: 0 hits across all checked creature tables and UI map assignments -
  documented as final and unsolvable, no further review needed unless a new leak/sniff source appears.
- **677 teleport target spells**: 141 of the potentially reachable targets identified, 106 of those already
  have a target position, 35 were missing - only 6 deployed with reasonable confidence (the rest are a
  genuine data gap or can only be closed by guessing, partly due to ambiguous name traps like "Bloodmaul Slag
  Mines" vs. "Bloodmaul Slave Mines").
- **Objectives 108787/108788/108789** ("Armor Polish"/"Weapon Enchantment"/"Food Kill Credit", affecting both
  an Alliance and the analogous Horde quest): none of the six checked sources contains a spawn or credit
  mechanism - formally closed as source-exhausted.

### d) Actively in development

Code under `src/server/scripts/Custom/Bots/` (`BotMgr.h/.cpp`, `BotCharacter.h/.cpp`, `bot_scriptloader.cpp`,
`bot_commandscript.cpp`) plus three small, additive changes in `src/server/game/Server/WorldSession.h/.cpp`.
Goal: a custom Playerbots module modeled on AzerothCore's `mod-playerbots` - bot-controlled player characters
("session faking": a real `Player` object on a socket-less `WorldSession`, not a `Creature`-based NPC bot).
Unlike the points under (c), this is **not a blocker or a research question**, but an ongoing build with a
working, deliberately incomplete milestone ("Phase 1") - hence its own section instead of (b) or (c).

**What works (confirmed live on the production server multiple times, via GM test commands `.bottest ...`):**

- **Login**: a bot account/character can log in without a real WoW client (`.bottest login`) - a real `Player`
  object, a real position in the world, not a fake NPC.
- **Logout**: clean logout during normal operation (`.bottest logout`), the online flags in `auth.account` and
  `characters.characters` are reset correctly.
- **Shutdown**: a controlled server shutdown with a logged-in, active bot completes without leaving the bot
  state inconsistent or crashing the server.
- **Single movement**: the bot can move in a straight line (`.bottest move`, `MotionMaster::MovePoint()`
  without pathfinding).
- **Commuting**: several consecutive movement cycles between two points (`.bottest patrol <n>`), including
  arrival detection and clean cycle completion.
- **Pathfinding**: movement over the real navmesh (`.bottest movepath`, Recast/Detour path calculation instead
  of a straight line).

All six mechanisms have been confirmed reproducible individually AND together, across several complete
regression runs under the current production binary (without diagnostic special flags) - not a simulation,
real live tests against the running server.

**What doesn't work (yet) / doesn't exist:**

- **No real combat behavior**: no auto-attack, no spell casting, no threat management, no handling of
  death/resurrection - an as-yet untested risk area.
- **No state machine / autonomous behavior**: the bot does nothing on its own. Every action (login, movement,
  logout) has to be triggered individually via a GM command - there is no self-contained life cycle ("logs in,
  patrols for a while, logs out again by itself").
- **No group/AI logic**: no following, no role assignment (tank/heal/DD), no decision logic of any kind - pure
  test infrastructure, not bot behavior in the proper sense.
- The test infrastructure itself (GM command
  `.bottest createaccount|createchar|login|logout|move|movepath|patrol|status`) is deliberately manual/
  step-by-step, not intended for production use.

**Three independent, structural crash causes found and fixed** (not just worked around symptomatically -
potentially interesting for other TrinityCore developers attempting something similar):

1. **Null pointer on the socket idle check**: `WorldSession::Update()` unconditionally calls
   `m_Socket[CONNECTION_TYPE_REALM]->CloseSocket()` once the idle timeout has expired - for a session without
   a real socket (`socket == nullptr`, which a socket-less bot inevitably has), that's a guaranteed null-pointer
   access that crashes the entire `worldserver` process the first time this session ever runs through the
   regular update cycle. Fix: an `IsBotSession()` guard skips the idle-kick entirely for bot sessions (purely
   additive, no behavior change for real players).
2. **DB access in the `WorldSession` destructor after the DB pool was already closed (finding 1)**: if the
   destructor of a still-logged-in bot session runs very late (e.g. in a singleton manager's `atexit` window,
   AFTER `main()` has returned), the DB pools (`CharacterDatabase` etc.) have already been closed via
   `StopDB()` and their internal `boost::asio` executor already destroyed - a `LogoutPlayer()->SaveToDB()` call
   triggered from there then dereferences an executor pointer that has already been reset to `nullptr`. Fix:
   the actual clean logout (including the DB write) now happens beforehand, synchronously within `main()` and
   guaranteed before any DB pool teardown (`WorldScript::OnShutdown()` -> `BotMgr::LogoutAllBots()`); the
   destructor itself is now, for bot sessions, entirely free of DB access.
3. **DB access in the `WorldSession` destructor after the DB pool was already closed (finding 2, independent)**:
   the same destructor contained a second, independent unconditional database write
   (`LoginDatabase.PExecute("UPDATE account SET online = 0 ...")`) that ran for every session regardless of
   player state - in the same late time window, also an access to an already-closed DB pool. Fixed the same
   way: this online-flag reset now also happens beforehand, while the `LoginDatabase` pool is still guaranteed
   to be alive; the destructor remains fully DB-free for bot sessions.

## Setup / Build

This is a pure **code build** - without the private SQL data package (see above), large parts of the Legion
content are missing.

1. **Build prerequisites**: the usual TrinityCore requirements - CMake, a C++17-capable compiler (tested with
   MSVC/Visual Studio on Windows), Boost, OpenSSL, MySQL/MariaDB client libraries. See the TrinityCore wiki
   links below for details (this fork doesn't deviate significantly from the standard here).
2. **Create databases**: `auth`, `characters`, `world`, `hotfixes` (standard TrinityCore schema).
3. **Server-side configuration**: copy `worldserver.conf.dist` / `bnetserver.conf.dist` to `worldserver.conf`
   / `bnetserver.conf` and fill in your own credentials (these files are deliberately excluded from version
   control via `.gitignore`).
4. **Populate the world database**: in addition to the official TrinityCore base DB, this requires the
   **own SQL data package not included in this repo** (`sql/ashamane/world/*`). Without it the server
   starts, but with significantly less Legion content (see the section above). Anyone wanting to build their
   own data: the updater automatically reads from `sql/ashamane/world` (not from the standard TrinityCore
   path `sql/updates/world/master`).
5. **Client**: WoW 7.3.5, Build 26972.

Detailed, general TrinityCore installation instructions (which apply to this fork as well):
[TrinityCore Wiki: Requirements](https://www.trinitycore.info/display/tc/Requirements) and
[Installation Guide](https://www.trinitycore.info/display/tc/Installation+Guide).

## Contributing

C++ fixes are welcome as pull requests. For category (c) above (deliberately open points), it's worth
checking this repo's GitHub issues in case matching topics have been filed there - each one states the
exact research approach that was already tried (and considered closed for the current state of available
sources), so no work gets duplicated.

## Copyright

License: GPL-2.0. See [`LICENSE`](LICENSE) / [`COPYING`](COPYING).

## Links

* [TrinityCore](https://www.trinitycore.org/)
* [Original fork: LegionEmulationProject/AshamaneCore](https://github.com/LegionEmulationProject/AshamaneCore)
