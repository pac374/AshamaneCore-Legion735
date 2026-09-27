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

#include "BotMgr.h"
#include "BotCharacter.h"
#include "Log.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include "World.h"
#include "CharacterPackets.h"
#include "Player.h"
#include "Opcodes.h"
#include "ObjectMgr.h"
#include "DatabaseEnv.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include <cmath>

BotMgr* BotMgr::instance()
{
    static BotMgr instance;
    return &instance;
}

// --- Runde B: Session-Faking-Kernstueck --------------------------------
// Siehe ausfuehrlichen Kopfkommentar in BotMgr.h ("Runde B") fuer die
// Begruendung jedes einzelnen Schritts (CharEnum-Notwendigkeit,
// ProcessQueryCallbacks()-Blocker, MapUpdate.Threads=1-Befund) und den
// vollstaendigen Bericht C:\LegionServer\reports\lcf2r64_2026-09-27_playerbots_rundeb.md.
// WICHTIG: dieser Code wurde bewusst NUR gebaut/kompiliert, NICHT gegen den
// laufenden Produktivserver ausgefuehrt (kein Neustart, keine .bottest-
// Befehle wurden in dieser Runde tatsaechlich abgesetzt) - siehe Stopp-Regel
// im Kopfkommentar und im Bericht Abschnitt 6.

bool BotMgr::CreateBotAccount(std::string const& accountName, std::string const& password, uint32& outAccountId)
{
    outAccountId = AccountMgr::GetId(accountName);
    if (outAccountId != 0)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::CreateBotAccount: Account '{}' existiert bereits (Id {}), nichts angelegt (idempotent).",
            accountName, outAccountId);
        return true;
    }

    AccountOpResult result = sAccountMgr->CreateAccount(accountName, password, "", /*bnetAccountId*/ 0, /*bnetIndex*/ 0);
    if (result != AccountOpResult::AOR_OK)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::CreateBotAccount: AccountMgr::CreateAccount('{}') fehlgeschlagen, AccountOpResult={}.",
            accountName, uint32(result));
        return false;
    }

    outAccountId = AccountMgr::GetId(accountName);
    TC_LOG_INFO("scripts.bots", "BotMgr::CreateBotAccount: Bot-Account '{}' neu angelegt, Id {} (kein BNet-Account, bnetAccountId=0).",
        accountName, outAccountId);
    return outAccountId != 0;
}

// Legt (falls noch nicht vorhanden) die socketlose WorldSession fuer diesen
// Bot-Account an. Wird von RequestCreateBotCharacter/RequestBotLogin bei
// Bedarf automatisch aufgerufen.
static std::unique_ptr<WorldSession> CreateBotWorldSession(uint32 accountId, std::string const& accountName)
{
    AccountTypes security = AccountTypes(AccountMgr::GetSecurity(accountId));
    uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));

    auto session = std::make_unique<WorldSession>(
        accountId,
        std::string(accountName),          // std::string&& name
        /*battlenetAccountId*/ 0,          // kein BNet-Account (siehe Runde-A-Frage 1)
        /*sock*/ std::shared_ptr<WorldSocket>(nullptr),
        security,
        expansion,
        /*mute_time*/ time_t(0),
        /*os*/ std::string("Bot"),
        LOCALE_enUS,
        /*recruiter*/ 0,
        /*isARecruiter*/ false,
        std::string()                      // battlenetAccountName, leer
    );

    // Runde F (27.09.2026): markiert die Session als Bot-Session, damit die
    // neuen Diagnose-Log-Zeilen in WorldSession::HandlePlayerLogin()
    // (CharacterHandler.cpp) fuer Runde G exakt sehen koennen, an welcher
    // Stelle der aus Runde E bekannte Absturz auftritt - siehe
    // lcf2r68_2026-09-27_playerbots_rundef.md. Keine Verhaltensaenderung fuer
    // diese Session, reiner Marker.
    session->SetBotSession(true);
    return session;
}

