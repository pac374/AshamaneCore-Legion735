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

#ifndef BOT_MGR_H
#define BOT_MGR_H

// ---------------------------------------------------------------------------
// AshamaneCore Playerbots-Modul - Skeleton/Interface-Runde, Stand 27.09.2026.
//
// BotMgr ist der zukuenftige zentrale Einstiegspunkt fuer Bot-Charaktere
// (Vorbild: mod-playerbots' PlayerbotMgr/RandomPlayerbotMgr, siehe
// C:\LegionServer\reports\lcf2r62_2026-09-27_playerbots_aufbau.md und die
// Memory-Notiz playerbots-module-idea.md). In dieser Runde ist BotMgr
// bewusst INERT: keine Methode erzeugt einen echten Bot-Player, keine
// Methode patcht WorldSession/World. Alle Kommentare "spaeter" markieren,
// was erst mit der separaten Session-Faking-Runde dazukommt.
//
// Architekturentscheidung (bestaetigt gegen echten mod-playerbots-Quellcode,
// Datei src/Bot/PlayerbotMgr.cpp, GPL-2.0): Bot-Sessions werden ueber
// `new WorldSession(..., /*socket*/ nullptr, ...)` + denselben
// LoginQueryHolder/HandlePlayerLoginFromDB-Pfad erzeugt, den auch echte
// Spieler durchlaufen ("WorldSession-Lifecycle wiederverwenden" statt
// eigene Runtime-Session) - das bestaetigt Variante A aus dem
// Konzeptpapier vom 22.09.2026 mit echtem Fremdcode, nicht nur Sekundaerquellen.
// Unser Kern hat den strukturell gleichen Andockpunkt:
// WorldSession::HandlePlayerLogin(LoginQueryHolder const&) in
// CharacterHandler.cpp. Der Legion-Doppelsocket-Handshake
// (SendConnectToInstance/HandleContinuePlayerLogin, CONNECTION_TYPE_INSTANCE)
// hat in mod-playerbots keine Entsprechung und muss eigenstaendig geloest
// werden - das ist explizit NICHT Teil dieser Runde.
//
// --- Runde A (27.09.2026): Session-Lifecycle-Entscheidung, gegen unseren
// eigenen Quellcode verifiziert (nicht nur gegen Fremdcode) ---------------
//
// Endgueltige Wahl: MINIMAL-FOOTPRINT (LegionBotAI-Stil) - Bot-WorldSession
// wird NIEMALS ueber World::AddSession()/AddSession_() registriert, Taktung
// ausschliesslich manuell ueber den bereits vorhandenen
// bot_playerscript_hooks::OnUpdate-Hook (bot_scriptloader.cpp). Grund: in
// UNSEREM Fork ist WorldSession::Update(uint32, PacketFilter&)
// (WorldSession.h:1058) NICHT virtuell deklariert - die Polymorph-Variante
// (LegionPlayerBot-Stil, eigene PlayerBotSession-Subklasse mit
// Update()-Override) wuerde also erst funktionieren, nachdem man diese
// zentrale, performancekritische Klasse virtuell macht (ABI-/Vtable-
// Aenderung an einer Kernklasse) - ein deutlich groesserer und riskanterer
// Core-Patch als bei LegionPlayerBot selbst, wo Update() bereits virtuell ist.
//
// Zusaetzlicher, in dieser Runde neu gefundener KONKRETER Absturzgrund gegen
// jede Variante, die eine socketlose Session durch World::UpdateSessions
// laufen laesst (bestaetigt in WorldSession.cpp):
//   - Zeile 337-338: `if (IsConnectionIdle()) m_Socket[CONNECTION_TYPE_REALM]
//     ->CloseSocket();` - IsConnectionIdle() (WorldSession.h:1180) wird nach
//     Ablauf von m_timeOutTime true, UNABHAENGIG davon ob ein Socket
//     existiert. Bei socket=nullptr ist das ein unbedingter Nullpointer-
//     Zugriff auf einen leeren shared_ptr -> WORLDSERVER-CRASH (nicht nur
//     Kick des Bots), sobald diese Session ueberhaupt einmal
//     WorldSession::Update() durchlaeuft.
//   - Zeile 495-496: `if (!m_Socket[CONNECTION_TYPE_REALM]) return false;`
//     direkt danach: World::UpdateSessions (World.cpp:3068-3076) loescht
//     jede Session, deren Update() false liefert, SOFORT im selben Tick
//     (m_sessions.erase + delete pSession). Eine per AddSession()
//     registrierte, socketlose Session wuerde sich also selbst binnen eines
//     World-Ticks zerstoeren, ausser Update()/IsConnectionIdle() werden
//     vorher abgefangen.
// Beide Punkte greifen NUR, wenn die Session ueberhaupt in die
// World::UpdateSessions-Schleife gelangt (d.h. nur bei AddSession()). Der
// Minimal-Footprint-Ansatz umgeht beide Absturzpunkte per Konstruktion,
// ohne WorldSession.h/.cpp anfassen zu muessen - das ist der Hauptgrund fuer
// die Entscheidung, nicht nur "geringerer Footprint als Bonus".
//
// Bereits jetzt (ungefaehrlich) verifizierte Guard-Stellen, die eine
// spaetere Minimal-Footprint-Implementierung ohnehin nicht braucht, weil sie
// nur bei echtem Socket ausgeloest werden (informativ fuer Runde B, falls
// doch Verhalten fuer Bots gewuenscht wird):
//   - WorldSession.cpp:349 Paket-Queue-Verarbeitung: `while (m_Socket[
//     CONNECTION_TYPE_REALM] && ...)` - bereits socket-sicher, keine Aenderung
//     noetig.
//   - WorldSession.cpp:458/472 Warden-Update: bereits an `m_Socket[
//     CONNECTION_TYPE_REALM] && ...` gebunden - bereits socket-sicher.
//   - WorldSession.cpp:151/184 (Konstruktor/Destruktor) `online=1/0`-Update
//     nur `if (sock)` - fuer Bots bleibt der `online`-Flag in der
//     account-Tabelle unveraendert (kosmetisch, kein Absturzrisiko).
//   - World.cpp:266-320 (AddSession_) macht Session-Limit-/Queue-Logik pro
//     Account (RemoveSession/AddQueuedPlayer) - fuer Minimal-Footprint
//     irrelevant, da diese Methode fuer Bots nie aufgerufen wird.
//
// Offen fuer Runde B (das eigentliche Session-Faking, NICHT Teil dieser
// Runde): der tatsaechliche Login-Trigger
// (HandlePlayerLoginOpcode()+HandleContinuePlayerLogin(), CharacterHandler.cpp
// :812-856) inklusive des asynchronen LoginQueryHolder-DB-Roundtrips und
// Player::LoadFromDB()/Map::AddPlayerToMap() wurde in dieser Runde bewusst
// NICHT implementiert oder getriggert - das haette einen echten Test gegen
// die laufende Produktiv-Weltinstanz/DB bedeutet (keine isolierte
// Testumgebung vorhanden), inklusive Server-Neustart mit Backup/DBErrors-
// Diff-Check. Das faellt unter die vom Nutzer vorgegebene Stopp-Regel bei
// echter Unsicherheit ueber Server-Stabilitaet und bleibt dediziert Runde B
// vorbehalten.
//
// --- Runde B (27.09.2026): Session-Faking-Kernstueck implementiert, LIVE-TEST
// (Server-Neustart + tatsaechlicher Login-Trigger) BEWUSST NICHT DURCHGEFUEHRT
// -----------------------------------------------------------------------
//
// Vollstaendiger Bericht: C:\LegionServer\reports\lcf2r64_2026-09-27_playerbots_rundeb.md
//
// Neue, in dieser Runde durch Lesen von CharacterHandler.cpp/WorldSession.h
// konkret aufgedeckte Bruchstellen (zusaetzlich zu Runde A):
//
// 1. IsLegitCharacterForAccount() (WorldSession.h:1948) prueft nur das private
//    Set _legitCharacters - das wird AUSSCHLIESSLICH in HandleCharEnum()
//    befuellt (CharacterHandler.cpp:300/331), also nur nachdem der
//    Char-Enum-Query-Roundtrip gelaufen ist. Ein Bot, der HandlePlayerLoginOpcode()
//    ohne vorherigen CharEnum aufruft, wuerde IMMER an dieser Pruefung
//    scheitern (KickPlayer(), kein Crash, aber funktionslos). Loesung: BotMgr
//    ruft vorher selbst HandleCharEnumOpcode() auf (oeffentliche Methode,
//    loest denselben CHAR_SEL_ENUM-Query aus, den ein echter Client beim
//    Charakterbildschirm ausloest) und wartet den Callback ab.
//
// 2. Die fuer HandleCharEnumOpcode()/HandleContinuePlayerLogin() noetigen
//    Async-DB-Callbacks (_queryProcessor / _queryHolderProcessor) werden
//    NUR von WorldSession::ProcessQueryCallbacks() (WorldSession.h:1901,
//    PRIVATE, "friend class World;") abgearbeitet - und DAS wiederum wird nur
//    aus WorldSession::Update() (WorldSession.cpp:461) heraus aufgerufen, dem
//    Update(), das laut Runde A wegen des IsConnectionIdle()-Nullpointer-
//    Absturzes fuer Bot-Sessions NIE aufgerufen werden darf. Ohne Aenderung
//    haetten Bot-Sessions also nie einen Weg gehabt, ihre eigenen
//    Login-Callbacks abzuarbeiten. Loesung (siehe WorldSession.h,
//    Kommentar direkt bei "friend class BotMgr;", Zeile ~1908): eine bewusst
//    minimale, rein additive Sichtbarkeits-Freigabe - `friend class BotMgr;`
//    direkt neben dem bestehenden `friend class World;` - damit BotMgr
//    ProcessQueryCallbacks() UND _legitCharacters direkt lesen kann, OHNE
//    WorldSession::Update()/IsConnectionIdle() jemals aufzurufen und OHNE
//    Vtable/ABI/Datenlayout zu aendern. Das ist die einzige Abweichung von
//    "WorldSession.h/.cpp gar nicht anfassen" in dieser Runde - bewusst, klein,
//    und ausschliesslich eine Zugriffsrechte-Lockerung fuer bereits
//    existierenden, unveraenderten Code.
//
// 3. MapUpdate.Threads steht auf diesem Server (server/worldserver.conf UND
//    server-asan/worldserver.conf) auf 1 - d.h. es gibt nur einen einzigen
//    Map-Update-Worker-Thread, und der Callback, der am Ende
//    Map::AddPlayerToMap() fuer den Bot aufruft, laeuft im selben
//    Weltserver-Hauptthread-Kontext wie bei jedem echten Spieler-Login auch
//    (World::Update() -> ProcessQueryCallbacks() -> Session-Callback-Kette).
//    Kein neues Thread-Race gegenueber dem regulaeren Login-Pfad.
//
// 4. SendPacket() (WorldSession.cpp:250-254), PlayerDisconnected()
//    (WorldSession.cpp:187-191) und der Destruktor (WorldSession.cpp:167-174)
//    wurden gegengelesen und sind alle bereits socket-sicher (Nullpruefung vor
//    jedem Zugriff) - kein zusaetzlicher Absturzpfad ausser dem in Runde A
//    gefundenen und hier vermiedenen Update()/IsConnectionIdle()-Fall.
//    PlayerDisconnected() liefert fuer eine Bot-Session immer true (beide
//    Sockets sind/bleiben nullptr) - bestaetigtes Verhalten, siehe Runde-B-
//    Bericht Abschnitt 4 (kosmetisch: Bot koennte in Chat/Gruppe/Gilde als
//    "getrennt" auftauchen, kein Stabilitaetsrisiko).
//
// Implementiert (Details in der .cpp): CreateBotAccount() (nutzt
// AccountMgr::CreateAccount(), bnetAccountId=0 - kein BNet-Account noetig,
// da der Bot nie ueber bnetserver authentifiziert), RequestCreateBotCharacter()
// (baut WorldSession(socket=nullptr) + treibt HandleCharCreateOpcode() exakt
// wie ein echter Client), RequestBotLogin() (treibt HandleCharEnumOpcode()
// dann HandlePlayerLoginOpcode()+HandleContinuePlayerLogin(), normalzero-Stil),
// Tick() (ruft NUR ProcessQueryCallbacks() pro Bot-Session auf, NIE Update()).
//
// BEWUSST NICHT DURCHGEFUEHRT (Stopp-Regel genutzt, siehe Bericht Abschnitt 6):
// Server-Neustart mit der neu gebauten Binary und der tatsaechliche
// Live-Login-Test (`.bottest`-Befehle gegen den laufenden Produktivserver).
// Grund: vier zuvor nie ausgefuehrte, verkettete Async-Codepfade
// (Accounterstellung -> Charaktererstellung -> CharEnum -> Login) wuerden
// beim ersten echten Lauf gleichzeitig zum ersten Mal gegen die echte
// Produktiv-DB und die laufende Weltinstanz getestet - das ist trotz aller
// in dieser Runde gefundenen Absicherungen genau die Art "echter
// Unsicherheit ueber Server-Stabilitaet", bei der die Nutzervorgabe
// STOPPEN vorschreibt. Naechster Schritt (Runde C, siehe Bericht): dieselbe
// Implementierung mit denselben `.bottest`-Befehlen schrittweise gegen den
// LAUFENDEN Produktivserver ausfuehren, aber unter direkter Aufsicht des
// Nutzers und mit Backup+DBErrors-Diff nach JEDEM einzelnen Schritt (nicht
// nur am Ende), nicht automatisiert in einer Sitzung.
// ---------------------------------------------------------------------------

