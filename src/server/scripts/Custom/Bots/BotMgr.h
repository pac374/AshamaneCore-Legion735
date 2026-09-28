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
#include <memory>
#include <string>

class IBotCharacter;
class Player;
class WorldSession;

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
    };
    std::unordered_map<uint32, BotSessionEntry> _botSessions;
};

#define sBotMgr BotMgr::instance()

#endif // BOT_MGR_H