bool BotMgr::RequestCreateBotCharacter(uint32 accountId, std::string const& charName, uint8 race, uint8 charClass, uint8 sex)
{
    if (accountId == 0)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: accountId 0 ungueltig.");
        return false;
    }

    std::string accountName;
    if (!AccountMgr::GetName(accountId, accountName))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Account {} existiert nicht.", accountId);
        return false;
    }

    // --- Runde D (27.09.2026): Root-Cause-Fix fuer den Runde-C-Fehlschlag -----
    //
    // Ursache war NICHT ein fehlendes Legion-Customizations-Feld (Verdacht aus
    // Runde C/Bericht lcf2r65) - CharacterCreateInfo::CustomDisplay ist bereits
    // per Default-Initialisierung ein Nullarray, das reicht Player::Create()
    // (siehe Player.cpp ValidateAppearance/SetByteValue PLAYER_BYTES_2) genauso
    // wie einem echten Client, der keine Anpassungen waehlt.
    //
    // Tatsaechliche Ursache (gegen echten Core-Code verifiziert,
    // ObjectMgr::CheckPlayerName -> isValidString(wname, strictMask,
    // /*numericOrSpace*/ false, create) in ObjectMgr.cpp): WoW-Charakternamen
    // duerfen KEINE Ziffern enthalten - "numericOrSpace" ist fuer Spielernamen
    // IMMER false (nur Chartername/Petname erlauben Ziffern). Der in Runde C
    // manuell getestete Name "Testbot1" enthaelt eine "1" und wird deshalb von
    // HandleCharCreateOpcode() SOFORT und SYNCHRON (vor jeder DB-Query) mit
    // SendCharCreate(CHAR_NAME_MIXED_LANGUAGES) abgelehnt - exakt das Timing
    // ("Prevented sending ... to non existent socket 0" quasi unmittelbar nach
    // dem Befehl, nicht erst nach dem asynchronen Namens-Check-Roundtrip), das
    // in Runde C beobachtet wurde. Kein Core-Bug, kein fehlendes Feld -
    // schlicht ein ungueltiger Testname.
    //
    // Fix hier statt im Core: BotMgr validiert den Namen VORHER mit denselben
    // Funktionen, die der Core selbst nutzt (normalizePlayerName +
    // ObjectMgr::CheckPlayerName), und bricht mit einer fuer uns sichtbaren,
    // klaren Fehlermeldung ab, statt den fehlschlagenden Opcode-Aufruf
    // auszuloesen und auf ein unsichtbares Antwortpaket zu warten. Core bleibt
    // unveraendert.
    std::string normalizedName = charName;
    if (!normalizePlayerName(normalizedName))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Bot-Charaktername '{}' konnte nicht "
            "normalisiert werden (leer nach UTF8/Locale-Konvertierung?) - abgebrochen, keine Opcode-Aufrufe ausgeloest.",
            charName);
        return false;
    }

    ResponseCodes nameCheck = ObjectMgr::CheckPlayerName(normalizedName, LOCALE_enUS, true);
    if (nameCheck != CHAR_NAME_SUCCESS)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Bot-Charaktername '{}' ist fuer den Core "
            "ungueltig (ObjectMgr::CheckPlayerName -> ResponseCode {}) - haeufigste Ursache: Ziffern/Sonderzeichen "
            "im Namen sind fuer Spielercharaktere NIE erlaubt (siehe Runde-D-Kommentar oben). Name unveraendert "
            "gelassen, keine Opcode-Aufrufe ausgeloest - bitte mit einem rein alphabetischen Namen erneut versuchen.",
            charName, uint32(nameCheck));
        return false;
    }

    BotSessionEntry& entry = _botSessions[accountId];
    if (!entry.Session)
    {
        entry.Session = CreateBotWorldSession(accountId, accountName);
        entry.State = BotCharacterState::STATE_UNINITIALIZED;
    }

    entry.CharacterName = normalizedName;

    // Baut das CreateCharacter-Paket genau wie ein echter Client es senden
    // wuerde (siehe CharacterPackets.h CharacterCreateInfo-Konstruktor) -
    // Read() wird NICHT aufgerufen, wir setzen die Felder direkt. Verwendet
    // normalizedName (bereits oben validiert), nicht das rohe charName-Argument.
    auto createInfo = std::make_shared<WorldPackets::Character::CharacterCreateInfo>(
        normalizedName, race, charClass, sex,
        /*skin*/ 0, /*face*/ 0, /*hairStyle*/ 0, /*hairColor*/ 0, /*facialHair*/ 0, /*outfitId*/ 0);

    WorldPacket emptyPacket(CMSG_CREATE_CHARACTER, 0);
    WorldPackets::Character::CreateCharacter charCreate(std::move(emptyPacket));
    charCreate.CreateInfo = createInfo;

    TC_LOG_INFO("scripts.bots", "BotMgr::RequestCreateBotCharacter: loese HandleCharCreateOpcode fuer '{}' (Account {}) aus - "
        "Ergebnis ist asynchron, siehe Server.log/DBErrors.log nach den naechsten Tick()-Aufrufen.", charName, accountId);
    entry.Session->HandleCharCreateOpcode(charCreate);
    return true;
}