#include "Define.h"
#include "ObjectGuid.h"
#include "BotCharacter.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>

class DynamicObject;
class Group;
class IBotCharacter;
class Player;
class Unit;
class WorldObject;
class WorldSession;
struct SpellInfo;

// --- Kampf-KI (Kampfmechaniken/Rotationen aller Klassen/Skillungen, siehe voller Design-Kommentar
// bei BotMgr::ProcessBotCombatAI() unten) --------------------------------------------------------
//
// Grundproblem, das dieses Design loest: Faehigkeiten-Namen/-Reihenfolgen sind aus Community-
// Recherche fuer Patch 7.3.5 belegbar, aber NUMERISCHE Spell-IDs sind es nicht zuverlaessig (siehe
// BotMgr::ResolveSpellIdByName()-Kommentar) - Faehigkeiten wurden zwischen Erweiterungen und sogar
// innerhalb von Legion mehrfach umgestaltet/umnummeriert. Deshalb sind Rotationen hier ausschliesslich
// ueber den ENGLISCHEN Faehigkeitsnamen (aus Recherche, siehe BotMgr.cpp-Kommentar bei
// g_BotSpecRotations) definiert und werden erst zur Laufzeit gegen das tatsaechlich auf DIESEM Server
// geladene Spell.db2 (Build 26972) aufgeloest - garantiert korrekt fuer genau diese Version, keine
// geratene ID.

enum class BotRole : uint8
{
    Unknown,
    Tank,
    Healer,
    MeleeDps,
    RangedDps
};

enum class BotRotationCondition : uint8
{
    Always,                  // sofort einsetzen, sobald bereit (typischer Fuellschlag/On-CD-Skill)
    TargetHealthPctBelow,    // Ziel (Gegner ODER gewaehltes Heilziel, je nach Rolle) unter X% Leben
    SelfHealthPctBelow,      // eigenes Leben unter X% (Defensiv-CDs/Selbstheilung)
    ResourceAtLeast,         // eigene Ressource (ConditionAuxPower) >= X
    AuraMissingOnSelf,       // Buff (ConditionAuxSpellName) NICHT aktiv auf dem Bot selbst
    AuraPresentOnSelf,       // Buff/Proc (ConditionAuxSpellName) IST aktiv auf dem Bot selbst
    AuraMissingOnTarget      // Aura (ConditionAuxSpellName, z.B. DoT/HoT) NICHT aktiv auf dem Ziel
};

// Runde 3: fuer Skillungen, deren Kernmechanik ZWEI verschiedene Ziele pro Rotation braucht (bisher
// nur Discipline Priest - Atonement heilt ueber Schaden an einem GEGNER, waehrend das eigentliche
// Heilziel ein VERBUENDETER mit Atonement-Buff ist). Ohne dieses Feld waere das Ziel starr an die
// Rolle gekoppelt (Heiler->immer Verbuendeter, DPS/Tank->immer Gegner), was fuer Disc strukturell
// falsch ist. RoleDefault (Standard, alle bisherigen 29 Skillungen nutzen implizit nur diesen Wert)
// aendert nichts am bestehenden Verhalten.
enum class BotRotationTargetOverride : uint8
{
    RoleDefault,     // wie bisher: Heiler-Rolle -> SelectBotHealTarget(), sonst -> SelectBotCombatTarget()
    ForceEnemy,      // IMMER der aktuelle Kampf-Gegner, unabhaengig von der Rolle der Skillung
    ForceHealTarget  // IMMER das per SelectBotHealTarget() gewaehlte Gruppenmitglied
};

// Ein einzelner Prioritaetseintrag. SpellName ist die recherchierte, patch-7.3.5-genaue Bezeichnung -
// die einzige "Wahrheitsquelle" in diesem Modul; ResolvedSpellId/ResolvedAuxSpellId werden EINMALIG
// pro Prozesslauf von BotMgr::GetOrResolveSpecRotation() befuellt (siehe dort) und sind bewusst
// `mutable`, weil g_BotSpecRotations (BotMgr.cpp) eine statische, unveraenderliche Datentabelle ist,
// die dennoch verzoegert (lazy) einmalig angereichert werden muss.
struct BotRotationStep
{
    char const* SpellName;
    BotRotationCondition Condition = BotRotationCondition::Always;
    float ConditionValue = 0.0f;
    uint32 ConditionAuxPower = 0;                  // nur fuer ResourceAtLeast (Powers-Enum-Wert)
    char const* ConditionAuxSpellName = nullptr;    // nur fuer die drei Aura*-Bedingungen
    BotRotationTargetOverride TargetOverride = BotRotationTargetOverride::RoleDefault;
    mutable uint32 ResolvedSpellId = 0;
    mutable uint32 ResolvedAuxSpellId = 0;
};

// Eine vollstaendige Rotationstabelle fuer EINE Spezialisierung (ChrSpecialization-Id, dieselbe
// Nummerierung wie bereits in BotMgr::GetArtifactItemForSpec()/GetDefaultSpecForClass() verwendet -
// siehe dort, Runde 143). Bewusst als flache, von oben nach unten ausgewertete Prioritaetsliste
// (kein Verhaltensbaum) - der erste Schritt, dessen Bedingung UND Ressourcen/Cooldown/Reichweite
// passen, wird gecastet, danach kehrt ProcessBotCombatAI() fuer diesen Tick zurueck (max. 1 Zauber
// pro Tick, da alle Schritte dieselbe GCD-Ressource teilen - siehe dortiger Kommentar).
struct BotSpecRotation
{
    uint32 SpecId = 0;
    uint32 SpellFamily = 0;          // SpellFamilyNames-Enum-Wert, filtert Namenskollisionen zwischen Klassen
    BotRole Role = BotRole::Unknown;
    std::vector<BotRotationStep> Priority;

    // Generische Boss-Mechanik-Reaktionen (siehe voller Design-Kommentar bei
    // BotMgr::ProcessBotMechanicReactions() unten) - spec-weit statt pro Prioritaetsschritt, weil
    // Interrupt/Dispel keine normalen Rotationsschritte sind (kein "Ziel je nach Rolle", kein
    // GCD-Slot-Wettbewerb mit der eigentlichen Rotation - sie werden VOR der Rotation geprueft und
    // preemptieren sie fuer den aktuellen Tick). Beide optional (nullptr = diese Skillung hat
    // keine/wird in dieser Runde nicht dafuer verdrahtet).
    char const* InterruptSpellName = nullptr;
    char const* DispelSpellName = nullptr;

    mutable bool ResolvedOnce = false;
    mutable uint32 ResolvedInterruptSpellId = 0;
    mutable uint32 ResolvedDispelSpellId = 0;
};

class TC_GAME_API BotMgr
{
public:
    static BotMgr* instance();

    BotMgr(BotMgr const&) = delete;
    BotMgr(BotMgr&&) = delete;
    BotMgr& operator=(BotMgr const&) = delete;
    BotMgr& operator=(BotMgr&&) = delete;

    // --- Interface fuer spaetere Runden (aktuell ohne Wirkung) -------------
    //
    // CreateBot: soll spaeter einen neuen Bot-Charakter fuer einen
    // Owner-Account anlegen (Master-Bot-Modell wie mod-playerbots
    // ".playerbots bot add"). In dieser Runde: gibt immer false zurueck
    // und legt nichts an - siehe Log-Hinweis in der .cpp.
    bool CreateBot(uint32 ownerAccountId, std::string const& botCharacterName);

    // RemoveBot: soll spaeter einen Bot sauber ausloggen/entfernen.
    // Aktuell: no-op, gibt false zurueck (kein Bot kann existieren).
    bool RemoveBot(ObjectGuid botGuid);

    // Lookup fuer spaetere Verwendung durch Script-Hooks/Kommandos.
    IBotCharacter* GetBot(ObjectGuid botGuid) const;
    std::size_t GetBotCount() const { return _bots.size(); }

    // --- Runde B (27.09.2026): Session-Faking-API, siehe Kopfkommentar -----
    //
    // Phasen bewusst getrennt (statt einem grossen CreateBot()-Rundumschlag),
    // damit ein GM-Befehl (bot_commandscript.cpp, ".bottest ...") jede Phase
    // einzeln ausloesen und dazwischen Server.log/DBErrors.log pruefen kann -
    // passt zur Vorgabe "inkrementell testen".

    // Phase 1: legt (idempotent) einen dedizierten Bot-Account an, per
    // AccountMgr::CreateAccount() (dieselbe Funktion, die auch das
    // ".account create"-GM-Kommando nutzt) - kein eigener SQL-Insert, kein
    // Umgehen der bestehenden SRP6-Logik. bnetAccountId bewusst 0: der Bot
    // authentifiziert sich nie ueber bnetserver, daher ist kein echter
    // BNet-Account noetig (beantwortet Runde-A-Offene-Frage 1).
    // Rueckgabe true bei Erfolg ODER wenn der Account bereits existiert
    // (idempotent), false nur bei einem echten Fehler.
    bool CreateBotAccount(std::string const& accountName, std::string const& password, uint32& outAccountId);

    // Phase 2: baut fuer diesen Account eine socketlose WorldSession auf (falls
    // noch keine existiert) und treibt HandleCharCreateOpcode() exakt wie ein
    // echter Client (gleiche DBC-Validierung, gleiche SaveToDB-Logik) - kein
    // manueller SQL-Insert in `characters`. Asynchron (Namens-Check-Query u.a.);
    // Ergebnis erst nach mehreren Tick()-Aufrufen sichtbar (GetBotSessionState()).
    bool RequestCreateBotCharacter(uint32 accountId, std::string const& charName,
        uint8 race, uint8 charClass, uint8 sex);

    // Phase 3: treibt HandleCharEnumOpcode() (fuellt _legitCharacters, siehe
    // Kopfkommentar Punkt 1), danach HandlePlayerLoginOpcode()+
    // HandleContinuePlayerLogin() (normalzero/LegionPlayerBot-Muster). Der Bot
    // bleibt danach nur "eingeloggt in der Welt" - keinerlei Movement/Kampf-KI
    // wird gestartet.
    bool RequestBotLogin(uint32 accountId);

    BotCharacterState GetBotSessionState(uint32 accountId) const;
    Player* GetBotPlayer(uint32 accountId) const;

    // Muss regelmaessig aufgerufen werden (aus OnPlayerUpdate/OnWorldUpdate),
    // damit die Bot-Sessions ihre eigenen DB-Query-Callbacks abarbeiten -
    // ruft NUR ProcessQueryCallbacks() auf, NIEMALS Update() (siehe Runde A).
    void Tick(uint32 diff);

    // --- Runde N (27.09.2026): Fix fuer den Runde-M-Shutdown-Absturz ---------
    //
    // Muss aus WorldScript::OnShutdown() aufgerufen werden (bot_scriptloader.cpp),
    // NICHT spaeter. Grund (vollstaendig per cdb+ln-Gegenprobe + Quellcode-Review
    // verifiziert, siehe Bericht lcf2r76_2026-09-27_playerbots_runde_n.md):
    // Bot-Sessions werden nie per World::AddSession() registriert (Minimal-
    // Footprint, Runde A) und werden deshalb von World::KickAll() nie erreicht.
    // Ohne diesen Fix ueberlebt eine eingeloggte Bot-Session bis zum
    // BotMgr::instance()-atexit-Destruktor, der erst NACH main()-Rueckkehr laeuft -
    // also NACHDEM Main.cpp's dbHandle-RAII-Guard bereits StopDB()/
    // CharacterDatabase.Close() (DatabaseWorkerPool::Close(), _ioContext.reset())
    // ausgefuehrt hat. Der dortige WorldSession::~WorldSession()->LogoutPlayer()->
    // Player::SaveToDB()->DatabaseWorkerPool::CommitTransaction()-Aufruf
    // dereferenziert dann boost::asio::post(_ioContext->get_executor(), ...) auf
    // dem bereits zurueckgesetzten (nullptr) _ioContext - exakt der beobachtete
    // Absturz. sScriptMgr->OnShutdown() (Main.cpp:359) laeuft dagegen synchron
    // VOR jeder dieser main()-lokalen RAII-Aufraeumaktionen - DB-Pool und
    // Weltkarten sind zu diesem Zeitpunkt garantiert noch vollstaendig intakt.
    void LogoutAllBots();

