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
#include "ObjectAccessor.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "SharedDefines.h"
#include "LootPackets.h"
#include "Loot.h"
#include "Chat.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "Random.h"
#include "Group.h"
#include "GroupMgr.h"
#include "DB2Stores.h"
#include "DB2Structure.h"
#include "ItemDefines.h"
#include <cmath>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <algorithm>

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
        TC_LOG_INFO("scripts.bots", "BotMgr::CreateBotAccount: Account '%s' existiert bereits (Id %u), nichts angelegt (idempotent).",
            accountName, outAccountId);
        return true;
    }

    AccountOpResult result = sAccountMgr->CreateAccount(accountName, password, "", /*bnetAccountId*/ 0, /*bnetIndex*/ 0);
    if (result != AccountOpResult::AOR_OK)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::CreateBotAccount: AccountMgr::CreateAccount('%s') fehlgeschlagen, AccountOpResult=%u.",
            accountName, uint32(result));
        return false;
    }

    outAccountId = AccountMgr::GetId(accountName);
    TC_LOG_INFO("scripts.bots", "BotMgr::CreateBotAccount: Bot-Account '%s' neu angelegt, Id %u (kein BNet-Account, bnetAccountId=0).",
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
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Account %u existiert nicht.", accountId);
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
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Bot-Charaktername '%s' konnte nicht "
            "normalisiert werden (leer nach UTF8/Locale-Konvertierung?) - abgebrochen, keine Opcode-Aufrufe ausgeloest.",
            charName);
        return false;
    }

    ResponseCodes nameCheck = ObjectMgr::CheckPlayerName(normalizedName, LOCALE_enUS, true);
    if (nameCheck != CHAR_NAME_SUCCESS)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: Bot-Charaktername '%s' ist fuer den Core "
            "ungueltig (ObjectMgr::CheckPlayerName -> ResponseCode %u) - haeufigste Ursache: Ziffern/Sonderzeichen "
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

    TC_LOG_INFO("scripts.bots", "BotMgr::RequestCreateBotCharacter: loese HandleCharCreateOpcode fuer '%s' (Account %u) aus - "
        "Ergebnis ist asynchron, siehe Server.log/DBErrors.log nach den naechsten Tick()-Aufrufen.", charName, accountId);
    entry.Session->HandleCharCreateOpcode(charCreate);
    return true;
}

bool BotMgr::RequestBotLogin(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestBotLogin: keine Bot-Session fuer Account %u vorhanden - "
            "erst RequestCreateBotCharacter() aufrufen.", accountId);
        return false;
    }

    BotSessionEntry& entry = itr->second;
    if (entry.Session->GetPlayer())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::RequestBotLogin: Account %u hat bereits einen Player in der Welt.", accountId);
        return true;
    }

    entry.LoginRequested = true;
    entry.State = BotCharacterState::STATE_LOGGING_IN;

    // Schritt 1: CharEnum ausloesen - fuellt _legitCharacters (siehe
    // Kopfkommentar Punkt 1). Asynchron; Schritt 2 folgt in Tick().
    WorldPacket enumPacket(CMSG_ENUM_CHARACTERS, 0);
    WorldPackets::Character::EnumCharacters enumCharacters(std::move(enumPacket));
    TC_LOG_INFO("scripts.bots", "BotMgr::RequestBotLogin: loese HandleCharEnumOpcode fuer Account %u aus.", accountId);
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

                TC_LOG_INFO("scripts.bots", "BotMgr::Tick: CharEnum fuer Account %u fertig, Charakter %s gefunden - "
                    "loese HandlePlayerLoginOpcode()+HandleContinuePlayerLogin() aus (normalzero-Muster).",
                    accountId, charGuid.ToString());

                // normalzero/LegionPlayerBot-Muster: beide Haelften des sonst
                // durch die zweite Client-Verbindung ausgeloesten Handshakes
                // manuell und synchron hintereinander aufrufen.
                entry.Session->HandlePlayerLoginOpcode(playerLogin);
                if (entry.Session->PlayerLoading())
                    entry.Session->HandleContinuePlayerLogin();
                else
                    TC_LOG_ERROR("scripts.bots", "BotMgr::Tick: HandlePlayerLoginOpcode fuer Account %u hat den Bot "
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
                TC_LOG_DEBUG("scripts.bots", "[BotIdle] Account %u (IdleState=Idle): periodischer Diagnose-Tick, "
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
                    TC_LOG_ERROR("scripts.bots", "BotMgr::Tick: Account %u waehrend aktivem Patrol nicht mehr "
                        "IsInWorld() - Patrol sicherheitshalber abgebrochen.", accountId);
                    entry.PatrolActive = false;
                    entry.IdleState = BotState::Idle;
                }
                else if (patrolPlayer->movespline->Finalized())
                {
                    if (entry.PatrolGoingToB)
                    {
                        // Etappe A->B abgeschlossen - Rueckweg B->A antreten.
                        TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account %u Patrol - Punkt B erreicht, "
                            "Rueckweg zu Punkt A (Zyklus %u/%u).", accountId,
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

                        TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account %u Patrol - Punkt A erreicht, Zyklus "
                            "abgeschlossen (%u von %u verbleibend).", accountId, entry.PatrolCyclesRemaining,
                            entry.PatrolCyclesTotal);

                        if (entry.PatrolCyclesRemaining == 0)
                        {
                            entry.PatrolActive = false;
                            entry.IdleState = BotState::Idle;
                            TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account %u Patrol - alle %u Zyklen "
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
            TC_LOG_INFO("scripts.bots", "BotMgr::LogoutAllBots: logge Bot-Account %u vor dem "
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
        TC_LOG_ERROR("scripts.bots", "BotMgr::LogoutBot: keine Bot-Session fuer Account %u vorhanden.", accountId);
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
        TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: Account %u hat aktuell keinen Player in der Welt "
            "(bereits ausgeloggt oder nie eingeloggt) - nichts zu tun.", accountId);
        entry.State = BotCharacterState::STATE_UNINITIALIZED;
        entry.LoginRequested = false;
        return true;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: logge Bot-Account %u waehrend laufendem Betrieb sauber aus "
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

    TC_LOG_INFO("scripts.bots", "BotMgr::LogoutBot: Account %u erfolgreich ausgeloggt, Session bleibt fuer "
        "einen moeglichen erneuten Login im selben Prozesslauf bestehen.", accountId);
    return true;
}

// --- Runde 93 (28.09.2026): EIN einziger Kartenwechsel-Live-Test -----------
// Siehe ausfuehrliche Begruendung/Code-Review im Kopfkommentar von BotMgr.h
// ("Runde 93"), lcf2r89 (Blocker-Fund) und lcf2r90 (volle Zeile-fuer-Zeile-Review
// von WorldSession::HandleMoveWorldportAck()). Ruft player->TeleportTo() auf,
// danach sofort manuell botSession->HandleMoveWorldportAck() - analog zum
// bereits produktiven Login-Muster (Runde B).
bool BotMgr::TeleportBot(uint32 accountId, uint32 mapId, float x, float y, float z, float orientation)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::TeleportBot: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    WorldSession* botSession = itr->second.Session.get();
    Player* player = botSession->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::TeleportBot: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    uint32 sourceMapId = player->GetMapId();
    TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - rufe TeleportTo() auf: Map %u (%f, %f, %f) -> "
        "Map %u (%f, %f, %f, o=%f).", accountId, sourceMapId, player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), mapId, x, y, z, orientation);

    // --- Runde 106 (28.09.2026): Bot-lokaler Bypass fuer die Broken-Isles-Levelsperre ---
    //
    // Voller Code-Review/Bericht: C:\LegionServer\reports\lcf2r106_2026-09-28_playerbots_map1220_fix.md
    // (Vorlauf: lcf2r104, Player.cpp:1587-1588 identifiziert als Ursache fuer
    // ".bottest teleport"-Fehlschlag auf Map 1220/Broken Isles: hartcodiertes
    // "mapid == MAP_BROKEN_ISLANDS && !IsGameMaster() && getLevel() < 98 && ..."-Gate,
    // KEIN Bug, beabsichtigte Spielmechanik).
    //
    // Bewusst NICHT: Player.cpp/TeleportTo() selbst aendern (Option c aus lcf2r104,
    // haette eine neue RBAC-Permission + Account-Zuweisung gebraucht - mehr bewegliche
    // Teile, staerkerer Eingriff an sicherheitsrelevanter Stelle). Bewusst NICHT: den
    // Bot dauerhaft/beim Login in GM-Modus versetzen (haette dauerhafte Nebenwirkungen
    // von Player::SetGameMaster(), siehe unten, waehrend spaeterer Existenz-/Spawn-Checks
    // verursacht). Stattdessen: GM-Flag nur fuer die Dauer dieses EINEN synchronen
    // TeleportTo()-Aufrufs setzen, direkt hier in BotMgr::TeleportBot() - diese Methode
    // wird ausschliesslich mit einem Player* aus _botSessions aufgerufen (siehe Lookup
    // oben), erreicht also nie einen echten Spieler/GM.
    //
    // Player::SetGameMaster(true) (Player.cpp:2376-2399) ist der EINZIGE Code, der
    // PLAYER_EXTRA_GM_ON setzt (neben SetGameMaster(false), das es wieder loescht) -
    // m_ExtraFlags ist private, es gibt keinen schlankeren offiziellen Setter. Die Methode
    // hat Nebenwirkungen ueber das reine Flag hinaus (SetFaction(35), UNIT_FLAG2_ALLOW_CHEAT_SPELLS,
    // PhasingHandler::SetAlwaysVisible, Contested-PvP-Reset, HostileRefManager-Offline-Schaltung).
    // Fuer einen dauerhaften GM-Bot waeren das reale Risiken fuer Existenz-/Spawn-Checks
    // (Fraktion 35 aendert NPC-Hostility-Reaktionen, was einen Spawn-/Aggro-Check verfaelschen
    // koennte) - deshalb NICHT dauerhaft, sondern nur fuer die Dauer dieses einzigen,
    // synchronen Funktionsaufrufs, danach sofort wieder per SetGameMaster(false) exakt auf den
    // Zustand vor dem Aufruf zurueckgesetzt (dieselbe Restore-Logik, die auch ein echtes
    // ".gm off" nutzt: setFactionForRace(), RemoveFlag(PLAYER_FLAGS_GM), etc.). Zwischen
    // SetGameMaster(true) und SetGameMaster(false) laeuft kein Map::Update()-Tick und kein
    // Netzwerk-Broadcast an andere Spieler (TeleportTo() selbst ist synchron bis zum
    // fruehen "return false"/bis zum Start der Fern-Teleport-Vorbereitung) - kein Fenster,
    // in dem die temporaeren Nebenwirkungen fuer Dritte sichtbar werden koennten.
    //
    // Nur fuer das betroffene Map-Ziel aktiviert (mapId == MAP_BROKEN_ISLANDS), damit das
    // Verhalten fuer alle anderen ".bottest teleport"-Ziele (Map 571, 1, ...) exakt
    // unveraendert bleibt (kein GM-Toggle, wo er nicht gebraucht wird - kleinstmoeglicher
    // Blast-Radius).
    //
    // Runde 113 (28.09.2026, lcf2r113): additiv Map 1152 ("FW Horde Garrison Level 1",
    // InstanceType=1, echte Dungeon-Instanz mit instance_template-Zeile) aufgenommen, um
    // den in lcf2r112 analysierten Dungeon-Instanz-Betreten-Pfad (MapManager::PlayerCannotEnter(),
    // Zeile 172-173 "Bypass checks for GMs") erstmals live zu testen - dieselbe Begruendung/
    // dasselbe Zeitfenster-Argument wie beim Map-1220-Fix (Runde 106) gilt unveraendert: das
    // GM-Flag wird nur fuer die Dauer dieses einen synchronen TeleportTo()-Aufrufs gesetzt.
    // Runde 118 (28.09.2026, lcf2r118): additiv Map 1494 ("The Violet Hold"/"AcquisitionVioletHold",
    // InstanceType=5, IsDungeon()==true da nicht IsGarrison()/IsWorldPvPMap()) aufgenommen, nachdem
    // die zugehoerige world.instance_template-Zeile in dieser Runde ergaenzt wurde (vorher fehlte sie
    // komplett, CANNOT_ENTER_UNINSTANCED_DUNGEON noch vor jedem GM-Check). Anders als Map 1152 (R113)
    // ist 1494 KEINE Garnisonskarte, der GM-Bypass wird hier also tatsaechlich gebraucht.
    bool needsGmBypass = (mapId == MAP_BROKEN_ISLANDS || mapId == 1152 || mapId == 1494) && !player->IsGameMaster();
    if (needsGmBypass)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - Ziel ist Map %u (Broken Isles/Level- "
            "oder Instanz-Zugangssperre) und Bot ist nicht bereits im GM-Modus: aktiviere GM-Flag NUR fuer die "
            "Dauer dieses TeleportTo()-Aufrufs.", accountId, mapId);
        player->SetGameMaster(true);
    }

    bool teleportOk = player->TeleportTo(mapId, x, y, z, orientation);

    if (needsGmBypass)
    {
        player->SetGameMaster(false);
        TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - GM-Flag nach TeleportTo()-Aufruf wieder "
            "deaktiviert (Bypass war nur temporaer).", accountId);
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - TeleportTo() Rueckgabe=%d, "
        "IsBeingTeleportedFar()=%d nach dem Aufruf.", accountId, teleportOk, player->IsBeingTeleportedFar());

    if (!teleportOk)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::TeleportBot: Account %u - TeleportTo() lieferte false, "
            "HandleMoveWorldportAck() wird NICHT aufgerufen (kein Kartenwechsel eingeleitet).", accountId);
        return false;
    }

    // Fuer den "gleiche Map"-Zweig von Player::TeleportTo() (siehe Player.cpp) wird
    // IsBeingTeleportedFar() nie gesetzt - dort ist der Teleport nach TeleportTo() bereits
    // vollstaendig abgeschlossen und ein zusaetzlicher HandleMoveWorldportAck()-Aufruf waere
    // ein No-Op (die Funktion selbst prueft das per fruehem "if (!IsBeingTeleportedFar()) return;",
    // siehe lcf2r90 Zeile 49-50) - trotzdem derselbe Aufruf fuer beide Faelle, um den Code
    // symmetrisch und unabhaengig vom tatsaechlichen Map-Wechsel-Zweig zu halten.
    TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - rufe manuell HandleMoveWorldportAck() auf "
        "(ersetzt den vom Bot nie gesendeten Client-Ack MSG_MOVE_WORLDPORT_ACK).", accountId);

    botSession->HandleMoveWorldportAck();

    // Map::AddPlayerToMap() selbst liegt in MovementHandler.cpp (andere Compilation-Unit) und
    // gibt BotMgr keinen direkten Rueckgabewert - als Ersatzindikator (siehe lcf2r90-Empfehlung
    // Punkt 4 und BotMgr.h-Kommentar) wird hier player->GetMapId() gegen das gewuenschte Ziel
    // verglichen: weicht die tatsaechliche Map vom Ziel ab (z.B. weil einer der beiden
    // Homebind-Fallback-Pfade aus lcf2r90, Zeilen 88-93/114-121, gegriffen hat), ist das ein
    // starkes Indiz fuer einen fehlgeschlagenen Kartenwechsel statt eines erfolgreichen.
    bool arrivedAtTarget = player->IsInWorld() && player->GetMapId() == mapId;
    TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - nach HandleMoveWorldportAck(): IsInWorld()=%d, "
        "GetMapId()=%u (Ziel war %u), Position (%f, %f, %f) - %s.", accountId, player->IsInWorld(),
        player->GetMapId(), mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        arrivedAtTarget ? "ZIEL ERREICHT" : "ABWEICHUNG - moeglicher Homebind-Fallback, Server.log pruefen");

    return arrivedAtTarget;
}