bool BotMgr::RequestBotLogin(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestBotLogin: keine Bot-Session fuer Account {} vorhanden - "
            "erst RequestCreateBotCharacter() aufrufen.", accountId);
        return false;
    }

    BotSessionEntry& entry = itr->second;
    if (entry.Session->GetPlayer())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::RequestBotLogin: Account {} hat bereits einen Player in der Welt.", accountId);
        return true;
    }

    entry.LoginRequested = true;
    entry.State = BotCharacterState::STATE_LOGGING_IN;

    // Schritt 1: CharEnum ausloesen - fuellt _legitCharacters (siehe
    // Kopfkommentar Punkt 1). Asynchron; Schritt 2 folgt in Tick().
    WorldPacket enumPacket(CMSG_ENUM_CHARACTERS, 0);
    WorldPackets::Character::EnumCharacters enumCharacters(std::move(enumPacket));
    TC_LOG_INFO("scripts.bots", "BotMgr::RequestBotLogin: loese HandleCharEnumOpcode fuer Account {} aus.", accountId);
    entry.Session->HandleCharEnumOpcode(enumCharacters);
    return true;
}

BotCharacterState BotMgr::GetBotSessionState(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    return itr != _botSessions.end() ? itr->second.State : BotCharacterState::STATE_UNINITIALIZED;
}

Player* BotMgr::GetBotPlayer(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    return (itr != _botSessions.end() && itr->second.Session) ? itr->second.Session->GetPlayer() : nullptr;
}

