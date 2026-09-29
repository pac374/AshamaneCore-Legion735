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
#include "LFGMgr.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "Util.h"
#include "QuestDef.h"
#include "DynamicObject.h"
#include "SpellAuras.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include <cmath>
#include <sstream>
#include <iomanip>
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

            // Kampf-KI (siehe BotMgr.h-Kopfkommentar "Kampf-KI"/ProcessBotCombatAI()): eigener
            // ~400ms-Akkumulator, deshalb unbedenklich JEDEN Tick aufzurufen.
            ProcessBotCombatAI(accountId, diff);

            // Autonomer Dungeon-Clear-Modus (siehe SetDungeonClearMode()-Kommentar): eigener ~1s-
            // Akkumulator, tut fuer die meisten Bots nichts (DungeonClearActive default false).
            ProcessDungeonClear(accountId, diff);

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

    // Gruppe Stufe 2, Teil A: server-weiter Lazy-Nachfuell-Trigger (siehe BotMgr.h-Kopfkommentar bei
    // ProcessLfgPoolFillTick()) - EINMAL pro BotMgr::Tick()-Aufruf, nicht pro Bot-Session (der Trigger
    // prueft ueber alle Bots/Queues hinweg selbst, siehe dortige Implementierung).
    ProcessLfgPoolFillTick(diff);
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

    // Bekannter, dokumentierter Nebenbug (README "Gruppe/LFR (Stufe 1)"): ohne einen expliziten
    // MotionMaster::MoveChase()-Aufruf bleibt der Bot stehen, waehrend Attack() nur den Kampfzustand
    // (UNIT_STATE_MELEE_ATTACKING/GetVictim()) setzt, aber KEINE Bewegung ansetzt - ein echter Client
    // haelt die Melee-Reichweite durch eigenes Nachlaufen des Spielers, das hier fehlt. Bei einem
    // stationaeren Ziel faellt das nicht auf (Bot steht ohnehin schon in Reichweite), bei einem
    // beweglichen/Critter-Ziel (creature_template.type=8, CREATURE_TYPE_CRITTER, typischerweise mit
    // RANDOM_MOTION_TYPE) laeuft das Ziel dem Bot ohne Fehlermeldung aus der Reichweite. Fix: denselben
    // MotionMaster::MoveFollow()-Unterbau wie StartBotFollow() (Runde 137) nutzen, hier aber ueber
    // MoveChase() (Combat-Aequivalent - haelt Waffenreichweite statt Follow-Abstand, bricht automatisch
    // ab, sobald der Bot den Kampf verlaesst/AttackStop() aufgerufen wird). Nur bei erfolgreichem
    // Attack()-Start ausgeloest, damit ein fehlgeschlagener Angriffsversuch keinen verwaisten
    // Chase-Generator hinterlaesst.
    if (attackOk)
        player->GetMotionMaster()->MoveChase(target);

    TC_LOG_INFO("scripts.bots", "BotMgr::StartBotAttack: Account %u - Attack() Rueckgabe=%d, "
        "HasUnitState(MELEE_ATTACKING)=%d, GetVictim() gesetzt=%d, MoveChase() ausgeloest=%d.", accountId,
        attackOk, player->HasUnitState(UNIT_STATE_MELEE_ATTACKING), player->GetVictim() != nullptr, attackOk);

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

    // Gegenstueck zum MoveChase()-Fix in StartBotAttack(): ohne diesen Abbruch wuerde der zuvor
    // angesetzte Chase-Generator (MOTION_SLOT_ACTIVE) den Bot weiter Richtung Ziel laufen lassen,
    // obwohl der Kampf bereits per AttackStop() beendet wurde. Dieselbe Notbremsen-Konvention wie
    // StopBotFollow() (Runde 137)/StopBotPatrol(): MoveIdle() setzt den aktiven Motion-Slot auf einen
    // einfachen IdleMovementGenerator zurueck, Bot bleibt sofort an der aktuellen Position stehen.
    player->GetMotionMaster()->MoveIdle();

    TC_LOG_INFO("scripts.bots", "BotMgr::StopBotAttack: Account %u - nach AttackStop()+MoveIdle(): "
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

namespace
{
    // Gruppe Stufe 2, Teil A: Tuning-Konstanten fuer den Lazy-Nachfuell-Trigger (siehe volle
    // Begruendung im BotMgr.h-Kopfkommentar bei ProcessLfgPoolFillTick()/TriggerLfgPoolFillOnce()).
    // Benannte Konstanten statt Magic Numbers, damit ein spaeterer Tuning-Durchgang (nach einem
    // echten Live-Test mit realen Wartezeiten) keine Funktionssignaturen aendern muss.
    constexpr uint32 LFG_POOL_FILL_INTERVAL_MS = 10000;            // alle 10s ein Nachfuell-Versuch
    constexpr time_t LFG_POOL_FILL_WAIT_THRESHOLD_SECONDS = 30;    // "Warteschlange lange leer"
    constexpr uint32 LFG_POOL_FILL_MAX_BOT_ATTEMPTS = 8;           // pro Fuellversuch max. Kandidaten testen
}

bool BotMgr::IsBotPlayerGuid(ObjectGuid guid) const
{
    // Siehe Begruendung im BotMgr.h-Kopfkommentar bei dieser Methode: _botSessions ist die einzige
    // autoritative Quelle, kein Account-Id-Bereich/keine Heuristik. Linearer Scan (Bot-Anzahl laut
    // Aufgabenstellung bis ~500) - unkritisch, da diese Methode nur aus dem alle ~10s laufenden
    // Lazy-Nachfuell-Tick heraus in relevanter Zahl aufgerufen wird, nicht pro Weltserver-Frame.
    for (auto const& [accountId, entry] : _botSessions)
    {
        if (entry.Session && entry.Session->GetPlayer() && entry.Session->GetPlayer()->GetGUID() == guid)
            return true;
    }
    return false;
}

// Gruppe Stufe 2, Teil A: siehe voller Design-Kommentar im BotMgr.h-Kopfkommentar bei dieser Methode.
bool BotMgr::TriggerLfgPoolFillOnce()
{
    using namespace lfg;

    for (uint8 team = TEAM_ALLIANCE; team <= TEAM_HORDE; ++team)
    {
        LfgQueueContainer const& queues = sLFGMgr->GetQueuesForTeam(team);
        for (auto const& [queueId, queue] : queues)
        {
            LfgQueueDataContainer const& queueData = queue.GetQueueDataStore();
            for (auto const& [candidateGuid, data] : queueData)
            {
                // Nur Queue-Eintraege mit MINDESTENS EINEM echten (Nicht-Bot-)Mitglied sind fuer den
                // Trigger relevant - ein reiner Bot-Kandidat (z.B. von uns selbst gerade erst
                // eingereiht) braucht keinen weiteren Fueller.
                bool hasRealMember = false;
                uint8 presentRoles = 0;
                for (auto const& [memberGuid, role] : data.roles)
                {
                    if (!IsBotPlayerGuid(memberGuid))
                        hasRealMember = true;
                    presentRoles |= role;
                }

                if (!hasRealMember)
                    continue;

                LfgQueueRoleCount const roleCount = LFGMgr::GetRoleCountByQueueId(queueId);
                bool missingTank = roleCount.minTanks > 0 && !(presentRoles & PLAYER_ROLE_TANK);
                bool missingHealer = roleCount.minHealers > 0 && !(presentRoles & PLAYER_ROLE_HEALER);

                time_t waited = time(nullptr) - data.joinTime;
                if (waited < LFG_POOL_FILL_WAIT_THRESHOLD_SECONDS && !missingTank && !missingHealer)
                    continue; // Auftragsvorgabe: "erst bei Bedarf", noch keine Notwendigkeit erkannt

                uint8 desiredRole = missingTank ? PLAYER_ROLE_TANK : (missingHealer ? PLAYER_ROLE_HEALER : PLAYER_ROLE_DAMAGE);
                LfgDungeonSet const dungeonsForBot = data.dungeons;

                uint32 attempts = 0;
                for (auto& [accountId, entry] : _botSessions)
                {
                    if (attempts >= LFG_POOL_FILL_MAX_BOT_ATTEMPTS)
                        break;

                    if (!entry.Session || entry.State != BotCharacterState::STATE_IN_WORLD)
                        continue;
                    if (_lfgFillerBotAccountIds.count(accountId))
                        continue;

                    Player* botPlayer = entry.Session->GetPlayer();
                    if (!botPlayer || !botPlayer->IsInWorld() || botPlayer->GetGroup())
                        continue;
                    if (botPlayer->GetTeamId() != TeamId(team))
                        continue;
                    if (sLFGMgr->GetState(botPlayer->GetGUID()) != LFG_STATE_NONE)
                        continue; // sollte bei korrektem _lfgFillerBotAccountIds-Tracking nicht vorkommen - Sicherheitsnetz

                    ++attempts;

                    // JoinLfg() nimmt eine nicht-const Referenz und mutiert/filtert die uebergebene
                    // Dungeon-Menge (GetCompatibleDungeons()) - fuer jeden Kandidaten-Bot eine frische
                    // Kopie uebergeben, damit ein fehlgeschlagener Versuch die Auswahl fuer den
                    // naechsten Kandidaten nicht verfaelscht.
                    LfgDungeonSet dungeonsCopy = dungeonsForBot;

                    TC_LOG_INFO("scripts.bots", "BotMgr::TriggerLfgPoolFillOnce: Queue %u - echter Kandidat %s "
                        "wartet %lld s (fehlende Pflichtrolle: Tank=%d Heiler=%d) - versuche Account %u ('%s') "
                        "als Rolle %u einzureihen.", queueId, candidateGuid.ToString().c_str(), (long long)waited,
                        missingTank, missingHealer, accountId, botPlayer->GetName().c_str(), uint32(desiredRole));

                    sLFGMgr->JoinLfg(botPlayer, desiredRole, dungeonsCopy);

                    if (sLFGMgr->GetState(botPlayer->GetGUID()) != LFG_STATE_NONE)
                    {
                        _lfgFillerBotAccountIds.insert(accountId);
                        TC_LOG_INFO("scripts.bots", "BotMgr::TriggerLfgPoolFillOnce: Account %u erfolgreich in "
                            "Queue %u eingereiht (LfgState=%u) - AdvanceLfgFillerBots() uebernimmt den weiteren "
                            "Fortschritt (Proposal/Teleport).", accountId, queueId,
                            uint32(sLFGMgr->GetState(botPlayer->GetGUID())));
                        return true; // Auftragsvorgabe: hoechstens EIN Bot pro Aufruf/Tick, keine Dauerlast
                    }

                    TC_LOG_INFO("scripts.bots", "BotMgr::TriggerLfgPoolFillOnce: Account %u von JoinLfg() "
                        "abgelehnt (Level-/Ilvl-/Lock-Check ueber GetCompatibleDungeons() nicht erfuellt fuer "
                        "diese Dungeon-Auswahl) - naechster Kandidat.", accountId);
                }
            }
        }
    }

    return false;
}

// Gruppe Stufe 2, Teil A: siehe voller Design-Kommentar im BotMgr.h-Kopfkommentar bei dieser Methode.
void BotMgr::AdvanceLfgFillerBots()
{
    using namespace lfg;

    if (_lfgFillerBotAccountIds.empty())
        return;

    std::vector<uint32> toErase;
    for (uint32 accountId : _lfgFillerBotAccountIds)
    {
        auto itr = _botSessions.find(accountId);
        if (itr == _botSessions.end() || !itr->second.Session)
        {
            toErase.push_back(accountId);
            continue;
        }

        Player* botPlayer = itr->second.Session->GetPlayer();
        if (!botPlayer || !botPlayer->IsInWorld())
        {
            toErase.push_back(accountId);
            continue;
        }

        ObjectGuid guid = botPlayer->GetGUID();
        LfgState state = sLFGMgr->GetState(guid);

        if (state == LFG_STATE_PROPOSAL)
        {
            uint32 proposalId = sLFGMgr->GetProposalId(guid);
            if (proposalId)
            {
                TC_LOG_INFO("scripts.bots", "BotMgr::AdvanceLfgFillerBots: Account %u - Proposal %u aktiv, kein "
                    "Client vorhanden - rufe automatisch UpdateProposal(true) auf (ersetzt das ausbleibende "
                    "CMSG_LFG_PROPOSAL_RESULT).", accountId, proposalId);
                sLFGMgr->UpdateProposal(proposalId, guid, true);
            }
        }

        // Egal ob durch den Aufruf direkt oben (letzte noch ausstehende Zusage - MakeNewGroup()/
        // TeleportPlayer() laufen synchron INNERHALB von UpdateProposal(), siehe LFGMgr.cpp) oder durch
        // einen spaeter zusagenden ECHTEN Mitspieler auf einem frueheren Tick bereits ausgeloest:
        // derselbe HandleMoveWorldportAck()-Nachtrag wie TeleportBot() (Runde 93), falls der Bot gerade
        // einen Kartenwechsel eingeleitet hat, den er ohne Client nie selbst per MSG_MOVE_WORLDPORT_ACK
        // bestaetigen wuerde.
        if (botPlayer->IsBeingTeleportedFar())
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::AdvanceLfgFillerBots: Account %u - IsBeingTeleportedFar()=true "
                "(LFG-Dungeon-Teleport), rufe manuell HandleMoveWorldportAck() auf.", accountId);
            itr->second.Session->HandleMoveWorldportAck();
        }

        if (state == LFG_STATE_DUNGEON || state == LFG_STATE_FINISHED_DUNGEON)
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::AdvanceLfgFillerBots: Account %u - Gruppe gefunden und Dungeon "
                "betreten (LfgState=%u), Fueller-Auftrag erfuellt, Bot bleibt regulaeres Gruppenmitglied.",
                accountId, uint32(state));
            toErase.push_back(accountId);
        }
        else if (state == LFG_STATE_NONE)
        {
            TC_LOG_INFO("scripts.bots", "BotMgr::AdvanceLfgFillerBots: Account %u - ohne Match wieder aus der "
                "LFG-Queue entfernt (Proposal abgelehnt oder Timeout) - Fueller-Slot freigegeben.", accountId);
            toErase.push_back(accountId);
        }
        // LFG_STATE_QUEUED/ROLECHECK: weiter warten, Matching laeuft tick-gesteuert in
        // LFGMgr::Update()->LFGQueue::FindGroups(), keine weitere Aktion hier noetig.
    }

    for (uint32 accountId : toErase)
        _lfgFillerBotAccountIds.erase(accountId);
}