// --- Runde 122 (28.09.2026): EIN einziger Kampf-Live-Test (Auto-Attack) ----
// Siehe ausfuehrliche Begruendung im Kopfkommentar von BotMgr.h ("Runde 122") und im Code-Review
// lcf2r121. Ruft ausschliesslich player->Attack(target, true) auf (reiner Melee-Auto-Attack, KEIN
// Spell-Cast) - der eigentliche periodische Schaden/die Pakete laufen danach tick-gesteuert ueber den
// normalen Unit::AttackerStateUpdate()-Zyklus, genau wie bei jedem echten Spieler.
bool BotMgr::StartBotAttack(uint32 accountId, ObjectGuid::LowType targetGuid)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    CreatureData const* data = sObjectMgr->GetCreatureData(targetGuid);
    if (!data)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: Account %u - keine Spawn-Daten fuer targetGuid " UI64FMTD " "
            "gefunden (creature-Tabelle).", accountId, targetGuid);
        return false;
    }

    if (data->mapid != player->GetMapId())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: Account %u - Ziel-Spawn " UI64FMTD " ist auf Map %u, Bot steht "
            "aber auf Map %u (erst per '.bottest teleport' dorthin bringen).", accountId, targetGuid, data->mapid,
            player->GetMapId());
        return false;
    }

    // Wichtiger Korrekturfund (Runde 122, waehrend des Live-Tests entdeckt): die tatsaechliche
    // Laufzeit-ObjectGuid einer Kreatur wird NICHT aus der DB-Spawn-Id gebildet - Creature::
    // LoadCreatureFromDB() (Creature.cpp:1528) ruft Create(map->GenerateLowGuid<HighGuid::Creature>(),
    // ...) auf, also einen pro-Map fortlaufenden Laufzeit-Zaehler, voellig unabhaengig von der
    // DB-Spalte `creature`.`guid`. Ein direkt aus (mapId, entry, spawnId) gebautes
    // ObjectGuid::Create<HighGuid::Creature>(...) passt deshalb so gut wie nie zur echten Laufzeit-Guid
    // (erste Testversuche dieser Runde scheiterten dadurch reproduzierbar, unabhaengig davon ob die
    // Karte/das Grid tatsaechlich aktiv war). Korrekter Weg: Creature::LoadCreatureFromDB() (Zeile 241)
    // pflegt zusaetzlich Map::GetCreatureBySpawnIdStore() (unordered_multimap<SpawnId, Creature*>,
    // Map.h:471-472, public) - exakt die vom Core selbst fuer denselben Zweck (Duplikat-Erkennung beim
    // Neuladen) genutzte Spawn-Id->Creature*-Abbildung. Wir nutzen dieselbe Map hier read-only.
    Creature* target = nullptr;
    auto creatureRange = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(targetGuid);
    for (auto rangeItr = creatureRange.first; rangeItr != creatureRange.second; ++rangeItr)
    {
        if (rangeItr->second && rangeItr->second->IsInWorld())
        {
            target = rangeItr->second;
            break;
        }
    }

    if (!target)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: Account %u - Zielkreatur (Spawn " UI64FMTD ", Entry %u) ist "
            "aktuell nicht als lebendes Objekt im Grid geladen (zu weit weg/nicht gespawnt).", accountId, targetGuid,
            data->id);
        return false;
    }

    if (!target->IsAlive())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotAttack: Account %u - Zielkreatur (Spawn " UI64FMTD ") ist bereits tot.",
            accountId, targetGuid);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotAttack: Account %u - rufe player->Attack(target, true) auf. Bot-Pos "
        "(%f, %f, %f), Ziel Entry %u Spawn " UI64FMTD " Pos (%f, %f, %f), Distanz %f.", accountId,
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), data->id, targetGuid,
        target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), player->GetDistance(target));

    // Diagnose-Zusatz (Runde 122, waehrend des Live-Tests ergaenzt): Unit::Attack() (Unit.cpp:5837)
    // hat mehrere fruehe "return false"-Ausstiege VOR dem eigentlichen Angriffsstart - bei einem
    // unerwarteten Fehlschlag hier einzeln protokollieren, statt nur das Endergebnis zu sehen.
    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotAttack: Account %u - Vorbedingungen: player->IsAlive()=%d, "
        "target->IsInWorld()=%d, target->IsAlive()=%d, player->IsMounted()=%d, player->HasFlag(PACIFIED)=%d, "
        "target->IsEvadingAttacks()=%d.", accountId, player->IsAlive(), target->IsInWorld(), target->IsAlive(),
        player->IsMounted(), player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED),
        target->IsEvadingAttacks());

    bool attackOk = player->Attack(target, true);

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotAttack: Account %u - Attack() Rueckgabe=%d, "
        "HasUnitState(MELEE_ATTACKING)=%d, GetVictim() gesetzt=%d.", accountId, attackOk,
        player->HasUnitState(UNIT_STATE_MELEE_ATTACKING), player->GetVictim() != nullptr);

    return attackOk;
}

// --- Runde 122 (28.09.2026): Gegenstueck - EIN sofortiger Kampfabbruch -----
// Siehe Kopfkommentar BotMgr.h ("Runde 122"). Ruft ausschliesslich player->AttackStop() auf.
void BotMgr::StopBotAttack(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StopBotAttack: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StopBotAttack: Account %u hat aktuell keinen Player in der Welt.",
            accountId);
        return;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::StopBotAttack: Account %u - rufe player->AttackStop() auf (vorher "
        "HasUnitState(MELEE_ATTACKING)=%d).", accountId, player->HasUnitState(UNIT_STATE_MELEE_ATTACKING));

    player->AttackStop();

    TC_LOG_INFO("scripts.bots", "BotMgr::StopBotAttack: Account %u - nach AttackStop(): "
        "HasUnitState(MELEE_ATTACKING)=%d, GetVictim() gesetzt=%d.", accountId,
        player->HasUnitState(UNIT_STATE_MELEE_ATTACKING), player->GetVictim() != nullptr);
}