void BotMgr::Tick(uint32 diff)
{
    // Bewusst NUR ProcessQueryCallbacks() - NIEMALS Session->Update(), das bei
    // socket=nullptr in IsConnectionIdle()->CloseSocket() abstuerzen wuerde
    // (Runde-A-Befund, siehe Kopfkommentar).
    for (auto& [accountId, entry] : _botSessions)
    {
        if (!entry.Session)
            continue;

        entry.Session->ProcessQueryCallbacks();

        if (entry.State == BotCharacterState::STATE_LOGGING_IN && entry.LoginRequested
            && !entry.Session->GetPlayer() && !entry.Session->PlayerLoading())
        {
            // _legitCharacters ist dank "friend class BotMgr;" (WorldSession.h)
            // direkt lesbar - sobald CharEnum durchgelaufen ist, enthaelt es
            // (mindestens) den gerade angelegten Bot-Charakter.
            if (!entry.Session->_legitCharacters.empty())
            {
                ObjectGuid charGuid = *entry.Session->_legitCharacters.begin();

                WorldPacket loginPacket(CMSG_PLAYER_LOGIN, 0);
                WorldPackets::Character::PlayerLogin playerLogin(std::move(loginPacket));
                playerLogin.Guid = charGuid;

                TC_LOG_INFO("scripts.bots", "BotMgr::Tick: CharEnum fuer Account {} fertig, Charakter {} gefunden - "
                    "loese HandlePlayerLoginOpcode()+HandleContinuePlayerLogin() aus (normalzero-Muster).",
                    accountId, charGuid.ToString());

                // normalzero/LegionPlayerBot-Muster: beide Haelften des sonst
                // durch die zweite Client-Verbindung ausgeloesten Handshakes
                // manuell und synchron hintereinander aufrufen.
                entry.Session->HandlePlayerLoginOpcode(playerLogin);
                if (entry.Session->PlayerLoading())
                    entry.Session->HandleContinuePlayerLogin();
                else
                    TC_LOG_ERROR("scripts.bots", "BotMgr::Tick: HandlePlayerLoginOpcode fuer Account {} hat den Bot "
                        "abgewiesen (IsLegitCharacterForAccount false oder bereits eingeloggt) - siehe Server.log.", accountId);

                entry.LoginRequested = false; // nur einmal versuchen, Ergebnis via GetBotSessionState/GetBotPlayer pruefen
            }
        }

        if (entry.Session->GetPlayer())
        {
            entry.State = BotCharacterState::STATE_IN_WORLD;

            // --- Runde R (27.09.2026): reiner Idle-Diagnose-Platzhalter --------
            // Bewusst KEINE Movement-/MotionMaster-Logik (explizit Runde S
            // vorbehalten, siehe BotCharacter.h "Runde R"-Kommentar). Tut nichts
            // ausser alle ~30s eine Diagnose-Logzeile zu schreiben, um zu
            // bestaetigen dass ein eingeloggter Bot regelmaessig getickt wird,
            // bevor in Runde S echte Logik darauf aufgebaut wird. Inert, kein
            // Zugriff auf MotionMaster/Map, kein Verhaltensrisiko.
            entry.IdleTickAccumMs += diff;
            if (entry.IdleTickAccumMs >= 30000)
            {
                entry.IdleTickAccumMs = 0;
                TC_LOG_DEBUG("scripts.bots", "[BotIdle] Account {} (IdleState=Idle): periodischer Diagnose-Tick, "
                    "keine Aktion (Platzhalter fuer Runde S).", accountId);
            }

            // --- Runde T (27.09.2026): Patrol-Fortschritt, siehe Kopfkommentar in
            // BotMgr.h ("Runde T") fuer die Begruendung von movespline->Finalized()
            // als Ankunftserkennung und den Mutate()-Speicherleck-Review. ----------
            if (entry.PatrolActive)
            {
                Player* patrolPlayer = entry.Session->GetPlayer();
                if (!patrolPlayer->IsInWorld())
                {
                    // Sicherheitsnetz: sollte der Bot waehrend eines laufenden Patrols
                    // aus der Welt verschwinden (z.B. externer Logout), Patrol sauber
                    // abbrechen statt einen toten Zustand haengen zu lassen.
                    TC_LOG_ERROR("scripts.bots", "BotMgr::Tick: Account {} waehrend aktivem Patrol nicht mehr "
                        "IsInWorld() - Patrol sicherheitshalber abgebrochen.", accountId);
                    entry.PatrolActive = false;
                    entry.IdleState = BotState::Idle;
                }
                else if (patrolPlayer->movespline->Finalized())
                {
                    if (entry.PatrolGoingToB)
                    {
                        // Etappe A->B abgeschlossen - Rueckweg B->A antreten.
                        TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account {} Patrol - Punkt B erreicht, "
                            "Rueckweg zu Punkt A (Zyklus {}/{}).", accountId,
                            entry.PatrolCyclesTotal - entry.PatrolCyclesRemaining + 1, entry.PatrolCyclesTotal);
                        entry.PatrolGoingToB = false;
                        patrolPlayer->GetMotionMaster()->MovePoint(0, entry.PatrolAX, entry.PatrolAY, entry.PatrolAZ,
                            /*generatePath*/ false);
                    }
                    else
                    {
                        // Etappe B->A abgeschlossen - ein voller Zyklus (A->B->A) fertig.
                        if (entry.PatrolCyclesRemaining > 0)
                            entry.PatrolCyclesRemaining--;

                        TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account {} Patrol - Punkt A erreicht, Zyklus "
                            "abgeschlossen ({} von {} verbleibend).", accountId, entry.PatrolCyclesRemaining,
                            entry.PatrolCyclesTotal);

                        if (entry.PatrolCyclesRemaining == 0)
                        {
                            entry.PatrolActive = false;
                            entry.IdleState = BotState::Idle;
                            TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account {} Patrol - alle {} Zyklen "
                                "abgeschlossen, Patrol beendet, Bot bleibt an Punkt A stehen.",
                                accountId, entry.PatrolCyclesTotal);
                        }
                        else
                        {
                            entry.PatrolGoingToB = true;
                            patrolPlayer->GetMotionMaster()->MovePoint(0, entry.PatrolBX, entry.PatrolBY, entry.PatrolBZ,
                                /*generatePath*/ false);
                        }
                    }
                }
            }
        }
    }
}