    // --- Runde R (27.09.2026): sauberes Einzel-Logout waehrend laufendem Betrieb ---
    //
    // Andere Ueberladung/anderer Speicher als das alte GUID-basierte RemoveBot() oben
    // (das gehoert zum bislang inerten _bots/IBotCharacter-Interface aus Runde 62 und
    // bleibt unveraendert No-Op, weil dieser Weg nie einen echten Bot erzeugt). Diese
    // account-basierte Variante arbeitet auf _botSessions, dem seit Runde B tatsaechlich
    // genutzten Speicher, und ist das Gegenstueck zu RequestBotLogin(): entfernt EINEN
    // bereits eingeloggten Bot sauber aus der Welt, WAEHREND der Server normal
    // weiterlaeuft - kein Shutdown, kein Prozessende. Nutzt denselben
    // Session->LogoutPlayer(true)-Aufruf, der in LogoutAllBots() (Runde N/P) bereits als
    // sicher bestaetigt ist (dort waehrend WorldScript::OnShutdown(), synchron vor
    // StopDB()). Der einzige Unterschied ist der Aufrufzeitpunkt (laufender
    // World-Update-Betrieb statt Shutdown) - der DB-Pool ist in beiden Faellen
    // garantiert vollstaendig aktiv, im laufenden Betrieb sogar trivial (kein
    // Teardown-Wettlauf moeglich). Die Session selbst bleibt danach in _botSessions
    // bestehen (State zurueck auf STATE_UNINITIALIZED, GetPlayer()==nullptr) - ein
    // erneuter ".bottest login <accountId>" im selben Prozesslauf bleibt dadurch
    // moeglich, ohne createaccount/createchar zu wiederholen.
    bool LogoutBot(uint32 accountId);

    // --- Runde S (27.09.2026): EIN einziger, isolierter Bewegungstest -------
    //
    // Code-Review vor dieser Methode (voller Bericht:
    // C:\LegionServer\reports\lcf2r81_2026-09-27_playerbots_runde_s.md):
    // MotionMaster::MovePoint() fuer einen Player nutzt denselben
    // PointMovementGenerator<Player>/MoveSplineInit-Mechanismus wie fuer eine
    // Creature - der resultierende SMSG_MONSTER_MOVE-Broadcast laeuft ueber
    // Unit::SendMessageToSet() -> Player::SendMessageToSetInRange() ->
    // WorldSession::SendPacket(), also GENAU denselben bereits vielfach
    // bestaetigten Null-Socket-Guard-Pfad ("Prevented sending of [...] to non
    // existent socket 0"), den auch alle bisherigen Bot-Runden schon getroffen
    // haben (z. B. beim Login/Logout). Die Ankunftserkennung
    // (PointMovementGenerator::DoUpdate -> movespline->Finalized()) ist rein
    // zeitbasiert/serverseitig, OHNE Client-Ack-Callback (anders als z. B.
    // Knockback/erzwungene Bewegung mit MSG_MOVE_*_ACK-Handshake) - kein
    // Haenger-Risiko durch einen ausbleibenden Client identifiziert.
    // Map::Update() (Map.cpp:789, "player->Update(t_diff)") ruft fuer JEDEN
    // Spieler auf der Karte inkl. Bot unconditional Update() auf (treibt
    // MotionMaster::UpdateMotion()) - das laeuft laut Runde R bereits seit
    // 90+ Sekunden nachweislich stabil, MovePoint() aktiviert hier nur einen
    // neuen MotionMaster-Generator auf demselben, bereits als sicher
    // bestaetigten Update-Zyklus.
    //
    // Bewusst MINIMAL gehalten: feste, kurze Distanz (8 Yards), gerade Linie
    // in aktueller Blickrichtung, generatePath=false (keine Pfadfindung),
    // EINMALIGER Ausloeser (kein Patrouillieren/Wiederholung). Eigener
    // expliziter GM-Befehl (".bottest move <accountId>") statt Verdrahtung in
    // den bestehenden 30s-Idle-Diagnose-Tick, damit der Test kontrolliert
    // genau einmal ausgeloest werden kann.
    bool MoveBotTestStep(uint32 accountId);

    // --- Runde U (27.09.2026): EIN einziger Navmesh-Pfadfindungstest (generatePath=true) ---
    //
    // Voller Bericht: C:\LegionServer\reports\lcf2r83_2026-09-27_playerbots_runde_u.md
    // Code-Review vor dieser Methode (PathGenerator.cpp, MoveSplineInit.cpp, MMapManager.cpp
    // vollstaendig gelesen): MoveSplineInit::MoveTo(dest, generatePath=true) konstruiert EIN
    // PathGenerator-Objekt AUF DEM STACK (kein Callback, keine asynchrone Ressource) und ruft
    // synchron CalculatePath() auf, WAEHREND desselben Map-Update-Tick, der auch MovePoint()
    // selbst ausgeloest hat - kein neuer Thread-/Ownership-Kontext gegenueber dem seit Runde S/T
    // bestaetigt sicheren MotionMaster-Update-Zyklus.
    //
    // MMap-Ladezustand ist UNKRITISCH: PathGenerator::PathGenerator() holt _navMesh/_navMeshQuery
    // per MMapManager::GetNavMesh()/GetNavMeshQuery() (reiner Map-Lookup, kein Datei-IO an dieser
    // Stelle - Tiles werden beim Betreten der Karte/des Grids geladen, nicht hier). Sind sie (aus
    // welchem Grund auch immer) nicht geladen, liefert GetNavMesh() nullptr (kein Crash) und
    // PathGenerator::CalculatePath() faellt ueber den bereits vorhandenen Guard
    // "if (!_navMesh || !_navMeshQuery || ... ) { BuildShortcut(); return true; }" automatisch auf
    // dieselbe gerade-Linie-Bewegung zurueck, die Runde S/T bereits live bestaetigt haben - striktes
    // Downgrade, kein neuer Fehlerzustand.
    //
    // Player-vs-Creature-Unterschied explizit geprueft (PathGenerator::BuildPolyPath): die einzigen
    // Stellen, die _sourceUnit auf Creature* casten (CanFly()/CanSwim() fuer Shortcut-Sonderfaelle
    // bei fehlendem Poly), sind JEDESMAL zusaetzlich per "_sourceUnit->GetTypeId() == TYPEID_UNIT"
    // abgesichert - fuer einen Player (unseren Bot) werden diese Zweige nie erreicht, kein
    // ungeschuetzter Cast moeglich. Kein Player-spezifischer Sonderpfad in PathGenerator gefunden.
    //
    // Verbleibendes, NICHT durch Code-Review ausschliessbares Restrisiko: die eigentliche
    // Recast/Detour-Bibliotheksarbeit in BuildPolyPath()/findPath()/findSmoothPath() (dtNavMeshQuery)
    // wurde bislang ausschliesslich mit Creature-Aufrufern in der Praxis beobachtet - ein erster
    // Live-Test mit einem Player-Bot an genau dieser Stelle steht noch aus. Deshalb bewusst als
    // EIGENER, von MoveBotTestStep() UNABHAENGIGER Testpfad implementiert (nicht einfach ein Flag an
    // der bestehenden, bereits getesteten Methode aendern) - ein Fehlschlag hier gefaehrdet nicht den
    // bereits bestaetigt stabilen generatePath=false-Pfad.
    bool MoveBotTestStepPath(uint32 accountId);

    // --- Runde T (27.09.2026): mehrfache Bewegungen / einfaches Pendeln -----
    //
    // Voller Bericht: C:\LegionServer\reports\lcf2r82_2026-09-27_playerbots_runde_t.md
    // Baut direkt auf dem Runde-S-Einzeltest auf: derselbe MotionMaster::MovePoint()/
    // PointMovementGenerator<Player>-Mechanismus, diesmal MEHRFACH hintereinander neu
    // angesetzt (Punkt A -> Punkt B -> Punkt A -> ... ueber mehrere Zyklen), um
    // Wiederholbarkeit/Langzeitstabilitaet statt nur eines Einzelaufrufs zu testen.
    // Weiterhin generatePath=false (gerade Linie, keine Pfadfindung - bewusst NICHT
    // Teil dieser Runde, siehe Runde-S-Empfehlung Punkt 2).
    //
    // Ankunfts-/Abschlusserkennung: TICK-basiert ueber den bereits vorhandenen
    // BotMgr::Tick()-Mechanismus, per player->movespline->Finalized() (exakt der in
    // PointMovementGenerator<T>::DoUpdate() selbst verwendete, rein zeitbasierte
    // Test - kein Client-Ack/Callback noetig, siehe Runde-S-Kopfkommentar oben).
    // Solange Finalized()==false wird NICHT erneut MovePoint() aufgerufen - die
    // naechste Etappe wird erst gestartet, nachdem die aktuelle nachweislich
    // abgeschlossen ist.
    //
    // Speicherleck-/Ressourcenpruefung (Code-Review vor der Implementierung,
    // MotionMaster.cpp gelesen): MotionMaster::MovePoint() ruft intern
    // Mutate(new PointMovementGenerator<Player>(...), MOTION_SLOT_ACTIVE) auf.
    // MotionMaster::Mutate() (MotionMaster.cpp:810-833) prüft VOR dem Einsetzen des
    // neuen Generators, ob im Ziel-Slot (hier MOTION_SLOT_ACTIVE) bereits ein alter
    // Generator sitzt, und loescht ihn explizit (DirectDelete() ausserhalb eines
    // laufenden Updates, DelayedDelete() waehrend eines laufenden Updates) BEVOR der
    // neue Zeiger eingesetzt wird - ein wiederholtes MovePoint() auf denselben Slot
    // kann sich also NICHT stapeln/lecken, das ist bereits im Core so vorgesehen und
    // wird von jedem Creature-Pathing/jeder Spielerbewegung im Core taeglich genauso
    // genutzt. Kein manuelles MotionMaster::Clear() noetig oder sinnvoll (wuerde
    // zusaetzlich den MOTION_SLOT_IDLE-Default-Generator anfassen, den wir gar nicht
    // beruehren wollen). Diese Runde ruft MovePoint() ausserdem ausschliesslich NACH
    // bestaetigtem Finalized()==true der vorherigen Etappe auf - der alte Generator
    // hat sich zu diesem Zeitpunkt (DoUpdate() gibt false zurueck) im selben
    // Map-Update-Tick ohnehin bereits selbst aus dem Motion-Stack entfernt.
    //
    // Bewusst weiterhin ein expliziter GM-Befehl (".bottest patrol <accountId>
    // <zyklen>") statt Verdrahtung in den 30s-Idle-Tick - ein Zyklus = ein
    // vollstaendiger Rundlauf A->B->A. cycles wird auf [1, 20] geklemmt (Sicherheits-
    // obergrenze gegen versehentliche Dauerlast). Nur EIN Patrol pro Bot-Session
    // gleichzeitig moeglich (Aufruf schlaegt fehl, wenn bereits ein Patrol aktiv ist).
    bool StartBotPatrol(uint32 accountId, uint32 cycles);

    // Notbremse: bricht ein laufendes Patrol sofort ab (Bot bleibt an der aktuellen
    // Position stehen, kein weiterer MovePoint()-Aufruf). Wird auch intern von
    // LogoutBot() genutzt, damit ein Logout waehrend eines laufenden Patrols keine
    // "Zombie"-Patrol-Fortsetzung nach einem erneuten Login hinterlaesst.
    void StopBotPatrol(uint32 accountId);

    // Fuer '.bottest status': aktueller Fortschritt, ohne den generischen
    // BotCharacterState/BotState zu ueberladen.
    bool IsBotPatrolActive(uint32 accountId) const;
    uint32 GetBotPatrolCyclesCompleted(uint32 accountId) const;
    uint32 GetBotPatrolCyclesTotal(uint32 accountId) const;

    // --- Runde 93 (28.09.2026): EIN einziger Kartenwechsel-Live-Test ---------
    //
    // Voller Vorlauf: lcf2r89 (Code-Review Player::TeleportTo(), Blocker gefunden:
    // Player::TeleportTo() schliesst einen Kartenwechsel NICHT ab, der eigentliche
    // Map-Wechsel [CreateMap/Relocate/SetMap/AddPlayerToMap fuer die NEUE Map] passiert
    // ausschliesslich in WorldSession::HandleMoveWorldportAck(), das normalerweise nur
    // durch den Client-Ack MSG_MOVE_WORLDPORT_ACK ausgeloest wird) und lcf2r90 (volle
    // Zeile-fuer-Zeile-Review von HandleMoveWorldportAck(), MovementHandler.cpp:46-231:
    // kein neuer Absturzpfad, SendPacket() bereits generisch socket-sicher, einziger
    // Restrisikopunkt sind die beiden Homebind-Fallback-Pfade bei ungueltigem
    // Teleport-Ziel - vermeidbar durch eine garantiert gueltige, offene
    // Kontinent-Zielkoordinate ohne Instanz/Dungeon).
    //
    // Analog zum bereits produktiven Login-Muster (Runde B/lcf2r64:
    // HandlePlayerLoginOpcode()+HandleContinuePlayerLogin() manuell statt durch einen
    // echten zweiten Client-Handshake): ruft player->TeleportTo(mapId, x, y, z, o) auf,
    // DANACH sofort manuell botSession->HandleMoveWorldportAck() (oeffentlich,
    // parameterlos, Header-Kommentar "for server-side calls"), um den sonst nie
    // eintreffenden Client-Ack zu ersetzen. Nur fuer bereits eingeloggte Bots
    // (STATE_IN_WORLD) sinnvoll - kein automatischer Login-Trigger hier.
    //
    // Diagnose-Logging exakt nach lcf2r90-Empfehlung (Punkt 4): TC_LOG_INFO-Marker vor/
    // nach TeleportTo() und vor/nach HandleMoveWorldportAck(), damit ein Abrutschen in
    // einen der beiden Homebind-Fallback-Pfade sofort sichtbar ist. Map::AddPlayerToMap()
    // selbst ist innerhalb von HandleMoveWorldportAck() (anderes Compilation-Unit,
    // MovementHandler.cpp) gekapselt und liefert BotMgr keinen direkten Rueckgabewert -
    // als Ersatzindikator wird nach dem Aufruf player->GetMapId() gegen das gewuenschte
    // Ziel verglichen (weicht die tatsaechliche Map vom Ziel ab, ist das ein starkes
    // Indiz fuer den Homebind-Fallback-Pfad, nicht fuer einen erfolgreichen Kartenwechsel).
    bool TeleportBot(uint32 accountId, uint32 mapId, float x, float y, float z, float orientation = 0.0f);