// --- Runde 129 (28.09.2026): Testbot-Unverwundbarkeit -----------------------------------------
// Siehe vollen Code-Review/Begruendung im Kopfkommentar BotMgr.h ("Runde 129"). Reiner
// Unit::ApplySpellImmune()-Aufruf (bereits oeffentlich, vom Core selbst z.B. fuer Divine Shield
// genutzt) - kein core-weiter Damage-Bypass, kein neuer Code in Unit.cpp/Player.cpp noetig.
// PLATZHALTER_SPELL_ID dient nur als Schluessel fuer das apply=true/false-Paar, ist keine echte
// Spell-ID und loest keinerlei Content-/Aura-Sichtbarkeitseffekt aus.
namespace
{
    uint32 constexpr BOT_TEST_INVULN_KEY = 999999;
}

bool BotMgr::SetBotTestInvulnerable(uint32 accountId, bool enable)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SetBotTestInvulnerable: keine Bot-Session fuer Account %u vorhanden.",
            accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SetBotTestInvulnerable: Account %u hat aktuell keinen Player in der "
            "Welt.", accountId);
        return false;
    }

    player->ApplySpellImmune(BOT_TEST_INVULN_KEY, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_ALL, enable);

    TC_LOG_INFO("scripts.bots", "BotMgr::SetBotTestInvulnerable: Account %u - Testbot-Damage-Immunitaet jetzt %s. "
        "NUR fuer Kampf-Livetests gedacht - nach dem Test unbedingt mit enable=false wieder deaktivieren.",
        accountId, enable ? "AN" : "AUS");
    return true;
}

// --- Runde 129 (28.09.2026): Loot-Implementierung (Plan aus lcf2r128 Abschnitt 7) --------------
// Siehe vollen Code-Review/Begruendung im Kopfkommentar BotMgr.h ("Runde 129"). 3-Schritt-Opcode-
// Nachbau, direkt/synchron ueber HandleLootOpcode()/HandleAutostoreLootItemOpcode()/
// HandleLootReleaseOpcode() - exakt dasselbe Direktaufruf-Muster wie StartBotAttack() oben.
bool BotMgr::BotLootTarget(uint32 accountId, ObjectGuid::LowType targetGuid)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    WorldSession* session = itr->second.Session.get();
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    CreatureData const* data = sObjectMgr->GetCreatureData(targetGuid);
    if (!data)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: Account %u - keine Spawn-Daten fuer targetGuid " UI64FMTD " "
            "gefunden (creature-Tabelle).", accountId, targetGuid);
        return false;
    }

    if (data->mapid != player->GetMapId())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: Account %u - Ziel-Spawn " UI64FMTD " ist auf Map %u, Bot steht "
            "aber auf Map %u.", accountId, targetGuid, data->mapid, player->GetMapId());
        return false;
    }

    // Dieselbe Spawn-Id->Laufzeit-Creature*-Aufloesung wie in StartBotAttack() (siehe dortigen
    // ausfuehrlichen Kommentar zu Map::GetCreatureBySpawnIdStore()) - hier aber MUSS das Ziel bereits
    // tot sein (Gegenstueck zur Lebend-Pruefung in StartBotAttack()).
    Creature* target = nullptr;
    auto creatureRange = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(targetGuid);
    for (auto rangeItr = creatureRange.first; rangeItr != creatureRange.second; ++rangeItr)
    {
        if (rangeItr->second && rangeItr->second->IsInWorld())
        {
            target = rangeItr->second;
            break;
        }
    }

    if (!target)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: Account %u - Zielkreatur (Spawn " UI64FMTD ", Entry %u) ist "
            "aktuell nicht als lebendes/totes Objekt im Grid geladen.", accountId, targetGuid, data->id);
        return false;
    }

    if (target->IsAlive())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotLootTarget: Account %u - Zielkreatur (Spawn " UI64FMTD ") lebt noch, "
            "kein Loot moeglich.", accountId, targetGuid);
        return false;
    }

    // --- Schritt 1: CMSG_LOOT-Nachbau -----------------------------------------------------------
    WorldPacket lootUnitRaw(CMSG_LOOT_UNIT, 8);
    WorldPackets::Loot::LootUnit lootUnit(std::move(lootUnitRaw));
    lootUnit.Unit = target->GetGUID();

    TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - rufe HandleLootOpcode() fuer Ziel %s "
        "(Spawn " UI64FMTD ") auf.", accountId, target->GetGUID().ToString(), targetGuid);
    session->HandleLootOpcode(lootUnit);

    ObjectGuid lootObjGuid = target->loot.GetGUID();
    TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - nach HandleLootOpcode(): "
        "player->GetLootGUID()=%s, target->loot.GetGUID()=%s, target->loot.items.size()=%zu, "
        "target->loot.gold=%u.", accountId, player->GetLootGUID().ToString(), lootObjGuid.ToString(),
        target->loot.items.size(), target->loot.gold);

    // --- Runde 131 (28.09.2026): Diagnose-Erweiterung (Plan aus lcf2r130 Abschnitt 5) -----------
    // Zaehlt roh (vor jeder Filterung) UND pro Slot das AllowedForPlayer()-Ergebnis, damit die zwei
    // R130-Kandidaten (FillLoot() liefert 0 Items vs. AllowedForPlayer()-Filterung blockt alles)
    // eindeutig unterscheidbar werden - genau das war mit den {}-kaputten TC_LOG-Zeilen vorher nicht
    // moeglich. PSendSysMessage zusaetzlich zu TC_LOG, analog zum etablierten "attack-diag"-Muster.
    uint32 rawItemCount = uint32(target->loot.items.size());
    uint32 allowedCount = 0;
    for (size_t i = 0; i < target->loot.items.size(); ++i)
    {
        LootItem const& li = target->loot.items[i];
        bool looted = li.is_looted;
        bool currency = li.currency;
        bool allowed = li.AllowedForPlayer(player);
        if (allowed && !looted && !currency)
            ++allowedCount;

        TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - [loot-diag] Slot %zu: itemid=%u, "
            "is_looted=%d, currency=%d, AllowedForPlayer()=%d.", accountId, i, li.itemid, looted, currency, allowed);
        ChatHandler(session).PSendSysMessage("[loot-diag] Slot %u: itemid=%u, is_looted=%u, currency=%u, AllowedForPlayer()=%u",
            uint32(i), li.itemid, uint32(looted), uint32(currency), uint32(allowed));
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - [loot-diag] Zusammenfassung: loot_id(Entry)=%u, "
        "rawItemCount(vor Filterung)=%u, allowedCount(nach AllowedForPlayer())=%u, target->loot.gold=%u.",
        accountId, data->id, rawItemCount, allowedCount, target->loot.gold);
    ChatHandler(session).PSendSysMessage("[loot-diag] Entry=%u rawItemCount=%u allowedCount=%u gold=%u",
        data->id, rawItemCount, allowedCount, target->loot.gold);

    // --- Schritt 2: CMSG_LOOT_ITEM-Nachbau (alle berechtigten Slots in EINEM Paket) -------------
    WorldPacket itemPacketRaw(CMSG_LOOT_ITEM, 8);
    WorldPackets::Loot::LootItem itemPacket(std::move(itemPacketRaw));
    uint32 queuedCount = 0;
    for (size_t i = 0; i < target->loot.items.size(); ++i)
    {
        LootItem const& li = target->loot.items[i];
        if (li.is_looted || li.currency)
            continue;
        if (!li.AllowedForPlayer(player))
            continue;

        WorldPackets::Loot::LootRequest req;
        req.Object = lootObjGuid;
        req.LootListID = uint8(i + 1); // dieselbe 1-basierte Zaehlung wie Loot::BuildLootResponse() (Loot.cpp)
        itemPacket.Loot.push_back(req);
        ++queuedCount;
    }

    if (queuedCount > 0)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - rufe HandleAutostoreLootItemOpcode() fuer "
            "%u Item-Slot(s) auf.", accountId, queuedCount);
        session->HandleAutostoreLootItemOpcode(itemPacket);
    }
    else
        TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - keine loot-berechtigten Item-Slots "
            "(leere Loot-Tabelle oder alles bereits gelootet) - ueberspringe Schritt 2.", accountId);

    // --- Schritt 3: CMSG_LOOT_RELEASE-Nachbau ---------------------------------------------------
    WorldPacket lootReleaseRaw(CMSG_LOOT_RELEASE, 8);
    WorldPackets::Loot::LootRelease lootRelease(std::move(lootReleaseRaw));
    lootRelease.Unit = target->GetGUID();

    TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - rufe HandleLootReleaseOpcode() auf.", accountId);
    session->HandleLootReleaseOpcode(lootRelease);

    TC_LOG_INFO("scripts.bots", "BotMgr::BotLootTarget: Account %u - fertig. queuedCount=%u, "
        "target->loot.isLooted()=%d.", accountId, queuedCount, target->loot.isLooted());
    ChatHandler(session).PSendSysMessage("[loot-diag] fertig: queuedCount=%u, isLooted=%u", queuedCount, uint32(target->loot.isLooted()));
    return true;
}