// --- Runde N (27.09.2026): Fix fuer den in Runde M gefundenen Shutdown-Absturz -
// Vollstaendige Herleitung/Verifikation: siehe Kopfkommentar in BotMgr.h und
// Bericht C:\LegionServer\reports\lcf2r76_2026-09-27_playerbots_runde_n.md.
// Kurzfassung der per cdb+ln-Gegenprobe UND Quellcode-Gegenlesen (nicht nur
// Hypothese) verifizierten Kausalkette des Runde-M-Absturzes:
//   1. Main.cpp:314 (sWorldSocketMgrHandle-RAII) ruft sWorld->KickAll() - das
//      erreicht NUR Sessions in World::m_sessions (World.cpp:2736), Bot-Sessions
//      sind dort nie registriert (Minimal-Footprint, Runde A) -> KickAll()
//      erreicht Bot-Sessions nie.
//   2. Main.cpp:232 (dbHandle-RAII) ruft danach StopDB() (Main.cpp:581-588) ->
//      CharacterDatabase.Close() -> DatabaseWorkerPool::Close()
//      (DatabaseWorkerPool.cpp:178-188) -> Zeile 188 "_ioContext.reset()".
//   3. main() kehrt zurueck (Main.cpp:370), die CRT ruft exit() auf, das die
//      atexit-Tabelle abarbeitet - darin (irgendwann zuvor bei erstem Zugriff
//      auf sBotMgr registriert) der "dynamic atexit destructor" fuer
//      BotMgr::instance() (Meyer's-Singleton), der _botSessions zerstoert.
//   4. Jede verbliebene Bot-Session mit noch geladenem Player durchlaeuft dabei
//      WorldSession::~WorldSession() (WorldSession.cpp:163-164, "if (_player)
//      LogoutPlayer(true)") -> WorldSession::LogoutPlayer() (WorldSession.cpp:589,
//      "_player->SaveToDB()") -> Player::SaveToDB() (Player.cpp:21373,
//      "CharacterDatabase.CommitTransaction(trans)") -> DatabaseWorkerPool::
//      CommitTransaction() (DatabaseWorkerPool.cpp:331, "boost::asio::post(
//      _ioContext->get_executor(), ...)") - _ioContext ist zu diesem Zeitpunkt
//      bereits (Schritt 2) auf nullptr zurueckgesetzt -> Nullpointer-Dereferenzierung
//      -> ACCESS_VIOLATION, exakt der in Runde M beobachtete und in Runde N per
//      cdb "!analyze -v; .ecxr; kb" + ln-Gegenprobe (thematisch kohaerente
//      Nachbarsymbole, kein ICF-Artefakt) zweifelsfrei bestaetigte Absturz.
//
// Fix: BotMgr::LogoutAllBots() wird aus WorldScript::OnShutdown()
// (bot_scriptloader.cpp) aufgerufen - das laeuft laut Main.cpp:359 SYNCHRON
// noch INNERHALB von main(), bevor irgendeine der obigen main()-lokalen
// RAII-Aufraeumaktionen (KickAll, StopDB) abgewickelt wird. Der DB-Pool ist zu
// diesem Zeitpunkt garantiert noch vollstaendig funktionsfaehig - LogoutPlayer()
// darf hier sicher aufgerufen werden. LogoutPlayer(true) setzt _player
// anschliessend auf nullptr (WorldSession.cpp:630 "SetPlayer(NULL)"), sodass der
// spaetere atexit-Destruktor in ~WorldSession() auf _player == nullptr trifft und
// LogoutPlayer() gar nicht mehr aufruft (WorldSession.cpp:163-164) - der
// beschriebene Absturzpfad wird dadurch vollstaendig und dauerhaft vermieden.
//
// Runde P (27.09.2026, siehe lcf2r78-Bericht): Runde O fand einen ZWEITEN,
// unabhaengigen unbedingten DB-Aufruf im selben Destruktor - die
// LoginDatabase.PExecute("UPDATE account SET online = 0 ...")-Zeile ganz am
// Ende von ~WorldSession() (WorldSession.cpp, vormals Zeile 184), die fuer
// JEDE Session lief, unabhaengig von _player. WorldSession.cpp macht den
// Destruktor jetzt fuer Bot-Sessions komplett DB-frei (IsBotSession()-Guard) -
// deshalb muss dieser Online-Flag-Reset jetzt HIER erfolgen, waehrend der
// LoginDatabase-Pool noch garantiert lebt (aus genau demselben Grund wie beim
// CharacterDatabase-Reset oben). Bewusst fuer JEDE Session in _botSessions
// ausgefuehrt (nicht nur die mit noch gesetztem GetPlayer()), weil
// HandlePlayerLogin() (CharacterHandler.cpp, LOGIN_UPD_ACCOUNT_ONLINE) das
// Online-Flag unabhaengig vom spaeteren Player-Zustand auf 1 setzt, sobald ein
// Bot einmal eingeloggt war - idempotent und ungefaehrlich, falls es schon 0 ist.
void BotMgr::LogoutAllBots()
{
    for (auto& [accountId, entry] : _botSessions)
    {
        if (!entry.Session)
            continue;

        // Runde T: laufendes Patrol vor dem Shutdown-Logout stoppen (siehe
        // LogoutBot()-Kommentar) - reine Feldzuruecksetzung, kein zusaetzlicher
        // DB-/Netzwerkzugriff, veraendert nichts an der bereits bestaetigt
        // sicheren Shutdown-Reihenfolge dieser Funktion.
        entry.PatrolActive = false;
        entry.IdleState = BotState::Idle;

        if (entry.Session->GetPlayer())
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::LogoutAllBots: logge Bot-Account {} vor dem "
                "Prozessende sauber aus (DB-Pool zu diesem Zeitpunkt noch garantiert intakt, "
                "siehe Kopfkommentar).", accountId);
            entry.Session->LogoutPlayer(true);
        }

        // Runde P: Online-Flag in der LoginDatabase (auth.account) hier zuruecksetzen,
        // solange der Pool noch lebt - der Destruktor selbst macht das fuer Bot-Sessions
        // nicht mehr (siehe WorldSession.cpp, IsBotSession()-Guard).
        LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE id = %u;", accountId);
    }
}