void BotMgr::ProcessLfgPoolFillTick(uint32 diff)
{
    // Fortschritt bereits aktiver Fueller-Bots JEDEN Tick pruefen (Proposal-Fenster ist mit
    // LFG_TIME_PROPOSAL=45s knapp - hier zu selten nachzusehen wuerde Proposals unnoetig verfallen
    // lassen), das eigentliche NEU-Einreihen dagegen nur alle LFG_POOL_FILL_INTERVAL_MS (siehe dort).
    AdvanceLfgFillerBots();

    _lfgFillTickAccumMs += diff;
    if (_lfgFillTickAccumMs < LFG_POOL_FILL_INTERVAL_MS)
        return;

    _lfgFillTickAccumMs = 0;
    TriggerLfgPoolFillOnce();
}

namespace
{
    // Kampf-KI: alle ~400ms statt jeden Weltserver-Tick (siehe BotSessionEntry::CombatAiTickAccumMs).
    constexpr uint32 BOT_COMBAT_AI_TICK_MS = 400;
    // Nahkampf-Engagement-Distanz fuer die automatische Mit-Kampf-Logik in SelectBotCombatTarget()
    // (Tank/Nahkampf-DPS folgen/engagieren erst innerhalb dieser Distanz automatisch mit).
    constexpr float BOT_MELEE_ENGAGE_RANGE = 30.0f;
    // Heiler-Rolle betrachtet ein Gruppenmitglied erst ab dieser Lebens-Schwelle ueberhaupt als
    // "braucht etwas" - verhindert sinnloses Dauerheilen bei vollem Leben.
    constexpr float BOT_HEAL_CONSIDER_THRESHOLD_PCT = 90.0f;