    // --- Runde 122 (28.09.2026): EIN einziger Kampf-Live-Test (Auto-Attack) --------------
    //
    // Voller Code-Review-Vorlauf: lcf2r121 (Kopfkommentar-Referenz,
    // C:\LegionServer\reports\lcf2r121_2026-09-28_playerbots_kampf_loot_codereview.md) - der komplette
    // Auto-Attack-Callstack (Unit::Attack()/AttackerStateUpdate()/CalculateMeleeDamage()/
    // SendAttackStateUpdate()/DealMeleeDamage()) hat KEINEN ungeschuetzten GetSession()/m_Socket[]-
    // Zugriff, alle Sends laufen durch den bereits abgesicherten WorldSession::SendPacket()-Guard.
    // Bewusst der einfachste Kampf-Fall: reiner Melee-Auto-Attack (player->Attack(target, true)), KEIN
    // Spell-Cast (der On-Hit-Item-Proc-Pfad CastItemCombatSpell() bleibt fuer diesen ersten Test
    // aussen vor, siehe lcf2r121 Abschnitt 1.5 - Testbot hat keine Waffe mit On-Hit-Enchant).
    //
    // targetGuid ist die DB-Spawn-Id (dieselbe "guid"-Spalte wie in der `creature`-Tabelle, NICHT die
    // laufzeit-volle ObjectGuid) - exakt dasselbe Muster wie in cs_npc.cpp (z. B. ".npc move"):
    // sObjectMgr->GetCreatureData(targetGuid) liefert Entry+Map, ObjectGuid::Create<HighGuid::Creature>(...)
    // baut daraus die volle ObjectGuid, ObjectAccessor::GetCreature(*player, guid) findet das lebende
    // Objekt (nur wenn aktuell im selben Grid/derselben Map geladen wie der Bot).
    bool StartBotAttack(uint32 accountId, ObjectGuid::LowType targetGuid);

    // Gegenstueck: bricht den per StartBotAttack() begonnenen Auto-Attack sofort ab
    // (player->AttackStop()) - fuer den vorgeschriebenen Testablauf "EIN Autoattack-Zyklus, dann
    // sofort AttackStop()".
    void StopBotAttack(uint32 accountId);

    // --- Runde 129 (28.09.2026): Testbot-Unverwundbarkeit (NUR fuer '.bottest'-Livetests) ------
    //
    // Root-Cause aus Runde 128: ein frischer Level-1/60-HP-Testbot stirbt am Gegenschlag eines
    // rechnerisch neutralen Ziels, bevor ein voller Kill-Zyklus je erreicht wird (Selbstverteidigung
    // ist unabhaengig von der Aggro-Reaktion). Gewaehlter Loesungsweg (siehe Bewertung im Bericht
    // lcf2r129): WEDER ein core-weiter Damage-Bypass in Unit::DealDamage (architektonisch
    // ausgeschlossen - "scripts" [dieses Modul] linkt GEGEN "game", nicht umgekehrt, ein Include von
    // BotMgr.h in Unit.cpp waere ein zirkulaerer Modul-Abhaengigkeitsbruch) NOCH UNIT_FLAG_IMMUNE_TO_NPC
    // (per Unit::_IsValidAttackTarget()-Code-Review, Unit.cpp ~8435-8436, SYMMETRISCH - wuerde auch
    // den eigenen ausgehenden player->Attack()-Aufruf gegen das Ziel blockieren, unbrauchbar). Stattdessen:
    // dieselbe bereits im Core existierende, oeffentliche Unit::ApplySpellImmune(spellId, IMMUNITY_DAMAGE,
    // SPELL_SCHOOL_MASK_ALL, apply)-Route, die auch echte Content-Auren wie Divine Shield nutzen
    // (SpellInfo.cpp:3544) - Unit::CalculateMeleeDamage() prueft bereits target->IsImmunedToDamage()
    // (Unit.cpp:1350) VOR jedem Melee-Schadenswert, kein neuer Code im Kern noetig, nur ein bereits
    // oeffentlicher Aufruf aus BotMgr.cpp heraus. spellId ist ein reiner Platzhalter-Schluessel (keine
    // echte Spell-ID/kein Aura-Sichtbarkeitseffekt), dient nur dem symmetrischen apply=true/false-Paar
    // in Unit::ApplySpellImmune() (siehe Unit.cpp:7984-7999, Schluessel-Match ueber (schoolMask, spellId)).
    // Wirkt NUR eingehenden Schaden - der Bot kann weiterhin normal per StartBotAttack() angreifen und
    // Schaden austeilen. Ausschliesslich ueber den GM-Befehlspfad ('.bottest invuln') fuer EINEN
    // konkreten Bot-Account schaltbar, nie automatisch/global, nie fuer echte Spieler erreichbar (kein
    // Aufrufpfad ausserhalb von BotMgr existiert dafuer). IMMER nach dem Test wieder mit enable=false
    // aufrufen (sonst bleibt die Immunitaet ueber Logout/Login hinweg am Player-Objekt bestehen, bis
    // der Prozess neu startet oder der Char neu geladen wird).
    bool SetBotTestInvulnerable(uint32 accountId, bool enable);

    // --- Runde 129 (28.09.2026): Loot-Implementierung nach Plan aus lcf2r128 Abschnitt 7 ------
    //
    // 3-Schritt-Opcode-Nachbau, DIREKT/SYNCHRON ueber die bereits oeffentlichen
    // WorldSession::Handle*Opcode()-Methoden (siehe LootHandler.cpp fuer die echten Signaturen/
    // Feldnamen) - exakt dasselbe Direktaufruf-Muster wie StartBotAttack()/TeleportBot() oben, NICHT
    // ueber QueuePacket()+Update() (fuer unsere socketlosen Bot-Sessions architektonisch ausgeschlossen,
    // Kopfkommentar-Referenz "Runde A"). targetGuid ist wie bei StartBotAttack() die DB-Spawn-Id aus
    // der `creature`-Tabelle (Spalte "guid"), NICHT die Laufzeit-ObjectGuid - dieselbe
    // Map::GetCreatureBySpawnIdStore()-Aufloesung wie in StartBotAttack() wird wiederverwendet, diesmal
    // aber muss das Ziel BEREITS TOT sein (Gegenstueck zur Lebend-Pruefung in StartBotAttack()).
    // Ablauf: (1) WorldPackets::Loot::LootUnit{Unit=target->GetGUID()} -> HandleLootOpcode() (fuellt
    // creature->loot server-seitig via Player::SendLoot()/Loot::FillLoot(), inkl. Gruppen-/Quest-Item-
    // Regeln - kein Roundtrip-Parsing eines SMSG_LOOT_RESPONSE noetig, wir lesen creature->loot direkt).
    // (2) fuer jeden loot-berechtigten, noch nicht gelooteten, nicht-Waehrungs-Slot in target->loot.items:
    // WorldPackets::Loot::LootItem{Loot=[{Object=target->loot.GetGUID(), LootListID=Index+1}, ...]} ->
    // HandleAutostoreLootItemOpcode() (reicht dieselbe Player::StoreNewItem()-Logik durch wie ein echter
    // Client). (3) WorldPackets::Loot::LootRelease{Unit=target->GetGUID()} -> HandleLootReleaseOpcode()
    // (setzt UNIT_DYNFLAG_LOOTABLE zurueck, schliesst den Vorgang sauber ab). Rueckgabewert true, wenn
    // Schritt (1) angelaufen ist (Item-Anzahl kann 0 sein, z. B. leere Loot-Tabelle - kein Fehler).
    bool BotLootTarget(uint32 accountId, ObjectGuid::LowType targetGuid);

    // --- Runde 132 (28.09.2026): Tod-Handling (Release/Graveyard/Resurrection Sickness) --------
    //
    // Code-Review-Vorlauf (Player.cpp/MiscHandler.cpp/NPCHandler.cpp gegengelesen, kein neuer
    // Core-Code noetig, dasselbe Direktaufruf-Muster wie BotLootTarget()/StartBotAttack() oben):
    // Der reguläre Client-Ablauf beim Tod ist NICHT eine einzelne Funktion, sondern drei bereits
    // oeffentliche WorldSession-/Player-Methoden hintereinander, ausgeloest durch
    // CMSG_REPOP_REQUEST (WorldSession::HandleRepopRequest(), MiscHandler.cpp:62):
    //   1. Player::KillPlayer() (Player.cpp:4497) - nur falls getDeathState()==JUST_DIED (Race-
    //      Fenster zwischen Server-seitigem Tod und Client-Request, siehe HandleRepopRequest()-
    //      Kommentar) - setzt deathState=CORPSE, startet den 6-Minuten-Reclaim-Timer. Fuer den
    //      Bot IMMER relevant, da kein Client existiert, der den Opcode "zufaellig" schon vorher
    //      ausgeloest haette.
    //   2. Player::BuildPlayerRepop() (Player.cpp:4356) - erzeugt den Geist-Zustand (SetHealth(1),
    //      Corpse-Objekt an der Todesposition, SPELL_AURA_GHOST) - KEIN Teleport.
    //   3. Player::RepopAtGraveyard() (Player.cpp:4830) - ermittelt den naechsten Friedhof
    //      (sObjectMgr->GetClosestGraveYard()) und ruft TeleportTo() dorthin auf; bleibt danach als
    //      Geist am Friedhof stehen (KEINE automatische volle Wiederbelebung - das macht erst
    //      Schritt 2 unten, analog zum echten Spirit-Healer-Rechtsklick).
    // HandleRepopRequest() ruft zusaetzlich RemovePet()/eine Instanz-Eingangs-Resurrection-
    // Sonderbehandlung auf - bewusst NICHT uebernommen: Bots haben in dieser Kampagne noch keine
    // Pets, und der Livetest findet wie bei StartBotAttack()/BotLootTarget() ausschliesslich auf
    // einer offenen Kontinent-Map statt (kein Instanz-Sonderfall moeglich).
    //
    // WICHTIG (dasselbe Muster wie TeleportBot(), Runde 93): RepopAtGraveyard() ruft TeleportTo()
    // INTERN auf. Ist das Ziel eine andere Map, schliesst TeleportTo() den Wechsel NICHT ab (siehe
    // Runde-93-Kopfkommentar) - der sonst vom Client gesendete MSG_MOVE_WORLDPORT_ACK muss auch
    // hier durch einen manuellen botSession->HandleMoveWorldportAck()-Aufruf ersetzt werden, FALLS
    // player->IsBeingTeleportedFar() danach true ist (bleibt der Friedhof auf derselben Map wie der
    // Todesort, ist das ein No-Op, siehe TeleportBot()-Kommentar).
    bool HandleBotDeath(uint32 accountId);

    // Gegenstueck zum Spirit-Healer-Rechtsklick (WorldSession::HandleSpiritHealerActivate() ->
    // SendSpiritResurrect(), NPCHandler.cpp:444/460) - volle Wiederbelebung MIT
    // Resurrection-Sickness (Player::ResurrectPlayer(0.5f, /*applySickness*/ true)), gefolgt von
    // DurabilityLossAll(0.25f)/SpawnCorpseBones()/ggf. einem zweiten TeleportTo(), falls der
    // naechste Friedhof zum Leichnam vom naechsten Friedhof zum Geist abweicht (siehe
    // SendSpiritResurrect()-Quellcode, NPCHandler.cpp:460-490). SendSpiritResurrect() ist bereits
    // eine oeffentliche WorldSession-Methode - kein NPC-Interaktionscheck noetig (der reale Check
    // in HandleSpiritHealerActivate() prueft nur GetNPCIfCanInteractWith(), rein clientseitige
    // Sichtbarkeits-/Distanzabsicherung, fuer einen synchronen Server-Aufruf ohne echten Client
    // irrelevant). Nur sinnvoll, wenn der Bot aktuell Geist ist (PLAYER_FLAGS_GHOST) - siehe
    // Guard in der .cpp. Denselben HandleMoveWorldportAck()-Nachtrag wie HandleBotDeath() oben,
    // falls der optionale zweite TeleportTo() in SendSpiritResurrect() eine andere Map trifft.
    bool ReviveBotAtGraveyard(uint32 accountId);