// --- Runde R (27.09.2026): sauberes Einzel-Logout waehrend laufendem Betrieb ---
// Siehe ausfuehrliche Begruendung im Kopfkommentar von BotMgr.h ("Runde R"). Kurz:
// derselbe LogoutPlayer(true)-Aufruf wie LogoutAllBots() (Runde N/P, dort waehrend
// OnShutdown() bereits zweifach live bestaetigt stabil, siehe lcf2r79-Bericht),
// hier aber zum ersten Mal waehrend normalem World-Update-Betrieb ausgeloest statt
// beim Shutdown - der DB-Pool ist dabei trivial aktiv (kein Teardown-Wettlauf wie
// beim Shutdown-Pfad moeglich). Die Session bleibt in _botSessions bestehen, damit
// ein erneuter Login desselben Accounts im selben Prozesslauf moeglich bleibt.
bool BotMgr::LogoutBot(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::LogoutBot: keine Bot-Session fuer Account {} vorhanden.", accountId);
        return false;
    }

    BotSessionEntry& entry = itr->second;

    // Runde T: ein laufendes Patrol MUSS vor dem Logout gestoppt werden - sonst
    // wuerde BotMgr::Tick() nach einem erneuten Login denselben Patrol-Zustand
    // (PatrolActive/PatrolCyclesRemaining) unveraendert vorfinden und unerwartet
    // fortsetzen ("Zombie"-Patrol). StopBotPatrol() ist idempotent (no-op falls
    // kein Patrol aktiv ist).
    StopBotPatrol(accountId);

    if (!entry.Session->GetPlayer())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: Account {} hat aktuell keinen Player in der Welt "
            "(bereits ausgeloggt oder nie eingeloggt) - nichts zu tun.", accountId);
        entry.State = BotCharacterState::STATE_UNINITIALIZED;
        entry.LoginRequested = false;
        return true;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: logge Bot-Account {} waehrend laufendem Betrieb sauber aus "
        "(gleicher LogoutPlayer(true)-Pfad wie LogoutAllBots(), diesmal im normalen World-Update-Kontext statt "
        "WorldScript::OnShutdown()).", accountId);
    entry.State = BotCharacterState::STATE_LOGGING_OUT;
    entry.Session->LogoutPlayer(true);

    // Online-Flag in der LoginDatabase (auth.account) explizit zuruecksetzen - folgt
    // bewusst demselben Muster wie LogoutAllBots() (Runde P), statt sich auf den
    // WorldSession-Destruktor zu verlassen (der fuer Bot-Sessions seit Runde P
    // grundsaetzlich keinen DB-Zugriff mehr macht, siehe WorldSession.cpp). Die Session
    // selbst wird hier NICHT zerstoert (kein Destruktor-Aufruf) - dieser Reset ist also
    // zusaetzlich zu, nicht anstelle von, einem etwaigen spaeteren Destruktor-Durchlauf.
    LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE id = %u;", accountId);

    entry.State = BotCharacterState::STATE_UNINITIALIZED;
    entry.LoginRequested = false;

    TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: Account {} erfolgreich ausgeloggt, Session bleibt fuer "
        "einen moeglichen erneuten Login im selben Prozesslauf bestehen.", accountId);
    return true;
}