// --- Runde 132 (28.09.2026): Tod-Handling (Release/Graveyard/Resurrection Sickness) ------------
// Siehe vollen Code-Review/Begruendung im Kopfkommentar BotMgr.h ("Runde 132"). Direktaufruf-Muster
// wie BotLootTarget()/StartBotAttack() - dieselben bereits oeffentlichen Player-/WorldSession-
// Methoden, die auch WorldSession::HandleRepopRequest() nutzt, synchron statt Opcode-getrieben.
bool BotMgr::HandleBotDeath(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::HandleBotDeath: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    WorldSession* session = itr->second.Session.get();
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::HandleBotDeath: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    if (player->IsAlive())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::HandleBotDeath: Account %u - Player lebt noch (IsAlive()=1), kein "
            "Tod zu verarbeiten.", accountId);
        return false;
    }

    if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u ist bereits Geist (PLAYER_FLAGS_GHOST) - "
            "nichts zu tun (idempotent).", accountId);
        return true;
    }

    // Gleicher stiller Abbruch wie im echten HandleRepopRequest() (MiscHandler.cpp:67-68) - der
    // Client wuerde hier selbst den Fehler anzeigen, fuer uns reicht eine Log-Zeile.
    if (player->HasAuraType(SPELL_AURA_PREVENT_RESURRECTION))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::HandleBotDeath: Account %u hat SPELL_AURA_PREVENT_RESURRECTION - "
            "Release/Graveyard-Ablauf abgebrochen (entspricht dem stillen Abbruch in HandleRepopRequest()).",
            accountId);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - getDeathState()=%u, Position vor Release "
        "(%f, %f, %f) Map %u.", accountId, uint32(player->getDeathState()), player->GetPositionX(),
        player->GetPositionY(), player->GetPositionZ(), player->GetMapId());

    // Schritt 1: analog HandleRepopRequest() - Race-Fenster zwischen Server-Tod und Verarbeitung
    // (siehe dortiger Kommentar) auch fuer den Bot abfangen, statt blind vorauszusetzen dass
    // KillPlayer() bereits gelaufen ist.
    if (player->getDeathState() == JUST_DIED)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - getDeathState()==JUST_DIED, rufe "
            "KillPlayer() auf (setzt CORPSE-Zustand, startet Reclaim-Timer).", accountId);
        player->KillPlayer();
    }

    // Schritt 2: Geist-Zustand herstellen (Corpse-Objekt, SetHealth(1), SPELL_AURA_GHOST) - kein
    // Teleport in diesem Schritt.
    TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - rufe BuildPlayerRepop() auf.", accountId);
    player->BuildPlayerRepop();

    // Schritt 3: zum naechsten Friedhof teleportieren, Geist bleibt dort stehen (keine volle
    // Wiederbelebung - das macht erst ReviveBotAtGraveyard()/SendSpiritResurrect()).
    TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - rufe RepopAtGraveyard() auf.", accountId);
    player->RepopAtGraveyard();

    // Dasselbe Muster wie TeleportBot() (Runde 93): RepopAtGraveyard() ruft TeleportTo() intern auf -
    // liegt der Friedhof auf einer anderen Map, ist der Wechsel erst nach einem manuellen
    // HandleMoveWorldportAck()-Aufruf abgeschlossen (der sonst nie eintreffende Client-Ack).
    if (player->IsBeingTeleportedFar())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - RepopAtGraveyard() hat einen "
            "Kartenwechsel eingeleitet (IsBeingTeleportedFar()=1), rufe manuell HandleMoveWorldportAck() auf.",
            accountId);
        session->HandleMoveWorldportAck();
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::HandleBotDeath: Account %u - fertig. IsAlive()=%d, "
        "HasFlag(PLAYER_FLAGS_GHOST)=%d, Map %u, Position (%f, %f, %f).", accountId, player->IsAlive(),
        player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST), player->GetMapId(), player->GetPositionX(),
        player->GetPositionY(), player->GetPositionZ());

    return true;
}

// --- Runde 132 (28.09.2026): Gegenstueck - volle Wiederbelebung MIT Resurrection Sickness --------
// Siehe Kopfkommentar BotMgr.h ("Runde 132"). Ruft die bereits oeffentliche
// WorldSession::SendSpiritResurrect() direkt auf (dieselbe Methode, die auch
// HandleSpiritHealerActivate() beim echten Spirit-Healer-Rechtsklick nutzt) - kein neuer Core-Code.
bool BotMgr::ReviveBotAtGraveyard(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ReviveBotAtGraveyard: keine Bot-Session fuer Account %u vorhanden.",
            accountId);
        return false;
    }

    WorldSession* session = itr->second.Session.get();
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u hat aktuell keinen Player in der "
            "Welt (erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    if (player->IsAlive())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u lebt bereits - nichts zu tun "
            "(idempotent).", accountId);
        return true;
    }

    if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u ist tot, aber noch kein Geist "
            "(PLAYER_FLAGS_GHOST fehlt) - erst '.bottest release %u' ausfuehren.", accountId, accountId);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u - rufe SendSpiritResurrect() auf "
        "(ResurrectPlayer(0.5f, applySickness=true) + DurabilityLossAll(0.25f) + SpawnCorpseBones(), ggf. "
        "zweiter TeleportTo() falls Leichnam-Friedhof != Geist-Friedhof).", accountId);
    session->SendSpiritResurrect();

    // Dasselbe Muster wie in HandleBotDeath()/TeleportBot(): SendSpiritResurrect() kann intern ein
    // zweites TeleportTo() ausloesen (NPCHandler.cpp, "corpseGrave != ghostGrave"-Zweig) - falls das
    // eine andere Map trifft, muss auch hier manuell nachgeholfen werden.
    if (player->IsBeingTeleportedFar())
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u - SendSpiritResurrect() hat einen "
            "zusaetzlichen Kartenwechsel eingeleitet (IsBeingTeleportedFar()=1), rufe manuell "
            "HandleMoveWorldportAck() auf.", accountId);
        session->HandleMoveWorldportAck();
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::ReviveBotAtGraveyard: Account %u - fertig. IsAlive()=%d, "
        "HasFlag(PLAYER_FLAGS_GHOST)=%d, HasAura(15007/Resurrection Sickness)=%d, Map %u, Position (%f, %f, %f).",
        accountId, player->IsAlive(), player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST),
        player->HasAura(15007), player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ());

    return true;
}

// --- Runde 133 (28.09.2026): Ausruesten - Test-Item einlagern + anlegen -------------------------
// Siehe vollen Code-Review/Begruendung im Kopfkommentar BotMgr.h ("Runde 133"). Direktaufruf-Muster
// wie BotLootTarget()/HandleBotDeath() - dieselben bereits oeffentlichen Player-Methoden, die auch
// ".additem" (Schritt A) und HandleAutoEquipItemOpcode() (Schritt B) nutzen, synchron statt
// Opcode-/GM-Chat-getrieben. AUSDRUECKLICH NICHT ueber den Loot-Pfad (BotLootTarget()/
// HandleLootOpcode()) - Runde 131 hat Loot bewusst aus der Roadmap gestrichen, dieser Weg bleibt
// unberuehrt liegen.
bool BotMgr::EquipBotItem(uint32 accountId, uint32 itemEntry)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    WorldSession* session = itr->second.Session.get();
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemEntry);
    if (!itemTemplate)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - itemEntry %u nicht in item_template "
            "gefunden.", accountId, itemEntry);
        return false;
    }

    // --- Schritt A: Test-Item ins Inventar (GM-Item-Vergabe-Pfad, dieselben zwei Aufrufe wie
    // ".additem", NICHT ueber Loot) ------------------------------------------------------------
    ItemPosCountVec dest;
    uint32 noSpaceForCount = 0;
    InventoryResult storeMsg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1, &noSpaceForCount);
    if (storeMsg != EQUIP_ERR_OK || dest.empty())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - CanStoreNewItem(itemEntry=%u) "
            "fehlgeschlagen, InventoryResult=%u, noSpaceForCount=%u.", accountId, itemEntry,
            uint32(storeMsg), noSpaceForCount);
        return false;
    }

    Item* item = player->StoreNewItem(dest, itemEntry, true, GenerateItemRandomPropertyId(itemEntry));
    if (!item)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - StoreNewItem(itemEntry=%u) hat "
            "nullptr geliefert.", accountId, itemEntry);
        return false;
    }
    item->SetBinding(false); // analog HandleAddItemCommand(): GM-Testitem soll nicht an den Bot gebunden bleiben

    TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotItem: Account %u - Test-Item %u (InventoryType=%u) erfolgreich "
        "ins Inventar gelegt (Guid %s), rufe jetzt CanEquipItem()/EquipItem() auf.", accountId, itemEntry,
        uint32(itemTemplate->GetInventoryType()), item->GetGUID().ToString().c_str());

    // --- Schritt B: Anlegen ----------------------------------------------------------------------
    uint16 equipDest = 0;
    InventoryResult equipMsg = player->CanEquipItem(NULL_SLOT, equipDest, item, true);
    if (equipMsg != EQUIP_ERR_OK)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - CanEquipItem(itemEntry=%u) "
            "fehlgeschlagen, InventoryResult=%u. Item bleibt im Rucksack (kein Rollback noetig).",
            accountId, itemEntry, uint32(equipMsg));
        return false;
    }

    if (Item* alreadyEquipped = player->GetItemByPos(equipDest))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - Zielslot %u bereits belegt mit "
            "Item %u - Swap ist in dieser Runde bewusst NICHT implementiert (Testbots starten "
            "ungeruestet). Item %u bleibt im Rucksack.", accountId, uint32(equipDest & 0xFF),
            alreadyEquipped->GetEntry(), itemEntry);
        return false;
    }

    uint16 srcPos = item->GetPos();
    player->RemoveItem(srcPos >> 8, srcPos & 0xFF, true);
    Item* equipped = player->EquipItem(equipDest, item, true);
    if (!equipped)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotItem: Account %u - EquipItem(itemEntry=%u) hat "
            "nullptr geliefert (nach erfolgreichem CanEquipItem() unerwartet).", accountId, itemEntry);
        return false;
    }

    // --- Schritt C: Bestaetigung ------------------------------------------------------------------
    Item* confirm = player->GetItemByPos(equipDest);
    bool success = confirm && confirm->GetEntry() == itemEntry;

    TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotItem: Account %u - fertig. equipSlot=%u, "
        "GetItemByPos(equipSlot)->GetEntry()=%u (erwartet %u), success=%d.", accountId,
        uint32(equipDest & 0xFF), confirm ? confirm->GetEntry() : 0, itemEntry, success);

    return success;
}

// --- Runde 135 (28.09.2026): Equipment-Pool-Implementierung nach Design lcf2r134 -----------------
// Siehe vollen Kopfkommentar in BotMgr.h ("Runde 135") und den Rundenbericht
// C:\LegionServer\reports\lcf2r135_2026-09-28_playerbots_equipment_pool_impl.md
// -----------------------------------------------------------------------------------------------

namespace
{
    // Level-Baender exakt wie im R134-Design (Abschnitt 3.3) und im Python-Generierungsskript
    // fuer bot_equipment_pool - MUSS mit der Bandzuordnung dort uebereinstimmen.
    uint8 BotEquipLevelBand(uint8 level)
    {
        if (level >= 110) return 110;
        if (level >= 100) return 100;
        if (level >= 80) return 80;
        if (level >= 50) return 50;
        if (level >= 20) return 20;
        return 1;
    }

    // Pyramiden-Gewichtung je Level-Band, exakt aus R134 Abschnitt 3.3 uebernommen
    // (schlecht, mittel, gut, episch - Summe je Band = 100).
    struct GearTierWeights { uint8 Bad, Medium, Good, Epic; };
    GearTierWeights BotGearTierWeightsForBand(uint8 band)
    {
        switch (band)
        {
            case 1:   return { 55, 35, 9, 1 };
            case 20:  return { 50, 35, 12, 3 };
            case 50:  return { 50, 35, 12, 3 };
            case 80:  return { 45, 35, 15, 5 };
            case 100: return { 40, 35, 18, 7 };
            case 110: return { 35, 35, 20, 10 };
            default:  return { 50, 35, 12, 3 };
        }
    }