    // --- Runde 133 (28.09.2026): Ausruesten (naechster Roadmap-Schritt nach Kampf/Tod-Handling) ------
    //
    // Nutzervorgabe Runde 131 bestaetigt: Loot bleibt gestrichen, "Ausruesten" ist trotzdem ein
    // EIGENSTAENDIGES Thema (Bots sollen Items anlegen koennen, unabhaengig davon WIE die Items ins
    // Inventar kommen) - deshalb hier bewusst KEIN Bezug zu BotLootTarget()/Loot::FillLoot().
    // Code-Review-Vorlauf (ItemHandler.cpp::HandleAutoEquipItemOpcode(), cs_misc.cpp::
    // HandleAddItemCommand() gegengelesen): beide Ablaeufe sind bereits Ketten oeffentlicher
    // Player-Methoden, dasselbe Direktaufruf-Muster wie BotLootTarget()/HandleBotDeath() oben.
    //
    // Schritt A (Test-Item ins Inventar, GM-Item-Vergabe-Pfad, NICHT Loot): Player::CanStoreNewItem()
    // + Player::StoreNewItem() - exakt dieselben zwei Aufrufe, die ".additem" (cs_misc.cpp,
    // HandleAddItemCommand()) intern nutzt. Kein eigener SQL-Insert, keine Umgehung der
    // Platz-/Stack-Pruefung.
    // Schritt B (Anlegen): Player::CanEquipItem(NULL_SLOT, dest, item, /*swap*/true) ermittelt den
    // passenden Ausruestungsslot ueber itemTemplate->InventoryType (dieselbe Logik wie beim echten
    // Ziehen ins Slot) - danach Player::RemoveItem() (aus dem Rucksackslot) + Player::EquipItem(dest,
    // item, true) (setzt VisualizeItem()/UpdateItemDependentAuras() intern, kein separater Aufruf
    // noetig - EquipItem() ruft VisualizeItem() bereits selbst auf).
    // Schritt C (Bestaetigung): Player::GetItemByPos(dest) + Vergleich Item::GetEntry() gegen die
    // angeforderte itemEntry.
    //
    // BEWUSST NICHT unterstuetzt in dieser Runde: Ziel-Slot bereits belegt (Swap alt<->neu, wie im
    // "else"-Zweig von HandleAutoEquipItemOpcode() fuer echte Spieler) - Testbots dieser Kampagne
    // starten ungeruestet (Level 1, leeres Inventar seit Runde 128/129), ein belegter Zielslot ist
    // fuer den vorgesehenen Testablauf kein erwarteter Fall. Bei belegtem Slot: Funktion bricht mit
    // Fehler ab, das bereits eingelagerte Test-Item bleibt unangetastet im Rucksack (kein Datenverlust,
    // kein Teil-Rollback noetig).
    bool EquipBotItem(uint32 accountId, uint32 itemEntry);

    // --- Runde 135 (28.09.2026): Equipment-Pool-Implementierung nach Design lcf2r134 -------------
    //
    // Voller Design-Bericht: C:\LegionServer\reports\lcf2r134_2026-09-28_playerbots_equipment_pool_design.md
    // Setzt direkt auf EquipBotItem() (Runde 133) auf - dieselbe Item-Einlager-/Anlege-Logik, nur
    // die AUSWAHL der itemEntry-Werte ist neu. Zwei neue Tabellen (Empfehlung R134 Abschnitt 7,
    // Option A): `world.bot_equipment_pool` (einmalig per Python-Generierungsskript aus dem lokalen
    // Client-DB2-CSV-Export item.csv+itemsparse.csv befuellt, siehe Rundenbericht 135) und
    // `characters.bot_gear_tier` (eine Zeile pro Bot-Charakter, dauerhaft fixe Qualitaetsstufe).
    //
    // Ablauf:
    // 1. GetOrAssignBotGearTier() liest `characters.bot_gear_tier` fuer die aktuelle Bot-GUID; falls
    //    noch keine Zeile existiert, wird EINMALIG per Pyramiden-Gewichtung (R134 Abschnitt 3.3,
    //    abhaengig vom Level-Band des Bots ZUM ZEITPUNKT DES WUERFELNS) eine Stufe 1-4 gewuerfelt und
    //    dauerhaft gespeichert - kein Re-Roll bei spaeteren Aufrufen (Nutzervorgabe "einmalig,
    //    dauerhaft fix").
    // 2. Fuer jeden Ausruestungs-Slot wird aus `bot_equipment_pool` (WHERE level_band=aktuelles
    //    Levelband, pool_quality=gewuerfelte Stufe, item_class/item_subclass/inventory_type passend
    //    zu Klasse+Level+Slot) zufaellig ein item_entry gezogen - Faellt die gewuerfelte Stufe fuer
    //    einen Slot leer aus (z. B. "episch" bei Level < 20, siehe R134 3.3), faellt die Auswahl
    //    Stufe fuer Stufe ab (analog mod-playerbots' Fallback-Schleife, R134 Abschnitt 2.2), bis ein
    //    Treffer da ist oder Stufe 1 erschoepft ist (dann bleibt der Slot leer, kein Fehler).
    // 3. Jedes gezogene item_entry wird ueber die BEREITS in Runde 133 live bestaetigte
    //    EquipBotItem()-Logik tatsaechlich ausgeruestet (StoreNewItem+CanEquipItem+EquipItem) - kein
    //    neuer Anlege-Code, nur eine neue Auswahlschicht davor.
    //
    // Ruestungstyp-/Waffentyp-Filterung exakt nach R134 Abschnitt 5 (Level-40-Schwelle
    // Krieger/Paladin Kette->Platte, Jaeger/Schamane Leder->Mail; Klassen-Waffentyp-Zuordnung
    // bewusst vereinfacht auf Klassenebene statt vollem Spec-Proficiency-System, siehe .cpp-
    // Kommentar bei GetAllowedWeaponSubclasses() - dokumentierte Vereinfachung fuer diese Runde).
    // Artefaktwaffen sind in `bot_equipment_pool` bereits beim Befuellen ausgeschlossen (ArtifactID
    // != 0 gefiltert), hier keine zusaetzliche Pruefung noetig.
    bool EquipBotFromPool(uint32 accountId);

    // --- Runde 137 (28.09.2026): Gruppen-Beitritt + Folgen-KI nach Design lcf2r136 ----------------
    //
    // Voller Design-Bericht: C:\LegionServer\reports\lcf2r136_2026-09-28_playerbots_gruppe_lfr_design.md
    // Code-Review-Vorlauf (Runde 136, Group.h/.cpp + GroupHandler.cpp gegengelesen): der normale
    // Invite/Accept-Zweischritt (Group::AddInvite()+Client-Antwort) ist fuer einen vom GM direkt
    // gesteuerten, socketlosen Bot verzichtbar - Group::AddMember(Player*) (Group.cpp, bereits public)
    // erledigt die komplette Beitritts-State-Aenderung (SetGroup(), DB-Insert characters.group_member,
    // SendUpdate()/BroadcastGroupUpdate() ueber den bereits mehrfach bestaetigten Null-Socket-sicheren
    // WorldSession::SendPacket()-Pfad) OHNE Opcode-Parser-Durchlauf - dasselbe Direktaufruf-Muster wie
    // BotLootTarget()/HandleBotDeath()/EquipBotItem() in den Vorrunden.
    //
    // Ablauf (siehe lcf2r136 Abschnitt 4): leader ist der AUSFUEHRENDE GM-Charakter (kein Bot) -
    // liefert der Aufrufer (bot_commandscript.cpp) automatisch per handler->GetPlayer(). Guard-Kette:
    // Bot muss STATE_IN_WORLD sein und darf noch in KEINER Gruppe sein (kein Fremdgruppen-Kick, kein
    // Datenverlust); existiert beim Leader noch keine Gruppe, wird sie per Group::Create()+
    // GroupMgr::AddGroup() neu angelegt (derselbe einfachere Weg wie im Design-Bericht empfohlen,
    // KEIN AddLeaderInvite()-Zwischenzustand noetig); existiert bereits eine BG/BF-Raid-Gruppe, wird
    // stattdessen player->GetOriginalGroup() verwendet (dieselbe Sonderfall-Behandlung wie
    // HandlePartyInviteResponseOpcode()). Gruppengroesse-Limit (MAX_GROUP_SIZE=5 normale Gruppe,
    // MAX_RAID_SIZE=40 falls Raid) greift automatisch ueber Group::IsFull()/AddMember()s
    // Subgroup-Suche - kein Zusatzcode noetig, sauberes Fehlschlagen ohne Mutation bei Ueberfuellung.
    bool InviteBotToGroup(uint32 botAccountId, Player* leader);

    // Gegenstueck (Design-Bericht Abschnitt 4, "optional .bottest groupleave"): entfernt den Bot
    // sauber aus seiner aktuellen Gruppe - Player::RemoveFromGroup() ist bereits eine oeffentliche
    // Player-Methode (dieselbe Symmetrie-Konvention wie LogoutBot()/StopBotAttack() zu den jeweiligen
    // Start-Methoden dieser Kampagne).
    bool RemoveBotFromGroup(uint32 botAccountId);

    // --- Runde 137 (28.09.2026): Folgen-KI (MotionMaster::MoveFollow()) ----------------------------
    //
    // Blocker-Check aus Runde 136 (lcf2r136 Abschnitt 5): MotionMaster::MoveFollow(Unit* target,
    // float dist, float angle, MovementSlot slot) ist bereits als oeffentliche Core-API vorhanden
    // (MotionMaster.h), bisher aber von keiner BotMgr-Runde aufgerufen worden. Nutzt denselben
    // MoveSplineInit/PathGenerator-Unterbau wie das bereits live bestaetigte
    // MovePoint(generatePath=true) aus Runde U (lcf2r83) - kontinuierliche Neuberechnung statt
    // Einmalauslösung, aber KEIN neuer Update-Pfad (MotionMaster::UpdateMotion() laeuft bereits
    // unconditional pro Map-Tick fuer jeden Player inkl. Bot, seit Runde S/T live bestaetigt).
    //
    // dist/angle bewusst analog zur ueblichen Begleiter-/Pet-Nutzung im Core gewaehlt (kleiner
    // Abstand hinter dem Ziel, keine feste Formation noetig fuer diesen ersten isolierten Test) -
    // feste Werte in der .cpp dokumentiert, kein Parameter in dieser Runde (bewusst minimal, analog
    // MoveBotTestStep()). targetGuid ist hier (anders als bei StartBotAttack()/BotLootTarget()) die
    // VOLLE Laufzeit-ObjectGuid (typischerweise der Gruppenleiter/Spielercharakter, hat keine
    // DB-Spawn-Id wie eine Creature) - Aufloesung ueber ObjectAccessor::GetUnit(*botPlayer, targetGuid).
    bool StartBotFollow(uint32 botAccountId, ObjectGuid targetGuid);

    // Notbremse: bricht ein laufendes Follow sofort ab (bot->GetMotionMaster()->Clear() vom aktiven
    // Follow-Slot, Bot bleibt an der aktuellen Position stehen) - dieselbe Konvention wie
    // StopBotPatrol()/StopBotAttack() zu ihren jeweiligen Start-Methoden.
    void StopBotFollow(uint32 botAccountId);

    // --- Runde 143 (28.09.2026): Artefaktwaffen - Zuweisung + Skillung nach Design lcf2r138 Teil B ---
    //
    // Voller Design-Bericht: C:\LegionServer\reports\lcf2r138_2026-09-28_playerbots_gruppe_stufe2_lfg_artefakt_design.md
    // (Abschnitt "Teil B"). Kernbefund von dort: es gibt KEINE separate `character_artifact`-Tabelle -
    // der Artefaktzustand (Traits/Raenge/Tier) liegt als Item-Dynamic-Field-Daten direkt auf dem
    // `item_instance`-Datensatz der Waffe. `Item::Create()` (derselbe Pfad wie EquipBotItem()/
    // `.additem`, seit Runde 133 genutzt) initialisiert bei `ItemTemplate::GetArtifactID()!=0`
    // automatisch die Tier-0-Traits - EquipBotArtifact() braucht deshalb KEINEN neuen Anlege-Code,
    // nur die Auswahl der richtigen itemEntry davor (siehe .cpp fuer die 36 Klasse/Spec->Item-
    // Zuordnung, per lokalem Client-DB2-CSV-Export Artifact_7.3.5.26972.csv + itemsparse.csv
    // ermittelt und gegen die bekannten 36 Legion-Spezialisierungen verifiziert).
    //
    // Ermittelt Klasse+aktuelle Primaerspezialisierung des Bots (Player::GetPrimarySpecialization()),
    // schlaegt daraus das passende Artefakt-Item nach (statische Tabelle in der .cpp) und ruestet es
    // ueber den bereits in Runde 133 live bestaetigten EquipBotItem()-Pfad aus - KEIN Sonderslot/
    // keine Sonderbehandlung noetig: Player::CanEquipItem()/EquipItem() ermitteln den Zielslot bereits
    // rein ueber ItemTemplate::InventoryType (auch fuer die als INVTYPE_2HWEAPON kodierten, optisch
    // dual-wield gerenderten Artefakte wie "Warswords of the Valarjar" - EIN Item-Entry pro
    // Spezialisierung reicht, die zweite Waffenhaelfte ist ein reiner Client-Spelldarstellungs-Effekt,
    // kein zweites Item, siehe .cpp-Kommentar bei der Zuordnungstabelle). Fallback, falls
    // GetPrimarySpecialization()==0 (frisch erstellter Bot ohne bewusste Spec-Wahl): erste Spec der
    // Klasse in der Zuordnungstabelle (dokumentierte Vereinfachung, analog GetArmorSubclassForClass()
    // aus Runde 135 - keine echte Rollen-Erkennung noetig fuer diesen ersten Wurf).
    bool EquipBotArtifact(uint32 botAccountId);