// --- Runde S (27.09.2026): EIN einziger, isolierter Bewegungstest ---------
// Siehe ausfuehrliche Begruendung/Code-Review im Kopfkommentar von BotMgr.h
// ("Runde S") und im vollen Bericht
// C:\LegionServer\reports\lcf2r81_2026-09-27_playerbots_runde_s.md. Kurz: feste,
// kurze Distanz (8 Yards) in aktueller Blickrichtung, keine Pfadfindung,
// einmaliger Ausloeser - bewusst die kleinstmoegliche Bewegung, um
// MotionMaster/MovePoint erstmals gegen einen socketlosen Bot zu testen (seit
// der urspruenglichen Recherche vom 27.09.2026 als hoechstes verbleibendes
// Einzelrisiko benannt).
bool BotMgr::MoveBotTestStep(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStep: keine Bot-Session fuer Account {} vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStep: Account {} hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    constexpr float distance = 8.0f;
    float angle = player->GetOrientation();
    float startX = player->GetPositionX();
    float startY = player->GetPositionY();
    float startZ = player->GetPositionZ();
    float destX = startX + distance * std::cos(angle);
    float destY = startY + distance * std::sin(angle);
    float destZ = startZ;

    TC_LOG_INFO("scripts.bots", "BotMgr::MoveBotTestStep: Account {} - loese MovePoint() aus (generatePath=false), "
        "Start ({}, {}, {}) -> Ziel ({}, {}, {}).", accountId, startX, startY, startZ, destX, destY, destZ);

    // generatePath=false: bewusst keine Pfadfindung, siehe Auftragsvorgabe Runde S
    // ("gerade Linie, keine Pfadfindung/Pathing noetig").
    player->GetMotionMaster()->MovePoint(0, destX, destY, destZ, /*generatePath*/ false);
    return true;
}

// --- Runde U (27.09.2026): EIN einziger Navmesh-Pfadfindungstest (generatePath=true) ---
// Siehe ausfuehrliche Begruendung/Code-Review im Kopfkommentar von BotMgr.h ("Runde U") und im
// vollen Bericht C:\LegionServer\reports\lcf2r83_2026-09-27_playerbots_runde_u.md. Bewusst eine
// eigene, von MoveBotTestStep() (Runde S, generatePath=false) unabhaengige Methode - dieselbe
// 8-Yard-Distanz/Blickrichtungs-Logik, einziger Unterschied ist generatePath=true.
bool BotMgr::MoveBotTestStepPath(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStepPath: keine Bot-Session fuer Account {} vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStepPath: Account {} hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    constexpr float distance = 8.0f;
    float angle = player->GetOrientation();
    float startX = player->GetPositionX();
    float startY = player->GetPositionY();
    float startZ = player->GetPositionZ();
    float destX = startX + distance * std::cos(angle);
    float destY = startY + distance * std::sin(angle);
    float destZ = startZ;

    TC_LOG_INFO("scripts.bots", "BotMgr::MoveBotTestStepPath: Account {} - loese MovePoint() aus (generatePath=true, "
        "Navmesh/PathGenerator), Start ({}, {}, {}) -> Ziel ({}, {}, {}).", accountId, startX, startY, startZ, destX, destY, destZ);

    // generatePath=true: einziger Unterschied zu MoveBotTestStep() (Runde S) - siehe
    // Kopfkommentar in BotMgr.h ("Runde U") fuer den vollen Code-Review von
    // PathGenerator::CalculatePath()/MMapManager::GetNavMesh().
    player->GetMotionMaster()->MovePoint(0, destX, destY, destZ, /*generatePath*/ true);
    return true;
}

// --- Runde T (27.09.2026): mehrfache Bewegungen / einfaches Pendeln -------
// Siehe ausfuehrliche Begruendung im Kopfkommentar von BotMgr.h ("Runde T").
bool BotMgr::StartBotPatrol(uint32 accountId, uint32 cycles)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: keine Bot-Session fuer Account {} vorhanden.", accountId);
        return false;
    }

    BotSessionEntry& entry = itr->second;
    Player* player = entry.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: Account {} hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    if (entry.PatrolActive)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: Account {} hat bereits ein aktives Patrol "
            "({} von {} Zyklen verbleibend) - erst abwarten oder '.bottest move'/Logout nutzen.",
            accountId, entry.PatrolCyclesRemaining, entry.PatrolCyclesTotal);
        return false;
    }

    // Sicherheitsobergrenze gegen versehentliche Dauerlast - Auftragsvorgabe nennt
    // 5-10 Zyklen als Zielgroesse fuer diese Runde, 20 ist eine grosszuegige, aber
    // endliche Obergrenze.
    if (cycles == 0)
        cycles = 1;
    if (cycles > 20)
        cycles = 20;

    constexpr float distance = 8.0f;
    float angle = player->GetOrientation();
    entry.PatrolAX = player->GetPositionX();
    entry.PatrolAY = player->GetPositionY();
    entry.PatrolAZ = player->GetPositionZ();
    entry.PatrolBX = entry.PatrolAX + distance * std::cos(angle);
    entry.PatrolBY = entry.PatrolAY + distance * std::sin(angle);
    entry.PatrolBZ = entry.PatrolAZ;

    entry.PatrolCyclesTotal = cycles;
    entry.PatrolCyclesRemaining = cycles;
    entry.PatrolGoingToB = true;
    entry.PatrolActive = true;
    entry.IdleState = BotState::Patrolling;

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotPatrol: Account {} - starte Patrol ueber {} Zyklen, "
        "Punkt A ({}, {}, {}) <-> Punkt B ({}, {}, {}), generatePath=false.", accountId, cycles,
        entry.PatrolAX, entry.PatrolAY, entry.PatrolAZ, entry.PatrolBX, entry.PatrolBY, entry.PatrolBZ);

    // Erste Etappe (A->B) sofort auf denselben Weg wie MoveBotTestStep() anstossen -
    // die weiteren Etappen laufen tick-gesteuert aus BotMgr::Tick() (siehe oben).
    player->GetMotionMaster()->MovePoint(0, entry.PatrolBX, entry.PatrolBY, entry.PatrolBZ, /*generatePath*/ false);
    return true;
}