    // --- Kampf-KI: Pilot-Rotationstabellen, Patch 7.3.5 (Build 26972) -----------------------------
    //
    // QUELLENLAGE (siehe voller PR-Bericht fuer die vollstaendige Aufschluesselung je Skillung):
    // Faehigkeiten-Namen, -Reihenfolge und -Bedingungen stammen aus einer gezielten Web-Recherche
    // dieser Runde (Icy-Veins-/Wowhead-/guiaswow.com-abgeleitete 7.2/7.3.5-Guides, mehrfach
    // gegengeprueft), AUSSCHLIESSLICH auf Patch 7.3.5 eingegrenzt (explizit NICHT BfA/Shadowlands/
    // aktuelles Retail, da sich Faehigkeiten seither mehrfach grundlegend geaendert haben). Die
    // Recherche selbst nennt je Skillung eine Konfidenzeinschaetzung ("hoch"/"mittel-hoch"/"mittel"/
    // "niedrig") auf Namen/Reihenfolge - siehe Kommentar bei jedem einzelnen Tabelleneintrag unten.
    // NUMERISCHE Spell-IDs waren dagegen in KEINER Quelle dieser
    // Runde zuverlaessig zu bestaetigen (Netzwerkzugriff auf Wowhead/Icy-Veins/web.archive.org war in
    // der Recherche-Sandbox blockiert) - deshalb enthaelt diese Tabelle bewusst KEINE IDs, sondern nur
    // die recherchierten Namen; ResolveSpellIdByName() (siehe dort) loest sie beim ersten Gebrauch
    // gegen das tatsaechlich auf DIESEM Server geladene Spell.db2 auf. Das ist die einzige Quelle, die
    // fuer GENAU diesen Build (26972) garantiert korrekt ist - kein Raten, kein Uebernehmen einer
    // moeglicherweise falschen/veralteten ID aus einer anderen Patch-Version.
    //
    // Runde 2 (diese Runde): von 4 auf 29 von 36 Skillungen erweitert - reine Dateneingabe nach
    // demselben Framework, kein Code-Umbau noetig (siehe PR-Bericht). Bewusst NICHT aufgenommen, mit
    // Begruendung (Mechanik passt nicht in das aktuelle Bedingungs-Vokabular Always/
    // TargetHealthPctBelow/SelfHealthPctBelow/ResourceAtLeast/Aura(Missing|Present)On(Self|Target),
    // siehe BotMgr.h):
    //   - Priest Discipline (256): Atonement-Mechanik heilt ueber Schaden an EINEM Ziel (Smite/Holy
    //     Fire) waehrend das eigentliche Heilziel ein ANDERES Ziel ist (Atonement-Traeger) - das
    //     Framework kennt aktuell nur EIN Ziel pro Tick-Entscheidung.
    //   - Monk Brewmaster (268): Stagger-Schweregrad (leicht/mittel/schwer) ist keine einfache
    //     Aura-Anwesenheit, sondern ein Stack-/Prozentwert - braucht einen neuen Bedingungstyp.
    //   - Monk Windwalker (269): Fists-of-Fury->Rising-Sun-Kick->Whirling-Dragon-Punch-Combo braucht
    //     Reihenfolge-/Timing-Gedaechtnis ueber mehrere Ticks hinweg, keine reine Prioritaetsliste.
    //   - Warlock Demonology (266): laut Recherche selbst als NIEDRIGE Konfidenz markiert (mehrfache
    //     grundlegende Neugestaltung waehrend Legion, keine 7.3.5-datierte Quelle verifizierbar).
    //   - Shaman Enhancement (263), Druid Feral (103), Druid Guardian (104): in dieser
    //     Recherche-Runde nicht abgedeckt (siehe Aufgabenverteilung der drei Recherche-Agents).
    // Rogue Outlaw (260) IST enthalten, aber mit einer bewusst vereinfachten "Roll the Bones nur
    // erneuern, wenn abgelaufen"-Regel statt der eigentlichen (in der Recherche selbst als umstritten/
    // patchabhaengig markierten) Wuerfel-Qualitaetsbewertung - siehe Kommentar dort.
    std::vector<BotSpecRotation> g_BotSpecRotations =
    {
        // --- Protection Warrior (specId 73) - Tank ---------------------------------------------
        // Quelle: "Shield Block > Ignore Pain, Rest ist Rage-Generierung" - als Kernphilosophie mit
        // HOHER Konfidenz recherchiert und laut Recherche ueber praktisch ganz Legion stabil (siehe
        // PR-Bericht). Revenge/Shield Slam/Devastate-Reihenfolge darunter mit MITTLERER Konfidenz.
        {
            73, SPELLFAMILY_WARRIOR, BotRole::Tank,
            {
                { "Shield Block",  BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Shield Block" },
                { "Ignore Pain",   BotRotationCondition::ResourceAtLeast,   60.0f, POWER_RAGE },
                { "Revenge",       BotRotationCondition::Always },
                { "Shield Slam",   BotRotationCondition::Always },
                { "Devastate",     BotRotationCondition::Always }
            },
            "Pummel"
        },
        // --- Fury Warrior (specId 72) - Nahkampf-DPS -------------------------------------------
        // Quelle: Bloodthirst/Raging Blow/Rampage-Kernschleife mit MITTEL-HOHER Konfidenz recherchiert
        // (Rampage-vs-Raging-Blow-Feinreihenfolge laut Recherche talentabhaengig/schwaecher belegt,
        // hier bewusst konservativ: Rampage erst ab hohem Rage-Wert, nicht bei jeder Gelegenheit).
        {
            72, SPELLFAMILY_WARRIOR, BotRole::MeleeDps,
            {
                { "Bloodthirst",   BotRotationCondition::Always },
                { "Raging Blow",   BotRotationCondition::Always },
                { "Rampage",       BotRotationCondition::ResourceAtLeast,     80.0f, POWER_RAGE },
                { "Execute",       BotRotationCondition::TargetHealthPctBelow, 20.0f },
                { "Whirlwind",     BotRotationCondition::Always }
            },
            "Pummel"
        },
        // --- Frost Mage (specId 64) - Fernkampf/Zauber-DPS -------------------------------------
        // Quelle: Brain-Freeze->Flurry->Ice-Lance-"Shatter" und Fingers-of-Frost-Verbrauch mit HOHER
        // Konfidenz recherchiert (mehrfach als stabile 7.3.5-Kernschleife bestaetigt). Frozen Orb/
        // Ebonbolt auf Cooldown, Frostbolt als ressourcenloser Fuellschlag am Ende der Liste (immer
        // bereit, feuert also automatisch, wenn nichts anderes bereit/zutreffend ist).
        {
            64, SPELLFAMILY_MAGE, BotRole::RangedDps,
            {
                { "Flurry",        BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Brain Freeze" },
                { "Ice Lance",     BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Fingers of Frost" },
                { "Frozen Orb",    BotRotationCondition::Always },
                { "Ebonbolt",      BotRotationCondition::Always },
                { "Frostbolt",     BotRotationCondition::Always }
            },
            "Counterspell"
        },
        // --- Restoration Shaman (specId 264) - Heiler ------------------------------------------
        // Quelle: Riptide-Erhalt + Healing-Wave/-Surge-Kosten-Abstufung mit HOHER Konfidenz
        // recherchiert (laut Recherche ueber praktisch ganz Legion stabile Kernidentitaet). "Ziel" ist
        // hier IMMER das von SelectBotHealTarget() gewaehlte Gruppenmitglied, nicht ein Gegner -
        // TargetHealthPctBelow greift deshalb identisch wie bei DPS-Rollen, nur bezogen auf das
        // Heilziel statt einen Feind (siehe BotRotationCondition-Kommentar in BotMgr.h).
        {
            264, SPELLFAMILY_SHAMAN, BotRole::Healer,
            {
                { "Healing Surge", BotRotationCondition::TargetHealthPctBelow, 35.0f },
                { "Riptide",       BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Riptide" },
                { "Chain Heal",    BotRotationCondition::TargetHealthPctBelow, 80.0f },
                { "Healing Wave",  BotRotationCondition::Always }
            },
            nullptr, "Purify Spirit"
        },

        // ==================== Runde 2: 25 weitere Skillungen (siehe Kopfkommentar oben) ====================

        // --- Warrior Arms (specId 71) - Nahkampf-DPS --- MITTEL: Colossus-Smash-Fenster-Feinsteuerung
        // (Mortal Strike bevorzugt WAEHREND des Fensters) ist hier NICHT modelliert - Colossus Smash und
        // Mortal Strike laufen beide einfach "on cooldown", was strukturell korrekt aber nicht
        // burst-optimal ist (dokumentierte Vereinfachung).
        {
            71, SPELLFAMILY_WARRIOR, BotRole::MeleeDps,
            {
                { "Colossus Smash", BotRotationCondition::Always },
                { "Mortal Strike",  BotRotationCondition::Always },
                { "Execute",        BotRotationCondition::TargetHealthPctBelow, 20.0f },
                { "Overpower",      BotRotationCondition::Always },
                { "Slam",           BotRotationCondition::Always }
            },
            "Pummel"
        },
        // --- Paladin Protection (specId 66) - Tank --- HOCH auf "Shield of the Righteous halten"-
        // Kernidentitaet, MITTEL auf Avenger's-Shield-vs-Judgment-Feinreihenfolge.
        {
            66, SPELLFAMILY_PALADIN, BotRole::Tank,
            {
                { "Shield of the Righteous", BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Shield of the Righteous" },
                { "Judgment",                BotRotationCondition::Always },
                { "Avenger's Shield",        BotRotationCondition::Always },
                { "Consecration",            BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Consecration" },
                { "Hammer of the Righteous", BotRotationCondition::Always }
            },
            "Rebuke"
        },
        // --- Paladin Retribution (specId 70) - Nahkampf-DPS --- MITTEL-HOCH: Judgment-Fenster-
        // Mechanik (Templar's Verdict bevorzugt waehrend des Judgment-Debuffs) recherchiert, hier
        // vereinfacht als reine Holy-Power-Schwelle statt Debuff-Timing-Praezision.
        {
            70, SPELLFAMILY_PALADIN, BotRole::MeleeDps,
            {
                { "Judgment",         BotRotationCondition::Always },
                { "Templar's Verdict", BotRotationCondition::ResourceAtLeast, 3.0f, POWER_HOLY_POWER },
                { "Blade of Justice", BotRotationCondition::Always },
                { "Crusader Strike",  BotRotationCondition::Always }
            },
            "Rebuke"
        },
        // --- Paladin Holy (specId 65) - Heiler --- HOCH: Beacon-of-Light/Holy-Shock-Kernidentitaet.
        // Light of Dawn (AoE-Holy-Power-Spender) bewusst weggelassen - Framework hat aktuell kein
        // AoE-Heilziel-Modell (siehe BotMgr.h SelectBotHealTarget(), waehlt IMMER genau EIN Ziel).
        {
            65, SPELLFAMILY_PALADIN, BotRole::Healer,
            {
                { "Beacon of Light", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Beacon of Light" },
                { "Holy Shock",      BotRotationCondition::Always },
                { "Flash of Light",  BotRotationCondition::TargetHealthPctBelow, 50.0f },
                { "Holy Light",      BotRotationCondition::Always }
            },
            nullptr, "Cleanse"
        },
        // --- Death Knight Blood (specId 250) - Tank --- MITTEL: Death-Strike/Bone-Shield-Kernloop
        // korrekt, aber exakte Bone-Shield-Stack-Schwelle/Rune-Kosten laut Recherche selbst nicht
        // patchgenau bestaetigt - hier vereinfacht als reine Aura-Anwesenheit statt Stack-Zaehler.
        {
            250, SPELLFAMILY_DEATHKNIGHT, BotRole::Tank,
            {
                { "Marrowrend",  BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Bone Shield" },
                { "Death Strike", BotRotationCondition::ResourceAtLeast, 45.0f, POWER_RUNIC_POWER },
                { "Heart Strike", BotRotationCondition::Always }
            },
            "Mind Freeze"
        },
        // --- Death Knight Frost (specId 251) - Nahkampf-DPS --- MITTEL-HOCH, inkl. 7.3.5-spezifischem
        // Disintegration-Talent-Detail (Killing-Machine-Verlaengerung durch Frost Strike/Howling
        // Blast). 2H-Build angenommen (laut Recherche in Spaet-Legion der dominante/haeufigere Build).
        {
            251, SPELLFAMILY_DEATHKNIGHT, BotRole::MeleeDps,
            {
                { "Obliterate",   BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Killing Machine" },
                { "Frost Strike", BotRotationCondition::ResourceAtLeast, 60.0f, POWER_RUNIC_POWER },
                { "Howling Blast", BotRotationCondition::Always },
                { "Remorseless Winter", BotRotationCondition::Always }
            },
            "Mind Freeze"
        },
        // --- Death Knight Unholy (specId 252) - Nahkampf-DPS --- MITTEL-HOCH (Festering-Wound-
        // Builder/Popper-Loop und die genannten Talente sind gut belegt). Apocalypse (Wound-Stack-
        // gated) bewusst weggelassen - braucht Stack-Zaehler, den das Framework noch nicht kennt.
        {
            252, SPELLFAMILY_DEATHKNIGHT, BotRole::MeleeDps,
            {
                { "Festering Strike", BotRotationCondition::Always },
                { "Scourge Strike",   BotRotationCondition::Always },
                { "Death Coil",       BotRotationCondition::ResourceAtLeast, 60.0f, POWER_RUNIC_POWER },
                { "Outbreak",         BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Virulent Plague" }
            },
            "Mind Freeze"
        },
        // --- Rogue Assassination (specId 259) - Nahkampf-DPS --- MITTEL-HOCH, explizit als 7.3.5-
        // Quelle bestaetigt (guiaswow.com "Patch 7.3.5"-Seite).
        {
            259, SPELLFAMILY_ROGUE, BotRole::MeleeDps,
            {
                { "Garrote",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Garrote" },
                { "Rupture",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Rupture" },
                { "Envenom",  BotRotationCondition::ResourceAtLeast, 4.0f, POWER_COMBO_POINTS },
                { "Mutilate", BotRotationCondition::Always }
            },
            "Kick"
        },
        // --- Rogue Outlaw (specId 260) - Nahkampf-DPS --- MITTEL: Roll-the-Bones-"Reroll wenn
        // schlechte Wuerfe"-Feinlogik ist laut Recherche selbst patchabhaengig/umstritten und deshalb
        // NICHT modelliert - hier bewusst vereinfacht auf "erneuere nur, wenn der Buff komplett
        // abgelaufen ist" (AuraMissingOnSelf), niemals eine aktive Buff-Kombination verwerfen.
        {
            260, SPELLFAMILY_ROGUE, BotRole::MeleeDps,
            {
                { "Roll the Bones",  BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Roll the Bones" },
                { "Between the Eyes", BotRotationCondition::ResourceAtLeast, 5.0f, POWER_COMBO_POINTS },
                { "Saber Slash",     BotRotationCondition::Always }
            },
            "Kick"
        },
        // --- Rogue Subtlety (specId 261) - Nahkampf-DPS --- MITTEL-HOCH inkl. konkretem Opener aus der
        // Recherche (hier nur die Kernschleife, kein separater Opener-Zustand).
        {
            261, SPELLFAMILY_ROGUE, BotRole::MeleeDps,
            {
                { "Symbols of Death", BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Symbols of Death" },
                { "Nightblade",       BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Nightblade" },
                { "Eviscerate",       BotRotationCondition::ResourceAtLeast, 5.0f, POWER_COMBO_POINTS },
                { "Backstab",         BotRotationCondition::Always }
            },
            "Kick"
        },
        // --- Demon Hunter Havoc (specId 577) - Nahkampf-DPS --- MITTEL-HOCH. Momentum-Build (Fel Rush
        // offensiv fuer den Buff nutzen) bewusst weggelassen - talentabhaengige Sonderlogik.
        {
            577, SPELLFAMILY_DEMON_HUNTER, BotRole::MeleeDps,
            {
                { "Chaos Strike", BotRotationCondition::Always },
                { "Blade Dance",  BotRotationCondition::Always },
                { "Eye Beam",     BotRotationCondition::Always },
                { "Demon's Bite", BotRotationCondition::Always }
            },
            "Disrupt"
        },
        // --- Demon Hunter Vengeance (specId 581) - Tank --- MITTEL: Soul-Fragment-Zaehler (steuert
        // Soul-Cleave-vs-Spirit-Bomb-Wahl) nicht modelliert - hier fest auf den Soul-Cleave-Build
        // vereinfacht (laut Recherche der "sicherere"/einfachere der beiden Spaet-Legion-Builds).
        {
            581, SPELLFAMILY_DEMON_HUNTER, BotRole::Tank,
            {
                { "Immolation Aura", BotRotationCondition::Always },
                { "Demon Spikes",    BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Demon Spikes" },
                { "Sigil of Flame",  BotRotationCondition::Always },
                { "Soul Cleave",     BotRotationCondition::Always }
            },
            "Disrupt"
        },
        // --- Mage Arcane (specId 62) - Fernkampf/Zauber-DPS --- HOCH: Arcane-Charges sind in diesem
        // Core als echte Ressource (POWER_ARCANE_CHARGES) implementiert, passt direkt ins
        // ResourceAtLeast-Modell. Burn/Evocation-Manazyklus (Rune of Power) bewusst weggelassen -
        // braucht eigenen Ressourcen-Pooling-Zustand ueber mehrere Ticks.
        {
            62, SPELLFAMILY_MAGE, BotRole::RangedDps,
            {
                { "Arcane Missiles", BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Clearcasting" },
                { "Arcane Barrage",  BotRotationCondition::ResourceAtLeast, 4.0f, POWER_ARCANE_CHARGES },
                { "Arcane Blast",    BotRotationCondition::Always }
            },
            "Counterspell"
        },
        // --- Mage Fire (specId 63) - Fernkampf/Zauber-DPS --- HOCH auf die Hot-Streak/Heating-Up-
        // Proc-Kernschleife. Combustion-Cooldown-Timing (auf Hot Streak warten, dann pop) bewusst
        // weggelassen - Cooldown selbst ist in dieser Tabelle nicht enthalten, kann spaeter als
        // zusaetzlicher Schritt (Always, hohe Prioritaet) ergaenzt werden.
        {
            63, SPELLFAMILY_MAGE, BotRole::RangedDps,
            {
                { "Pyroblast",       BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Hot Streak" },
                { "Fire Blast",      BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Heating Up" },
                { "Phoenix's Flames", BotRotationCondition::Always },
                { "Fireball",        BotRotationCondition::Always }
            },
            "Counterspell"
        },
        // --- Warlock Affliction (specId 265) - Fernkampf/Zauber-DPS --- HOCH auf die
        // Agony/Corruption/Unstable-Affliction-Dauerpflege-Identitaet.
        {
            265, SPELLFAMILY_WARLOCK, BotRole::RangedDps,
            {
                { "Agony",               BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Agony" },
                { "Corruption",          BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Corruption" },
                { "Unstable Affliction", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Unstable Affliction" },
                { "Drain Soul",          BotRotationCondition::Always }
            }
        },
        // --- Warlock Destruction (specId 267) - Fernkampf/Zauber-DPS --- HOCH: Immolate-Dauerpflege +
        // Conflagrate-fuer-Shards + Chaos-Bolt-als-Spender ist laut Recherche ueber 7.2/7.3.5 stabil.
        {
            267, SPELLFAMILY_WARLOCK, BotRole::RangedDps,
            {
                { "Immolate",    BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Immolate" },
                { "Conflagrate", BotRotationCondition::Always },
                { "Chaos Bolt",  BotRotationCondition::ResourceAtLeast, 2.0f, POWER_SOUL_SHARDS },
                { "Incinerate",  BotRotationCondition::Always }
            }
        },
        // --- Priest Shadow (specId 258) - Fernkampf/Zauber-DPS --- HOCH auf SW:P/VT-Dauerpflege und
        // die Void-Eruption/Voidform-Kernmechanik. Voidform-Eintrittsschwelle als ResourceAtLeast(90)
        // angenaehert (Recherche nennt 65-90 talentabhaengig) - bewusst der hoehere/sicherere Wert.
        {
            258, SPELLFAMILY_PRIEST, BotRole::RangedDps,
            {
                { "Void Eruption",     BotRotationCondition::ResourceAtLeast, 90.0f, POWER_INSANITY },
                { "Void Bolt",         BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Voidform" },
                { "Shadow Word: Pain", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Shadow Word: Pain" },
                { "Vampiric Touch",    BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Vampiric Touch" },
                { "Mind Blast",        BotRotationCondition::Always },
                { "Mind Flay",         BotRotationCondition::Always }
            }
        },
        // --- Priest Holy (specId 257) - Heiler --- HOCH auf Heal/Flash-Heal-Fuellschlag +
        // Renew-Dauerpflege. Holy-Word-Serenity-Freicast-Proc (alle ~4 Casts) nicht modelliert -
        // braucht einen Cast-Zaehler-Zustand, den das Framework noch nicht kennt.
        {
            257, SPELLFAMILY_PRIEST, BotRole::Healer,
            {
                { "Flash Heal", BotRotationCondition::TargetHealthPctBelow, 40.0f },
                { "Renew",      BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Renew" },
                { "Heal",       BotRotationCondition::Always }
            },
            nullptr, "Dispel Magic"
        },
        // --- Shaman Elemental (specId 262) - Fernkampf/Zauber-DPS --- HOCH auf Flame-Shock/Lava-Burst/
        // Maelstrom-Kernschleife (direkt analog zu Immolate/Destruction oben).
        {
            262, SPELLFAMILY_SHAMAN, BotRole::RangedDps,
            {
                { "Lava Burst",    BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Lava Surge" },
                { "Earth Shock",   BotRotationCondition::ResourceAtLeast, 60.0f, POWER_MAELSTROM },
                { "Flame Shock",   BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Flame Shock" },
                { "Lightning Bolt", BotRotationCondition::Always }
            },
            "Wind Shear"
        },
        // --- Druid Balance (specId 102) - Fernkampf/Zauber-DPS --- HOCH auf Moonfire/Sunfire-
        // Dauerpflege. Astral Power ist in diesem Core ueber POWER_LUNAR_POWER hinterlegt (historischer
        // interner Name, siehe SharedDefines.h). Eclipse-Builder-Wechsel (Wrath/Starfire je nach
        // Sonne/Mond-Zustand) nicht modelliert - Wrath wird hier immer als Fuellschlag genutzt.
        {
            102, SPELLFAMILY_DRUID, BotRole::RangedDps,
            {
                { "Starsurge", BotRotationCondition::ResourceAtLeast, 50.0f, POWER_LUNAR_POWER },
                { "Moonfire",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Moonfire" },
                { "Sunfire",   BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Sunfire" },
                { "Wrath",     BotRotationCondition::Always }
            }
        },
        // --- Druid Restoration (specId 105) - Heiler --- HOCH auf die HoT-Weaving-Kernidentitaet
        // (Rejuvenation-Dauerpflege + Regrowth-Fuellheilung). Lifebloom (normalerweise fest auf dem
        // Tank statt dem Niedrigst-Leben-Ziel gehalten) bewusst weggelassen - passt nicht zum
        // "Heilziel = niedrigstes Leben"-Modell von SelectBotHealTarget().
        {
            105, SPELLFAMILY_DRUID, BotRole::Healer,
            {
                { "Rejuvenation", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Rejuvenation" },
                { "Regrowth",     BotRotationCondition::TargetHealthPctBelow, 50.0f },
                { "Wild Growth",  BotRotationCondition::TargetHealthPctBelow, 80.0f }
            },
            nullptr, "Remove Corruption"
        },
        // --- Monk Mistweaver (specId 270) - Heiler --- HOCH auf Renewing-Mist/Vivify/Enveloping-Mist-
        // Kernidentitaet. Soothing-Mist-Channel-Interaktion (erlaubt Bewegung waehrend andere Zauber
        // gecastet werden) nicht modelliert - fuer einen Bot ohnehin irrelevant (kein Movement-Zwang).
        {
            270, SPELLFAMILY_MONK, BotRole::Healer,
            {
                { "Renewing Mist",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Renewing Mist" },
                { "Enveloping Mist", BotRotationCondition::TargetHealthPctBelow, 50.0f },
                { "Vivify",         BotRotationCondition::Always }
            },
            nullptr, "Detox"
        },
        // --- Hunter Beast Mastery (specId 253) - Fernkampf-DPS --- MITTEL-HOCH auf Kill-Command/
        // Barbed-Shot/Bestial-Wrath-Kernschleife. Frenzy-Stack-Pflege auf dem PET (nicht dem Bot
        // selbst) nicht modelliert - AuraPresentOnSelf/AuraMissingOnSelf koennen nur Bot-eigene Auren
        // pruefen, keine Pet-Auren (Framework-Grenze, dokumentiert).
        {
            253, SPELLFAMILY_HUNTER, BotRole::RangedDps,
            {
                { "Kill Command", BotRotationCondition::Always },
                { "Barbed Shot",  BotRotationCondition::Always },
                { "Bestial Wrath", BotRotationCondition::Always },
                { "Cobra Shot",   BotRotationCondition::Always }
            },
            "Counter Shot"
        },
        // --- Hunter Marksmanship (specId 254) - Fernkampf-DPS --- MITTEL: "Vulnerable"-Debuff-Synergie
        // (Aimed Shot/Marked Shot bevorzugt WAEHREND Vulnerable aktiv ist) nicht modelliert - beide
        // Schuesse sind unabhaengig von Vulnerable castbar, hier bewusst auf eine simple 2-Schritt-
        // Prioritaet reduziert statt eine falsche Bedingung zu erfinden.
        {
            254, SPELLFAMILY_HUNTER, BotRole::RangedDps,
            {
                { "Aimed Shot",  BotRotationCondition::Always },
                { "Arcane Shot", BotRotationCondition::Always }
            },
            "Counter Shot"
        },
        // --- Hunter Survival (specId 255) - Nahkampf-DPS --- MITTEL. WICHTIG: in Legion ist Survival
        // eine NAHKAMPF-Skillung (Wildfire Bomb/Raptor Strike/Mongoose Bite) - komplett anders als in
        // jedem anderen Patch (Classic-BfA-Fernkampf bzw. Shadowlands+-Rework). Mongoose-Bite-Stack-
        // Fenster (mehrfach hintereinander casten waehrend Mongoose-Fury aktiv ist) nicht modelliert -
        // Raptor Strike als sichererer, stack-unabhaengiger Standard-Finisher gewaehlt.
        {
            255, SPELLFAMILY_HUNTER, BotRole::MeleeDps,
            {
                { "Wildfire Bomb",  BotRotationCondition::Always },
                { "Serpent Sting",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Serpent Sting" },
                { "Raptor Strike",  BotRotationCondition::Always }
            },
            "Counter Shot"
        },

        // ==================== Runde 3: die 4 zuvor mit Begruendung ausgelassenen Skillungen, jetzt ====
        // ==================== per Framework-Erweiterung (TargetOverride) bzw. Mehrfach-Schritten ======
        // ==================== geloest - siehe BotMgr.h "BotRotationTargetOverride"-Kommentar. =========

        // --- Priest Discipline (specId 256) - Heiler --- HOCH auf die Atonement/Barrier/Rapture-
        // Beziehung. Loest das urspruengliche Problem "Atonement heilt ueber Schaden an einem ANDEREN
        // Ziel als dem Heilziel" ueber TargetOverride: Power Word: Shield/Shadow Mend gehen an das per
        // ForceHealTarget erzwungene Heilziel (traegt danach Atonement), Smite geht an ForceEnemy (den
        // aktuellen Kampf-Gegner) und heilt darueber alle Atonement-Traeger als Nebeneffekt der
        // Blizzard-eigenen Spell-Effekt-Logik - kein Zusatzcode in BotMgr fuer den Heilungs-Nebeneffekt
        // noetig, das macht der Core-Spelleffekt von Smite/Holy Fire bereits selbst. Rapture/Power Word:
        // Barrier (AoE-Cooldowns) bewusst weggelassen - kein AoE-Ziel-Modell vorhanden (siehe Holy
        // Paladin/Light of Dawn-Kommentar oben).
        {
            256, SPELLFAMILY_PRIEST, BotRole::Healer,
            {
                { "Power Word: Shield", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Atonement",
                    BotRotationTargetOverride::ForceHealTarget },
                { "Shadow Mend",        BotRotationCondition::TargetHealthPctBelow, 30.0f, 0, nullptr,
                    BotRotationTargetOverride::ForceHealTarget },
                { "Smite",              BotRotationCondition::Always, 0.0f, 0, nullptr,
                    BotRotationTargetOverride::ForceEnemy }
            },
            nullptr, "Dispel Magic"
        },
        // --- Monk Brewmaster (specId 268) - Tank --- HOCH auf die Stagger/Ironskin-Brew/Purifying-
        // Brew-Ladungs-Mechanik als KONZEPT, MITTEL auf die konkrete Umsetzung hier: die echte Client-
        // Mechanik kennt abgestufte Stagger-Schweregrade (leicht/mittel/schwer) als eigene, interne
        // Auren ("Light/Moderate/Heavy Stagger") - werden hier als zwei AuraPresentOnSelf-Schritte
        // (Heavy zuerst, dann Moderate als Rueckfall) angenommen. NICHT unabhaengig bestaetigt, dass
        // diese Aura-NAMEN exakt so in diesem Server-Build 26972 vorliegen - ResolveSpellIdByName()
        // schlaegt sauber fehl (TC_LOG_ERROR, Schritt bleibt inaktiv) statt falsch zu casten, falls
        // nicht; mit '.lookup spell stagger' pruefen und ggf. den Namen hier anpassen.
        {
            268, SPELLFAMILY_MONK, BotRole::Tank,
            {
                { "Purifying Brew", BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Heavy Stagger" },
                { "Purifying Brew", BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Moderate Stagger" },
                { "Ironskin Brew",  BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Ironskin Brew" },
                { "Keg Smash",      BotRotationCondition::Always },
                { "Blackout Strike", BotRotationCondition::Always },
                { "Tiger Palm",     BotRotationCondition::Always }
            },
            "Spear Hand Strike"
        },
        // --- Monk Windwalker (specId 269) - Nahkampf-DPS --- MITTEL-HOCH auf die Namen/Grundreihenfolge
        // (Fists of Fury vor Rising Sun Kick vor Whirling Dragon Punch), NIEDRIG auf die exakte Combo-
        // Timing-Voraussetzung: Whirling Dragon Punch verlangt laut Recherche, dass BEIDE anderen
        // Faehigkeiten kuerzlich benutzt wurden - dieses Framework hat aktuell kein Mehr-Tick-
        // Sequenz-Gedaechtnis (siehe BotMgr.h-Roadmap-Kommentar), deshalb steht der Schritt hier als
        // einfaches "Always" in Prioritaetsreihenfolge. Ist die Combo-Voraussetzung nicht erfuellt,
        // scheitert Spell::CheckCast() intern (Core-eigene Validierung, KEIN Absturz) - der Bot
        // verschwendet in diesem Fall einen Tick-Versuch, bevor die naechste Prioritaet (Tiger Palm)
        // beim naechsten Kampf-KI-Tick zum Zug kommt. Touch of Death/Serenity/Storm-Earth-and-Fire
        // bewusst weggelassen (Execute-Schwelle bzw. talentabhaengige Cooldowns).
        {
            269, SPELLFAMILY_MONK, BotRole::MeleeDps,
            {
                { "Fists of Fury",        BotRotationCondition::Always },
                { "Rising Sun Kick",      BotRotationCondition::Always },
                { "Whirling Dragon Punch", BotRotationCondition::Always },
                { "Tiger Palm",           BotRotationCondition::Always }
            },
            "Spear Hand Strike"
        },
        // --- Warlock Demonology (specId 266) - Fernkampf/Zauber-DPS --- NIEDRIG (von der urspruenglichen
        // Recherche selbst so markiert: Demonology wurde waehrend Legion mehrfach grundlegend
        // umgestaltet, keine 7.3.5-datierte Primaerquelle in der Recherche-Sandbox verifizierbar - vor
        // Live-Einsatz dringend gegen eine 7.3.5-spezifische Quelle/SimC-APL nachpruefen, mehr als bei
        // jeder anderen Tabellenzeile in dieser Datei).
        {
            266, SPELLFAMILY_WARLOCK, BotRole::RangedDps,
            {
                { "Doom",               BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Doom" },
                { "Call Dreadstalkers", BotRotationCondition::Always },
                { "Hand of Gul'dan",    BotRotationCondition::ResourceAtLeast, 4.0f, POWER_SOUL_SHARDS },
                { "Demonbolt",          BotRotationCondition::AuraPresentOnSelf, 0.0f, 0, "Demonic Core" },
                { "Shadow Bolt",        BotRotationCondition::Always }
            }
        },

        // ==================== Runde 4: die letzten 3 Skillungen - 36/36 vollstaendig ====================

        // --- Shaman Enhancement (specId 263) - Nahkampf-DPS --- HOCH auf den Generator/Spender-Loop
        // (Boulderfist/Rockbiter baut Maelstrom auf, Stormstrike/Lava Lash geben es aus), MITTEL auf die
        // genaue Talentreihen-Platzierung der Level-100-Cooldown-Wahl (laut Recherche selbst als
        // niedrigste Konfidenz-Einzelheit markiert - hier bewusst nicht aufgenommen). Der Stormbringer-
        // Freicast-Proc AENDERT laut Recherche nicht WELCHE Faehigkeit gecastet wird (weiterhin
        // Stormstrike), nur ihre Kosten/ihr Cooldown - deshalb reicht ein einfacher "Always"-Schritt statt
        // einer eigenen Proc-Bedingung (anders als z.B. bei Frost Mage, wo der Proc die Faehigkeitswahl
        // selbst bestimmt).
        {
            263, SPELLFAMILY_SHAMAN, BotRole::MeleeDps,
            {
                { "Feral Spirit", BotRotationCondition::Always },
                { "Stormstrike",  BotRotationCondition::Always },
                { "Lava Lash",    BotRotationCondition::Always },
                { "Boulderfist",  BotRotationCondition::Always }
            },
            "Wind Shear"
        },
        // --- Druid Feral (specId 103) - Nahkampf-DPS --- HOCH auf die Rake/Rip/Savage-Roar-Dauerpflege-
        // Identitaet (7.3.5-spezifisch bestaetigt: Savage-Roar-Dauer erhoeht/Schadensbonus gesenkt
        // gegenueber frueherem Legion-Patch, siehe Recherche). Savage Roar selbst (reiner Schadens-Buff,
        // keine DoT/Aura-auf-Ziel) bewusst weggelassen - passt nicht sauber in AuraMissingOnSelf ohne
        // eigene Prioritaets-Verzerrung. Rip/Ferocious-Bite-Kombopunkt-Kosten werden NICHT separat
        // geprueft (kein UND-Verknuepfer fuer zwei Bedingungen im aktuellen Vokabular) - verlaesst sich
        // auf die core-eigene Ressourcenpruefung beim tatsaechlichen Cast-Versuch (dieselbe dokumentierte
        // Vereinfachung wie bei Windwalker Monk).
        {
            103, SPELLFAMILY_DRUID, BotRole::MeleeDps,
            {
                { "Tiger's Fury",   BotRotationCondition::Always },
                { "Rake",           BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Rake" },
                { "Rip",            BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Rip" },
                { "Ferocious Bite", BotRotationCondition::ResourceAtLeast, 5.0f, POWER_COMBO_POINTS },
                { "Shred",          BotRotationCondition::Always }
            },
            "Skull Bash"
        },
        // --- Druid Guardian (specId 104) - Tank --- HOCH: Ironfur als kontinuierlich zu erneuernde
        // aktive Mitigation ist derselbe "diese Faehigkeit hochhalten"-Musterfall wie Shield Block
        // (Warrior)/Shield of the Righteous (Paladin)/Demon Spikes (DH) oben - Recherche bestaetigt
        // explizit "Ironfur vor Mangle/Thrash priorisieren, wenn physischer Schaden eingeht".
        {
            104, SPELLFAMILY_DRUID, BotRole::Tank,
            {
                { "Ironfur", BotRotationCondition::AuraMissingOnSelf, 0.0f, 0, "Ironfur" },
                { "Mangle",  BotRotationCondition::Always },
                { "Thrash",  BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Thrash" },
                { "Moonfire", BotRotationCondition::AuraMissingOnTarget, 0.0f, 0, "Moonfire" },
                { "Maul",    BotRotationCondition::Always }
            },
            "Skull Bash"
        }
    };
}

uint32 BotMgr::ResolveSpellIdByName(std::string const& englishName, uint32 spellFamily) const
{
    // Siehe voller Begruendung im BotMgr.h-Kopfkommentar ("Kampf-KI") und bei g_BotSpecRotations
    // oben: numerische Spell-IDs sind ueber Patches hinweg NICHT stabil genug, um sie aus einer
    // Web-Recherche zu uebernehmen. Stattdessen wird hier - nach demselben Muster wie das bereits
    // existierende GM-Kommando '.lookup spell' (cs_lookup.cpp) - das TATSAECHLICH auf diesem Server
    // geladene Spell.db2 (Build 26972) nach einem EXAKTEN, gross-/kleinschreibungsunabhaengigen
    // Namens-Treffer durchsucht, zusaetzlich auf spellFamily gefiltert (verhindert Kollisionen mit
    // gleichnamigen Faehigkeiten anderer Klassen). Ergebnis ist dadurch garantiert korrekt fuer GENAU
    // diese Server-Version, unabhaengig davon, ob die urspruengliche Recherchequelle fuer eine andere
    // Buildnummer eine andere ID hatte.
    static std::unordered_map<std::string, uint32> resolveCache;
    std::string cacheKey = std::to_string(spellFamily) + ":" + englishName;
    auto cacheItr = resolveCache.find(cacheKey);
    if (cacheItr != resolveCache.end())
        return cacheItr->second;

    std::wstring wanted;
    Utf8toWStr(englishName, wanted);
    wstrToLower(wanted);

    uint32 found = 0;
    uint32 matchCount = 0;
    for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(id);
        if (!spellInfo || spellInfo->SpellFamilyName != spellFamily)
            continue;
        if (!spellInfo->SpellName || !spellInfo->SpellName->Str[LOCALE_enUS])
            continue;

        std::wstring candidate;
        Utf8toWStr(spellInfo->SpellName->Str[LOCALE_enUS], candidate);
        wstrToLower(candidate);
        if (candidate == wanted)
        {
            if (matchCount == 0)
                found = id;
            ++matchCount;
        }
    }

    if (matchCount == 0)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ResolveSpellIdByName: '%s' (SpellFamilyName %u) wurde in diesem "
            "Server-Spell.db2 NICHT gefunden - der zugehoerige Rotationsschritt bleibt dauerhaft inaktiv "
            "(kein Absturz). Moegliche Ursachen: Schreibweise weicht vom recherchierten 7.3.5-Namen ab, oder "
            "diese Faehigkeit heisst in Build 26972 anders (z.B. Talent-Umbenennung) - mit '.lookup spell "
            "%s' pruefen.", englishName.c_str(), spellFamily, englishName.c_str());
    }
    else if (matchCount > 1)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ResolveSpellIdByName: '%s' (SpellFamilyName %u) ist MEHRDEUTIG "
            "(%u exakte Treffer in Spell.db2) - verwende Spell-Id %u (erster Treffer), das sollte manuell per "
            "'.lookup spell %s' verifiziert werden.", englishName.c_str(), spellFamily, matchCount, found,
            englishName.c_str());
    }

    resolveCache[cacheKey] = found;
    return found;
}

BotSpecRotation const* BotMgr::GetOrResolveSpecRotation(uint32 specId) const
{
    for (BotSpecRotation const& rotation : g_BotSpecRotations)
    {
        if (rotation.SpecId != specId)
            continue;

        if (!rotation.ResolvedOnce)
        {
            for (BotRotationStep const& step : rotation.Priority)
            {
                step.ResolvedSpellId = ResolveSpellIdByName(step.SpellName, rotation.SpellFamily);
                if (step.ConditionAuxSpellName)
                    step.ResolvedAuxSpellId = ResolveSpellIdByName(step.ConditionAuxSpellName, rotation.SpellFamily);
            }
            if (rotation.InterruptSpellName)
                rotation.ResolvedInterruptSpellId = ResolveSpellIdByName(rotation.InterruptSpellName, rotation.SpellFamily);
            if (rotation.DispelSpellName)
                rotation.ResolvedDispelSpellId = ResolveSpellIdByName(rotation.DispelSpellName, rotation.SpellFamily);
            rotation.ResolvedOnce = true;
            TC_LOG_INFO("scripts.bots", "BotMgr::GetOrResolveSpecRotation: Rotation fuer specId %u (SpellFamily %u) "
                "einmalig gegen Spell.db2 aufgeloest (%u Schritte).", specId, rotation.SpellFamily,
                uint32(rotation.Priority.size()));
        }

        return &rotation;
    }

    return nullptr;
}

bool BotMgr::EvaluateBotRotationCondition(Player* player, Unit* target, BotRotationStep const& step) const
{
    switch (step.Condition)
    {
        case BotRotationCondition::Always:
            return true;
        case BotRotationCondition::TargetHealthPctBelow:
            return target && target->GetHealthPct() <= step.ConditionValue;
        case BotRotationCondition::SelfHealthPctBelow:
            return player->GetHealthPct() <= step.ConditionValue;
        case BotRotationCondition::ResourceAtLeast:
            return player->GetPower(Powers(step.ConditionAuxPower)) >= int32(step.ConditionValue);
        case BotRotationCondition::AuraMissingOnSelf:
            return step.ResolvedAuxSpellId != 0 && !player->HasAura(step.ResolvedAuxSpellId);
        case BotRotationCondition::AuraPresentOnSelf:
            return step.ResolvedAuxSpellId != 0 && player->HasAura(step.ResolvedAuxSpellId);
        case BotRotationCondition::AuraMissingOnTarget:
            return step.ResolvedAuxSpellId != 0 && target && !target->HasAura(step.ResolvedAuxSpellId);
        default:
            return false;
    }
}

Unit* BotMgr::SelectBotCombatTarget(Player* bot) const
{
    if (Unit* victim = bot->GetVictim())
        if (victim->IsAlive())
            return victim;

    // README-Luecke "kein autonomer Zustandsautomat" (Teilaspekt): kaempft bereits ein
    // Gruppenmitglied, engagiert der Bot automatisch dasselbe Ziel mit, statt untaetig danebenzustehen
    // und auf einen manuellen '.bottest attack'-Befehl zu warten. Fuer Nahkampf-naehe Distanz wird
    // dieselbe Attack()+MoveChase()-Logik wie StartBotAttack() (Runde 122/Nebenbugfix) direkt hier
    // ausgeloest - fuer Fernkampf/Zauber-Rollen reicht spaeter der reine Reichweiten-/LOS-Check in
    // ProcessBotCombatAI() vor dem eigentlichen Spruch.
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == bot || !member->IsInWorld())
                continue;

            Unit* victim = member->GetVictim();
            if (!victim || !victim->IsAlive() || victim->GetMapId() != bot->GetMapId())
                continue;

            if (!bot->IsInCombat() && bot->GetDistance(victim) <= BOT_MELEE_ENGAGE_RANGE)
            {
                bot->Attack(victim, true);
                bot->GetMotionMaster()->MoveChase(victim);
            }
            return victim;
        }
    }

    return nullptr;
}

Unit* BotMgr::SelectBotHealTarget(Player* bot) const
{
    Unit* lowestMember = nullptr;
    float lowestPct = 100.0f;

    auto consider = [&](Unit* candidate)
    {
        if (!candidate || !candidate->IsAlive() || candidate->GetMapId() != bot->GetMapId())
            return;
        float pct = candidate->GetHealthPct();
        if (pct < lowestPct)
        {
            lowestPct = pct;
            lowestMember = candidate;
        }
    };

    consider(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != bot)
                consider(member);
        }
    }

    // Bewusst nur zurueckgeben, wenn ueberhaupt jemand unter der Schwelle liegt - sonst tut die
    // Heiler-Rotation in dieser ersten Runde schlicht nichts (kein Fuellschaden/-heilung ohne Bedarf).
    return lowestPct <= BOT_HEAL_CONSIDER_THRESHOLD_PCT ? lowestMember : nullptr;
}

BotRole BotMgr::GetBotRole(uint32 accountId) const
{
    Player* player = GetBotPlayer(accountId);
    if (!player)
        return BotRole::Unknown;

    if (BotSpecRotation const* rotation = GetOrResolveSpecRotation(player->GetPrimarySpecialization()))
        return rotation->Role;

    return BotRole::Unknown;
}

void BotMgr::ProcessBotCombatAI(uint32 accountId, uint32 diff)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
        return;

    itr->second.CombatAiTickAccumMs += diff;
    if (itr->second.CombatAiTickAccumMs < BOT_COMBAT_AI_TICK_MS)
        return;
    itr->second.CombatAiTickAccumMs = 0;

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld() || !player->IsAlive())
        return;

    // Laufender Fremd-Zauber (z.B. noch von einer vorherigen Entscheidung) wird nicht abgebrochen/
    // ueberschrieben ("geclippt") - naechster Versuch beim naechsten Kampf-KI-Tick.
    if (player->IsNonMeleeSpellCast(false))
        return;

    BotSpecRotation const* rotation = GetOrResolveSpecRotation(player->GetPrimarySpecialization());
    if (!rotation)
        return; // Skillung noch nicht verdrahtet - siehe Kopfkommentar bei g_BotSpecRotations

    // Runde 3: BEIDE moeglichen Ziele im Voraus ermitteln (billig - jeweils nur eine Gruppen-Iteration/
    // ein GetVictim()-Zugriff), damit einzelne Schritte per TargetOverride unabhaengig von der
    // Skillungs-Rolle ein Gegner- oder Heilziel erzwingen koennen (siehe BotRotationTargetOverride-
    // Kommentar in BotMgr.h, noetig fuer Discipline Priest's Atonement-Mechanik).
    Unit* combatTarget = SelectBotCombatTarget(player);
    Unit* healTarget = SelectBotHealTarget(player);
    Unit* roleDefaultTarget = rotation->Role == BotRole::Healer ? healTarget : combatTarget;

    if (!combatTarget && !healTarget)
        return; // weder ein Kampfziel noch ein Heilbedarf - fuer diese Skillung aktuell nichts zu tun

    // Generische Boss-Mechanik-Reaktionen (Ausweichen/Interrupt/Dispel, siehe BotMgr.h-Kommentar bei
    // ProcessBotMechanicReactions()) haben Vorrang vor der normalen Rotation.
    if (ProcessBotMechanicReactions(player, rotation, combatTarget, healTarget))
        return;

    for (BotRotationStep const& step : rotation->Priority)
    {
        if (!step.ResolvedSpellId)
            continue; // Namensaufloesung ist fehlgeschlagen (siehe ResolveSpellIdByName()-Fehlerlog)

        Unit* target = roleDefaultTarget;
        if (step.TargetOverride == BotRotationTargetOverride::ForceEnemy)
            target = combatTarget;
        else if (step.TargetOverride == BotRotationTargetOverride::ForceHealTarget)
            target = healTarget;

        if (!target)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(step.ResolvedSpellId);
        if (!spellInfo || !player->HasSpell(step.ResolvedSpellId))
            continue; // (noch) nicht erlernt, z.B. talentabhaengige Faehigkeit ohne diese Talentwahl

        if (!player->GetSpellHistory()->IsReady(spellInfo))
            continue;

        if (!EvaluateBotRotationCondition(player, target, step))
            continue;

        float maxRange = spellInfo->GetMaxRange(false, player);
        if (maxRange > 0.0f && player->GetDistance(target) > maxRange)
            continue;
        if (!player->IsWithinLOSInMap(target))
            continue;

        if (player->CastSpell(target, step.ResolvedSpellId, TRIGGERED_NONE))
        {
            TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotCombatAI: Account %u castet '%s' (Id %u) auf %s.",
                accountId, step.SpellName, step.ResolvedSpellId, target->GetGUID().ToString().c_str());
            return; // maximal ein Zauber pro Tick (gemeinsame GCD-Ressource, siehe Kopfkommentar)
        }
    }
}

namespace
{
    // Check-Funktor fuer FindHarmfulGroundEffectUnderBot()/WorldObjectListSearcher<Check> (siehe
    // BotMgr.h-Kommentar bei ProcessBotMechanicReactions()). WorldObjectListSearcher<Check> ruft
    // i_check(...) fuer JEDEN WorldObject-Untertyp auf, der in einer Grid-/World-Zelle vorkommen kann
    // (Player/Creature/Corpse/GameObject/DynamicObject/AreaTrigger/SceneObject/Conversation - siehe
    // GridNotifiersImpl.h), nicht nur fuer den einen Typ, an dem dieser Suchcode interessiert ist -
    // anders als der schmalere AreaTriggerListSearcher<Check>, der nur AreaTrigger* kennt. Der
    // Ueberladungs-Vorrang von C++ (nicht-Template-Ueberladung schlaegt Template-Instanziierung)
    // erlaubt hier einen generischen Fallback fuer alle "uninteressanten" Typen plus genau eine
    // konkrete Ueberladung fuer DynamicObject*, ohne dass fuer jeden Typ einzeln eine leere
    // Ueberladung geschrieben werden muesste.
    class BotHarmfulDynObjCheck
    {
    public:
        BotHarmfulDynObjCheck(WorldObject const* searcher, float range) : _searcher(searcher), _range(range) { }

        template<typename T>
        bool operator()(T*) const { return false; }

        bool operator()(DynamicObject* dynObj) const
        {
            return _searcher->IsWithinDistInMap(dynObj, _range);
        }

    private:
        WorldObject const* _searcher;
        float _range;
    };
}

DynamicObject* BotMgr::FindHarmfulGroundEffectUnderBot(Player* bot, float searchRadius) const
{
    std::list<WorldObject*> candidates;

    CellCoord cellCoord(Trinity::ComputeCellCoord(bot->GetPositionX(), bot->GetPositionY()));
    Cell cell(cellCoord);
    cell.SetNoCreate();

    BotHarmfulDynObjCheck check(bot, searchRadius);
    Trinity::WorldObjectListSearcher<BotHarmfulDynObjCheck> searcher(bot, candidates, check,
        GRID_MAP_TYPE_MASK_DYNAMICOBJECT);

    TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulDynObjCheck>, WorldTypeMapContainer> worldVisitor(searcher);
    TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulDynObjCheck>, GridTypeMapContainer> gridVisitor(searcher);

    cell.Visit(cellCoord, worldVisitor, *bot->GetMap(), *bot, searchRadius);
    cell.Visit(cellCoord, gridVisitor, *bot->GetMap(), *bot, searchRadius);

    for (WorldObject* candidate : candidates)
    {
        DynamicObject* dynObj = candidate->ToDynObject();
        if (!dynObj)
            continue;

        // Nur SCHAEDLICHE persistente Flaecheneffekte sind fuer die Ausweich-Logik relevant (positive
        // Bodeneffekte, z.B. Heil-Totems/-Zonen, sollen der Bot natuerlich nicht verlassen).
        SpellInfo const* spellInfo = dynObj->GetSpellInfo();
        if (!spellInfo || spellInfo->IsPositive())
            continue;

        // Zwei getrennte Radien (siehe BotMgr.h-Kommentar): searchRadius nur fuer die Grid-Vorauswahl,
        // hier zaehlt einzig der TATSAECHLICHE Wirkradius des Effekts selbst.
        if (bot->GetExactDist2d(dynObj) <= dynObj->GetRadius())
            return dynObj;
    }

    return nullptr;
}

bool BotMgr::ProcessBotMechanicReactions(Player* bot, BotSpecRotation const* rotation, Unit* combatTarget,
    Unit* healTarget)
{
    // 1. Gefaehrlichen Bodeneffekt verlassen - hoechste Prioritaet, da Steh'nbleiben potentiell toedlich
    // ist, waehrend Interrupt/Dispel "nur" DPS/Heilausfall bedeuten. Suchradius bewusst klein gewaehlt
    // (der Bot steht ja bereits im/nahe am Effekt, wenn dieser ueberhaupt relevant wird).
    if (DynamicObject* harmfulEffect = FindHarmfulGroundEffectUnderBot(bot, 15.0f))
    {
        float fleeX, fleeY, fleeZ;
        // Radial vom Effektzentrum weg, ueber den Wirkradius hinaus (plus Sicherheitsabstand) -
        // MovePoint(generatePath=true) uebernimmt die eigentliche Navmesh-Route dorthin, damit der Bot
        // nicht durch Waende/von Klippen "flieht".
        float angle = harmfulEffect->GetAngle(bot);
        float distance = harmfulEffect->GetRadius() + 5.0f;
        fleeX = harmfulEffect->GetPositionX() + std::cos(angle) * distance;
        fleeY = harmfulEffect->GetPositionY() + std::sin(angle) * distance;
        fleeZ = harmfulEffect->GetPositionZ();
        bot->GetMotionMaster()->MovePoint(0, fleeX, fleeY, fleeZ, true);

        TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s weicht Bodeneffekt "
            "(Spell %u) aus.", bot->GetGUID().ToString().c_str(), harmfulEffect->GetSpellId());
        return true;
    }

    // 2. Interrupt - nur, wenn die Skillung ueberhaupt eine hat (siehe g_BotSpecRotations) und der
    // Bot sie bereits erlernt hat/sie einsatzbereit ist. Der Core prueft beim tatsaechlichen Cast von
    // Spell::EffectInterruptCast() selbst, ob combatTarget gerade unterbrechbar castet - hier reicht
    // die billige Vorabpruefung "castet ueberhaupt gerade etwas", um unnoetige Fehlversuche zu vermeiden.
    if (rotation->ResolvedInterruptSpellId && combatTarget)
    {
        bool targetIsCasting = combatTarget->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr
            || combatTarget->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;

        if (targetIsCasting && bot->HasSpell(rotation->ResolvedInterruptSpellId))
        {
            if (SpellInfo const* interruptInfo = sSpellMgr->GetSpellInfo(rotation->ResolvedInterruptSpellId))
            {
                if (bot->GetSpellHistory()->IsReady(interruptInfo)
                    && bot->GetDistance(combatTarget) <= interruptInfo->GetMaxRange(false, bot)
                    && bot->IsWithinLOSInMap(combatTarget))
                {
                    if (bot->CastSpell(combatTarget, rotation->ResolvedInterruptSpellId, TRIGGERED_NONE))
                    {
                        TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s "
                            "unterbricht %s (Interrupt-Spell %u).", bot->GetGUID().ToString().c_str(),
                            combatTarget->GetGUID().ToString().c_str(), rotation->ResolvedInterruptSpellId);
                        return true;
                    }
                }
            }
        }
    }

    // 3. Dispel - der Core waehlt die zu entfernende Aura selbst aus (Unit::GetDispellableAuraList(),
    // dieselbe Logik wie Spell::EffectDispel() sie fuer echte Spieler-Dispels nutzt), anhand der
    // DispelMask des Dispel-Spells selbst (SpellInfo::Dispel-Feld, z.B. "Dispel Magic" -> Magic).
    if (rotation->ResolvedDispelSpellId && healTarget && bot->HasSpell(rotation->ResolvedDispelSpellId))
    {
        if (SpellInfo const* dispelInfo = sSpellMgr->GetSpellInfo(rotation->ResolvedDispelSpellId))
        {
            if (bot->GetSpellHistory()->IsReady(dispelInfo)
                && bot->GetDistance(healTarget) <= dispelInfo->GetMaxRange(false, bot)
                && bot->IsWithinLOSInMap(healTarget))
            {
                DispelChargesList dispelList;
                healTarget->GetDispellableAuraList(bot, dispelInfo->GetDispelMask(), dispelList);
                if (!dispelList.empty())
                {
                    if (bot->CastSpell(healTarget, rotation->ResolvedDispelSpellId, TRIGGERED_NONE))
                    {
                        TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s "
                            "dispelt %s (Dispel-Spell %u).", bot->GetGUID().ToString().c_str(),
                            healTarget->GetGUID().ToString().c_str(), rotation->ResolvedDispelSpellId);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// Gemeinsamer Hilfscode fuer BotAcceptQuest()/BotTurnInQuest(): loest questGiverSpawnGuid (DB-Spawn-Id
// aus der `creature`-Tabelle, dieselbe Konvention wie bei StartBotAttack()/BotLootTarget()) zu einer
// lebenden Creature* auf demselben Map wie der Bot auf. nullptr bei jedem Fehlschlag, jeweils bereits
// mit TC_LOG_ERROR protokolliert.
static Creature* ResolveBotQuestGiver(Player* player, ObjectGuid::LowType questGiverSpawnGuid, char const* callerName)
{
    CreatureData const* data = sObjectMgr->GetCreatureData(questGiverSpawnGuid);
    if (!data)
    {
        TC_LOG_ERROR("scripts.bots", "%s: keine Spawn-Daten fuer questGiverSpawnGuid " UI64FMTD " gefunden "
            "(creature-Tabelle).", callerName, questGiverSpawnGuid);
        return nullptr;
    }

    if (data->mapid != player->GetMapId())
    {
        TC_LOG_ERROR("scripts.bots", "%s: Questgeber-Spawn " UI64FMTD " ist auf Map %u, Bot steht aber auf Map %u.",
            callerName, questGiverSpawnGuid, data->mapid, player->GetMapId());
        return nullptr;
    }

    Creature* questGiver = nullptr;
    auto range = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(questGiverSpawnGuid);
    for (auto rangeItr = range.first; rangeItr != range.second; ++rangeItr)
    {
        if (rangeItr->second && rangeItr->second->IsInWorld())
        {
            questGiver = rangeItr->second;
            break;
        }
    }

    if (!questGiver)
        TC_LOG_ERROR("scripts.bots", "%s: Questgeber (Spawn " UI64FMTD ") ist aktuell nicht als lebendes Objekt "
            "im Grid geladen (zu weit weg/nicht gespawnt).", callerName, questGiverSpawnGuid);

    return questGiver;
}

bool BotMgr::BotAcceptQuest(uint32 accountId, ObjectGuid::LowType questGiverSpawnGuid, uint32 questId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotAcceptQuest: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotAcceptQuest: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    Creature* questGiver = ResolveBotQuestGiver(player, questGiverSpawnGuid, "BotMgr::BotAcceptQuest");
    if (!questGiver)
        return false;

    if (!questGiver->hasQuest(questId))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotAcceptQuest: Account %u - Questgeber '%s' (Spawn " UI64FMTD ") "
            "bietet Quest %u nicht an.", accountId, questGiver->GetName().c_str(), questGiverSpawnGuid, questId);
        return false;
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotAcceptQuest: Account %u - Quest-Id %u existiert nicht in "
            "quest_template.", accountId, questId);
        return false;
    }

    // CanTakeQuest()/CanAddQuest() sind dieselben Pruefungen, die auch
    // WorldSession::HandleQuestgiverAcceptQuestOpcode() vor AddQuestAndCheckCompletion() aufruft
    // (Level-/Klassen-/Rassen-/Vorquest-/Ruf-Voraussetzungen, bereits aktiv/erledigt, Tagesquest-
    // Limit etc.) - msg=true schreibt bei Fehlschlag zusaetzlich eine SendSysMessage-Zeile in
    // Server.log/an den Bot (null-socket-sicher, siehe Kopfkommentar-Referenz).
    if (!player->CanTakeQuest(quest, true) || !player->CanAddQuest(quest, true))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotAcceptQuest: Account %u - CanTakeQuest()/CanAddQuest() fuer Quest "
            "%u ('%s') lieferte false - Voraussetzungen nicht erfuellt oder Quest bereits aktiv/erledigt.",
            accountId, questId, quest->GetLogTitle().c_str());
        return false;
    }

    // Derselbe Aufruf, den HandleQuestgiverAcceptQuestOpcode() selbst nach den obigen Checks macht
    // (QuestHandler.cpp) - kein Opcode-/Packet-Nachbau noetig, siehe BotMgr.h-Kopfkommentar.
    player->AddQuestAndCheckCompletion(quest, questGiver);

    TC_LOG_INFO("scripts.bots", "BotMgr::BotAcceptQuest: Account %u - Quest %u ('%s') von Questgeber '%s' "
        "(Spawn " UI64FMTD ") angenommen.", accountId, questId, quest->GetLogTitle().c_str(),
        questGiver->GetName().c_str(), questGiverSpawnGuid);
    return true;
}

bool BotMgr::BotTurnInQuest(uint32 accountId, ObjectGuid::LowType questGiverSpawnGuid, uint32 questId,
    uint32 rewardItemChoiceId)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: keine Bot-Session fuer Account %u vorhanden.", accountId);
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u hat aktuell keinen Player in der Welt "
            "(erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    Creature* questGiver = ResolveBotQuestGiver(player, questGiverSpawnGuid, "BotMgr::BotTurnInQuest");
    if (!questGiver)
        return false;

    if (!questGiver->hasInvolvedQuest(questId))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - Questgeber '%s' (Spawn " UI64FMTD ") "
            "nimmt Quest %u nicht entgegen.", accountId, questGiver->GetName().c_str(), questGiverSpawnGuid, questId);
        return false;
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - Quest-Id %u existiert nicht in "
            "quest_template.", accountId, questId);
        return false;
    }

    if (player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - Quest %u ('%s') ist noch nicht "
            "QUEST_STATUS_COMPLETE (aktueller Status %u) - Zielfortschritt zuerst abschliessen.", accountId,
            questId, quest->GetLogTitle().c_str(), uint32(player->GetQuestStatus(questId)));
        return false;
    }

    if (!player->CanRewardQuest(quest, rewardItemChoiceId, true))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - CanRewardQuest() fuer Quest %u ('%s') "
            "mit rewardItemChoiceId %u lieferte false (ungueltige Belohnungswahl?).", accountId, questId,
            quest->GetLogTitle().c_str(), rewardItemChoiceId);
        return false;
    }

    // Derselbe Aufruf, den HandleQuestgiverChooseRewardOpcode() selbst nach den obigen Checks macht.
    player->RewardQuest(quest, rewardItemChoiceId, questGiver);

    TC_LOG_INFO("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - Quest %u ('%s') bei Questgeber '%s' "
        "(Spawn " UI64FMTD ") abgegeben, rewardItemChoiceId %u.", accountId, questId, quest->GetLogTitle().c_str(),
        questGiver->GetName().c_str(), questGiverSpawnGuid, rewardItemChoiceId);
    return true;
}

int32 BotMgr::GetBotQuestStatus(uint32 accountId, uint32 questId) const
{
    Player* player = GetBotPlayer(accountId);
    if (!player)
        return -1;

    return int32(player->GetQuestStatus(questId));
}

namespace
{
    // Autonomer Dungeon-Clear-Modus: siehe voller Design-Kommentar bei BotMgr::SetDungeonClearMode()
    // (BotMgr.h) fuer die Begruendung (Ideenreferenz mod-dungeon-clear, komplett neu gebaut).
    constexpr uint32 BOT_DUNGEON_CLEAR_TICK_MS = 1000;         // Routing-Entscheidung alle ~1s
    constexpr float BOT_DUNGEON_CLEAR_TRASH_AGGRO_RADIUS = 15.0f;
    constexpr float BOT_DUNGEON_CLEAR_LOOT_RADIUS = 10.0f;
    constexpr float BOT_DUNGEON_CLEAR_ARRIVAL_DISTANCE = 5.0f; // "am Boss angekommen"-Toleranz
}

Creature* BotMgr::FindNearestLivingDungeonBoss(Player* bot) const
{
    Creature* best = nullptr;
    float bestDist = 0.0f;

    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* creature = pair.second;
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || !creature->IsDungeonBoss())
            continue;

        float dist = bot->GetDistance(creature);
        if (!best || dist < bestDist)
        {
            best = creature;
            bestDist = dist;
        }
    }

    return best;
}

Creature* BotMgr::FindNearestAggroableTrash(Player* bot, float radius) const
{
    Creature* best = nullptr;
    float bestDist = radius;

    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* creature = pair.second;
        // Dungeon-Bosse werden bewusst ausgeschlossen - die behandelt FindNearestLivingDungeonBoss()
        // separat, damit ein Boss nicht "nebenbei" wie gewoehnlicher Trash gepullt wird.
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsDungeonBoss())
            continue;
        if (!bot->IsValidAttackTarget(creature))
            continue;

        float dist = bot->GetDistance(creature);
        if (dist <= bestDist)
        {
            best = creature;
            bestDist = dist;
        }
    }

    return best;
}

Creature* BotMgr::FindNearestLootableCorpse(Player* bot, float radius) const
{
    Creature* best = nullptr;
    float bestDist = radius;

    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* creature = pair.second;
        if (!creature || !creature->IsInWorld() || creature->IsAlive())
            continue;
        if (!creature->HasFlag(OBJECT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
            continue;

        float dist = bot->GetDistance(creature);
        if (dist <= bestDist)
        {
            best = creature;
            bestDist = dist;
        }
    }

    return best;
}

std::vector<std::string> BotMgr::FindNpcSpawnsByName(WorldObject const* center, std::string const& namePart,
    float radius) const
{
    std::vector<std::string> results;
    if (!center || !center->GetMap() || namePart.empty())
        return results;

    std::string needle = namePart;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });

    // Dieselbe bereits mehrfach bestaetigte Quelle wie FindNearestLivingDungeonBoss()/
    // FindNearestAggroableTrash() oben - GetCreatureBySpawnIdStore() ist nach creature.guid (der
    // DB-Spawn-Id) indiziert, genau der Wert, den '.bottest attack/loot/questaccept/questturnin'
    // erwarten. Nur AKTUELL GELADENE Grids werden gefunden - kein direkter SQL-Zugriff, bewusst so
    // (siehe BotMgr.h-Kommentar bei FindNpcSpawnsByName()).
    for (auto const& pair : center->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* creature = pair.second;
        if (!creature || !creature->IsInWorld())
            continue;

        std::string name = creature->GetName();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name.find(needle) == std::string::npos)
            continue;

        float dist = center->GetDistance(creature);
        if (dist > radius)
            continue;

        std::ostringstream line;
        line << "spawnGuid=" << pair.first << " entry=" << creature->GetEntry() << " name='"
             << creature->GetName() << "' " << (creature->IsAlive() ? "lebt" : "tot") << " dist="
             << std::fixed << std::setprecision(1) << dist << "y pos=" << creature->GetPosition().ToString();
        results.push_back(line.str());
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::string BotMgr::DiagnoseSpecRotations() const
{
    std::ostringstream out;
    uint32 specsWithIssues = 0;

    for (BotSpecRotation const& rotation : g_BotSpecRotations)
    {
        // GetOrResolveSpecRotation() loest nur EINMAL PRO PROZESS auf (ResolvedOnce-Flag) - ein
        // erneuter Aufruf hier ist also immer billig, auch wenn diese Diagnose mehrfach laeuft.
        GetOrResolveSpecRotation(rotation.SpecId);

        std::vector<std::string> failures;
        for (BotRotationStep const& step : rotation.Priority)
        {
            if (!step.ResolvedSpellId)
                failures.push_back(std::string(step.SpellName) + " (Prioritaetsschritt)");
            if (step.ConditionAuxSpellName && !step.ResolvedAuxSpellId)
                failures.push_back(std::string(step.ConditionAuxSpellName) + " (Bedingungs-Aura)");
        }
        if (rotation.InterruptSpellName && !rotation.ResolvedInterruptSpellId)
            failures.push_back(std::string(rotation.InterruptSpellName) + " (Interrupt)");
        if (rotation.DispelSpellName && !rotation.ResolvedDispelSpellId)
            failures.push_back(std::string(rotation.DispelSpellName) + " (Dispel)");

        if (failures.empty())
            continue;

        ++specsWithIssues;
        out << "specId " << rotation.SpecId << ": " << failures.size() << " ungeloeste(r) Name(n): ";
        for (size_t i = 0; i < failures.size(); ++i)
        {
            if (i)
                out << ", ";
            out << failures[i];
        }
        out << "\n";
    }

    if (specsWithIssues == 0)
        out << "Alle " << g_BotSpecRotations.size() << " Skillungen: saemtliche Faehigkeits-/Aura-/"
            "Interrupt-/Dispel-Namen erfolgreich gegen das aktuell geladene Spell.db2 aufgeloest.";
    else
        out << specsWithIssues << " von " << g_BotSpecRotations.size() << " Skillungen haben mindestens "
            "einen ungeloesten Namen (siehe oben) - mit '.lookup spell <Name>' pruefen und den Namen im "
            "betroffenen g_BotSpecRotations-Eintrag in BotMgr.cpp korrigieren.";

    return out.str();
}

bool BotMgr::SetDungeonClearMode(uint32 accountId, bool enable)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SetDungeonClearMode: keine Bot-Session fuer Account %u vorhanden.",
            accountId);
        return false;
    }

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SetDungeonClearMode: Account %u hat aktuell keinen Player in der "
            "Welt (erst '.bottest login' ausfuehren).", accountId);
        return false;
    }

    if (enable && !bot->GetMap()->IsDungeon())
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::SetDungeonClearMode: Account %u steht nicht auf einer Dungeon-Karte "
            "(Map::IsDungeon()==false) - Modus wird nicht aktiviert (erst per '.bottest teleport' in eine "
            "Instanz bringen).", accountId);
        return false;
    }

    itr->second.DungeonClearActive = enable;
    itr->second.DungeonClearTickAccumMs = 0;

    TC_LOG_INFO("scripts.bots", "BotMgr::SetDungeonClearMode: Account %u - Dungeon-Clear-Modus %s.", accountId,
        enable ? "AKTIVIERT" : "deaktiviert");
    return true;
}

bool BotMgr::IsDungeonClearModeActive(uint32 accountId) const
{
    auto itr = _botSessions.find(accountId);
    return itr != _botSessions.end() && itr->second.DungeonClearActive;
}

uint32 BotMgr::GetBotAccountIdByGuid(ObjectGuid guid) const
{
    // Siehe BotMgr.h-Kommentar - derselbe lineare Scan/dieselbe Groessenordnung wie IsBotPlayerGuid().
    for (auto const& [accountId, entry] : _botSessions)
    {
        if (entry.Session && entry.Session->GetPlayer() && entry.Session->GetPlayer()->GetGUID() == guid)
            return accountId;
    }
    return 0;
}

uint32 BotMgr::SetDungeonClearModeForPlayerGroup(Player* requester, bool enable)
{
    if (!requester)
        return 0;

    Group* group = requester->GetGroup();
    if (!group)
        return 0;

    uint32 toggledCount = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !IsBotPlayerGuid(member->GetGUID()))
            continue;

        uint32 accountId = GetBotAccountIdByGuid(member->GetGUID());
        if (accountId && SetDungeonClearMode(accountId, enable))
            ++toggledCount;
    }

    TC_LOG_INFO("scripts.bots", "BotMgr::SetDungeonClearModeForPlayerGroup: Spieler %s hat den Dungeon-Clear-"
        "Modus fuer %u Bot(s) der eigenen Gruppe %s.", requester->GetName().c_str(), toggledCount,
        enable ? "AKTIVIERT" : "deaktiviert");
    return toggledCount;
}

uint32 BotMgr::CountActiveDungeonClearBotsInGroup(Player* player) const
{
    if (!player)
        return 0;

    Group* group = player->GetGroup();
    if (!group)
        return 0;

    uint32 activeCount = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !IsBotPlayerGuid(member->GetGUID()))
            continue;

        uint32 accountId = GetBotAccountIdByGuid(member->GetGUID());
        if (accountId && IsDungeonClearModeActive(accountId))
            ++activeCount;
    }
    return activeCount;
}

void BotMgr::ProcessDungeonClear(uint32 accountId, uint32 diff)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session || !itr->second.DungeonClearActive)
        return;

    itr->second.DungeonClearTickAccumMs += diff;
    if (itr->second.DungeonClearTickAccumMs < BOT_DUNGEON_CLEAR_TICK_MS)
        return;
    itr->second.DungeonClearTickAccumMs = 0;

    Player* bot = itr->second.Session->GetPlayer();
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return;

    // Kampf laeuft bereits (gegen Boss ODER Trash) - ProcessBotCombatAI() bzw. der ganz normale
    // Auto-Attack-Zyklus uebernehmen, Dungeon-Clear greift erst wieder ein, sobald der Bot nicht mehr
    // kaempft.
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false))
        return;