    // Vergibt Artefakt-Trait-Raenge auf der aktuell in der Main-Hand ausgeruesteten Artefaktwaffe des
    // Bots (muss vorher per EquipBotArtifact() angelegt worden sein). Direktaufruf-Muster identisch zu
    // `WorldSession::HandleArtifactAddPower()` (ArtifactHandler.cpp) - dieselben zwei bereits
    // oeffentlichen Bausteine `Item::SetArtifactPower()`+`Player::ApplyArtifactPowerRank()`, nur ohne
    // XP-Abzug/Forge-Interaktionscheck (Bot hat keinen Client, der eine Schmiede anklicken koennte).
    // Ablauf (R138 B.2, "einfachste robuste Variante"): alle `sArtifactPowerStore`-Eintraege fuer die
    // ArtifactID der Waffe, sortiert nach Tier aufsteigend (dann ID aufsteigend als stabiler
    // Ersatzschluessel fuer eine "goldene" Prioritaetsreihenfolge - 36 einzeln recherchierte
    // Community-Prioritaetslisten sind laut R138 explizit NICHT Teil dieser ersten Runde). Pro Trait
    // wird - EXAKT wie in HandleArtifactAddPower() (ArtifactHandler.cpp:77-101) - zuerst die
    // ArtifactPowerLink-Verkettung geprueft (ein Trait mit Link-Pflicht wird uebersprungen, bis ein
    // verlinkter Trait seinen Maximalrang erreicht hat) - dadurch werden nie regelwidrige Zustaende
    // erzeugt, auch ohne vollstaendiges Baum-Pathfinding. Mehrere Durchlaeufe ueber die sortierte
    // Traitliste, bis entweder das Budget aufgebraucht ist oder ein voller Durchlauf keinen
    // Fortschritt mehr macht (alle verbleibenden Traits gesperrt/voll) - kein Endlosschleifenrisiko.
    //
    // levelBudget: 0 = automatische Berechnung aus dem aktuellen Bot-Level (R138 B.3-Faustregel,
    // diese Runde kalibriert: 1 zusaetzlicher Rang pro 2 Charakterlevel oberhalb der
    // Artefakt-Content-Schwelle Level 98, siehe .cpp - bewusst grob/dokumentiert, keine echte
    // GtArtifactLevelXPEntry-Kalibrierung in dieser ersten Runde). Ungleich 0: expliziter
    // Rang-Budget-Override fuer gezielte Tests. Tier-1 ("Konkordanz") wird nur ab Level 102
    // (grobe Naeherung R138 B.3) automatisch mit InitArtifactPowers() freigeschaltet - dieser Core
    // kennt laut `MAX_ARTIFACT_TIER` (DBCEnums.h) ohnehin nur Tier 0 und Tier 1, hoehere Tiers sind
    // architektonisch nicht vorhanden (kein Scope-Verlust gegenueber dem Core).
    bool SkillBotArtifact(uint32 botAccountId, uint32 levelBudget);

    // --- Gruppe Stufe 2, Teil A (LFG-Pool-Matchmaking) nach Design lcf2r138 -------------------------
    //
    // Voller Design-Bericht: C:\LegionServer\reports\lcf2r138_2026-09-28_playerbots_gruppe_stufe2_lfg_artefakt_design.md
    // (Abschnitt "Teil A"). Umsetzung folgt der dort empfohlenen Architektur ("Kern-Pfad wiederverwenden,
    // kein Bot-Sonderweg"): ein Fuell-Bot wird SOLO (ohne eigene Gruppe) exakt wie ein echter
    // Solo-Spieler per LFGMgr::JoinLfg() in die bestehende, bereits produktive Queue eingereiht -
    // LFGQueue::FindGroups()/CheckCompatibility() und LFGMgr::MakeNewGroup() (Group::AddMember(), siehe
    // Kopfkommentar-Referenz "LFGMgr::JoinLfg() -> ... -> Group::AddMember()") bleiben UNVERAENDERT und
    // matchen den Bot ganz normal zusammen mit echten Spielern. Level-/Ilvl-Passung passiert dadurch
    // KOSTENLOS ueber den bereits vorhandenen Lock-Check LFGMgr::GetCompatibleDungeons() (prueft
    // LFGDungeonData::minlevel/maxlevel/requiredItemLevel gegen Player::getLevel()/
    // GetAverageItemLevelEquipped() - funktioniert transparent fuer einen Bot, weil er ein echter
    // Player mit echten, aus dem Equipment-Pool (Runde 135) ausgeruesteten Items ist) - JoinLfg()
    // schlaegt fuer einen ungeeigneten Bot lediglich sauber fehl (dungeons.empty() -> LFG_JOIN_*-
    // Fehlercode, sendet nur ein fuer Bots ohnehin socket-sicheres Ergebnis-Paket, keine Mutation),
    // KEIN eigener Level-/Ilvl-Vorab-Check in BotMgr noetig oder sinnvoll (waere Logikduplizierung).
    //
    // Nur drei nachtraeglich noetige, rein additive Erweiterungen im LFG-Kern selbst (siehe
    // LFGQueue::GetQueueDataStore() und LFGMgr::GetQueuesForTeam()/GetProposalId() - alle rein lesend,
    // kein bestehender Aufrufpfad geaendert): der Kern hatte bisher keinen Weg, von AUSSEN (ausserhalb
    // von LFGMgr/LFGQueue selbst) festzustellen, WER gerade wartet und WORAUF eine laufende Proposal
    // von einem Spieler ohne Client (kein CMSG_LFG_PROPOSAL_RESULT-Antwortpfad) wartet.
    //
    // Lazy-Nachfuell-Trigger (Auftragsvorgabe, Skalierungsrisiko bei 500+ Bots vermeiden): KEINE
    // Dauer-Queue (Bots stehen NICHT permanent in der LFG-Queue). Stattdessen prueft
    // ProcessLfgPoolFillTick() periodisch (siehe LFG_POOL_FILL_INTERVAL_MS in der .cpp) ALLE aktiven
    // Queues beider Fraktionen auf Kandidaten mit MINDESTENS EINEM echten (Nicht-Bot-)Mitglied, die
    // entweder laenger als LFG_POOL_FILL_WAIT_THRESHOLD_SECONDS warten ODER denen eine per
    // LFGMgr::GetRoleCountByQueueId() als Pflicht markierte Rolle (Tank/Heiler) komplett fehlt - und
    // reiht dafuer HOECHSTENS EINEN passenden, aktuell untaetigen Bot pro Tick ein (kein
    // Massen-Einreihen, keine Dauerlast). Bot/Spieler-Unterscheidung ueber IsBotPlayerGuid() (siehe
    // dort) - laut Aufgabenstellung nur zulaessig, wenn zuverlaessig moeglich; siehe dortige
    // Begruendung, warum das hier zutrifft (kein Fallback auf "nur eigener Account" noetig).
    //
    // Trigger-Scope (Auftragsvorgabe): serverweit fuer ALLE echten Spieler beider Fraktionen, nicht nur
    // fuer einen einzelnen Account - siehe IsBotPlayerGuid()-Begruendung.
    void ProcessLfgPoolFillTick(uint32 diff);

    // Manueller Einzelschritt (fuer '.bottest lfgfill' und inkrementelles Live-Testen, unabhaengig vom
    // Timer in ProcessLfgPoolFillTick()): stoesst GENAU EINEN Nachfuell-Versuch ueber alle Queues
    // beider Fraktionen an. Rueckgabe true, wenn dabei ein Bot per JoinLfg() eingereiht wurde.
    bool TriggerLfgPoolFillOnce();

    // Fortschritt eines bereits als Fueller aktiven Bots (Proposal automatisch annehmen, da kein
    // Client existiert, der SMSG_LFG_PROPOSAL_UPDATE beantworten wuerde; Kartenwechsel-Ack
    // nachreichen, siehe TeleportBot()/Runde 93) - wird aus Tick() fuer JEDEN aktuell als Fueller
    // getrackten Bot aufgerufen, NICHT als eigener GM-Befehl (rein interne Fortsetzungslogik,
    // analog zum Patrol-Fortschritt in Tick()).
    void AdvanceLfgFillerBots();

    // Zuverlaessige Bot/Spieler-Unterscheidung (Auftragsvorgabe: server-weiter Trigger nur zulaessig,
    // "wenn das System echte Spieler zuverlaessig von Bots unterscheiden kann"). _botSessions (siehe
    // unten) ist die EINZIGE autoritative Quelle dafuer, welche Accounts/Player-Objekte Bots sind -
    // anders als eine Account-Id-Bereichs-Heuristik (die bei manuell angelegten/importierten Accounts
    // falsch liegen koennte) ist dies der Speicher, den BotMgr selbst beim Anlegen/Einloggen jedes
    // Bots pflegt (CreateBotAccount()/RequestBotLogin()) - ein echter Spieler-Account landet nie darin.
    // Deshalb ist der in der Aufgabenstellung vorgesehene Fallback ("nur der eigene Account des
    // Betreibers") hier NICHT noetig; der Trigger wirkt serverweit fuer alle echten Spieler.
    // Implementierung: linearer Scan ueber _botSessions (Bot-Anzahl laut Aufgabenstellung bis
    // ~500 - unkritisch fuer einen alle paar Sekunden laufenden Tick, siehe LFG_POOL_FILL_INTERVAL_MS).
    bool IsBotPlayerGuid(ObjectGuid guid) const;

    // --- Kampf-KI (Kampfmechaniken fuer alle Klassen/Skillungen) - erste Runde ------------------------
    //
    // Ziel laut Auftrag: Faehigkeiten-Nutzung/Rotationen fuer DPS/Heiler/Tank, aber NUR mit Daten, die
    // fuer genau Patch 7.3.5 (Build 26972) recherchiert und belegt sind - siehe ausfuehrlichen
    // Design-Kommentar oben bei BotRotationStep/BotSpecRotation sowie bei ResolveSpellIdByName() und
    // g_BotSpecRotations (BotMgr.cpp) fuer die Quellenlage.
    //
    // Umfang: ALLE 36 Skillungen haben einen Eintrag in g_BotSpecRotations (BotMgr.cpp), ueber vier
    // Runden aufgebaut (siehe voller Kommentar dort fuer Quelle/Konfidenz je Skillung):
    //   - Runde 1: vier Pilot-Skillungen, je eine pro Rollen-Archetyp (Protection Warrior/Tank,
    //     Fury Warrior/Nahkampf-DPS, Frost Mage/Fernkampf-DPS, Restoration Shaman/Heiler).
    //   - Runde 2: 25 weitere auf Basis derselben Recherche (reine Dateneingabe).
    //   - Runde 3: vier zuvor ausgeschlossene Skillungen ueber die neue BotRotationTargetOverride-
    //     Erweiterung bzw. dokumentierte Vereinfachungen geloest (Discipline Priest, Brewmaster/
    //     Windwalker Monk, Demonology Warlock - letzterer mit NIEDRIGER Recherche-Konfidenz).
    //   - Runde 4: die letzten drei (Enhancement Shaman, Feral/Guardian Druid) nach gezielter
    //     Zusatzrecherche ergaenzt.
    // Trotz vollstaendiger Abdeckung bleiben pro Skillung dokumentierte Vereinfachungen bestehen (siehe
    // Kommentar je Tabelleneintrag) - "verdrahtet" bedeutet NICHT "perfekt bis ins Detail", sondern
    // "strukturell korrekt mit klar benannten Einschraenkungen". Sollte eine zukuenftige Skillung
    // dennoch fehlen (z.B. neue ChrSpecialization), liefert GetOrResolveSpecRotation() nullptr und
    // ProcessBotCombatAI() tut in diesem Fall NICHTS zusaetzlich (der Bot bleibt beim bereits
    // bestehenden reinen Nahkampf-Auto-Attack-Verhalten aus StartBotAttack(), falls per GM-Befehl
    // ausgeloest) - kein Absturz, kein falsches Verhalten, einfach "noch nicht implementiert".
    //
    // Wird pro eingeloggtem Bot alle ~400ms aus Tick() aufgerufen (eigener Akkumulator
    // CombatAiTickAccumMs in BotSessionEntry, analog IdleTickAccumMs/PatrolCyclesRemaining). Schliesst
    // nebenbei einen Teil der im README dokumentierten Luecke "kein autonomer Zustandsautomat": ein
    // Bot in einer Gruppe engagiert automatisch dasselbe Ziel wie ein bereits kaempfendes
    // Gruppenmitglied (siehe SelectBotCombatTarget()), ohne dass '.bottest attack' manuell fuer jeden
    // einzelnen Kampf noetig waere - '.bottest attack' bleibt fuer gezielte Einzeltests weiterhin
    // nutzbar und unveraendert.
    void ProcessBotCombatAI(uint32 accountId, uint32 diff);