    // Wuerfelt EINMALIG (Aufrufer ist fuer "nur wenn noch keine Zeile existiert" verantwortlich)
    // eine Pool-Qualitaetsstufe 1-4 nach der Pyramiden-Gewichtung des uebergebenen Levels.
    uint8 RollBotGearTier(uint8 level)
    {
        GearTierWeights w = BotGearTierWeightsForBand(BotEquipLevelBand(level));
        uint32 roll = urand(0, 99);
        if (roll < w.Bad) return 1;
        if (roll < uint32(w.Bad) + w.Medium) return 2;
        if (roll < uint32(w.Bad) + w.Medium + w.Good) return 3;
        return 4;
    }

    std::string BuildInClauseU8(std::vector<uint8> const& values)
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i) oss << ',';
            oss << uint32(values[i]);
        }
        return oss.str();
    }

    // --- Ruestungstyp je Klasse+Level (R134 Abschnitt 5, Level-40-Schwelle Krieger/Paladin/
    // Jaeger/Schamane uebernommen) - Subklassenwerte: 1=Stoff, 2=Leder, 3=Kette, 4=Platte. -------
    uint8 GetArmorSubclassForClass(uint8 cls, uint8 level)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
                return level >= 40 ? 4 : 3; // Kette bis 39, Platte ab 40
            case CLASS_DEATH_KNIGHT:
                return 4; // startet ohnehin > Level 40 in dieser Kampagne
            case CLASS_HUNTER:
            case CLASS_SHAMAN:
                return level >= 40 ? 3 : 2; // Leder bis 39, Mail ab 40
            case CLASS_DRUID:
            case CLASS_ROGUE:
            case CLASS_MONK:
            case CLASS_DEMON_HUNTER:
                return 2; // immer Leder
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
            default:
                return 1; // immer Stoff
        }
    }

    bool BotClassCanUseShield(uint8 cls)
    {
        return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN;
    }

    bool BotClassCanDualWieldWeapon(uint8 cls)
    {
        return cls == CLASS_WARRIOR || cls == CLASS_ROGUE || cls == CLASS_DEATH_KNIGHT ||
            cls == CLASS_SHAMAN || cls == CLASS_MONK || cls == CLASS_DEMON_HUNTER || cls == CLASS_HUNTER;
    }

    bool BotClassCanUseHoldable(uint8 cls)
    {
        return cls == CLASS_PRIEST || cls == CLASS_MAGE || cls == CLASS_WARLOCK ||
            cls == CLASS_DRUID || cls == CLASS_PALADIN || cls == CLASS_SHAMAN;
    }

    // --- Waffentyp je Klasse (R134 Abschnitt 5) - BEWUSST auf Klassenebene vereinfacht, NICHT
    // pro Spec/Talent (mod-playerbots' volle InitWeaponProficiency()-Tabelle waere Spec-genau,
    // das haette aber eine Spec-Erkennung fuer Bots vorausgesetzt, die in dieser Kampagne noch
    // nicht existiert - dokumentierte, bewusste Vereinfachung fuer diese Runde). Subklassen:
    // 0 Axt1H,1 Axt2H,2 Bogen,3 Gewehr,4 Streitkolben1H,5 Streitkolben2H,6 Stangenwaffe,
    // 7 Schwert1H,8 Schwert2H,10 Stab,13 Faustwaffe,15 Dolch,18 Armbrust,19 Zauberstab. -----------
    std::vector<uint8> GetMainHandOneHandSubclasses(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return {0,4,7,13,15};
            case CLASS_PALADIN:      return {0,4,7};
            case CLASS_DEATH_KNIGHT: return {0,4,7,13,15};
            case CLASS_HUNTER:       return {0,7,13,15,2,3,18}; // inkl. Fernkampf (Legion: Hunter-Waffe in Haupthand)
            case CLASS_ROGUE:        return {0,4,7,13,15};
            case CLASS_PRIEST:       return {4,15,19};          // + Zauberstab
            case CLASS_SHAMAN:       return {0,4,13,15};
            case CLASS_MAGE:         return {7,15,19};          // + Zauberstab
            case CLASS_WARLOCK:      return {7,15,19};          // + Zauberstab
            case CLASS_MONK:         return {0,4,7,13};
            case CLASS_DRUID:        return {4,13,15};
            case CLASS_DEMON_HUNTER: return {0,7,13,15};
            default:                 return {};
        }
    }

    std::vector<uint8> GetMainHandTwoHandSubclasses(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return {1,5,6,8};
            case CLASS_PALADIN:      return {1,5,6,8};
            case CLASS_DEATH_KNIGHT: return {1,5,6,8};
            case CLASS_HUNTER:       return {6};
            case CLASS_ROGUE:        return {};
            case CLASS_PRIEST:       return {10};
            case CLASS_SHAMAN:       return {10};
            case CLASS_MAGE:         return {10};
            case CLASS_WARLOCK:      return {10};
            case CLASS_MONK:         return {6,10};
            case CLASS_DRUID:        return {10};
            case CLASS_DEMON_HUNTER: return {};
            default:                 return {};
        }
    }

    // Sucht ein einzelnes zufaelliges item_entry aus bot_equipment_pool, faellt bei leerem
    // Ergebnis Stufe fuer Stufe ab (analog mod-playerbots' Fallback-Schleife, R134 Abschnitt 2.2).
    // Gibt 0 zurueck, wenn selbst bei Stufe 1 ("schlecht") kein Treffer existiert.
    uint32 PickPoolItemWithFallback(uint8 band, uint8 startTier, uint8 itemClass,
        std::vector<uint8> const& subclasses, std::vector<uint8> const& invTypes)
    {
        if (subclasses.empty() || invTypes.empty())
            return 0;

        std::string subIn = BuildInClauseU8(subclasses);
        std::string invIn = BuildInClauseU8(invTypes);

        for (int tier = int(startTier); tier >= 1; --tier)
        {
            QueryResult result = WorldDatabase.PQuery(
                "SELECT item_entry FROM bot_equipment_pool WHERE level_band = %u AND pool_quality = %u "
                "AND item_class = %u AND item_subclass IN (%s) AND inventory_type IN (%s) "
                "ORDER BY RAND() LIMIT 1",
                uint32(band), uint32(tier), uint32(itemClass), subIn.c_str(), invIn.c_str());
            if (result)
                return (*result)[0].GetUInt32();
        }
        return 0;
    }
}

// Liest/erstellt die dauerhaft fixe Pool-Qualitaetsstufe fuer den aktuell eingeloggten Bot
// (characters.bot_gear_tier, ein Zeile pro Bot-GUID). Wird bei der ERSTEN Zuweisung mit dem
// Level des Bots ZU DIESEM ZEITPUNKT gewuerfelt (siehe RollBotGearTier()) und danach nie wieder
// veraendert, auch wenn der Bot spaeter Level aufsteigt.
uint8 BotMgr::GetOrAssignBotGearTier(Player* player)
{
    uint32 guidLow = player->GetGUID().GetCounter();

    if (QueryResult result = CharacterDatabase.PQuery(
        "SELECT gear_tier FROM bot_gear_tier WHERE guid = %u", guidLow))
    {
        return (*result)[0].GetUInt8();
    }

    uint8 tier = RollBotGearTier(player->getLevel());
    CharacterDatabase.PExecute(
        "INSERT INTO bot_gear_tier (guid, gear_tier, assigned_level) VALUES (%u, %u, %u)",
        guidLow, uint32(tier), uint32(player->getLevel()));

    TC_LOG_INFO("scripts.bots", "BotMgr::GetOrAssignBotGearTier: Bot-GUID %u - neue Qualitaetsstufe "
        "%u gewuerfelt bei Level %u (dauerhaft fix gespeichert).", guidLow, uint32(tier),
        uint32(player->getLevel()));
    return tier;
}