    if (Creature* corpse = FindNearestLootableCorpse(bot, BOT_DUNGEON_CLEAR_LOOT_RADIUS))
    {
        BotLootTarget(accountId, corpse->GetSpawnId());
        return; // ein Schritt pro Tick - naechster Schritt (Trash/Boss-Suche) beim naechsten Tick
    }

    // Trash auf dem Weg hat Vorrang vor dem Weiterlaufen zum Boss - "auf dem Weg toeten", nicht dran
    // vorbeilaufen und im Ruecken stehen lassen.
    if (Creature* trash = FindNearestAggroableTrash(bot, BOT_DUNGEON_CLEAR_TRASH_AGGRO_RADIUS))
    {
        bot->Attack(trash, true);
        bot->GetMotionMaster()->MoveChase(trash);
        TC_LOG_INFO("scripts.bots", "BotMgr::ProcessDungeonClear: Account %u engagiert Trash '%s' auf dem Weg "
            "zum Boss.", accountId, trash->GetName().c_str());
        return;
    }

    Creature* boss = FindNearestLivingDungeonBoss(bot);
    if (!boss)
    {
        TC_LOG_INFO("scripts.bots", "BotMgr::ProcessDungeonClear: Account %u - kein lebender Dungeon-Boss mehr "
            "auf dieser Karte gefunden (Instanz vermutlich clear) - Dungeon-Clear-Modus wird automatisch "
            "beendet.", accountId);
        itr->second.DungeonClearActive = false;
        return;
    }