    // Fuer '.bottest status'/Diagnose: aktuell erkannte Rolle des Bots (Unknown, falls die
    // Primaerspezialisierung noch keinen Eintrag in g_BotSpecRotations hat).
    BotRole GetBotRole(uint32 accountId) const;

    // --- Quest-KI, Teil 1 (Annahme/Fortschritt/Abgabe) -------------------------------------------
    //
    // Umfang dieser ersten Runde (README-Luecke "Quest-KI noch nicht begonnen" teilweise geschlossen):
    // die serverseitige Annahme-/Abgabe-Mechanik, DIREKT ueber dieselben oeffentlichen Player-Methoden
    // aufgerufen, die auch WorldSession::HandleQuestgiverAcceptQuestOpcode()/
    // HandleQuestgiverChooseRewardOpcode() (QuestHandler.cpp) intern nutzen - passend zum bereits
    // etablierten Direktaufruf-Muster dieses Moduls (kein Opcode-/Packet-Nachbau noetig, anders als
    // z.B. bei BotLootTarget() in Runde 129, weil Player::AddQuestAndCheckCompletion()/RewardQuest()
    // bereits die vollstaendige serverseitige Arbeit sind, die der Opcode-Handler selbst aufruft).
    //
    // BEWUSST NICHT Teil dieser Runde (naechste Ausbaustufe): eigenstaendige Entscheidung, WELCHE
    // Quest angenommen wird, und autonome Navigation zum Questgeber/Questziel (Wegfindung ueber
    // mehrere Zonen) - das ist der groessere, im README separat als "kein autonomer Zustandsautomat"
    // dokumentierte Punkt. Toetungsfortschritt fuer Kill-Quest-Ziele braucht dagegen KEINEN
    // zusaetzlichen Code: der Core vergibt Quest-Kill-Credit ueber die normale, rollenunabhaengige
    // KillRewarder-Logik an JEDEN an einem Kill beteiligten Player - sobald ein Bot per
    // StartBotAttack()/ProcessBotCombatAI() aktiv am Kill mitwirkt, laeuft Kill-Credit automatisch mit,
    // exakt wie bei einem echten Spieler.
    //
    // questGiverSpawnGuid ist wie bei StartBotAttack()/BotLootTarget() die DB-Spawn-Id aus der
    // `creature`-Tabelle (Spalte "guid"), NICHT die Laufzeit-ObjectGuid - dieselbe
    // Map::GetCreatureBySpawnIdStore()-Aufloesung wird wiederverwendet.
    bool BotAcceptQuest(uint32 accountId, ObjectGuid::LowType questGiverSpawnGuid, uint32 questId);

    // Gegenstueck: Abgabe/Belohnung. rewardItemChoiceId ist der ECHTE Item-Entry der gewaehlten
    // Belohnung (nicht ein Belohnungs-Slot-Index) - siehe WorldPackets::Quest::QuestGiverChooseReward.
    // 0 ist gueltig fuer Quests ohne Auswahl-Belohnung.
    bool BotTurnInQuest(uint32 accountId, ObjectGuid::LowType questGiverSpawnGuid, uint32 questId,
        uint32 rewardItemChoiceId);

    // Fuer '.bottest queststatus'/Diagnose und fuer eine spaetere autonome Schleife ("ist dieses
    // Questziel schon fertig?"): liefert den rohen QuestStatus-Enum-Wert als int32 (QUEST_STATUS_NONE/
    // INCOMPLETE/COMPLETE/FAILED/...), ohne dass Aufrufer aus bot_commandscript.cpp QuestDef.h
    // einbinden muessen.
    int32 GetBotQuestStatus(uint32 accountId, uint32 questId) const;

    // --- Autonomer Dungeon-Clear-Modus (Ideenreferenz mod-dungeon-clear, komplett neu gebaut - siehe
    // README Abschnitt e) fuer die Lizenz-/Kompatibilitaets-Begruendung: AGPL-3.0 + andere Core-Version,
    // deshalb keine Codezeile uebernommen, nur das Feature-Konzept) --------------------------------------
    //
    // Uebernommene Kernidee: Routen werden LIVE aus dem Navmesh generiert
    // (`MotionMaster::MovePoint(generatePath=true)`, seit Runde U/`MoveBotTestStepPath()` bestaetigt
    // funktionsfaehig) - KEINE handgepflegten Wegpunkte pro Dungeon. Ein Bot mit aktiviertem Modus
    // navigiert autonom zum naechsten lebenden Dungeon-Boss (`Creature::IsDungeonBoss()` - dynamisch aus
    // der `instance_encounters`-Tabelle gesetztes `flags_extra`-Bit, zuverlaessiger als
    // `CreatureTemplate::rank`, das bei 5-Mann-Bossen haeufig nur `ELITE`/`RAREELITE` ist), engagiert
    // dabei automatisch Trash in Aggro-Reichweite (dieselbe `Attack()+MoveChase()`-Logik wie
    // `StartBotAttack()`) und loest nach jedem Kill automatisch `BotLootTarget()` fuer die naechste
    // lootbare Leiche aus, bevor er weiterroutet.
    //
    // BEWUSST NICHT Teil dieser ersten Runde (siehe README-Roadmap fuer die vollstaendige Liste, jeweils
    // mit Begruendung): Boss-Mechanik-Ausweichen, Pull-Stile (Leeroy/Advanced/Dynamic), Dungeon-
    // Encounter-Skripte (Hebel/Altare/Eskorten/Wellen), Heiler-Positionierung waehrend des Kampfes,
    // Tod-Wiederbelebungs-Choreographie bei Gruppenwipes. Jeder Bot routet ausserdem UNABHAENGIG - kein
    // "ein Bot fuehrt, der Rest folgt"-Konzept wie im Referenzmodul; da die Routenwahl deterministisch
    // ("naechster lebender Boss") ist, konvergieren mehrere gleichzeitig aktive Bots derselben Gruppe in
    // der Praxis trotzdem auf denselben Pfad, aber das ist eine bewusste Vereinfachung, keine echte
    // Formations-/Fuehrungslogik.
    //
    // Nur auf Dungeon-Karten aktivierbar (`Map::IsDungeon()`) - auf offenen Weltkarten gaebe es keine
    // sinnvolle "naechster Boss"-Zielsuche (Weltbosse sind bewusst ausgeschlossen, siehe
    // `Creature::IsDungeonBoss()`-Definition). Deaktiviert sich automatisch, sobald kein lebender
    // Dungeon-Boss mehr auf der aktuellen Karte gefunden wird (Instanz vermutlich clear).
    bool SetDungeonClearMode(uint32 accountId, bool enable);
    bool IsDungeonClearModeActive(uint32 accountId) const;

    // --- Spieler-Steuerung fuer den Dungeon-Clear-Modus (Chat-Schluesselwoerter + Addon-Kanal, siehe
    // bot_dungeonclear_control.cpp und README Abschnitt e)/mod-dungeon-clear-addon-Ideenreferenz) -----
    //
    // Bisher war SetDungeonClearMode() nur ueber den GM-Befehl '.bottest dungeonclear' erreichbar
    // (RBAC_PERM_COMMAND_ACCOUNT_CREATE) - fuer eine echte Spielernutzung (Chat-Schluesselwort "dc on"
    // in der eigenen Gruppe, oder ueber ein Addon) braucht es einen Weg, der KEINE GM-Rechte
    // voraussetzt und automatisch alle Bot-Mitglieder der GRUPPE DES ANFRAGENDEN SPIELERS behandelt,
    // nicht eine einzelne accountId. Diese drei Methoden sind die gemeinsame Grundlage fuer beide
    // Steuerwege (Chat-Schluesselwort UND Addon-Nachricht), damit die eigentliche Umschalt-Logik nur
    // einmal existiert.

    // Reverse-Lookup fuer eine Laufzeit-ObjectGuid -> Bot-Account-Id (0, falls guid kein Bot ist).
    // Linearer Scan ueber _botSessions, dieselbe Begruendung/Groessenordnung wie IsBotPlayerGuid().
    uint32 GetBotAccountIdByGuid(ObjectGuid guid) const;

    // Schaltet den Dungeon-Clear-Modus fuer ALLE Bot-Mitglieder der aktuellen Gruppe von "requester"
    // (ein echter Spieler ODER ein anderer Bot - keine GM-Pruefung hier, das ist bewusst: jedes
    // Gruppenmitglied darf die Bots der EIGENEN Gruppe steuern, dieselbe Berechtigungsgrenze wie ein
    // normaler Party-Invite/-Kick). Liefert die Anzahl tatsaechlich umgeschalteter Bots (0, falls
    // requester in keiner Gruppe ist oder keine Bots in der Gruppe sind, oder falls SetDungeonClearMode()
    // fuer jeden einzelnen Bot fehlschlaegt, z.B. weil keiner von ihnen auf einer Dungeon-Karte steht).
    uint32 SetDungeonClearModeForPlayerGroup(Player* requester, bool enable);

    // Fuer die Addon-"STATUS"-Abfrage: Anzahl der Bot-Mitglieder in der Gruppe von "player", die
    // GERADE JETZT IsDungeonClearModeActive()==true haben.
    uint32 CountActiveDungeonClearBotsInGroup(Player* player) const;

    // --- Aktive Selbstdiagnose (Nutzer-Feedback "suche aktiv nach Fehlern und fehlenden Werten") ------
    //
    // Zwei konkrete, wiederkehrende Reibungspunkte beim Livetest/Betrieb dieses Moduls, die bisher
    // manuelles SQL-Nachschlagen bzw. "36 Bots einzeln einloggen und Server.log lesen" erforderten:
    //
    //   1. Mehrere '.bottest'-Befehle (attack/loot/questaccept/questturnin) brauchen die DB-Spawn-Id
    //      (creature.guid) eines Ziel-NPCs - "NPC-Positionen fehlen" ist damit ein wiederkehrendes
    //      Problem, wenn diese Id nicht bekannt ist. FindNpcSpawnsByName() loest genau das: durchsucht
    //      die BEREITS GELADENEN Grid-Kreaturen um eine gegebene Position herum nach einem
    //      Namens-Teilstring und liefert Spawn-Id+Position+Distanz je Treffer zurueck - kein SQL-Zugriff
    //      noetig, funktioniert nur fuer Kreaturen, deren Grid gerade aktiv ist (derselbe Radius-/
    //      Sichtbarkeits-Rahmen wie bei FindNearestAggroableTrash() oben).
    //   2. Ob eine der 36 Skillungs-Rotationen (g_BotSpecRotations, siehe BotMgr.cpp) tatsaechlich
    //      gegen DIESES Server-Build (26972) aufloest, war bisher nur sichtbar, wenn ein Bot mit genau
    //      dieser Skillung im Kampf war (TC_LOG_ERROR bei fehlgeschlagener ResolveSpellIdByName()).
    //      DiagnoseSpecRotations() erzwingt die Aufloesung ALLER 36 Eintraege auf einen Schlag (rein
    //      lesend, derselbe ResolveSpellIdByName()-Pfad, den auch ein echter Kampf-Tick nutzen wuerde)
    //      und meldet jeden Namen, der NICHT im aktuell geladenen Spell.db2 gefunden wurde - deckt z.B.
    //      falsch geratene Brewmaster-Stagger-Aura-Namen oder die als NIEDRIG-Konfidenz markierte
    //      Demonology-Warlock-Zeile auf, ohne dafuer 36 verschiedene Bots anlegen/ausruesten zu muessen.

    // Kreaturen (lebend ODER tot) innerhalb radius Yards um center, deren Name (creature_template.name,
    // aktuelle Client-Locale) namePart als Teilstring (case-insensitiv) enthaelt - liefert je Treffer
    // die DB-Spawn-Id (creature.guid, fuer '.bottest attack/loot/questaccept/questturnin'), Entry,
    // Distanz und Position als formatierte Zeile. Leerer Vektor, falls nichts (mehr) im geladenen Grid
    // steht oder kein Treffer passt.
    std::vector<std::string> FindNpcSpawnsByName(WorldObject const* center, std::string const& namePart,
        float radius) const;

    // Erzwingt die einmalige Aufloesung ALLER g_BotSpecRotations-Eintraege (nicht nur der bereits per
    // echtem Bot-Kampf beruehrten) und liefert einen mehrzeiligen Bericht ueber jede Skillung mit
    // mindestens einem gegen das aktuelle Spell.db2 NICHT aufloesbaren Faehigkeits-/Aura-/Interrupt-/
    // Dispel-Namen. Rein lesend, ergebnisstabil (derselbe Cache wie im normalen Kampf-KI-Betrieb).
    std::string DiagnoseSpecRotations() const;

    // --- Hooks, die bereits jetzt gefahrlos verdrahtet werden koennen ------
    //
    // Werden aus PlayerScript-Hooks (bot_scriptloader.cpp) fuer JEDEN
    // Spieler aufgerufen, nicht nur fuer Bots - Body ist bewusst leer/billig,
    // damit bis zur naechsten Runde keinerlei Verhaltensaenderung entsteht.
    void OnPlayerUpdate(Player* player, uint32 diff);
    void OnPlayerLogin(Player* player);
    void OnPlayerLogout(Player* player);

private:
    BotMgr() = default;
    ~BotMgr() = default;