// --- Runde 135: Hauptfunktion - rÃ¼stet einen Bot vollstaendig aus dem Equipment-Pool aus ---------
bool BotMgr::EquipBotFromPool(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotFromPool: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    WorldSession* session = itr->second.Session.get();
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotFromPool: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    uint8 cls = player->getClass();
    uint8 level = player->getLevel();
    uint8 band = BotEquipLevelBand(level);
    uint8 tier = GetOrAssignBotGearTier(player);

    TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Klasse %u, Level %u (Band %u), "
        "Pool-Qualitaetsstufe %u. Beginne Slot-fuer-Slot-Ausruestung.", accountId, uint32(cls),
        uint32(level), uint32(band), uint32(tier));

    uint8 armorSub = GetArmorSubclassForClass(cls, level);
    std::vector<uint8> armorSubWithMisc = { armorSub, 0 }; // 0=Misc (z.B. klassenunabhaengige Robe)

    uint32 slotsAttempted = 0, slotsFilled = 0;

    auto tryEquipSlot = [&](char const* label, uint8 itemClass, std::vector<uint8> const& subclasses,
        std::vector<uint8> const& invTypes)
    {
        ++slotsAttempted;
        uint32 entry = PickPoolItemWithFallback(band, tier, itemClass, subclasses, invTypes);
        if (!entry)
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': kein passendes "
                "Pool-Item gefunden (auch nicht per Stufen-Fallback), Slot bleibt leer.", accountId, label);
            return;
        }
        if (EquipBotItem(accountId, entry))
        {
            ++slotsFilled;
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': itemEntry %u erfolgreich "
                "ausgeruestet.", accountId, label, entry);
        }
        else
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': itemEntry %u aus dem "
                "Pool konnte NICHT ausgeruestet werden (siehe EquipBotItem()-Fehler oben, z.B. Slot bereits "
                "belegt) - kein Datenverlust, naechster Slot wird trotzdem versucht.", accountId, label, entry);
        }
    };

    // --- Ruestungs-Koerperslots (Klassen-/Level-gebundener Ruestungstyp) --------------------
    tryEquipSlot("Head",      4, armorSubWithMisc, {1});
    tryEquipSlot("Shoulders", 4, armorSubWithMisc, {3});
    tryEquipSlot("Chest",     4, armorSubWithMisc, {5, 20});
    tryEquipSlot("Waist",     4, armorSubWithMisc, {6});
    tryEquipSlot("Legs",      4, armorSubWithMisc, {7});
    tryEquipSlot("Feet",      4, armorSubWithMisc, {8});
    tryEquipSlot("Wrists",    4, armorSubWithMisc, {9});
    tryEquipSlot("Hands",     4, armorSubWithMisc, {10});

    // --- Slots ohne Ruestungstyp-Bindung (Schmuck/Umhang) - Subklasse wird nicht gefiltert -----
    std::vector<uint8> anyArmorSub = {0,1,2,3,4,5};
    tryEquipSlot("Neck",     4, anyArmorSub, {2});
    tryEquipSlot("Cloak",    4, anyArmorSub, {16});
    tryEquipSlot("Finger1",  4, anyArmorSub, {11});
    tryEquipSlot("Finger2",  4, anyArmorSub, {11});
    tryEquipSlot("Trinket1", 4, anyArmorSub, {12});
    tryEquipSlot("Trinket2", 4, anyArmorSub, {12});

    // --- Waffen: zuerst Haupthand (1H bevorzugt, sonst 2H), danach ggf. Nebenhand ------------
    std::vector<uint8> oneHandSub = GetMainHandOneHandSubclasses(cls);
    std::vector<uint8> twoHandSub = GetMainHandTwoHandSubclasses(cls);

    bool wantsTwoHand = !twoHandSub.empty() && (oneHandSub.empty() || urand(0, 1) == 0);
    uint32 mainHandEntry = 0;
    if (wantsTwoHand)
        mainHandEntry = PickPoolItemWithFallback(band, tier, 2, twoHandSub, {17});
    if (!mainHandEntry && !oneHandSub.empty())
        mainHandEntry = PickPoolItemWithFallback(band, tier, 2, oneHandSub, {13, 15, 21, 26});
    if (!mainHandEntry && !twoHandSub.empty())
        mainHandEntry = PickPoolItemWithFallback(band, tier, 2, twoHandSub, {17});

    ++slotsAttempted;
    bool equippedTwoHand = false;
    if (mainHandEntry)
    {
        if (EquipBotItem(accountId, mainHandEntry))
        {
            ++slotsFilled;
            // grobe Erkennung, ob es eine 2H-Waffe war (Subklasse in twoHandSub) - fuer die
            // Offhand-Entscheidung unten (2H-Waffe blockiert Schild/Offhand ohnehin ueber
            // CanEquipItem(), das ist hier nur eine Log-/Diagnose-Vereinfachung).
            equippedTwoHand = wantsTwoHand;
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot 'MainHand': itemEntry %u "
                "erfolgreich ausgeruestet.", accountId, mainHandEntry);
        }
        else
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot 'MainHand': itemEntry %u "
                "aus dem Pool konnte NICHT ausgeruestet werden.", accountId, mainHandEntry);
        }
    }
    else
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot 'MainHand': kein passendes "
            "Pool-Item gefunden.", accountId);
    }

    if (!equippedTwoHand)
    {
        // Nebenhand: Schild > Waffe (Dual-Wield) > Holdable, je nach Klassenfaehigkeit.
        uint32 offhandEntry = 0;
        char const* offhandLabel = "Offhand";
        if (BotClassCanUseShield(cls) && urand(0, 1) == 0)
        {
            offhandEntry = PickPoolItemWithFallback(band, tier, 4, {6}, {14});
            offhandLabel = "Offhand(Schild)";
        }
        if (!offhandEntry && BotClassCanDualWieldWeapon(cls) && !oneHandSub.empty())
        {
            offhandEntry = PickPoolItemWithFallback(band, tier, 2, oneHandSub, {22});
            offhandLabel = "Offhand(Waffe)";
        }
        if (!offhandEntry && BotClassCanUseShield(cls))
        {
            offhandEntry = PickPoolItemWithFallback(band, tier, 4, {6}, {14});
            offhandLabel = "Offhand(Schild)";
        }
        if (!offhandEntry && BotClassCanUseHoldable(cls))
        {
            offhandEntry = PickPoolItemWithFallback(band, tier, 4, {0}, {23});
            offhandLabel = "Offhand(Holdable)";
        }

        ++slotsAttempted;
        if (offhandEntry)
        {
            if (EquipBotItem(accountId, offhandEntry))
            {
                ++slotsFilled;
                TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': itemEntry %u "
                    "erfolgreich ausgeruestet.", accountId, offhandLabel, offhandEntry);
            }
            else
            {
                TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': itemEntry %u aus "
                    "dem Pool konnte NICHT ausgeruestet werden (z.B. weil die Haupthand-Waffe bereits "
                    "zweihaendig ist).", accountId, offhandLabel, offhandEntry);
            }
        }
        else
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - Slot '%s': kein passendes "
                "Pool-Item gefunden.", accountId, offhandLabel);
        }
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotFromPool: Account %u - fertig. %u/%u Slots erfolgreich "
        "befuellt (Pool-Qualitaetsstufe %u, Level-Band %u).", accountId, slotsFilled, slotsAttempted,
        uint32(tier), uint32(band));

    return slotsFilled > 0;
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
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStep: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStep: Account %u hat aktuell keinen Player in der Welt "
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

    TC_LOG_INFO("scripts.bots", "BotMgr::MoveBotTestStep: Account %u - loese MovePoint() aus (generatePath=false), "
        "Start (%f, %f, %f) -> Ziel (%f, %f, %f).", accountId, startX, startY, startZ, destX, destY, destZ);

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
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStepPath: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::MoveBotTestStepPath: Account %u hat aktuell keinen Player in der Welt "
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

    TC_LOG_INFO("scripts.bots", "BotMgr::MoveBotTestStepPath: Account %u - loese MovePoint() aus (generatePath=true, "
        "Navmesh/PathGenerator), Start (%f, %f, %f) -> Ziel (%f, %f, %f).", accountId, startX, startY, startZ, destX, destY, destZ);

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
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    BotSessionEntry& entry = itr->second;
    Player* player = entry.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    if (entry.PatrolActive)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotPatrol: Account %u hat bereits ein aktives Patrol "
            "(%u von %u Zyklen verbleibend) - erst abwarten oder '.bottest move'/Logout nutzen.",
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

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotPatrol: Account %u - starte Patrol ueber %u Zyklen, "
        "Punkt A (%f, %f, %f) <-> Punkt B (%f, %f, %f), generatePath=false.", accountId, cycles,
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
        TC_LOG_INFO("scripts.bots", "BotMgr::StopBotPatrol: Account %u - Patrol manuell/sicherheitshalber "
            "abgebrochen (%u von %u Zyklen verbleibend), kein weiterer MovePoint()-Aufruf.",
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

// --- Runde 137 (28.09.2026): Gruppen-Beitritt + Folgen-KI nach Design lcf2r136 --------------------
// Siehe vollen Kopfkommentar in BotMgr.h ("Runde 137") und den Rundenbericht
// C:\LegionServer\reports\lcf2r137_2026-09-28_playerbots_gruppe_follow_impl.md
// ----------------------------------------------------------------------------------------------

bool BotMgr::InviteBotToGroup(uint32 botAccountId, Player* leader)
{
    if (!leader)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: kein Leader-Player uebergeben.");
        return false;
    }

    auto itr = _botSessions.find(botAccountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: keine Bot-Session fuer Account %u vorhanden.", botAccountId);
        return false;
    }

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", botAccountId);
        return false;
    }

    if (bot->GetGroup())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: Account %u (Player '%s') ist bereits Mitglied einer "
            "Gruppe (Guid %s) - kein automatischer Fremdgruppen-Kick, Abbruch ohne Mutation.",
            botAccountId, bot->GetName().c_str(), bot->GetGroup()->GetGUID().ToString().c_str());
        return false;
    }

    // Dieselbe BG/BF-Raid-Sonderfall-Behandlung wie HandlePartyInviteResponseOpcode() (siehe lcf2r136
    // Abschnitt 1) - fuer einen echten Leader-Charakter in einer BG/BF-Instanzgruppe wird stattdessen
    // dessen "originale" Gruppe verwendet.
    Group* group = leader->GetGroup();
    if (group && (group->isBGGroup() || group->isBFGroup()))
        group = leader->GetOriginalGroup();

    bool newlyCreated = false;
    if (!group)
    {
        group = new Group();
        if (!group->Create(leader))
        {
            TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: Group::Create(leader='%s') fehlgeschlagen - "
                "kein Speicherleck (delete group direkt), Abbruch.", leader->GetName().c_str());
            delete group;
            return false;
        }
        sGroupMgr->AddGroup(group);
        newlyCreated = true;
    }

    if (group->IsFull())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: Gruppe von Leader '%s' (Guid %s) ist bereits voll "
            "(MAX_GROUP_SIZE=%d normale Gruppe / MAX_RAID_SIZE=%d Raid) - Account %u NICHT hinzugefuegt.",
            leader->GetName().c_str(), group->GetGUID().ToString().c_str(), MAX_GROUP_SIZE, MAX_RAID_SIZE, botAccountId);
        return false;
    }

    if (!group->AddMember(bot))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::InviteBotToGroup: Group::AddMember(Account %u, '%s') fehlgeschlagen "
            "(siehe Group::AddMember()-interne Guards) - keine weitere Mutation.", botAccountId, bot->GetName().c_str());
        return false;
    }

    group->BroadcastGroupUpdate();

    TC_LOG_INFO("scripts.bots", "BotMgr::InviteBotToGroup: Account %u ('%s') erfolgreich in Gruppe von Leader '%s' "
        "(Guid %s, %s) eingefuegt - characters.group_member aktualisiert, BroadcastGroupUpdate() ausgefuehrt.",
        botAccountId, bot->GetName().c_str(), leader->GetName().c_str(), group->GetGUID().ToString().c_str(),
        newlyCreated ? "neu angelegt" : "bestehende Gruppe");
    return true;
}

bool BotMgr::RemoveBotFromGroup(uint32 botAccountId)
{
    auto itr = _botSessions.find(botAccountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RemoveBotFromGroup: keine Bot-Session fuer Account %u vorhanden.", botAccountId);
        return false;
    }

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RemoveBotFromGroup: Account %u hat aktuell keinen Player in der Welt.",
            botAccountId);
        return false;
    }

    if (!bot->GetGroup())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RemoveBotFromGroup: Account %u ('%s') ist aktuell in keiner Gruppe.",
            botAccountId, bot->GetName().c_str());
        return false;
    }

    bot->RemoveFromGroup();
    TC_LOG_INFO("scripts.bots", "BotMgr::RemoveBotFromGroup: Account %u ('%s') aus seiner Gruppe entfernt.",
        botAccountId, bot->GetName().c_str());
    return true;
}

bool BotMgr::StartBotFollow(uint32 botAccountId, ObjectGuid targetGuid)
{
    auto itr = _botSessions.find(botAccountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotFollow: keine Bot-Session fuer Account %u vorhanden.", botAccountId);
        return false;
    }

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotFollow: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", botAccountId);
        return false;
    }

    Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
    if (!target)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotFollow: Account %u - Ziel-Guid %s nicht gefunden/nicht im "
            "selben Grid geladen wie der Bot.", botAccountId, targetGuid.ToString().c_str());
        return false;
    }

    if (target == bot)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::StartBotFollow: Account %u - Ziel ist der Bot selbst, Abbruch.", botAccountId);
        return false;
    }

    // Feste, bewusst kleine Nachlauf-Distanz/Winkel fuer diesen ersten isolierten Test - analog zur
    // ueblichen Begleiter-/Pet-Formation im Core (kein Parameter in dieser Runde, siehe Kopfkommentar
    // in BotMgr.h). MOTION_SLOT_ACTIVE default deckt sich mit dem bereits von MovePoint()/StartBotPatrol()
    // genutzten Slot - kein Konflikt mit MOTION_SLOT_IDLE.
    float const followDist = 3.0f;
    float const followAngle = float(M_PI / 2.0f);
    bot->GetMotionMaster()->MoveFollow(target, followDist, followAngle);

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotFollow: Account %u ('%s') folgt jetzt Ziel '%s' (Guid %s), "
        "dist=%.1f, angle=%.2f.", botAccountId, bot->GetName().c_str(), target->GetName().c_str(),
        targetGuid.ToString().c_str(), followDist, followAngle);
    return true;
}