    if (bot->GetDistance(boss) <= BOT_DUNGEON_CLEAR_ARRIVAL_DISTANCE)
    {
        // Am Boss angekommen, aber (noch) nicht im Kampf (z.B. weil der Encounter erst durch aktives
        // Angreifen ausgeloest wird) - aktiv angreifen statt daneben stehen zu bleiben.
        bot->Attack(boss, true);
        bot->GetMotionMaster()->MoveChase(boss);
        TC_LOG_INFO("scripts.bots", "BotMgr::ProcessDungeonClear: Account %u am Boss '%s' angekommen, engagiert.",
            accountId, boss->GetName().c_str());
        return;
    }

    // Navmesh-Routing zum naechsten Boss - KEINE Wegpunkte, dieselbe bereits bestaetigte
    // generatePath=true-Logik wie MoveBotTestStepPath()/Runde U. Nur neu ansetzen, wenn der Bot gerade
    // NICHT schon unterwegs ist (movespline->Finalized()) - verhindert, dass ein laufender Pfad jede
    // Sekunde neu berechnet/unterbrochen wird (dieselbe Konvention wie der Patrol-Fortschritt in Tick()).
    if (bot->movespline->Finalized())
    {
        bot->GetMotionMaster()->MovePoint(0, boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ(),
            /*generatePath*/ true);
        TC_LOG_INFO("scripts.bots", "BotMgr::ProcessDungeonClear: Account %u routet per Navmesh zu Boss '%s' "
            "(Distanz %.1f).", accountId, boss->GetName().c_str(), bot->GetDistance(boss));
    }
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