    // Runde 135: liest/erstellt die dauerhaft fixe Pool-Qualitaetsstufe eines Bots
    // (characters.bot_gear_tier) - siehe .cpp fuer Details.
    uint8 GetOrAssignBotGearTier(Player* player);

    // Kampf-KI (siehe ProcessBotCombatAI()-Kommentar oben): loest einen recherchierten englischen
    // Faehigkeitsnamen (Patch 7.3.5) zur Laufzeit gegen das tatsaechlich geladene Spell.db2 dieses
    // Servers auf - siehe voller Begruendung in der .cpp. Exaktes, case-insensitives Namens-Match,
    // gefiltert auf spellFamily (SpellFamilyNames-Enum), damit gleichnamige Faehigkeiten anderer
    // Klassen keine Kollision verursachen. Rein lesend, Ergebnis wird prozessweit gecacht.
    uint32 ResolveSpellIdByName(std::string const& englishName, uint32 spellFamily) const;

    // Liefert die (statische) Rotationstabelle fuer eine ChrSpecialization-Id, oder nullptr, falls
    // diese Spec noch keinen Eintrag in g_BotSpecRotations hat (siehe BotMgr.cpp). Loest beim ERSTEN
    // Aufruf fuer eine gegebene Tabelle alle SpellName/ConditionAuxSpellName-Eintraege einmalig per
    // ResolveSpellIdByName() auf (BotSpecRotation::ResolvedOnce-Flag) - kein wiederholtes Scannen des
    // gesamten Spell.db2 pro Kampf-Tick.
    BotSpecRotation const* GetOrResolveSpecRotation(uint32 specId) const;

    // Wertet eine einzelne BotRotationStep-Bedingung gegen den aktuellen Bot-/Zielzustand aus - siehe
    // BotRotationCondition-Kommentar in BotMgr.h fuer die Bedeutung jedes Falls.
    bool EvaluateBotRotationCondition(Player* player, Unit* target, BotRotationStep const& step) const;

    // Zielauswahl fuer DPS/Tank-Rollen: eigenes aktuelles Kampfziel, sonst (falls in einer Gruppe) das
    // Ziel eines bereits kaempfenden Gruppenmitglieds - engagiert den Bot in letzterem Fall automatisch
    // mit (Attack()+MoveChase(), dieselbe Logik wie StartBotAttack()) statt nur zuzusehen. Liefert
    // nullptr, wenn aktuell niemand in der Gruppe kaempft.
    Unit* SelectBotCombatTarget(Player* bot) const;

    // Zielauswahl fuer Heiler-Rollen: das Gruppenmitglied (inkl. des Bots selbst) mit dem niedrigsten
    // Lebensprozentsatz, aber NUR wenn dieser unter BOT_HEAL_CONSIDER_THRESHOLD_PCT liegt - liefert
    // sonst nullptr (bewusst kein Fuellschaden/-heilung ohne Bedarf in dieser ersten Runde).
    Unit* SelectBotHealTarget(Player* bot) const;

    // Dungeon-Clear-Modus (siehe SetDungeonClearMode()-Kommentar oben): wird pro Bot mit aktiviertem
    // Modus alle ~1s aus Tick() aufgerufen (eigener Akkumulator DungeonClearTickAccumMs in
    // BotSessionEntry). Tut nichts, waehrend der Bot bereits im Kampf ist (ProcessBotCombatAI()
    // uebernimmt), sonst: lootbare Leiche in der Naehe? loten. Sonst: Trash in Aggro-Reichweite? mit
    // engagieren. Sonst: naechster lebender Dungeon-Boss noch zu weit weg? per Navmesh dorthin routen.
    // Kein lebender Boss mehr gefunden -> Modus automatisch beenden (Instanz vermutlich clear).
    void ProcessDungeonClear(uint32 accountId, uint32 diff);

    // Naechster lebender Dungeon-Boss auf der aktuellen Karte des Bots (Creature::IsDungeonBoss()), oder
    // nullptr, falls keiner mehr lebt - lineare Suche ueber Map::GetCreatureBySpawnIdStore() (derselbe
    // bereits mehrfach genutzte Container wie in StartBotAttack()/BotLootTarget(), hier aber ueber ALLE
    // Werte statt eines einzelnen equal_range()-Schluessels iteriert, weil das Ziel nicht vorher bekannt
    // ist) - unkritisch, da ein einzelner Dungeon typischerweise nur wenige hundert Kreaturen gleichzeitig
    // geladen hat und diese Suche nur alle ~1s pro aktivem Bot laeuft.
    Creature* FindNearestLivingDungeonBoss(Player* bot) const;

    // Naechste angreifbare Nicht-Boss-Kreatur (Trash) innerhalb radius Yards - Unit::IsValidAttackTarget()
    // uebernimmt Hostilitaets-/CC-/Sichtbarkeits-Pruefung (dieselbe Kern-API, die auch der reguraere
    // Client-Zielwahl-Pfad nutzt), Dungeon-Bosse werden hier bewusst ausgeschlossen (die behandelt
    // FindNearestLivingDungeonBoss() separat, damit ein Boss nicht "nebenbei" wie Trash gepullt wird).
    Creature* FindNearestAggroableTrash(Player* bot, float radius) const;

    // Naechste lootbare (bereits tote, UNIT_DYNFLAG_LOOTABLE) Leiche innerhalb radius Yards - genutzt, um
    // nach einem Kill automatisch BotLootTarget() aufzurufen, bevor zum naechsten Ziel weitergeroutet wird.
    Creature* FindNearestLootableCorpse(Player* bot, float radius) const;

    // --- Generische Boss-Mechanik-Reaktionen (Ideenreferenz: Nutzer-Feedback "Bots brauchen Wissen
    // ueber Boss-Mechaniken, sonst haben sie keine Ahnung was zu tun ist") ----------------------------
    //
    // Kernproblem: Legion-Dungeon-Boss-Mechaniken sind NICHT recherchiert (siehe README-Roadmap) - eine
    // Wissensdatenbank "Boss X macht bei Y% Mechanik Z, weiche nach Sueden aus" existiert nicht und
    // waere ein eigenes, sehr grosses Rechercheprojekt pro Dungeon/Boss. Diese Runde loest stattdessen
    // das, was OHNE Boss-spezifische Daten bereits generisch aus dem Core herleitbar ist (per
    // Recherche bestaetigt, siehe BotMgr.cpp-Kommentar bei ProcessBotMechanicReactions()):
    //   1. Gefaehrliche Bodeneffekte verlassen (jede persistente Flaechen-Aura, nicht nur bekannte) -
    //      DynamicObject::GetSpellInfo()->IsPositive()==false + Bot steht innerhalb GetRadius().
    //   2. Gegnerische Zauber unterbrechen, wenn die Skillung einen Interrupt hat - der Core prueft
    //      beim Cast der Interrupt-Faehigkeit selbst, ob das Ziel gerade unterbrechbar castet
    //      (Spell::EffectInterruptCast()) - der Bot muss nur "casted das Ziel gerade ueberhaupt etwas"
    //      pruefen und dann draufhalten, kein Fehlversuch-Risiko.
    //   3. Gefaehrliche, entfernbare Debuffs von sich/Gruppenmitgliedern dispellen, wenn die Skillung
    //      einen Dispel hat - der Core waehlt die zu entfernende Aura selbst aus
    //      (Unit::GetDispellableAuraList()/Spell::EffectDispel()).
    // Das ist AUSDRUECKLICH KEIN Ersatz fuer echtes Boss-Mechanik-Skripting (Ausweich-Positionen, Soak-
    // Mechaniken, Phasenwechsel, Adds-Prioritaet etc. bleiben unbehandelt, da dafuer eine Boss-genaue
    // Wissensbasis noetig waere) - es ist die generische Teilmenge, die jeder Encounter (in JEDER
    // Instanz, nicht nur Legion-Dungeons) gemeinsam hat.

    // Wird von ProcessBotCombatAI() VOR der eigentlichen Rotationsschleife aufgerufen (siehe dort) -
    // liefert true, wenn eine Mechanik-Reaktion diesen Tick bereits "verbraucht" hat (Interrupt/Dispel
    // gecastet ODER eine Fluchtbewegung ausgeloest), die normale Rotation wird dann fuer diesen Tick
    // uebersprungen (Sicherheit vor Schadensoutput).
    bool ProcessBotMechanicReactions(Player* bot, BotSpecRotation const* rotation, Unit* combatTarget,
        Unit* healTarget);

    // Naechstes DynamicObject (persistente Flaechen-Aura) innerhalb radius Yards, dessen Zauber laut
    // SpellInfo::IsPositive() SCHAEDLICH ist UND in dessen GetRadius() der Bot aktuell tatsaechlich
    // steht (zwei getrennte Radien: Suchradius vs. tatsaechlicher Wirkradius des Effekts selbst) -
    // sonst nullptr. Neu geschriebener Grid-Suchcode (kein bestehender Helfer dafuer im Core
    // gefunden, siehe Rechercheergebnis) nach demselben Muster wie das bereits im Core vorhandene
    // Unit::GetAreatriggerListInRange().
    DynamicObject* FindHarmfulGroundEffectUnderBot(Player* bot, float searchRadius) const;

    // Absichtlich leer in dieser Runde - kein Bot kann derzeit angelegt werden.
    std::unordered_map<ObjectGuid, std::unique_ptr<IBotCharacter>> _bots;

    // Runde B: eine socketlose WorldSession pro Bot-Account, NIE ueber
    // World::AddSession() registriert (Minimal-Footprint-Entscheidung Runde A).
    struct BotSessionEntry
    {
        std::unique_ptr<WorldSession> Session;
        BotCharacterState State = BotCharacterState::STATE_UNINITIALIZED;
        std::string CharacterName;
        bool LoginRequested = false;

        // --- Runde R (27.09.2026): Idle-Grundstruktur, siehe BotCharacter.h -----
        // Rein vorbereitend fuer Runde S (MotionMaster/echtes Idle-Verhalten).
        // IdleState wird aktuell nirgends ausgewertet, nur mitgefuehrt.
        // IdleTickAccumMs sammelt Tick-Deltas fuer den inerten Diagnose-Tick in
        // BotMgr::Tick() (nur eine Logzeile alle ~30s fuer eingeloggte Bots,
        // KEINE Bewegungs-/KI-Logik).
        BotState IdleState = BotState::Idle;
        uint32 IdleTickAccumMs = 0;

        // --- Runde T (27.09.2026): Patrol-Testlauf-Zustand ----------------------
        bool PatrolActive = false;
        bool PatrolGoingToB = false;     // true: unterwegs A->B, false: unterwegs B->A
        uint32 PatrolCyclesTotal = 0;
        uint32 PatrolCyclesRemaining = 0;
        float PatrolAX = 0.0f, PatrolAY = 0.0f, PatrolAZ = 0.0f;
        float PatrolBX = 0.0f, PatrolBY = 0.0f, PatrolBZ = 0.0f;

        // Kampf-KI: eigener Tick-Akkumulator fuer ProcessBotCombatAI() (alle ~400ms statt jeden
        // Weltserver-Tick - Rotationsentscheidungen muessen nicht Millisekunden-praezise sein, und ein
        // seltenerer Tick reduziert die Spell.db2/GetSpellHistory()-Pruefungen bei vielen Bots).
        uint32 CombatAiTickAccumMs = 0;

        // Autonomer Dungeon-Clear-Modus (siehe SetDungeonClearMode()) - eigener, groeberer Akkumulator
        // (~1s statt ~400ms), da Routing-Entscheidungen weniger zeitkritisch sind als Rotationsschritte.
        bool DungeonClearActive = false;
        uint32 DungeonClearTickAccumMs = 0;
    };
    std::unordered_map<uint32, BotSessionEntry> _botSessions;

    // Gruppe Stufe 2, Teil A: Bot-Accounts, die BotMgr aktuell als LFG-Fuell-Kandidat eingereiht hat
    // (per JoinLfg(), siehe TriggerLfgPoolFillOnce()) - verhindert Doppel-Einreihung desselben Bots
    // und markiert, fuer welche Bots AdvanceLfgFillerBots() pro Tick den Proposal-/Teleport-Fortschritt
    // nachziehen muss. Ein Eintrag wird entfernt, sobald der Bot entweder erfolgreich in eine
    // Dungeon-Gruppe uebernommen wurde (LFG_STATE_DUNGEON) oder die Queue ohne Match wieder verlassen
    // hat (LFG_STATE_NONE, z.B. Proposal abgelehnt/Timeout) - siehe AdvanceLfgFillerBots().
    std::unordered_set<uint32> _lfgFillerBotAccountIds;

    // Akkumulator fuer den Lazy-Nachfuell-Timer (siehe ProcessLfgPoolFillTick()/
    // LFG_POOL_FILL_INTERVAL_MS) - dieselbe Diff-Aufsummierungs-Konvention wie IdleTickAccumMs oben,
    // aber EINMAL pro BotMgr statt pro Bot-Session (der Trigger prueft serverweit ueber alle Bots
    // hinweg, nicht pro einzelner Session).
    uint32 _lfgFillTickAccumMs = 0;
};

#define sBotMgr BotMgr::instance()

#endif // BOT_MGR_H