void BotMgr::StopBotFollow(uint32 botAccountId)
{
    auto itr = _botSessions.find(botAccountId);
    if (itr == _botSessions.end() || !itr->second.Session)
        return;

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
        return;

    // Dieselbe Notbremsen-Konvention wie StopBotPatrol()/StopBotAttack(): MoveIdle() setzt den aktiven
    // Motion-Slot auf einen einfachen IdleMovementGenerator zurueck, Bot bleibt sofort an der aktuellen
    // Position stehen - kein Clear() aller Slots noetig (wuerde auch MOTION_SLOT_IDLE-Default anfassen).
    bot->GetMotionMaster()->MoveIdle();
    TC_LOG_INFO("scripts.bots", "BotMgr::StopBotFollow: Account %u ('%s') - Follow abgebrochen (MoveIdle()).",
        botAccountId, bot->GetName().c_str());
}

// --- Runde 143 (28.09.2026): Artefaktwaffen nach Design lcf2r138 Teil B -------------------------
// Voller Design-Bericht: C:\LegionServer\reports\lcf2r138_2026-09-28_playerbots_gruppe_stufe2_lfg_artefakt_design.md
// Kopfkommentar-Referenz in BotMgr.h ("Runde 143") fuer den vollen Code-Review-Vorlauf.
// -------------------------------------------------------------------------------------------------
namespace
{
    // --- Datenabfrage-Ergebnis (Schritt 1 des Auftrags): 36 ChrSpecializationID -> Artefakt-Item-Entry.
    //
    // Ermittelt per lokalem Client-DB2-CSV-Export (Build 26972, dieselbe Quelle wie R134/R135):
    //   1. C:\LegionServer\downloads\artifact\Artifact_7.3.5.26972.csv (Runtime-DB2 "Artifact",
    //      ArtifactID -> ChrSpecializationID -> Name) gefiltert auf ArtifactCategoryID=1 (Waffen,
    //      Kategorie 2 ist der Fischerstab "Underlight Angler", kein Klassen-Artefakt) UND
    //      ChrSpecializationID!=0 (ArtifactID 1 "Dandor's Fury" ist ein reines NPC-Artefakt) ->
    //      lieferte GENAU 36 Zeilen fuer GENAU 36 verschiedene Spezialisierungen (bestaetigt exakt
    //      die bekannten Legion-7.3.5-Spezialisierungen: 10 Klassen mit 3 Specs, Druide mit 4,
    //      Dämonenjäger mit 2 = 36) - drei Faelle mit mehreren ArtifactID-Zeilen pro Spec waren
    //      Test-/Zweitartefakte (Spec 70 zusaetzlich "Oakbringer"+"Test Artifact 2", Spec 65/66
    //      je ein "Test Artifact") und wurden auf die jeweils kanonische ArtifactID reduziert.
    //   2. C:\LegionServer\downloads\db2_full_26972\itemsparse.csv (Spalte ArtifactID) gefiltert auf
    //      genau diese 36 kanonischen ArtifactID-Werte, um die tatsaechliche item_entry (Spalte ID)
    //      zu finden. Mehrfachtreffer (Recolor-/Alt-Appearance-Items wie "Odyn's Fury" fuer Fury-
    //      Warrior spec72, "Hilt of Frostmourne" fuer Retribution-Paladin spec70, "Verus" fuer
    //      Havoc-DH spec577) wurden anhand des exakt mit dem Artefaktnamen uebereinstimmenden
    //      Display_lang-Feldes auf die Basisversion reduziert.
    //   3. Kein Runtime-DB2-Storage liefert die (Klasse/Spec)->Item-Zuordnung direkter als dieser
    //      Zwei-Schritt-Join (Artifact-DB2 hat kein Item-Feld, ItemSparse hat kein Spec-Feld) - die
    //      hier verwendete Tabelle IST bereits die zuverlaessigste verfuegbare Quelle.
    //
    // WICHTIG (per Code-Review CanEquipItem()/EquipItem() bestaetigt, kein Sonderfall noetig): auch
    // die als InventoryType=17 (INVTYPE_2HWEAPON) kodierten, optisch dual-wield gerenderten Artefakte
    // (z.B. Fury-Warrior "Warswords of the Valarjar") brauchen NUR diesen einen Item-Entry - die
    // Slotwahl laeuft automatisch ueber die bereits bestehende EquipBotItem()-Logik wie bei jeder
    // anderen Waffe, die zweite sichtbare Waffenhaelfte ist ein reiner Spielvisualisierungseffekt
    // (SpellVisualKitID in der Artifact-DB2), kein zweites Item.
    struct BotArtifactEntry { uint32 SpecId; uint32 ItemEntry; char const* Label; };
    BotArtifactEntry const BotArtifactTable[] =
    {
        // Krieger (Klasse 1)
        { 71,  128910, "Warrior-Arms: Strom'kar, the Warbreaker" },
        { 72,  128908, "Warrior-Fury: Warswords of the Valarjar" },
        { 73,  128289, "Warrior-Protection: Scale of the Earth-Warder" },
        // Paladin (Klasse 2)
        { 65,  128823, "Paladin-Holy: The Silver Hand" },
        { 66,  128866, "Paladin-Protection: Truthguard" },
        { 70,  120978, "Paladin-Retribution: Ashbringer" },
        // Jaeger (Klasse 3)
        { 253, 128861, "Hunter-BeastMastery: Titanstrike" },
        { 254, 128826, "Hunter-Marksmanship: Thas'dorah, Legacy of the Windrunners" },
        { 255, 128808, "Hunter-Survival: Talonclaw, Spear of the Wild Gods" },
        // Schurke (Klasse 4)
        { 259, 128870, "Rogue-Assassination: The Kingslayers" },
        { 260, 128872, "Rogue-Outlaw: The Dreadblades" },
        { 261, 128476, "Rogue-Subtlety: Fangs of the Devourer" },
        // Priester (Klasse 5)
        { 256, 128868, "Priest-Discipline: Light's Wrath" },
        { 257, 128825, "Priest-Holy: T'uure, Beacon of the Naaru" },
        { 258, 128827, "Priest-Shadow: Xal'atath, Blade of the Black Empire" },
        // Todesritter (Klasse 6)
        { 250, 128402, "DeathKnight-Blood: Maw of the Damned" },
        { 251, 128292, "DeathKnight-Frost: Blades of the Fallen Prince" },
        { 252, 128403, "DeathKnight-Unholy: Apocalypse" },
        // Schamane (Klasse 7)
        { 262, 128935, "Shaman-Elemental: The Fist of Ra-den" },
        { 263, 128819, "Shaman-Enhancement: Doomhammer" },
        { 264, 128911, "Shaman-Restoration: Sharas'dal, Scepter of Tides" },
        // Magier (Klasse 8)
        { 62,  127857, "Mage-Arcane: Aluneth, Greatstaff of the Magna" },
        { 63,  128820, "Mage-Fire: Felo'melorn" },
        { 64,  128862, "Mage-Frost: Ebonchill, Greatstaff of Alodi" },
        // Hexenmeister (Klasse 9)
        { 265, 128942, "Warlock-Affliction: Ulthalesh, the Deadwind Harvester" },
        { 266, 128943, "Warlock-Demonology: Skull of the Man'ari" },
        { 267, 128941, "Warlock-Destruction: Scepter of Sargeras" },
        // Moench (Klasse 10)
        { 268, 128938, "Monk-Brewmaster: Fu Zan, the Wanderer's Companion" },
        { 269, 128940, "Monk-Windwalker: Fists of the Heavens" },
        { 270, 128937, "Monk-Mistweaver: Sheilun, Staff of the Mists" },
        // Druide (Klasse 11, 4 Specs)
        { 102, 128858, "Druid-Balance: Scythe of Elune" },
        { 103, 128860, "Druid-Feral: Fangs of Ashamane" },
        { 104, 128821, "Druid-Guardian: Claws of Ursoc" },
        { 105, 128306, "Druid-Restoration: G'Hanir, the Mother Tree" },
        // Dämonenjaeger (Klasse 12, 2 Specs)
        { 577, 127829, "DemonHunter-Havoc: Twinblades of the Deceiver" },
        { 581, 128832, "DemonHunter-Vengeance: The Aldrachi Warblades" },
    };

    uint32 GetArtifactItemForSpec(uint32 specId)
    {
        for (BotArtifactEntry const& entry : BotArtifactTable)
            if (entry.SpecId == specId)
                return entry.ItemEntry;
        return 0;
    }

    // Fallback, falls der Bot noch keine bewusste Primaerspezialisierung hat
    // (GetPrimarySpecialization()==0, z.B. ganz frisch erstellter Charakter) - erste Spec der
    // Klasse in derselben Reihenfolge wie oben in der Tabelle (dokumentierte Vereinfachung, analog
    // GetArmorSubclassForClass() aus Runde 135 - keine echte Rollen-/Talent-Erkennung noetig).
    uint32 GetDefaultSpecForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return 71;
            case CLASS_PALADIN:      return 65;
            case CLASS_HUNTER:       return 253;
            case CLASS_ROGUE:        return 259;
            case CLASS_PRIEST:       return 256;
            case CLASS_DEATH_KNIGHT: return 250;
            case CLASS_SHAMAN:       return 262;
            case CLASS_MAGE:         return 62;
            case CLASS_WARLOCK:      return 265;
            case CLASS_MONK:         return 268;
            case CLASS_DRUID:        return 102;
            case CLASS_DEMON_HUNTER: return 577;
            default:                 return 0;
        }
    }
}