void BotMgr::StopBotPatrol(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end())
        return;

    if (itr->second.PatrolActive)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::StopBotPatrol: Account {} - Patrol manuell/sicherheitshalber "
            "abgebrochen ({} von {} Zyklen verbleibend), kein weiterer MovePoint()-Aufruf.",
            accountId, itr->second.PatrolCyclesRemaining, itr->second.PatrolCyclesTotal);
    }
    itr->second.PatrolActive = false;
    itr->second.IdleState = BotState::Idle;
}

bool BotMgr::IsBotPatrolActive(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    return itr != _botSessions.end() && itr->second.PatrolActive;
}

uint32 BotMgr::GetBotPatrolCyclesCompleted(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end())
        return 0;
    return itr->second.PatrolCyclesTotal - itr->second.PatrolCyclesRemaining;
}

uint32 BotMgr::GetBotPatrolCyclesTotal(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    return itr != _botSessions.end() ? itr->second.PatrolCyclesTotal : 0;
}

bool BotMgr::CreateBot(uint32 /*ownerAccountId*/, std::string const& botCharacterName)
{
    // Alter, generischer Einstiegspunkt aus der Skeleton-Runde (62/63) - bleibt
    // bewusst inert. Die tatsaechliche Runde-B-Implementierung laeuft ueber die
    // drei expliziten Phasen-Methoden oben (CreateBotAccount/
    // RequestCreateBotCharacter/RequestBotLogin), die von bot_commandscript.cpp
    // (".bottest ...") einzeln angesteuert werden, damit zwischen jeder Phase
    // Server.log/DBErrors.log geprueft werden kann (siehe Bericht Abschnitt 5).
    TC_LOG_INFO("scripts.bots", "BotMgr::CreateBot('{}'): bitte stattdessen die '.bottest'-Befehle verwenden "
        "(createaccount/createchar/login) - dieser generische Alt-Einstiegspunkt bleibt inert.", botCharacterName);
    return false;
}

bool BotMgr::RemoveBot(ObjectGuid /*botGuid*/)
{
    // Runde B implementiert kein sauberes Bot-Logout - bleibt Runde C
    // vorbehalten (siehe Bericht Abschnitt 7, "Naechste Schritte").
    return false;
}

IBotCharacter* BotMgr::GetBot(ObjectGuid botGuid) const
{
    auto itr = _bots.find(botGuid);
    return itr != _bots.end() ? itr->second.get() : nullptr;
}

void BotMgr::OnPlayerUpdate(Player* /*player*/, uint32 /*diff*/)
{
    // Bewusst weiterhin leer fuer den regulaeren Spieler-Pfad: Bot-Sessions
    // werden NICHT ueber diesen Hook getickt (der Hook feuert nur fuer echte,
    // eingeloggte Player-Objekte - eine Bot-Session ohne Player wuerde ihn nie
    // erreichen, bevor sie ueberhaupt eingeloggt ist). Taktung der
    // Bot-Sessions erfolgt separat ueber BotMgr::Tick(), aufgerufen aus einem
    // World-Update-Hook (siehe bot_scriptloader.cpp WorldScript-Ergaenzung).
}

void BotMgr::OnPlayerLogin(Player* /*player*/)
{
    // Bewusst leer in dieser Runde.
}

void BotMgr::OnPlayerLogout(Player* /*player*/)
{
    // Bewusst leer in dieser Runde.
}