bool BotMgr::EquipBotArtifact(uint32 accountId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotArtifact: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotArtifact: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    uint32 specId = player->GetPrimarySpecialization();
    bool usedFallback = false;
    if (specId == 0)
    {
        specId = GetDefaultSpecForClass(player->getClass());
        usedFallback = true;
    }

    uint32 itemEntry = GetArtifactItemForSpec(specId);
    if (itemEntry == 0)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotArtifact: Account %u - keine Artefakt-Zuordnung fuer "
            "specId %u (Klasse %u) gefunden.", accountId, specId, uint32(player->getClass()));
        return false;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotArtifact: Account %u (Klasse %u, specId %u%s) -> itemEntry %u, "
        "rufe EquipBotItem() auf.", accountId, uint32(player->getClass()), specId,
        usedFallback ? " [Fallback, keine Primaerspec gesetzt]" : "", itemEntry);

    // --- Runde 143 Nachtrag (Livetest-Befund): EquipBotItem() (Runde 133) unterstuetzt bewusst KEINEN
    // Slot-Swap - fuer normale Testitems unproblematisch, aber Artefaktwaffen gehen IMMER in die
    // Main-Hand, und praktisch JEDE Klasse startet bereits mit einer Waffe genau dort (anders als die
    // in Runde 133/135 gewaehlten leeren Finger-/Ring-Slots). Ohne diesen Schritt wuerde
    // EquipBotArtifact() deshalb bei so gut wie jedem frischen Bot fehlschlagen (Artefakt-Item wird
    // zwar korrekt erzeugt/initialisiert, bleibt aber im Rucksack liegen). Loesung: die aktuell
    // belegte Main-Hand-Waffe (falls vorhanden) VOR dem EquipBotItem()-Aufruf sicher in den Rucksack
    // verschieben (Player::CanStoreItem()+RemoveItem()+StoreItem(), dieselben bereits oeffentlichen
    // Player-Methoden wie in EquipBotItem() selbst) - kein Verlust, die alte Waffe bleibt im
    // Bot-Inventar erhalten, nur der Slot wird frei. Bricht sauber ab (kein Datenverlust), falls der
    // Rucksack bereits voll ist.
    if (Item* oldMainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
    {
        ItemPosCountVec freeSlot;
        InventoryResult storeMsg = player->CanStoreItem(NULL_BAG, NULL_SLOT, freeSlot, oldMainHand, false);
        if (storeMsg != EQUIP_ERR_OK || freeSlot.empty())
        {
            TC_LOG_ERROR("scripts.bots", "BotMgr::EquipBotArtifact: Account %u - Main-Hand-Slot ist mit Item %u "
                "belegt und der Rucksack hat keinen Platz dafuer (CanStoreItem InventoryResult=%u) - breche ab, "
                "die alte Waffe bleibt unangetastet ausgeruestet.", accountId, oldMainHand->GetEntry(),
                uint32(storeMsg));
            return false;
        }
        player->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
        player->StoreItem(freeSlot, oldMainHand, true);
        TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotArtifact: Account %u - alte Main-Hand-Waffe %u in den "
            "Rucksack verschoben, Slot jetzt frei fuer die Artefaktwaffe.", accountId, oldMainHand->GetEntry());
    }

    bool success = EquipBotItem(accountId, itemEntry);
    if (success)
    {
        // Zusatzbestaetigung (Auftragsvorgabe "bestaetige dass die Waffe wirklich als Artefakt
        // erkannt wird"): Main-Hand-Item nach dem Anlegen erneut lesen und GetArtifactID() pruefen.
        if (Item* equipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        {
            uint8 artifactId = equipped->GetTemplate()->GetArtifactID();
            TC_LOG_INFO("scripts.bots", "BotMgr::EquipBotArtifact: Account %u - Main-Hand-Item %u, "
                "GetArtifactID()=%u (0 = KEIN Artefakt, Fehler), GetTotalPurchasedArtifactPowers()=%u.",
                accountId, equipped->GetEntry(), uint32(artifactId), equipped->GetTotalPurchasedArtifactPowers());
        }
    }
    return success;
}

bool BotMgr::SkillBotArtifact(uint32 accountId, uint32 levelBudget)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SkillBotArtifact: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SkillBotArtifact: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    Item* artifact = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!artifact || artifact->GetTemplate()->GetArtifactID() == 0)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SkillBotArtifact: Account %u - kein Artefakt in der Main-Hand "
            "ausgeruestet (erst '.bottest equipartifact %u' aufrufen).", accountId, accountId);
        return false;
    }

    uint8 artifactId = artifact->GetTemplate()->GetArtifactID();

    // --- Rang-Budget (R138 B.3, diese Runde kalibriert): 1 zusaetzlicher Rang pro 2 Charakterlevel
    // oberhalb der Artefakt-Content-Schwelle Level 98 (Legion-Hauptcontent-Start) - grobe, dokumentierte
    // Faustregel statt echter GtArtifactLevelXPEntry-Kalibrierung (laut R138 explizit fuer diese erste
    // Runde ausreichend, "keine perfekte Balance noetig, aber plausibel"). Level 100 -> 1 Rang,
    // Level 110 -> 6 Raenge zusaetzlich zur automatischen Tier-0-Grundausstattung aus Item::Create().
    uint32 budget = levelBudget;
    bool autoComputed = false;
    if (budget == 0)
    {
        uint8 level = player->getLevel();
        int32 levelsAboveThreshold = int32(level) - 98;
        if (levelsAboveThreshold < 0)
            levelsAboveThreshold = 0;
        budget = uint32(levelsAboveThreshold) / 2;
        autoComputed = true;
    }

    // --- Tier-Freischaltung (R138 B.3: Tier 0 ab Erhalt, Tier 1 ab grob Level 102-110). Dieser Core
    // kennt laut MAX_ARTIFACT_TIER (DBCEnums.h) ohnehin nur Tier 0/1 - kein Scope-Verlust.
    uint32 currentTier = artifact->GetModifier(ITEM_MODIFIER_ARTIFACT_TIER);
    uint32 desiredTier = (player->getLevel() >= 102) ? uint32(MAX_ARTIFACT_TIER) : 0;
    if (desiredTier > currentTier)
    {
        for (uint32 t = currentTier + 1; t <= desiredTier; ++t)
            artifact->InitArtifactPowers(artifactId, uint8(t));
        artifact->SetModifier(ITEM_MODIFIER_ARTIFACT_TIER, desiredTier);
        currentTier = desiredTier;
    }

    if (budget == 0)
    {
        artifact->SetState(ITEM_CHANGED, player);
        TC_LOG_INFO("scripts.bots", "BotMgr::SkillBotArtifact: Account %u - Budget 0 (Level %u, Schwelle 98), "
            "keine zusaetzlichen Raenge vergeben. Tier-0-Basis aus Item::Create() bleibt bestehen.",
            accountId, uint32(player->getLevel()));
        return true;
    }

    // --- Traits sammeln, Tier aufsteigend sortieren (dann ID als stabiler Ersatzschluessel) - R138
    // B.2 "einfachste robuste Variante", KEINE 36 einzeln recherchierten Community-Prioritaetslisten.
    std::vector<ArtifactPowerEntry const*> powers = sDB2Manager.GetArtifactPowers(artifactId);
    std::sort(powers.begin(), powers.end(), [](ArtifactPowerEntry const* a, ArtifactPowerEntry const* b)
    {
        if (a->Tier != b->Tier)
            return a->Tier < b->Tier;
        return a->ID < b->ID;
    });

    uint32 grantedRanks = 0;
    bool progressMade = true;
    while (budget > 0 && progressMade)
    {
        progressMade = false;
        for (ArtifactPowerEntry const* power : powers)
        {
            if (budget == 0)
                break;
            if (power->Tier > currentTier)
                continue; // Tier noch nicht freigeschaltet

            ItemDynamicFieldArtifactPowers const* learned = artifact->GetArtifactPower(power->ID);
            if (!learned)
                continue; // sollte durch InitArtifactPowers() oben bereits initialisiert sein

            if (learned->PurchasedRank >= power->MaxPurchasableRank)
                continue; // bereits maximal

            // Link-Abhaengigkeit pruefen - EXAKT dasselbe Muster wie HandleArtifactAddPower()
            // (ArtifactHandler.cpp:77-101), damit nie ein regelwidriger Zustand entsteht.
            if (!(power->Flags & ARTIFACT_POWER_FLAG_NO_LINK_REQUIRED))
            {
                if (std::unordered_set<uint32> const* links = sDB2Manager.GetArtifactPowerLinks(power->ID))
                {
                    bool hasAnyLink = false;
                    for (uint32 linkId : *links)
                    {
                        ArtifactPowerEntry const* linkEntry = sArtifactPowerStore.LookupEntry(linkId);
                        if (!linkEntry)
                            continue;
                        ItemDynamicFieldArtifactPowers const* linkLearned = artifact->GetArtifactPower(linkId);
                        if (!linkLearned)
                            continue;
                        if (linkLearned->PurchasedRank >= linkEntry->MaxPurchasableRank)
                        {
                            hasAnyLink = true;
                            break;
                        }
                    }
                    if (!hasAnyLink)
                        continue; // Voraussetzung noch nicht erfuellt, diesen Trait ueberspringen
                }
            }

            ArtifactPowerRankEntry const* rankEntry = sDB2Manager.GetArtifactPowerRank(power->ID, learned->CurrentRankWithBonus + 1 - 1);
            if (!rankEntry)
                continue;

            ItemDynamicFieldArtifactPowers newPower = *learned;
            ++newPower.PurchasedRank;
            ++newPower.CurrentRankWithBonus;
            artifact->SetArtifactPower(&newPower);
            player->ApplyArtifactPowerRank(artifact, rankEntry, true);

            --budget;
            ++grantedRanks;
            progressMade = true;
        }
    }

    artifact->SetState(ITEM_CHANGED, player);

    TC_LOG_INFO("scripts.bots", "BotMgr::SkillBotArtifact: Account %u - fertig. Level %u, Budget %s%u, "
        "tatsaechlich vergebene Raenge=%u, verbleibendes ungenutztes Budget=%u (0 = voller Baum bis Tier %u "
        "ausgeschoepft), GetTotalPurchasedArtifactPowers()=%u.", accountId, uint32(player->getLevel()),
        autoComputed ? "[auto] " : "[override] ", levelBudget == 0 ? (grantedRanks + budget) : levelBudget,
        grantedRanks, budget, currentTier, artifact->GetTotalPurchasedArtifactPowers());

    return true;
}

bool BotMgr::CreateBot(uint32 /*ownerAccountId*/, std::string const& botCharacterName)
{
    // Alter, generischer Einstiegspunkt aus der Skeleton-Runde (62/63) - bleibt
    // bewusst inert. Die tatsaechliche Runde-B-Implementierung laeuft ueber die
    // drei expliziten Phasen-Methoden oben (CreateBotAccount/
    // RequestCreateBotCharacter/RequestBotLogin), die von bot_commandscript.cpp
    // (".bottest ...") einzeln angesteuert werden, damit zwischen jeder Phase
    // Server.log/DBErrors.log geprueft werden kann (siehe Bericht Abschnitt 5).
    TC_LOG_INFO("scripts.bots", "BotMgr::CreateBot('%s'): bitte stattdessen die '.bottest'-Befehle verwenden "
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
