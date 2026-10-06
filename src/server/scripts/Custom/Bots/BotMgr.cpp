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
#include "Config.h"
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
#include "AreaTrigger.h"
#include "AreaTriggerTemplate.h"
#include "ThreatManager.h"
#include "Util.h"
#include "QuestDef.h"
#include "DynamicObject.h"
#include "SpellAuras.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include <cctype>
#include <map>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <vector>
#include <unordered_set>
#include <set>
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
    // OI-051: gueltiges Aussehen suchen (Todesritter/Daemonenjaeger lehnt der Server mit 0/0/0 ab)
    uint8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    std::array<uint8, PLAYER_CUSTOM_DISPLAY_SIZE> customDisplay = { };
    if (!Player::FindValidAppearance(race, charClass, sex, skin, face, hairStyle, hairColor, facialHair, customDisplay))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestCreateBotCharacter: fuer Volk %u/Klasse %u/Geschlecht %u wurde kein "
            "gueltiges Aussehen gefunden - versuche 0/0/0 (Server wird die Anlage vermutlich ablehnen).", uint32(race), uint32(charClass), uint32(sex));
        skin = face = hairStyle = hairColor = facialHair = 0;
        customDisplay = { };
    }
    auto createInfo = std::make_shared<WorldPackets::Character::CharacterCreateInfo>(
        normalizedName, race, charClass, sex,
        skin, face, hairStyle, hairColor, facialHair, /*outfitId*/ 0);
    createInfo->CustomDisplay = customDisplay;

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

bool BotMgr::RequestBotLoginExistingAccount(uint32 accountId)
{
    std::string accountName;
    if (accountId == 0 || !AccountMgr::GetName(accountId, accountName))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::RequestBotLoginExistingAccount: Account %u existiert nicht.", accountId);
        return false;
    }

    BotSessionEntry& entry = _botSessions[accountId];
    if (!entry.Session)
    {
        entry.Session = CreateBotWorldSession(accountId, accountName);
        entry.State = BotCharacterState::STATE_UNINITIALIZED;
    }
    return RequestBotLogin(accountId);
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

void BotMgr::LoadConfig()
{
    bool wasEnabled = _moduleEnabled;
    _moduleEnabled = sConfigMgr->GetBoolDefault("Playerbots.Enable", true);
    _combatDebug = sConfigMgr->GetBoolDefault("Playerbots.Debug.Combat", false);

    if (wasEnabled != _moduleEnabled)
        TC_LOG_INFO("scripts.bots", "BotMgr::LoadConfig: Playerbots.Enable=%d - Tick()-Heartbeat wird ab "
            "sofort %s (bereits eingeloggte Bots %s, '.bottest ...'-Befehle bleiben unabhaengig davon "
            "nutzbar).", _moduleEnabled, _moduleEnabled ? "fortgesetzt" : "uebersprungen",
            _moduleEnabled ? "laufen normal weiter" : "frieren ein, kein Logout/Datenverlust");

    // --- OI-020: Autostart nach Serverneustart ---------------------------------------------------
    // Playerbots.Autostart.Enable (Default 0), .AccountIds (Liste "30,31,40-60"; Bereiche "a-b" erlaubt),
    // .PerSecond (Logins pro Sekunde, Default 2, 1-50), .DelaySeconds (Wartezeit nach Serverstart, Default 15).
    // Die Warteschlange wird nur EINMAL pro Prozess gefuellt (nicht bei '.reload config').
    _autostartPerSecond = uint32(std::max<int32>(1, std::min<int32>(50, sConfigMgr->GetIntDefault("Playerbots.Autostart.PerSecond", 2))));
    _autostartDelayMs = uint32(std::max<int32>(0, std::min<int32>(3600, sConfigMgr->GetIntDefault("Playerbots.Autostart.DelaySeconds", 15)))) * 1000;
    if (!_autostartQueued && sConfigMgr->GetBoolDefault("Playerbots.Autostart.Enable", false))
    {
        _autostartQueued = true;
        std::string const list = sConfigMgr->GetStringDefault("Playerbots.Autostart.AccountIds", "");
        std::stringstream ss(list);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
            if (token.empty())
                continue;
            uint32 from = 0, to = 0;
            std::string::size_type const dash = token.find('-');
            if (dash == std::string::npos)
                from = to = uint32(std::strtoul(token.c_str(), nullptr, 10));
            else
            {
                from = uint32(std::strtoul(token.substr(0, dash).c_str(), nullptr, 10));
                to = uint32(std::strtoul(token.substr(dash + 1).c_str(), nullptr, 10));
            }
            if (from == 0 || to < from || to - from > 5000)
            {
                TC_LOG_ERROR("scripts.bots", "BotMgr::LoadConfig: Autostart.AccountIds-Eintrag '%s' ungueltig - uebersprungen.", token.c_str());
                continue;
            }
            for (uint32 id = from; id <= to; ++id)
                if (std::find(_autostartQueue.begin(), _autostartQueue.end(), id) == _autostartQueue.end())
                    _autostartQueue.push_back(id);
        }
        TC_LOG_INFO("scripts.bots", "BotMgr::LoadConfig: Autostart aktiv - %u Bot-Accounts in der Warteschlange, "
            "%u pro Sekunde, Start nach %u s.", uint32(_autostartQueue.size()), _autostartPerSecond,
            _autostartDelayMs / 1000);
    }
}

void BotMgr::Tick(uint32 diff)
{
    // OI-020: gestaffelter Autostart - nach der Startverzoegerung pro Sekunde _autostartPerSecond Logins
    // ausloesen (CharEnum -> Login laeuft danach wie bei '.bottest login' unten in der Schleife).
    if (_autostartNext < _autostartQueue.size())
    {
        _autostartAccumMs += diff;
        if (_autostartAccumMs >= _autostartDelayMs + 1000)
        {
            _autostartAccumMs -= 1000;
            for (uint32 i = 0; i < _autostartPerSecond && _autostartNext < _autostartQueue.size(); ++i)
            {
                uint32 const accountId = _autostartQueue[_autostartNext++];
                if (GetBotPlayer(accountId))
                    continue; // bereits online (z. B. per .bottest)
                if (RequestBotLoginExistingAccount(accountId))
                    ++_autostartStartedLogins;
            }
            if (_autostartNext >= _autostartQueue.size())
                TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Autostart abgeschlossen - %u Logins ausgeloest (von %u Accounts).",
                    _autostartStartedLogins, uint32(_autostartQueue.size()));
        }
    }

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

                // OI-051: Todesritter starten in Map 609 (Ebon Hold, Startszenario); ein Bot ohne Client kommt dort nie
                // vollstaendig in die Welt (Player ist nach dem Login nicht IsInWorld). Vor dem Login auf die
                // Hauptstadt der Fraktion umsetzen (nur beim allerersten Login, solange die Position noch auf 609 steht).
                if (QueryResult startMap = CharacterDatabase.PQuery("SELECT map, race FROM characters WHERE guid = %u",
                    uint32(charGuid.GetCounter())))
                {
                    Field* sf = (*startMap).Fetch();
                    if (sf[0].GetUInt16() == 609)
                    {
                        bool const alliance = Player::TeamForRace(sf[1].GetUInt8()) == ALLIANCE;
                        CharacterDatabase.DirectPExecute("UPDATE characters SET map = %u, zone = %u, position_x = %f, position_y = %f, "
                            "position_z = %f, orientation = %f WHERE guid = %u",
                            alliance ? 0u : 1u, alliance ? 1519u : 1637u, alliance ? -8842.09f : 1629.36f,
                            alliance ? 626.358f : -4373.39f, alliance ? 94.0866f : 31.2564f, alliance ? 3.61363f : 3.54839f,
                            uint32(charGuid.GetCounter()));
                        TC_LOG_INFO("scripts.bots", "BotMgr::Tick: Account %u - Todesritter-Startposition (Map 609) vor dem Login auf %s gesetzt.",
                            accountId, alliance ? "Sturmwind" : "Orgrimmar");
                    }
                }

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
            ProcessBotGroupInstance(accountId, diff);

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

    // OI-017 (03.10.2026, Bot-Test): Bei einem Teleport auf DIESELBE Map wartet der Core auf das
    // MSG_MOVE_TELEPORT_ACK des Clients, bevor die Position uebernommen wird - ein Bot sendet das nie, der
    // Bot blieb stehen, obwohl "ZIEL ERREICHT" gemeldet wurde (Check verglich nur die Map-Id). Hier den
    // Nah-Teleport selbst abschliessen.
    if (sourceMapId == mapId && player->IsInWorld() && player->IsBeingTeleportedNear())
    {
        player->SetSemaphoreTeleportNear(false);
        player->UpdatePosition(x, y, z, orientation, true);
        TC_LOG_INFO("scripts.bots", "BotMgr::TeleportBot: Account %u - Nah-Teleport (gleiche Map) manuell abgeschlossen.", accountId);
    }

    // Map::AddPlayerToMap() selbst liegt in MovementHandler.cpp (andere Compilation-Unit) und
    // gibt BotMgr keinen direkten Rueckgabewert - als Ersatzindikator (siehe lcf2r90-Empfehlung
    // Punkt 4 und BotMgr.h-Kommentar) wird hier player->GetMapId() gegen das gewuenschte Ziel
    // verglichen: weicht die tatsaechliche Map vom Ziel ab (z.B. weil einer der beiden
    // Homebind-Fallback-Pfade aus lcf2r90, Zeilen 88-93/114-121, gegriffen hat), ist das ein
    // starkes Indiz fuer einen fehlgeschlagenen Kartenwechsel statt eines erfolgreichen.
    bool arrivedAtTarget = player->IsInWorld() && player->GetMapId() == mapId
        && (sourceMapId != mapId || (std::fabs(player->GetPositionX() - x) < 5.0f && std::fabs(player->GetPositionY() - y) < 5.0f));
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
    // OI-051: Bewerber werden gegen den Bot geprueft (Player::CanUseItem: Klasse, Volk, Stufe, Fertigkeit),
    // sonst landeten Items mit Klassen-/Stufenbeschraenkung ("CANT_EQUIP_EVER"/"LEVEL") im Rucksack. Der
    // Pool-Band reicht ueber mehrere Stufen (z. B. 20-49): zusaetzlich nur Items bis zur Stufe des Bots.
    // Ziel-Itemlevel fuer die laufende Provisionierung (0 = aus): gesetzt/zurueckgesetzt von
    // BotMgr::ProvisionBot() (Weltserver-Update ist single-threaded). Mit Ziel-Ilvl werden nur Items
    // >= Ziel gewaehlt, ueber ALLE Qualitaetsstufen (4 -> 1); gibt es keine, faellt die Wahl auf die
    // normale Pool-Logik zurueck.
    uint16 g_botPoolMinIlvl = 0;

    uint32 PickPoolItemWithFallback(Player const* player, uint8 band, uint8 startTier, uint8 itemClass,
        std::vector<uint8> const& subclasses, std::vector<uint8> const& invTypes)
    {
        if (subclasses.empty() || invTypes.empty())
            return 0;

        std::string subIn = BuildInClauseU8(subclasses);
        std::string invIn = BuildInClauseU8(invTypes);

        for (int pass = 0; pass < 2; ++pass)
        {
            uint32 const minIlvl = (pass == 0) ? uint32(g_botPoolMinIlvl) : 0;
            if (pass == 1 && g_botPoolMinIlvl == 0)
                break; // ohne Ziel-Ilvl gibt es keinen zweiten Durchlauf
            for (int tier = (minIlvl ? 4 : int(startTier)); tier >= 1; --tier)
            {
                QueryResult result = WorldDatabase.PQuery(
                    "SELECT item_entry FROM bot_equipment_pool WHERE level_band = %u AND pool_quality = %u "
                    "AND item_class = %u AND item_subclass IN (%s) AND inventory_type IN (%s) "
                    "AND required_level <= %u AND item_level >= %u ORDER BY RAND() LIMIT 40",
                    uint32(band), uint32(tier), uint32(itemClass), subIn.c_str(), invIn.c_str(),
                    uint32(player->getLevel()), minIlvl);
                if (!result)
                    continue;
                do
                {
                    uint32 entry = (*result)[0].GetUInt32();
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                    if (proto && player->CanUseItem(proto) == EQUIP_ERR_OK)
                        return entry;
                } while (result->NextRow());
            }
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
        uint32 entry = PickPoolItemWithFallback(player, band, tier, itemClass, subclasses, invTypes);
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
        mainHandEntry = PickPoolItemWithFallback(player, band, tier, 2, twoHandSub, {17});
    if (!mainHandEntry && !oneHandSub.empty())
        mainHandEntry = PickPoolItemWithFallback(player, band, tier, 2, oneHandSub, {13, 15, 21, 26});
    if (!mainHandEntry && !twoHandSub.empty())
        mainHandEntry = PickPoolItemWithFallback(player, band, tier, 2, twoHandSub, {17});

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
            offhandEntry = PickPoolItemWithFallback(player, band, tier, 4, {6}, {14});
            offhandLabel = "Offhand(Schild)";
        }
        if (!offhandEntry && BotClassCanDualWieldWeapon(cls) && !oneHandSub.empty())
        {
            offhandEntry = PickPoolItemWithFallback(player, band, tier, 2, oneHandSub, {22});
            offhandLabel = "Offhand(Waffe)";
        }
        if (!offhandEntry && BotClassCanUseShield(cls))
        {
            offhandEntry = PickPoolItemWithFallback(player, band, tier, 4, {6}, {14});
            offhandLabel = "Offhand(Schild)";
        }
        if (!offhandEntry && BotClassCanUseHoldable(cls))
        {
            offhandEntry = PickPoolItemWithFallback(player, band, tier, 4, {0}, {23});
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
            return !_lfgTestRealAccounts.count(accountId); // Test-Konten gelten als echte Spieler
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
            // OI-023: LFR-Fluegel bedient HandleLfrDemand() (vorgebaute Raidgruppe), nicht der Einzel-Fueller
            if (LFGDungeonData const* lfrCheck = sLFGMgr->GetLFGDungeon(queueId))
                if (lfrCheck->subtype == LFG_SUBTYPE_LFR)
                    continue;
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

                // Explizit qualifiziert (nicht nur "LfgQueueRoleCount"): LFGMgr.h deklariert im GLOBALEN
                // Namespace zusaetzlich ein unabhaengiges, nie definiertes "struct LfgQueueRoleCount;"
                // (Vorwaertsdeklaration, vermutlich fuer einen anderen/aelteren Zweck) - das echte, in
                // LFGQueue.h definierte Struct liegt in namespace lfg. Mit der obigen "using namespace
                // lfg;" sind beide Namen im selben effektiven Suchbereich sichtbar, was MSVC (C2872) als
                // mehrdeutig ablehnt; die explizite Qualifikation entfernt die Mehrdeutigkeit eindeutig
                // zugunsten des echten, vollstaendigen Typs.
                lfg::LfgQueueRoleCount const roleCount = LFGMgr::GetRoleCountByQueueId(queueId);
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
        if (botPlayer && !botPlayer->IsInWorld() && botPlayer->IsBeingTeleportedFar())
        {
            // Fern-Teleport (LFG-Dungeon) wurde von einem anderen Bot im selben Tick ausgeloest: der Spieler ist waehrenddessen nicht in
            // der Welt und wuerde ohne Client nie MSG_MOVE_WORLDPORT_ACK senden - hier abschliessen, Eintrag bleibt fuer die Zustandspruefung.
            itr->second.Session->HandleMoveWorldportAck();
            continue;
        }
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

        if (state == LFG_STATE_DUNGEON)
        {
            // Der Core teleportiert nur NEU hinzugekommene Spieler; bereits gruppierte Mitglieder fordern den Teleport sonst per
            // CMSG_LFG_TELEPORT (Schaltflaeche "Dungeon betreten") an - ein Bot hat keinen Client, also hier nachholen.
            Group* lfgGroup = botPlayer->GetGroup();
            LFGDungeonData const* lfgDungeon = lfgGroup ? sLFGMgr->GetLFGDungeon(sLFGMgr->GetDungeon(lfgGroup->GetGUID())) : nullptr;
            if (lfgDungeon && botPlayer->GetMapId() != uint32(lfgDungeon->map) && !botPlayer->IsBeingTeleported())
            {
                sLFGMgr->TeleportPlayer(botPlayer, false, true);
                TC_LOG_INFO("scripts.bots", "BotMgr::AdvanceLfgFillerBots: Account %u - LFG-Teleport in Map %u nachgeholt (IsBeingTeleportedFar=%u).",
                    accountId, uint32(lfgDungeon->map), uint32(botPlayer->IsBeingTeleportedFar()));
                if (botPlayer->IsBeingTeleportedFar())
                    itr->second.Session->HandleMoveWorldportAck();
            }
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
    HandleLfrDemand();
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
        // Bestial-Wrath-Kernschleife, Cobra Shot als Fokus-Fuellschlag. KORREKTUR (Livetest-Fund Runde
        // 7, '.bottest diagspells' meldete "Barbed Shot" als in diesem Build nicht auffindbar): "Barbed
        // Shot" ist eine Battle-for-Azeroth-Faehigkeit (Patch 8.0), existiert in Legion 7.3.5 schlicht
        // noch nicht - die urspruengliche Recherche hatte hier faelschlich eine spaetere Patch-Version
        // uebernommen (genau der Fehler, den die "nur 7.3.5-verifizierte Namen"-Regel verhindern soll).
        // Ersatzlos entfernt statt durch eine geratene 7.3.5-Alternative ersetzt.
        {
            253, SPELLFAMILY_HUNTER, BotRole::RangedDps,
            {
                { "Kill Command", BotRotationCondition::Always },
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
        // OFFENER LIVETEST-FUND (Runde 7): '.bottest diagspells' meldete "Wildfire Bomb" als in diesem
        // Build nicht aufloesbar (weder unter SPELLFAMILY_HUNTER noch ueber den neuen klassenuebergreifenden
        // Fallback in ResolveSpellIdByName()). Anders als bei "Barbed Shot" (Hunter BM, oben) ist NICHT
        // sicher genug bekannt, ob "Wildfire Bomb" in 7.3.5 schlicht noch nicht existiert (wie Barbed
        // Shot) oder nur anders benannt ist - deshalb bewusst NICHT geraten/ersetzt. Vor Live-Einsatz mit
        // '.lookup spell wildfire' bzw. '.lookup spell bomb' pruefen und diese Zeile entsprechend
        // korrigieren; bis dahin bleibt der Schritt inaktiv, der Rest der Rotation (Raptor Strike/Serpent
        // Sting) funktioniert unveraendert weiter.
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

namespace
{
    // Ein einzelner Scan-Durchlauf ueber sSpellMgr, optional auf eine SpellFamilyName gefiltert (siehe
    // BotMgr::ResolveSpellIdByName() unten fuer die beiden Aufrufer dieser Funktion: der gefilterte
    // Haupt-Durchlauf und der ungefilterte Fallback-Durchlauf). Unter mehreren exakten Namenstreffern
    // wird die HOECHSTE Spell-Id zurueckgegeben (siehe Kommentar im Aufrufer fuer die Begruendung -
    // Livetest-Fund Runde 7: alte Rang-Duplikate aus Classic-Cata haben durchgehend NIEDRIGERE Ids als
    // die aktuelle 7.3.5-Version derselben Faehigkeit).
    uint32 ScanSpellDb2ByName(std::wstring const& wantedLower, int64 spellFamilyFilter, uint32& outMatchCount)
    {
        uint32 found = 0;
        outMatchCount = 0;
        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(id);
            if (!spellInfo)
                continue;
            if (spellFamilyFilter >= 0 && int64(spellInfo->SpellFamilyName) != spellFamilyFilter)
                continue;
            if (!spellInfo->SpellName || !spellInfo->SpellName->Str[LOCALE_enUS])
                continue;

            std::wstring candidate;
            Utf8toWStr(spellInfo->SpellName->Str[LOCALE_enUS], candidate);
            wstrToLower(candidate);
            if (candidate == wantedLower)
            {
                found = id;
                ++outMatchCount;
            }
        }
        return found;
    }

    // OI-017 (03.10.2026, Bot-Test): Der Namens-Resolver waehlt bei mehrdeutigen Namen die HOECHSTE Id
    // (z. B. Frostbolt 228597), die Level-1-Bots kennen aber eine ANDERE Id desselben Namens (Frostbolt 116,
    // Slam 1464, Cobra Shot 193455): player->HasSpell(HoechsteId) war false, jeder Rotationsschritt wurde
    // uebersprungen, kein Bot hat im Kampf gezaubert. Loesung: pro Spieler aus ALLEN Namenstreffern die Id
    // waehlen, die der Spieler tatsaechlich kennt. Die Kandidatenlisten werden pro (Familie, Name) einmal
    // aus dem Spell.db2 gebaut und gecacht.
    uint32 FindKnownSpellIdByName(Player const* player, std::string const& englishName, uint32 spellFamily, uint32 preferredId)
    {
        if (preferredId && player->HasSpell(preferredId))
            return preferredId;

        static std::unordered_map<std::string, std::vector<uint32>> candidateCache;
        std::string const cacheKey = std::to_string(spellFamily) + ":" + englishName;
        auto itr = candidateCache.find(cacheKey);
        if (itr == candidateCache.end())
        {
            std::wstring wanted;
            Utf8toWStr(englishName, wanted);
            wstrToLower(wanted);

            std::vector<uint32> candidates;
            // OI-023: ALLE Namenstreffer sammeln (nicht nur die der Klassenfamilie) - z.B. Agony 980 hat nicht die Familie
            // des Warlock-Treffers 231792; die Auswahl unten nimmt ohnehin nur eine vom Spieler gekannte Id.
            for (int pass = 1; pass < 2 && candidates.empty(); ++pass)
            {
                for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
                {
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(id);
                    if (!spellInfo || !spellInfo->SpellName || !spellInfo->SpellName->Str[LOCALE_enUS])
                        continue;
                    if (pass == 0 && spellInfo->SpellFamilyName != spellFamily)
                        continue;
                    std::wstring candidate;
                    Utf8toWStr(spellInfo->SpellName->Str[LOCALE_enUS], candidate);
                    wstrToLower(candidate);
                    if (candidate == wanted)
                        candidates.push_back(id);
                }
            }
            itr = candidateCache.emplace(cacheKey, std::move(candidates)).first;
        }

        // Hoechste bekannte Id zuerst (aktuellste Version), sonst irgendeine bekannte.
        uint32 best = 0;
        for (uint32 id : itr->second)
            if (id > best && player->HasSpell(id))
                best = id;
        return best;
    }
}

uint32 BotMgr::ResolveSpellIdByName(std::string const& englishName, uint32 spellFamily) const
{
    // Siehe voller Begruendung im BotMgr.h-Kopfkommentar ("Kampf-KI") und bei g_BotSpecRotations
    // oben: numerische Spell-IDs sind ueber Patches hinweg NICHT stabil genug, um sie aus einer
    // Web-Recherche zu uebernehmen. Stattdessen wird hier - nach demselben Muster wie das bereits
    // existierende GM-Kommando '.lookup spell' (cs_lookup.cpp) - das TATSAECHLICH auf diesem Server
    // geladene Spell.db2 (Build 26972) nach einem EXAKTEN, gross-/kleinschreibungsunabhaengigen
    // Namens-Treffer durchsucht, zuerst auf spellFamily gefiltert (verhindert Kollisionen mit
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

    uint32 matchCount = 0;
    uint32 found = ScanSpellDb2ByName(wanted, int64(spellFamily), matchCount);

    // Livetest-Fund (Runde 7, siehe README-Commands.md/PR): Spell.db2 enthaelt fuer die meisten
    // Grundfaehigkeiten mehrere exakte Namenstreffer - die alten Rang-Duplikate aus Classic bis Cata
    // (z.B. "Frostbolt" existiert als Rank-1-Vanilla-Spell UND als aktuelle Legion-Version, gleicher
    // Name, gleiche SpellFamilyName). Der urspruengliche "ersten Treffer nehmen"-Ansatz waehlte dadurch
    // fast immer die NIEDRIGSTE/AELTESTE Id (Ids steigen historisch mit dem Einfuehrungspatch) statt
    // der aktuellen 7.3.5-Version - ScanSpellDb2ByName() liefert stattdessen die HOECHSTE der
    // gefundenen Ids (Blizzard fuegt bei einer Faehigkeits-Ueberarbeitung ueber Xpacs hinweg neue
    // Spell.db2-Eintraege hinzu, alte Rang-Eintraege behalten dauerhaft ihre urspruengliche niedrige
    // Id) - kein Ersatz fuer eine echte Verifikation (siehe '.bottest diagspells'/'.lookup spell'),
    // aber eine deutlich zuverlaessigere Standardannahme als "erster Treffer".

    if (matchCount == 0)
    {
        // Zweiter Livetest-Fund (Runde 7): einzelne, tatsaechlich existierende 7.3.5-Faehigkeiten/Auren
        // (u.a. Demon Hunters "Disrupt", Mage "Clearcasting"/"Heating Up", Warlock "Demonic Core")
        // loesten trotz korrektem Namen NICHT auf, vermutlich weil ihr SpellFamilyName-Feld in diesem
        // DB2-Build von der fuer die jeweilige Klasse erwarteten SpellFamilyNames-Konstante abweicht
        // (z.B. als SPELLFAMILY_GENERIC statt der Klassenfamilie klassifiziert). Fallback: wird beim
        // gefilterten Durchlauf NICHTS gefunden, wird ungefiltert (ueber ALLE Klassen) erneut gesucht -
        // GENAU EIN Treffer wird trotzdem uebernommen (Kollisionsschutz bleibt bestehen: liefert der
        // Fallback mehrere Treffer ueber verschiedene Klassen, ist der Name zu mehrdeutig fuer eine
        // automatische Entscheidung und es bleibt bei "nicht gefunden").
        uint32 fallbackMatchCount = 0;
        uint32 fallbackFound = ScanSpellDb2ByName(wanted, -1, fallbackMatchCount);
        if (fallbackMatchCount == 1)
        {
            found = fallbackFound;
            matchCount = 1;
            TC_LOG_INFO("scripts.bots", "BotMgr::ResolveSpellIdByName: '%s' wurde NICHT unter "
                "SpellFamilyName %u gefunden, aber eindeutig (1 Treffer) ueber alle Klassen hinweg - "
                "verwende Spell-Id %u (tatsaechliche SpellFamilyName pruefen, falls das ueberrascht).",
                englishName.c_str(), spellFamily, found);
        }
    }

    if (matchCount == 0)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ResolveSpellIdByName: '%s' (SpellFamilyName %u) wurde in diesem "
            "Server-Spell.db2 NICHT gefunden (auch nicht klassenuebergreifend) - der zugehoerige "
            "Rotationsschritt bleibt dauerhaft inaktiv (kein Absturz). Moegliche Ursachen: Schreibweise "
            "weicht vom recherchierten 7.3.5-Namen ab, oder diese Faehigkeit heisst in Build 26972 anders "
            "(z.B. Talent-Umbenennung), oder sie existiert in diesem Patch schlicht noch nicht (spaeterer "
            "Xpac) - mit '.lookup spell %s' pruefen.", englishName.c_str(), spellFamily, englishName.c_str());
    }
    else if (matchCount > 1)
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::ResolveSpellIdByName: '%s' (SpellFamilyName %u) ist MEHRDEUTIG "
            "(%u exakte Treffer in Spell.db2, vermutlich alte Rang-Duplikate) - verwende Spell-Id %u "
            "(HOECHSTE der gefundenen Ids), das sollte manuell per '.lookup spell %s' verifiziert werden.",
            englishName.c_str(), spellFamily, matchCount, found, englishName.c_str());
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

namespace
{
    // Plan-Schritt 3: Spott je Klasse (Namen aus dem Spell.db2 per FindKnownSpellIdByName, keine geratenen IDs)
    bool BotTryTaunt(Player* bot, Unit* target)
    {
        if (!target || !target->IsAlive())
            return false;
        Unit* victim = target->GetVictim();
        if (!victim || victim == bot)
            return false;
        char const* name = nullptr;
        switch (bot->getClass())
        {
            case CLASS_WARRIOR: name = "Taunt"; break;
            case CLASS_PALADIN: name = "Hand of Reckoning"; break;
            case CLASS_DEATH_KNIGHT: name = "Dark Command"; break;
            case CLASS_DRUID: name = "Growl"; break;
            case CLASS_MONK: name = "Provoke"; break;
            case CLASS_DEMON_HUNTER: name = "Torment"; break;
            default: return false;
        }
        uint32 const spellId = FindKnownSpellIdByName(bot, name, 0, 0);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !bot->GetSpellHistory()->IsReady(info))
            return false;
        float const range = info->GetMaxRange(false, bot);
        if ((range > 0.0f && bot->GetDistance(target) > range) || !bot->IsWithinLOSInMap(target))
            return false;
        return bot->CastSpell(target, spellId, TRIGGERED_NONE);
    }

    // Plan-Schritt 6: Wiederbelebung eines toten Gruppenmitglieds ausserhalb des Kampfes
    bool BotTryResurrectMember(Player* bot)
    {
        char const* name = nullptr;
        switch (bot->getClass())
        {
            case CLASS_PRIEST: name = "Resurrection"; break;
            case CLASS_PALADIN: name = "Redemption"; break;
            case CLASS_SHAMAN: name = "Ancestral Spirit"; break;
            case CLASS_DRUID: name = "Revive"; break;
            case CLASS_MONK: name = "Resuscitate"; break;
            default: return false;
        }
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        uint32 const spellId = FindKnownSpellIdByName(bot, name, 0, 0);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !bot->GetSpellHistory()->IsReady(info))
            return false;
        for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
        {
            Player* member = gr->GetSource();
            if (!member || member == bot || member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() || member->IsResurrectRequested())
                continue;
            float const range = info->GetMaxRange(false, bot);
            if (bot->GetDistance(member) > (range > 0.0f ? range - 2.0f : 30.0f))
            {
                if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                    bot->GetMotionMaster()->MovePoint(0, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), true);
                return true;
            }
            return bot->CastSpell(member, spellId, TRIGGERED_NONE);
        }
        return false;
    }
}

// Plan-Schritt 3: Tank-Zielwahl - ein Gegner, der gerade ein anderes Gruppenmitglied (nicht den Tank) angreift
Unit* BotMgr::SelectBotTankTarget(Player* bot) const
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;
    Unit* best = nullptr;
    float bestDist = 40.0f;
    for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
    {
        Player* member = gr->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;
        for (Unit* attacker : member->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() == bot || !bot->IsValidAttackTarget(attacker))
                continue;
            float const dist = bot->GetDistance(attacker);
            if (dist < bestDist)
            {
                best = attacker;
                bestDist = dist;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------
// Boss-Playbook (05.10.2026, Konzept BOSS_PLAYBOOK_RECHERCHE.md, Tabellen bot_boss_* aus fixes\lcf2r173): datengetriebene Regeln je Boss
// fuer Add-Prioritaet/-Verhalten, Aufstellung und Ausweichen. Bosse ohne Eintrag behalten das generische Verhalten.
// Rollen-Bits: 1 Tank, 2 Heiler, 4 Nahkampf, 8 Fernkampf (= 1 << (BotRole - 1)).
// ---------------------------------------------------------------------------------------------------
namespace
{
    struct PbAdd { uint32 Add; uint8 Phase; uint8 Prio; uint8 RoleMask; uint8 Mode; };            // Mode: 0 KILL, 1 IGNORE, 2 AVOID, 3 TANK_ONLY
    struct PbPos { uint8 Phase; uint8 Role; uint8 Anchor; int16 Angle; float Dist; float Spread; }; // Anchor: 0 BOSS, 1 BOSS_FACING_BACK, 2 ROOM_EDGE_AWAY, 3 TANK
    struct PbAvoid { uint32 Spell; uint8 Kind; uint8 Phase; uint8 RoleMask; float Radius; };      // Kind: 0 AREATRIGGER, 1 AURA_SELF, 2 FRONTAL_CONE, 3 REAR_CONE, 4 CAST_AREA
    struct PbPhase { uint8 Phase; uint8 Trigger; int32 Value; uint32 Ref; };                      // Trigger: 0 START, 1 HP_BELOW, 2 POWER_ZERO, 3 AURA_ON_BOSS, 4 BOSS_PASSIVE, 5 ADD_DEAD_COUNT
    struct PbInterrupt { uint32 Caster; uint32 Spell; uint8 Prio; };                              // Caster 0 = beliebiger Wirker
    struct PbDispel { uint32 Aura; uint8 Prio; uint8 MinStacks; };
    struct PbTankSwap { uint32 Aura; uint8 Stacks; };
    struct Playbook
    {
        uint32 Map = 0;
        uint32 Flags = 0;
        std::vector<PbInterrupt> Interrupts;
        std::vector<PbDispel> Dispels;
        std::vector<PbTankSwap> TankSwaps;
        std::vector<PbAdd> Adds;
        std::vector<PbPos> Pos;
        std::vector<PbAvoid> Avoid;
        std::vector<PbPhase> Phases;
    };

    std::unordered_map<uint32, Playbook> g_playbooks; // boss entry -> Playbook
    bool g_playbooksLoaded = false;

    uint8 PbEnum(std::string const& v, std::initializer_list<char const*> names)
    {
        uint8 i = 0;
        for (char const* n : names)
        {
            if (v == n)
                return i;
            ++i;
        }
        return 0;
    }

    void LoadPlaybooks()
    {
        g_playbooksLoaded = true;
        g_playbooks.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, map, flags FROM bot_boss_playbook"))
            do
            {
                Field* f = r->Fetch();
                Playbook& p = g_playbooks[f[0].GetUInt32()];
                p.Map = f[1].GetUInt32();
                p.Flags = f[2].GetUInt32();
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, add_entry, phase, priority, role_mask, mode FROM bot_boss_add_priority"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Adds.push_back({ f[1].GetUInt32(), f[2].GetUInt8(), f[3].GetUInt8(), f[4].GetUInt8(), PbEnum(f[5].GetString(), { "KILL", "IGNORE", "AVOID", "TANK_ONLY" }) });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, phase, role, anchor, angle_deg, distance, spread FROM bot_boss_position"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Pos.push_back({ f[1].GetUInt8(), f[2].GetUInt8(), PbEnum(f[3].GetString(), { "BOSS", "BOSS_FACING_BACK", "ROOM_EDGE_AWAY", "TANK" }), f[4].GetInt16(), f[5].GetFloat(), f[6].GetFloat() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, spell_id, kind, phase, radius, role_mask FROM bot_boss_avoid"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Avoid.push_back({ f[1].GetUInt32(), PbEnum(f[2].GetString(), { "AREATRIGGER", "AURA_SELF", "FRONTAL_CONE", "REAR_CONE", "CAST_AREA" }), f[3].GetUInt8(), f[5].GetUInt8(), f[4].GetFloat() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, phase, trigger_type, trigger_value, trigger_ref FROM bot_boss_phase"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Phases.push_back({ f[1].GetUInt8(), PbEnum(f[2].GetString(), { "START", "HP_BELOW", "POWER_ZERO", "AURA_ON_BOSS", "BOSS_PASSIVE", "ADD_DEAD_COUNT" }), f[3].GetInt32(), f[4].GetUInt32() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, caster_entry, spell_id, priority FROM bot_boss_interrupt"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Interrupts.push_back({ f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetUInt8() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, aura_spell_id, priority, min_stacks FROM bot_boss_dispel"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.Dispels.push_back({ f[1].GetUInt32(), f[2].GetUInt8(), f[3].GetUInt8() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, aura_spell_id, swap_stacks FROM bot_boss_tankswap"))
            do
            {
                Field* f = r->Fetch();
                auto itr = g_playbooks.find(f[0].GetUInt32());
                if (itr != g_playbooks.end())
                    itr->second.TankSwaps.push_back({ f[1].GetUInt32(), f[2].GetUInt8() });
            } while (r->NextRow());
        TC_LOG_INFO("scripts.bots", "BotMgr: Boss-Playbook geladen: %u Boss-Eintraege.", uint32(g_playbooks.size()));
    }

    bool PbHasMap(uint32 mapId)
    {
        if (!g_playbooksLoaded)
            LoadPlaybooks();
        for (auto const& kv : g_playbooks)
            if (kv.second.Map == mapId)
                return true;
        return false;
    }

    // der Playbook-Boss der Karte, falls er lebt und kaempft (sonst nullptr)
    Creature* PbFindBoss(Player* bot, Playbook const*& outPb)
    {
        outPb = nullptr;
        if (!PbHasMap(bot->GetMapId()))
            return nullptr;
        for (auto const& kv : g_playbooks)
            if (kv.second.Map == bot->GetMapId())
                if (Creature* c = bot->FindNearestCreature(kv.first, 250.0f, true))
                    if (c->IsInCombat())
                    {
                        outPb = &kv.second;
                        return c;
                    }
        return nullptr;
    }

    uint8 PbPhaseOf(Creature* boss, Playbook const& pb)
    {
        uint8 fallback = 0;
        for (PbPhase const& ph : pb.Phases)
        {
            if (ph.Trigger == 3 && boss->HasAura(uint32(ph.Value)))
                return ph.Phase;
            if (ph.Trigger == 2 && boss->GetPowerType() != POWER_HEALTH && boss->GetPower(boss->GetPowerType()) == 0)     // POWER_ZERO
                return ph.Phase;
            if (ph.Trigger == 4 && (boss->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) || boss->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC))) // BOSS_PASSIVE
                return ph.Phase;
        }
        for (PbPhase const& ph : pb.Phases)
            if (ph.Trigger == 1 && boss->GetHealthPct() < float(ph.Value))
                fallback = std::max(fallback, ph.Phase);
        if (fallback)
            return fallback;
        for (PbPhase const& ph : pb.Phases)
            if (ph.Trigger == 5 && boss->FindNearestCreature(ph.Ref, 150.0f, true))
                return ph.Phase;
        for (PbPhase const& ph : pb.Phases)
            if (ph.Trigger == 0)
                fallback = ph.Phase;
        return fallback;
    }

    uint8 PbRoleBit(BotRole role) { return role == BotRole::Unknown ? 0 : uint8(1u << (uint8(role) - 1)); }

    // Add-Verhalten fuer ein Creature in dieser Phase; nullptr = kein Eintrag
    PbAdd const* PbAddRule(Playbook const& pb, uint32 entry, uint8 phase)
    {
        PbAdd const* best = nullptr;
        for (PbAdd const& a : pb.Adds)
            if (a.Add == entry && (a.Phase == 0 || a.Phase == phase) && (!best || a.Phase == phase))
                best = &a;
        return best;
    }

    // bestes Kampfziel fuer einen Schadensverteiler laut Playbook (Adds nach Prioritaet vor dem Boss, IGNORE/AVOID/TANK_ONLY nie)
    Unit* PbSelectTarget(Player* bot, uint8 roleBit, Creature* boss, Playbook const& pb, uint8 phase)
    {
        uint8 bossPrio = 10;
        if (PbAdd const* br = PbAddRule(pb, boss->GetEntry(), phase))
            bossPrio = br->Prio;
        Creature* best = nullptr;
        uint8 bestPrio = 255;
        std::set<uint32> done;
        for (PbAdd const& a : pb.Adds)
        {
            if (a.Mode != 0 || a.Add == boss->GetEntry() || !(a.RoleMask & roleBit) || (a.Phase != 0 && a.Phase != phase) || !done.insert(a.Add).second)
                continue;
            PbAdd const* rule = PbAddRule(pb, a.Add, phase);
            if (!rule || rule->Mode != 0 || !(rule->RoleMask & roleBit))
                continue;
            std::list<Creature*> found;
            bot->GetCreatureListWithEntryInGrid(found, a.Add, 100.0f);
            for (Creature* c : found)
            {
                if (!c->IsAlive() || !bot->IsValidAttackTarget(c))
                    continue;
                if (!best || rule->Prio < bestPrio || (rule->Prio == bestPrio && c->GetHealth() < best->GetHealth()))
                {
                    best = c;
                    bestPrio = rule->Prio;
                }
            }
        }
        if (best && bestPrio < bossPrio)
            return best;
        return bot->IsValidAttackTarget(boss) ? boss : nullptr;
    }

    // Zielposition laut Playbook-Zeile (nur Anker BOSS); Links/Rechts und Streuung aus der Bot-GUID
    bool PbSpot(Player* bot, Creature* boss, PbPos const& pp, float& x, float& y)
    {
        if (pp.Anchor != 0 || pp.Dist <= 0.0f)
            return false;
        uint64 const g = bot->GetGUID().GetCounter();
        uint32 const h = uint32(g * 2654435761u);
        float const side = (g & 1) ? 1.0f : -1.0f;
        float const jitterD = (float(h & 0xFF) / 255.0f - 0.5f) * 2.0f * pp.Spread;
        float const jitterA = (float((h >> 8) & 0xFF) / 255.0f - 0.5f) * 0.5f;
        float const ang = boss->GetOrientation() + side * (float(pp.Angle) * float(M_PI) / 180.0f) + jitterA;
        float const d = std::max(2.0f, pp.Dist + jitterD) + (pp.Role == 3 ? boss->GetCombatReach() : 0.0f);
        x = boss->GetPositionX() + std::cos(ang) * d;
        y = boss->GetPositionY() + std::sin(ang) * d;
        return true;
    }
}

Unit* BotMgr::SelectBotCombatTarget(Player* bot) const
{
    // Playbook-Boss auf dieser Karte: Schadensverteiler folgen den Playbook-Regeln (Add-Prioritaet, IGNORE/AVOID/TANK_ONLY)
    bool playbookMap = false;
    if (Map* pbMap = bot->GetMap(); pbMap && pbMap->IsRaid() && PbHasMap(bot->GetMapId()))
    {
        playbookMap = true;
        BotRole const role = GetBotRole(GetBotAccountIdByGuid(bot->GetGUID()));
        if (bot->IsInCombat() && (role == BotRole::MeleeDps || role == BotRole::RangedDps))
        {
            Playbook const* pb = nullptr;
            if (Creature* boss = PbFindBoss(bot, pb))
            {
                uint8 const phase = PbPhaseOf(boss, *pb);
                if (Unit* v = bot->GetVictim())
                    if (Creature* vc = v->ToCreature())
                        if (PbAdd const* vr = PbAddRule(*pb, vc->GetEntry(), phase))
                            if (vr->Mode != 0)
                                bot->AttackStop();
                if (Unit* t = PbSelectTarget(bot, PbRoleBit(role), boss, *pb, phase))
                {
                    if (bot->GetVictim() != t)
                    {
                        bot->Attack(t, true);
                        if (role == BotRole::MeleeDps && bot->GetDistance(t) > 7.0f)
                            bot->GetMotionMaster()->MoveChase(t);
                    }
                    return t;
                }
            }
        }
    }

    // Raid ohne Playbook: Schadensverteiler toeten kaempfende Adds (Nicht-Bosse) zuerst, am schwaechsten angeschlagenen zuerst; erst ohne Adds geht es auf den Boss.
    if (Map* map = bot->GetMap(); !playbookMap && map && map->IsRaid() && bot->IsInCombat())
        if (ChrSpecializationEntry const* spec = sChrSpecializationStore.LookupEntry(bot->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID)))
            if (spec->Role == 2)
                if (Group* group = bot->GetGroup())
                {
                    Creature* bestAdd = nullptr;
                    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* member = itr->GetSource();
                        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                            continue;
                        for (Unit* attacker : member->getAttackers())
                        {
                            Creature* c = attacker ? attacker->ToCreature() : nullptr;
                            if (!c || !c->IsAlive() || c->IsDungeonBoss() || c->GetMaxHealth() > 5000000 || c->IsPet() || c->IsTotem()
                                || bot->GetDistance(c) > 40.0f || !bot->IsValidAttackTarget(c))
                                continue;
                            if (!bestAdd || c->GetHealth() < bestAdd->GetHealth())
                                bestAdd = c;
                        }
                    }
                    if (bestAdd)
                    {
                        if (bot->GetVictim() != bestAdd)
                        {
                            bot->Attack(bestAdd, true);
                            if (bot->GetDistance(bestAdd) > 7.0f && spec->ClassID != CLASS_MAGE && spec->ClassID != CLASS_WARLOCK && spec->ClassID != CLASS_PRIEST)
                                bot->GetMotionMaster()->MoveChase(bestAdd);
                        }
                        return bestAdd;
                    }
                }

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

    // Verteidigung: im Kampf ohne eigenes Ziel (z. B. von einem Gegner angegriffen, den noch niemand der Gruppe als Opfer hat) den
    // naechsten Angreifer des Bots oder eines Gruppenmitglieds annehmen.
    if (bot->IsInCombat())
    {
        Unit* best = nullptr;
        float bestDist = 45.0f;
        auto consider = [&](Unit* attacker)
        {
            if (!attacker || !attacker->IsAlive() || !bot->IsValidAttackTarget(attacker))
                return;
            float const dist = bot->GetDistance(attacker);
            if (dist < bestDist)
            {
                best = attacker;
                bestDist = dist;
            }
        };
        for (Unit* attacker : bot->getAttackers())
            consider(attacker);
        if (!best)
            if (Group* group = bot->GetGroup())
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    if (Player* member = itr->GetSource())
                        if (member != bot && member->IsInWorld() && member->GetMapId() == bot->GetMapId())
                            for (Unit* attacker : member->getAttackers())
                                consider(attacker);
        if (best)
        {
            bot->Attack(best, true);
            bot->GetMotionMaster()->MoveChase(best);
            return best;
        }
    }

    return nullptr;
}

Unit* BotMgr::SelectBotHealTarget(Player* bot) const
{
    Unit* lowestMember = nullptr;
    float lowestPct = 100.0f;
    float lowestKey = 100.0f;

    auto consider = [&](Unit* candidate)
    {
        if (!candidate || !candidate->IsAlive() || candidate->GetMapId() != bot->GetMapId())
            return;
        float pct = candidate->GetHealthPct();
        // Plan-Schritt 6: Tanks werden bevorzugt (15 Prozentpunkte "Vorsprung" beim Vergleich)
        float key = pct;
        if (Player const* cp = candidate->ToPlayer())
            if (ChrSpecializationEntry const* cs = sChrSpecializationStore.LookupEntry(cp->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID)))
                if (cs->Role == 0)
                    key -= 15.0f;
        if (key < lowestKey)
        {
            lowestKey = key;
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

namespace
{
    // OI-023 Diagnose: warum wirkt ein Bot im Raid nicht? Je Bot hoechstens alle 10 s eine Zeile (nur Raid-Karten).
    void BotCombatDbg(Player* p, uint32 accountId, char const* reason, Unit* target)
    {
        static std::unordered_map<uint32, uint32> lastLog;
        if (!sBotMgr->IsCombatDebug() || !p->GetMap() || !p->GetMap()->IsRaid())
            return;
        uint32 const now = getMSTime();
        uint32& last = lastLog[accountId];
        if (now - last < 10000)
            return;
        last = now;
        TC_LOG_INFO("scripts.bots", "BotMgr::CombatDbg: %s spec %u: %s; Ziel '%s' hp %.2f%% d%.0f LOS %u, Victim '%s', imKampf %u.",
            p->GetName().c_str(), uint32(p->GetPrimarySpecialization()), reason, target ? target->GetName().c_str() : "-",
            target ? target->GetHealthPct() : 0.0f,
            target ? p->GetDistance(target) : 0.0f, target ? uint32(p->IsWithinLOSInMap(target)) : 0u,
            p->GetVictim() ? p->GetVictim()->GetName().c_str() : "-", uint32(p->IsInCombat()));
    }
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

    // OI-023: Zauber, die ein Spezwechsel/Ausbau als "disabled" markiert hat (character_spell.disabled=1), zaehlen fuer
    // HasSpell() als unbekannt - Bots wirkten dann nie. Einmal pro Bot-Sitzung wieder freischalten (LearnSpell hebt disabled auf).
    {
        static std::unordered_set<uint64> spellsReenabled;
        if (spellsReenabled.insert(player->GetGUID().GetCounter()).second)
        {
            // Direkt das Flag zuruecksetzen (kein LearnSpell: das sendet Pakete und lernt rekursiv Raenge - dabei ist der Server abgestuerzt).
            // Nur Zauber, die die Rotation dieser Skillung auch nutzt (alle zu aktivieren schaltete Passiv-Auren fremder Specs frei und
            // loeste eine Endlosrekursion UpdateAttackPowerAndDamage <-> UpdateSpellDamageAndHealingBonus aus).
            uint32 reenabled = 0;
            std::set<std::wstring> wanted;
            if (BotSpecRotation const* rot = GetOrResolveSpecRotation(player->GetPrimarySpecialization()))
            {
                auto addName = [&wanted](char const* n)
                {
                    if (!n || !*n)
                        return;
                    std::wstring w;
                    Utf8toWStr(std::string(n), w);
                    wstrToLower(w);
                    wanted.insert(w);
                };
                for (BotRotationStep const& st : rot->Priority)
                    addName(st.SpellName);
                addName(rot->InterruptSpellName);
                addName(rot->DispelSpellName);
            }
            for (auto& kv : player->GetSpellMap())
                if (kv.second->disabled && kv.second->state != PLAYERSPELL_REMOVED)
                {
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(kv.first);
                    if (!si || !si->SpellName || !si->SpellName->Str[LOCALE_enUS])
                        continue;
                    std::wstring wn;
                    Utf8toWStr(si->SpellName->Str[LOCALE_enUS], wn);
                    wstrToLower(wn);
                    if (!wanted.count(wn))
                        continue;
                    kv.second->disabled = false;
                    if (kv.second->state == PLAYERSPELL_UNCHANGED)
                        kv.second->state = PLAYERSPELL_CHANGED;
                    ++reenabled;
                }
            if (reenabled)
                TC_LOG_INFO("scripts.bots", "BotMgr::ProcessBotCombatAI: %s: %u deaktivierte Zauber wieder freigeschaltet.", player->GetName().c_str(), reenabled);
        }
    }

    BotSpecRotation const* rotation = GetOrResolveSpecRotation(player->GetPrimarySpecialization());
    if (!rotation)
    {
        BotCombatDbg(player, accountId, "keine Rotation fuer Skillung", nullptr);
        return; // Skillung noch nicht verdrahtet - siehe Kopfkommentar bei g_BotSpecRotations
    }

    // Runde 3: BEIDE moeglichen Ziele im Voraus ermitteln (billig - jeweils nur eine Gruppen-Iteration/
    // ein GetVictim()-Zugriff), damit einzelne Schritte per TargetOverride unabhaengig von der
    // Skillungs-Rolle ein Gegner- oder Heilziel erzwingen koennen (siehe BotRotationTargetOverride-
    // Kommentar in BotMgr.h, noetig fuer Discipline Priest's Atonement-Mechanik).
    Unit* combatTarget = SelectBotCombatTarget(player);
    Unit* healTarget = SelectBotHealTarget(player);
    // Plan-Schritt 6: ausserhalb des Kampfes tote Gruppenmitglieder wiederbeleben (Heiler-Rollen)
    if (!combatTarget && !player->IsInCombat() && rotation->Role == BotRole::Healer && BotTryResurrectMember(player))
        return;

    // Boss-Playbook: Tankwechsel (bot_boss_tankswap) - traegt der aktive Tank genug Stapel des Debuffs, spottet der andere Tank den Boss
    if (rotation->Role == BotRole::Tank && player->GetMap()->IsRaid())
    {
        Playbook const* pb = nullptr;
        if (Creature* pbBoss = PbFindBoss(player, pb))
            if (Unit* active = pbBoss->GetVictim())
                if (active != player && active->GetTypeId() == TYPEID_PLAYER)
                    for (PbTankSwap const& ts : pb->TankSwaps)
                        if (Aura* a = active->GetAura(ts.Aura))
                            if (a->GetStackAmount() >= ts.Stacks && !player->HasAura(ts.Aura) && BotTryTaunt(player, pbBoss))
                                return;
    }

    // Plan-Schritt 3: Tank-Aggro. Der Tank waehlt ein Ziel, das gerade ein anderes Gruppenmitglied angreift, und spottet es heran.
    if (rotation->Role == BotRole::Tank)
    {
        if (Unit* tankTarget = SelectBotTankTarget(player))
        {
            combatTarget = tankTarget;
            if (player->GetVictim() != tankTarget)
            {
                player->Attack(tankTarget, true);
                player->GetMotionMaster()->MoveChase(tankTarget);
            }
            if (BotTryTaunt(player, tankTarget))
                return;
        }
    }
    else if (combatTarget && rotation->Role != BotRole::Healer)
    {
        // Schadensverteiler warten, bis ein Tank der Gruppe das Ziel haelt, und bremsen bei zu hohem Threat gegenueber dem Haupt-Ziel.
        Player* tank = nullptr;
        if (Group* g = player->GetGroup())
            for (GroupReference* gr = g->GetFirstMember(); gr && !tank; gr = gr->next())
            {
                Player* m = gr->GetSource();
                if (m && m != player && m->IsAlive() && m->GetMapId() == player->GetMapId() && IsBotPlayerGuid(m->GetGUID()) && GetBotRole(GetBotAccountIdByGuid(m->GetGUID())) == BotRole::Tank
                    && m->GetDistance(player) < 60.0f)
                    tank = m;
            }
        if (tank && combatTarget->ToCreature())
        {
            if (!combatTarget->GetVictim())
            {
                BotCombatDbg(player, accountId, "wartet: Tank hat Ziel nicht gezogen", combatTarget);
                return; // der Tank hat das Ziel noch nicht gezogen
            }
            ThreatManager& tm = combatTarget->getThreatManager();
            HostileReference* top = tm.getCurrentVictim();
            if (top && top->getTarget() != player && tm.getThreat(player) > top->getThreat() * 1.3f)
            {
                BotCombatDbg(player, accountId, "Threat-Bremse", combatTarget);
                return; // fast Aggro-Uebernahme: diesen Tick nichts wirken
            }
        }
    }

    if (!combatTarget && !healTarget)
    {
        BotCombatDbg(player, accountId, "kein Kampfziel/Heilziel", nullptr);
        return; // weder ein Kampfziel noch ein Heilbedarf - fuer diese Skillung aktuell nichts zu tun
    }

    // Generische Boss-Mechanik-Reaktionen (Ausweichen/Interrupt/Dispel, siehe BotMgr.h-Kommentar bei ProcessBotMechanicReactions())
    // haben Vorrang vor Positionierung und Rotation (sonst schickt die Aufstellung den Bot sofort zurueck in den Bodeneffekt).
    if (ProcessBotMechanicReactions(player, rotation, combatTarget, healTarget))
    {
        BotCombatDbg(player, accountId, "Mechanik-Reaktion hat Vorrang", combatTarget);
        return;
    }

    // Boss-Playbook: Aufstellung laut bot_boss_position (ersetzt die Standard-Positionierung dieser Rolle, solange der Boss kaempft)
    bool pbPositioned = false;
    if (rotation->Role != BotRole::Tank && player->GetMap()->IsRaid())
    {
        Playbook const* pb = nullptr;
        if (Creature* pbBoss = PbFindBoss(player, pb))
        {
            uint8 const phase = PbPhaseOf(pbBoss, *pb);
            for (PbPos const& pp : pb->Pos)
                if (pp.Role == uint8(rotation->Role) && (pp.Phase == 0 || pp.Phase == phase))
                {
                    float sx = 0.0f, sy = 0.0f;
                    if (PbSpot(player, pbBoss, pp, sx, sy))
                    {
                        pbPositioned = true;
                        if (player->GetExactDist2d(sx, sy) > 3.0f + pp.Spread && player->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                            player->GetMotionMaster()->MovePoint(0, sx, sy, pbBoss->GetPositionZ(), true);
                    }
                    break;
                }
        }
    }

    // Plan-Schritt 4: Positionierung nach Rolle (Nahkampf an den Gegner, Fernkampf/Heiler auf Zauberdistanz mit Sichtlinie)
    if (!pbPositioned)
    {
        Unit* posTarget = rotation->Role == BotRole::Healer ? healTarget : combatTarget;
        if (posTarget && posTarget != player)
        {
            float const dist = player->GetDistance(posTarget);
            if (rotation->Role == BotRole::MeleeDps || rotation->Role == BotRole::Tank)
            {
                // Schadensverteiler-Nahkaempfer stellen sich bei grossen Bossen seitlich (90 Grad zur Blickrichtung): Atem trifft vorne,
                // Schwanzhieb (Tail Lash) hinten. Links/rechts nach Bot-GUID gestreut.
                Creature* bossTarget = combatTarget ? combatTarget->ToCreature() : nullptr;
                bool const sideSpot = rotation->Role == BotRole::MeleeDps && bossTarget && (bossTarget->IsDungeonBoss() || bossTarget->GetMaxHealth() > 5000000);
                if (sideSpot)
                {
                    float const side = (player->GetGUID().GetCounter() & 1) ? float(M_PI / 2) : -float(M_PI / 2);
                    float const reach = std::max(2.5f, bossTarget->GetCombatReach() + 1.0f);
                    float const sx = bossTarget->GetPositionX() + std::cos(bossTarget->GetOrientation() + side) * reach;
                    float const sy = bossTarget->GetPositionY() + std::sin(bossTarget->GetOrientation() + side) * reach;
                    if (player->GetExactDist2d(sx, sy) > 3.0f && player->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                        player->GetMotionMaster()->MovePoint(0, sx, sy, bossTarget->GetPositionZ(), true);
                }
                else if (combatTarget && dist > 7.0f && player->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                    player->GetMotionMaster()->MoveChase(combatTarget);
                // Nahkaempfer muessen das Ziel auch ANGREIFEN (Auto-Attack), sonst bleibt "Victim" leer und nur Zauber wirken
                if (combatTarget && dist <= 9.0f && player->GetVictim() != combatTarget && player->IsValidAttackTarget(combatTarget))
                    player->Attack(combatTarget, true);
            }
            else if ((dist > 28.0f || !player->IsWithinLOSInMap(posTarget)) && player->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float const ang = posTarget->GetAngle(player);
                float const want = rotation->Role == BotRole::Healer ? 18.0f : 22.0f;
                player->GetMotionMaster()->MovePoint(0, posTarget->GetPositionX() + std::cos(ang) * want, posTarget->GetPositionY() + std::sin(ang) * want, posTarget->GetPositionZ(), true);
            }
        }
    }


    Unit* roleDefaultTarget = rotation->Role == BotRole::Healer ? healTarget : combatTarget;

    std::string dbgUnknownName;
    uint32 dbgUnresolved = 0, dbgUnknown = 0, dbgCooldown = 0, dbgCond = 0, dbgRange = 0, dbgLos = 0, dbgCastFail = 0;
    for (BotRotationStep const& step : rotation->Priority)
    {
        if (!step.ResolvedSpellId)
        {
            ++dbgUnresolved;
            continue; // Namensaufloesung ist fehlgeschlagen (siehe ResolveSpellIdByName()-Fehlerlog)
        }

        Unit* target = roleDefaultTarget;
        if (step.TargetOverride == BotRotationTargetOverride::ForceEnemy)
            target = combatTarget;
        else if (step.TargetOverride == BotRotationTargetOverride::ForceHealTarget)
            target = healTarget;

        if (!target)
            continue;

        // OI-017: die vom Bot TATSAECHLICH gekannte Id desselben Namens verwenden (siehe FindKnownSpellIdByName()).
        uint32 const castSpellId = FindKnownSpellIdByName(player, step.SpellName, rotation->SpellFamily, step.ResolvedSpellId);
        SpellInfo const* spellInfo = castSpellId ? sSpellMgr->GetSpellInfo(castSpellId) : nullptr;
        if (!spellInfo)
        {
            if (!dbgUnknown)
                dbgUnknownName = std::string(step.SpellName) + " resolved " + std::to_string(step.ResolvedSpellId) + " has " + std::to_string(uint32(player->HasSpell(step.ResolvedSpellId))) + " fam " + std::to_string(uint32(rotation->SpellFamily)) + " cast " + std::to_string(castSpellId);
            ++dbgUnknown;
            continue; // (noch) nicht erlernt, z.B. talentabhaengige Faehigkeit ohne diese Talentwahl
        }

        if (!player->GetSpellHistory()->IsReady(spellInfo))
        {
            ++dbgCooldown;
            continue;
        }

        if (!EvaluateBotRotationCondition(player, target, step))
        {
            ++dbgCond;
            continue;
        }

        float maxRange = spellInfo->GetMaxRange(false, player);
        if (maxRange > 0.0f && player->GetDistance(target) > maxRange)
        {
            ++dbgRange;
            continue;
        }
        if (!player->IsWithinLOSInMap(target))
        {
            ++dbgLos;
            continue;
        }

        if (player->CastSpell(target, castSpellId, TRIGGERED_NONE))
        {
            TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotCombatAI: Account %u castet '%s' (Id %u) auf %s.",
                accountId, step.SpellName, castSpellId, target->GetGUID().ToString().c_str());
            return; // maximal ein Zauber pro Tick (gemeinsame GCD-Ressource, siehe Kopfkommentar)
        }
        ++dbgCastFail;
    }
    {
        char why[260];
        snprintf(why, sizeof(why), "kein Rotationsschritt wirkbar (%u Schritte: unaufgeloest %u, unbekannt %u ['%s'], Abklingzeit %u, Bedingung %u, Reichweite %u, Sicht %u, CastFehler %u)",
            uint32(rotation->Priority.size()), dbgUnresolved, dbgUnknown, dbgUnknownName.c_str(), dbgCooldown, dbgCond, dbgRange, dbgLos, dbgCastFail);
        BotCombatDbg(player, accountId, why, roleDefaultTarget);
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

namespace
{
    // Plan-Schritt 5: schaedliche AreaTrigger (typische Legion-Bossboeden) - feindlicher Wirker, nicht-positiver Zauber, Radius <= 15
    class BotHarmfulAreaTriggerCheck
    {
    public:
        BotHarmfulAreaTriggerCheck(WorldObject const* searcher, float range) : _searcher(searcher), _range(range) { }

        template<typename T>
        bool operator()(T*) const { return false; }

        bool operator()(AreaTrigger* at) const
        {
            return !at->IsRemoved() && _searcher->IsWithinDistInMap(at, _range);
        }

    private:
        WorldObject const* _searcher;
        float _range;
    };

    AreaTrigger* FindHarmfulAreaTriggerUnderBot(Player* bot, float searchRadius, float& outRadius)
    {
        std::list<WorldObject*> candidates;
        CellCoord cellCoord(Trinity::ComputeCellCoord(bot->GetPositionX(), bot->GetPositionY()));
        Cell cell(cellCoord);
        cell.SetNoCreate();

        BotHarmfulAreaTriggerCheck check(bot, searchRadius);
        Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck> searcher(bot, candidates, check, GRID_MAP_TYPE_MASK_AREATRIGGER);
        TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck>, WorldTypeMapContainer> worldVisitor(searcher);
        TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck>, GridTypeMapContainer> gridVisitor(searcher);
        cell.Visit(cellCoord, worldVisitor, *bot->GetMap(), *bot, searchRadius);
        cell.Visit(cellCoord, gridVisitor, *bot->GetMap(), *bot, searchRadius);

        for (WorldObject* candidate : candidates)
        {
            AreaTrigger* at = candidate->ToAreaTrigger();
            if (!at)
                continue;
            Unit* caster = at->GetCaster();
            SpellInfo const* info = sSpellMgr->GetSpellInfo(at->GetSpellId());
            // Der Ausloese-Zauber eines Boss-Bodeneffekts (z.B. Infested Ground 203044) hat oft keine Schadenseffekte und gilt als "positiv" -
            // daher zaehlt hier jeder AreaTrigger einer feindlichen Kreatur (Schaden macht das AreaTrigger-Skript).
            if (!caster || !caster->IsHostileTo(bot) || !info || !caster->ToCreature())
                continue;
            float radius = at->GetTemplate() ? at->GetTemplate()->MaxSearchRadius : 0.0f;
            if (radius <= 0.0f)
                radius = 4.0f;
            radius += 1.5f; // Sicherheitsrand
            radius = std::min(radius, 15.0f);
            if (bot->GetExactDist2d(at) <= radius)
            {
                outRadius = radius;
                return at;
            }
        }
        return nullptr;
    }
}

namespace
{
    struct HarmfulZone { float X, Y, Radius; };

    // alle schaedlichen AreaTrigger feindlicher Kreaturen im Umkreis (Mittelpunkt + Wirkradius inkl. Sicherheitsrand)
    void CollectHarmfulAreaTriggers(Player* bot, float searchRadius, std::vector<HarmfulZone>& out)
    {
        std::list<WorldObject*> candidates;
        CellCoord cellCoord(Trinity::ComputeCellCoord(bot->GetPositionX(), bot->GetPositionY()));
        Cell cell(cellCoord);
        cell.SetNoCreate();
        BotHarmfulAreaTriggerCheck check(bot, searchRadius);
        Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck> searcher(bot, candidates, check, GRID_MAP_TYPE_MASK_AREATRIGGER);
        TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck>, WorldTypeMapContainer> worldVisitor(searcher);
        TypeContainerVisitor<Trinity::WorldObjectListSearcher<BotHarmfulAreaTriggerCheck>, GridTypeMapContainer> gridVisitor(searcher);
        cell.Visit(cellCoord, worldVisitor, *bot->GetMap(), *bot, searchRadius);
        cell.Visit(cellCoord, gridVisitor, *bot->GetMap(), *bot, searchRadius);
        for (WorldObject* candidate : candidates)
        {
            AreaTrigger* at = candidate->ToAreaTrigger();
            if (!at)
                continue;
            Unit* caster = at->GetCaster();
            if (!caster || !caster->ToCreature() || !caster->IsHostileTo(bot) || !sSpellMgr->GetSpellInfo(at->GetSpellId()))
                continue;
            float radius = at->GetTemplate() ? at->GetTemplate()->MaxSearchRadius : 0.0f;
            if (radius <= 0.0f)
                radius = 4.0f;
            out.push_back({ at->GetPositionX(), at->GetPositionY(), std::min(radius, 15.0f) + 1.5f });
        }
    }
}

bool BotMgr::ProcessBotMechanicReactions(Player* bot, BotSpecRotation const* rotation, Unit* combatTarget,
    Unit* healTarget)
{
    // Boss-Playbook (bot_boss_avoid): schaedliche Aura auf mir (vom Raid weglaufen) und Frontal-Kegel (seitlich ausweichen)
    if (bot->GetMap()->IsRaid())
    {
        Playbook const* pb = nullptr;
        if (Creature* pbBoss = PbFindBoss(bot, pb))
        {
            uint8 const phase = PbPhaseOf(pbBoss, *pb);
            uint8 const roleBit = PbRoleBit(rotation->Role);
            for (PbAvoid const& av : pb->Avoid)
            {
                if ((av.Phase != 0 && av.Phase != phase) || !(av.RoleMask & roleBit))
                    continue;
                if (av.Kind == 1 && bot->HasAura(av.Spell)) // AURA_SELF: Abstand zu den anderen Gruppenmitgliedern herstellen
                {
                    float want = av.Radius > 0.0f ? av.Radius : 10.0f;
                    Unit* nearest = nullptr;
                    float nearestDist = 9999.0f;
                    if (Group* g = bot->GetGroup())
                        for (GroupReference* gr = g->GetFirstMember(); gr; gr = gr->next())
                            if (Player* m = gr->GetSource())
                                if (m != bot && m->IsAlive() && m->GetMapId() == bot->GetMapId())
                                {
                                    float const d = bot->GetExactDist2d(m);
                                    if (d < nearestDist)
                                    {
                                        nearestDist = d;
                                        nearest = m;
                                    }
                                }
                    if (nearest && nearestDist < want)
                    {
                        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                        {
                            float const ang = nearest->GetAngle(bot);
                            float const step = want - nearestDist + 3.0f;
                            bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX() + std::cos(ang) * step, bot->GetPositionY() + std::sin(ang) * step, bot->GetPositionZ(), true);
                        }
                        return true;
                    }
                }
                else if (av.Kind == 2 && pbBoss->GetVictim() != bot) // FRONTAL_CONE: waehrend der Boss den Zauber wirkt nicht vor ihm stehen
                {
                    Spell* cur = pbBoss->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                    if (!cur)
                        cur = pbBoss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    if (cur && cur->GetSpellInfo()->Id == av.Spell && pbBoss->HasInArc(1.75f, bot) && bot->GetDistance(pbBoss) < 40.0f)
                    {
                        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                        {
                            float const sideAng = pbBoss->GetOrientation() + ((bot->GetGUID().GetCounter() & 1) ? float(M_PI / 2) : -float(M_PI / 2));
                            bot->GetMotionMaster()->MovePoint(0, pbBoss->GetPositionX() + std::cos(sideAng) * 8.0f, pbBoss->GetPositionY() + std::sin(sideAng) * 8.0f, pbBoss->GetPositionZ(), true);
                        }
                        return true;
                    }
                }
            }
        }
    }

    // Boss-Playbook: gezielter Interrupt (bot_boss_interrupt) und Dispel (bot_boss_dispel); je Wirker/Ziel nur ein Bot pro 1,5 s
    if (bot->GetMap()->IsRaid())
    {
        Playbook const* pb = nullptr;
        if (Creature* pbBoss = PbFindBoss(bot, pb))
        {
            static std::unordered_map<uint64, uint32> claims; // GUID-Zaehler des Wirkers/Ziels -> Zeit des letzten Zugriffs
            uint32 const now = getMSTime();
            uint32 const intSpell = (rotation->InterruptSpellName && !pb->Interrupts.empty())
                ? FindKnownSpellIdByName(bot, rotation->InterruptSpellName, rotation->SpellFamily, rotation->ResolvedInterruptSpellId) : 0;
            if (intSpell)
                if (SpellInfo const* ii = sSpellMgr->GetSpellInfo(intSpell))
                    if (bot->GetSpellHistory()->IsReady(ii))
                    {
                        std::list<Creature*> casters;
                        casters.push_back(pbBoss);
                        if (Group* g = bot->GetGroup())
                            for (GroupReference* gr = g->GetFirstMember(); gr; gr = gr->next())
                                if (Player* m = gr->GetSource())
                                    for (Unit* at : m->getAttackers())
                                        if (Creature* ac = at ? at->ToCreature() : nullptr)
                                            casters.push_back(ac);
                        for (Creature* c : casters)
                        {
                            if (!c->IsAlive() || bot->GetDistance(c) > ii->GetMaxRange(false, bot) || !bot->IsWithinLOSInMap(c))
                                continue;
                            Spell* cur = c->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                            if (!cur)
                                cur = c->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                            if (!cur)
                                continue;
                            bool wanted = false;
                            for (PbInterrupt const& pi : pb->Interrupts)
                                if (pi.Spell == cur->GetSpellInfo()->Id && (pi.Caster == 0 || pi.Caster == c->GetEntry()))
                                    wanted = true;
                            uint32& last = claims[c->GetGUID().GetCounter()];
                            if (!wanted || now - last < 1500)
                                continue;
                            if (bot->CastSpell(c, intSpell, TRIGGERED_NONE))
                            {
                                last = now;
                                return true;
                            }
                        }
                    }
            uint32 const dispSpell = (rotation->DispelSpellName && !pb->Dispels.empty())
                ? FindKnownSpellIdByName(bot, rotation->DispelSpellName, rotation->SpellFamily, rotation->ResolvedDispelSpellId) : 0;
            if (dispSpell)
                if (SpellInfo const* di = sSpellMgr->GetSpellInfo(dispSpell))
                    if (bot->GetSpellHistory()->IsReady(di))
                        if (Group* g = bot->GetGroup())
                            for (GroupReference* gr = g->GetFirstMember(); gr; gr = gr->next())
                                if (Player* m = gr->GetSource())
                                    if (m->IsAlive() && m->GetMapId() == bot->GetMapId() && bot->GetDistance(m) <= di->GetMaxRange(false, bot) && bot->IsWithinLOSInMap(m))
                                        for (PbDispel const& pd : pb->Dispels)
                                            if (Aura* a = m->GetAura(pd.Aura))
                                                if (a->GetStackAmount() >= pd.MinStacks)
                                                {
                                                    uint32& last = claims[m->GetGUID().GetCounter() + 0x100000000ull];
                                                    if (now - last < 1500)
                                                        continue;
                                                    if (bot->CastSpell(m, dispSpell, TRIGGERED_NONE))
                                                    {
                                                        last = now;
                                                        return true;
                                                    }
                                                }
        }
    }

    // Schaedliche Boden-AreaTrigger: steht der Bot in einem, laeuft er in die sicherste Richtung (groesster Abstand zu ALLEN Flaechen in 35 yd),
    // und prueft das alle 0,5 s neu - nicht nur eine feste Richtung, die in die naechste Flaeche fuehren kann.
    {
        std::vector<HarmfulZone> zones;
        CollectHarmfulAreaTriggers(bot, 35.0f, zones);
        // Playbook: als AVOID markierte Adds (z.B. Corrupted Vermin mit Burst of Corruption) sind wie Boden-Flaechen mit 9 yd Radius zu meiden
        if (bot->GetMap()->IsRaid())
        {
            Playbook const* avoidPb = nullptr;
            if (PbFindBoss(bot, avoidPb))
            {
                std::set<uint32> avoidEntries;
                for (PbAdd const& a : avoidPb->Adds)
                    if (a.Mode == 2)
                        avoidEntries.insert(a.Add);
                for (uint32 entry : avoidEntries)
                {
                    std::list<Creature*> found;
                    bot->GetCreatureListWithEntryInGrid(found, entry, 40.0f);
                    for (Creature* c : found)
                        if (c->IsAlive())
                            zones.push_back({ c->GetPositionX(), c->GetPositionY(), 9.0f });
                }
            }
        }
        bool under = false;
        for (HarmfulZone const& z : zones)
            if (bot->GetExactDist2d(z.X, z.Y) <= z.Radius)
                under = true;
        if (under)
        {
            static std::unordered_map<uint64, uint32> lastDodge;
            uint32 const now = getMSTime();
            uint32& last = lastDodge[bot->GetGUID().GetCounter()];
            if (now - last >= 500)
            {
                last = now;
                float bestScore = -9999.0f, bestX = bot->GetPositionX(), bestY = bot->GetPositionY();
                for (float dist : { 7.0f, 11.0f, 15.0f })
                    for (int i = 0; i < 12; ++i)
                    {
                        float const ang = float(i) * float(M_PI) / 6.0f;
                        float const px = bot->GetPositionX() + std::cos(ang) * dist;
                        float const py = bot->GetPositionY() + std::sin(ang) * dist;
                        float score = 9999.0f;
                        for (HarmfulZone const& z : zones)
                            score = std::min(score, float(std::hypot(px - z.X, py - z.Y)) - z.Radius);
                        score -= dist * 0.15f; // kurze Wege leicht bevorzugen
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestX = px;
                            bestY = py;
                        }
                    }
                bot->GetMotionMaster()->MovePoint(0, bestX, bestY, bot->GetPositionZ(), true);
            }
            BotCombatDbg(bot, uint32(bot->GetGUID().GetCounter()) + 1000000u, "weicht AreaTrigger aus", combatTarget);
            return true;
        }
    }

    // 1. Gefaehrlichen Bodeneffekt verlassen - hoechste Prioritaet, da Steh'nbleiben potentiell toedlich
    // ist, waehrend Interrupt/Dispel "nur" DPS/Heilausfall bedeuten. Suchradius bewusst klein gewaehlt
    // (der Bot steht ja bereits im/nahe am Effekt, wenn dieser ueberhaupt relevant wird).
    if (DynamicObject* harmfulEffect = FindHarmfulGroundEffectUnderBot(bot, 15.0f))
    {
        // Skalierungs-Haertung (Livetest-Fund Runde 7, siehe README-Commands.md/PR - "500+ gleichzeitige
        // Bots"-Bedenken): ProcessBotMechanicReactions() wird bei aktivem Bodeneffekt JEDEN Kampf-KI-Tick
        // (alle ~400ms) erneut aufgerufen, solange der Bot noch innerhalb des Suchradius steht - das
        // navmesh-basierte Herauslaufen dauert aber typischerweise laenger als 400ms. Ohne diese Sperre
        // wuerde MovePoint() bei jedem Tick erneut ausgeloest (jeweils ein neues SMSG_ON_MONSTER_MOVE),
        // obwohl der Bot bereits unterwegs ist - unnoetiger Paket-Overhead, der sich bei vielen
        // gleichzeitig betroffenen Bots summiert. POINT_MOTION_TYPE ist bereits aktiv, waehrend eine
        // vorherige Ausweichbewegung noch laeuft - dann hier nichts erneut auf den Weg schicken, aber
        // (Sicherheit vor Sparsamkeit) trotzdem "true" liefern, damit die normale Rotation fuer diesen
        // Tick weiterhin pausiert, bis der Bot den Effekt tatsaechlich verlassen hat.
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            // Radial vom Effektzentrum weg, ueber den Wirkradius hinaus (plus Sicherheitsabstand) -
            // MovePoint(generatePath=true) uebernimmt die eigentliche Navmesh-Route dorthin, damit der
            // Bot nicht durch Waende/von Klippen "flieht".
            float angle = harmfulEffect->GetAngle(bot);
            float distance = harmfulEffect->GetRadius() + 5.0f;
            float fleeX = harmfulEffect->GetPositionX() + std::cos(angle) * distance;
            float fleeY = harmfulEffect->GetPositionY() + std::sin(angle) * distance;
            float fleeZ = harmfulEffect->GetPositionZ();
            bot->GetMotionMaster()->MovePoint(0, fleeX, fleeY, fleeZ, true);

            TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s weicht Bodeneffekt "
                "(Spell %u) aus.", bot->GetGUID().ToString().c_str(), harmfulEffect->GetSpellId());
        }
        BotCombatDbg(bot, uint32(bot->GetGUID().GetCounter()) + 1000000u, "weicht Bodeneffekt aus", combatTarget);
        return true;
    }

    // 2. Interrupt - nur, wenn die Skillung ueberhaupt eine hat (siehe g_BotSpecRotations) und der
    // Bot sie bereits erlernt hat/sie einsatzbereit ist. Der Core prueft beim tatsaechlichen Cast von
    // Spell::EffectInterruptCast() selbst, ob combatTarget gerade unterbrechbar castet - hier reicht
    // die billige Vorabpruefung "castet ueberhaupt gerade etwas", um unnoetige Fehlversuche zu vermeiden.
    uint32 const interruptSpellId = rotation->InterruptSpellName
        ? FindKnownSpellIdByName(bot, rotation->InterruptSpellName, rotation->SpellFamily, rotation->ResolvedInterruptSpellId) : 0;
    if (interruptSpellId && combatTarget)
    {
        bool targetIsCasting = combatTarget->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr
            || combatTarget->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;

        if (targetIsCasting)
        {
            if (SpellInfo const* interruptInfo = sSpellMgr->GetSpellInfo(interruptSpellId))
            {
                if (bot->GetSpellHistory()->IsReady(interruptInfo)
                    && bot->GetDistance(combatTarget) <= interruptInfo->GetMaxRange(false, bot)
                    && bot->IsWithinLOSInMap(combatTarget))
                {
                    if (bot->CastSpell(combatTarget, interruptSpellId, TRIGGERED_NONE))
                    {
                        TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s "
                            "unterbricht %s (Interrupt-Spell %u).", bot->GetGUID().ToString().c_str(),
                            combatTarget->GetGUID().ToString().c_str(), interruptSpellId);
                        BotCombatDbg(bot, uint32(bot->GetGUID().GetCounter()) + 1000000u, "Interrupt", combatTarget);
                        return true;
                    }
                }
            }
        }
    }

    // 3. Dispel - der Core waehlt die zu entfernende Aura selbst aus (Unit::GetDispellableAuraList(),
    // dieselbe Logik wie Spell::EffectDispel() sie fuer echte Spieler-Dispels nutzt), anhand der
    // DispelMask des Dispel-Spells selbst (SpellInfo::Dispel-Feld, z.B. "Dispel Magic" -> Magic).
    uint32 const dispelSpellId = rotation->DispelSpellName
        ? FindKnownSpellIdByName(bot, rotation->DispelSpellName, rotation->SpellFamily, rotation->ResolvedDispelSpellId) : 0;
    if (dispelSpellId && healTarget)
    {
        if (SpellInfo const* dispelInfo = sSpellMgr->GetSpellInfo(dispelSpellId))
        {
            if (bot->GetSpellHistory()->IsReady(dispelInfo)
                && bot->GetDistance(healTarget) <= dispelInfo->GetMaxRange(false, bot)
                && bot->IsWithinLOSInMap(healTarget))
            {
                DispelChargesList dispelList;
                healTarget->GetDispellableAuraList(bot, dispelInfo->GetDispelMask(), dispelList);
                if (!dispelList.empty())
                {
                    if (bot->CastSpell(healTarget, dispelSpellId, TRIGGERED_NONE))
                    {
                        TC_LOG_DEBUG("scripts.bots", "BotMgr::ProcessBotMechanicReactions: Bot %s "
                            "dispelt %s (Dispel-Spell %u).", bot->GetGUID().ToString().c_str(),
                            healTarget->GetGUID().ToString().c_str(), dispelSpellId);
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
    uint32 rewardItemEntry)
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

    if (!player->CanRewardQuest(quest, rewardItemEntry, true))
    {
        TC_LOG_ERROR("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - CanRewardQuest() fuer Quest %u ('%s') "
            "mit rewardItemEntry %u lieferte false (ungueltige Belohnungswahl?).", accountId, questId,
            quest->GetLogTitle().c_str(), rewardItemEntry);
        return false;
    }

    // Derselbe Aufruf, den HandleQuestgiverChooseRewardOpcode() selbst nach den obigen Checks macht.
    player->RewardQuest(quest, rewardItemEntry, questGiver);

    TC_LOG_INFO("scripts.bots", "BotMgr::BotTurnInQuest: Account %u - Quest %u ('%s') bei Questgeber '%s' "
        "(Spawn " UI64FMTD ") abgegeben, rewardItemEntry %u.", accountId, questId, quest->GetLogTitle().c_str(),
        questGiver->GetName().c_str(), questGiverSpawnGuid, rewardItemEntry);
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

namespace
{
    // Plan-Schritt 1 (04.10.2026): Boss-Erkennung ohne Core-Flag. Die Tabelle world.bot_raid_boss_order (map, order, entry) nennt die
    // Bosse mit Reihenfolge; Legion-Raid-Bosse haben kein instance_encounters-Eintrag und damit kein IsDungeonBoss().
    std::map<uint32, std::map<uint32, uint32>>& RaidBossTable() // map -> (entry -> order)
    {
        static std::map<uint32, std::map<uint32, uint32>> table;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            if (QueryResult result = WorldDatabase.Query("SELECT `map`, `order`, `entry` FROM `bot_raid_boss_order`"))
            {
                do
                {
                    Field* f = result->Fetch();
                    table[f[0].GetUInt32()][f[2].GetUInt32()] = f[1].GetUInt32();
                } while (result->NextRow());
            }
            TC_LOG_INFO("scripts.bots", "BotMgr: bot_raid_boss_order geladen: %u Karten.", uint32(table.size()));
        }
        return table;
    }

    bool IsRaidTableBoss(uint32 mapId, uint32 entry)
    {
        auto m = RaidBossTable().find(mapId);
        return m != RaidBossTable().end() && m->second.count(entry);
    }
}

Creature* BotMgr::FindNearestLivingDungeonBoss(Player* bot) const
{
    Map* map = bot->GetMap();
    auto const& tableForMap = RaidBossTable();
    auto tm = tableForMap.find(map->GetId());

    // Plan-Schritt 10: lebende Boss-GUIDs je Instanz hoechstens einmal pro Sekunde neu ermitteln (statt Voll-Scan je Bot und Tick)
    struct BossCache { uint32 StampMs = 0; std::vector<ObjectGuid> Guids; };
    static std::unordered_map<Map const*, BossCache> cache;
    BossCache& bc = cache[map];
    uint32 const nowMs = getMSTime();
    if (bc.StampMs == 0 || getMSTimeDiff(bc.StampMs, nowMs) > 1000)
    {
        bc.StampMs = nowMs ? nowMs : 1;
        bc.Guids.clear();
        for (auto const& pair : map->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature || !creature->IsInWorld() || !creature->IsAlive())
                continue;
            if (creature->IsDungeonBoss() || (tm != tableForMap.end() && tm->second.count(creature->GetEntry())))
                bc.Guids.push_back(creature->GetGUID());
        }
    }

    Creature* best = nullptr;
    uint32 bestOrder = 0xFFFFFFFF;
    float bestDist = 0.0f;
    for (ObjectGuid const& guid : bc.Guids)
    {
        Creature* creature = map->GetCreature(guid);
        if (!creature || !creature->IsInWorld() || !creature->IsAlive())
            continue;
        uint32 order = 0xFFFFFFFE; // Bosse ohne Tabelleneintrag (IsDungeonBoss) nach den Tabellen-Bossen, dann nach Naehe
        if (tm != tableForMap.end())
        {
            auto o = tm->second.find(creature->GetEntry());
            if (o != tm->second.end())
                order = o->second;
        }
        float dist = bot->GetDistance(creature);
        if (!best || order < bestOrder || (order == bestOrder && dist < bestDist))
        {
            best = creature;
            bestOrder = order;
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
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsDungeonBoss() || IsRaidTableBoss(bot->GetMapId(), creature->GetEntry()))
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
             << std::fixed << std::setprecision(1) << dist << "y pos=" << creature->GetPosition().ToString()
             << " phase=" << (center->IsInPhase(creature) ? "sichtbar" : "ANDERE-PHASE(unsichtbar)");
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

    // Gruppenweite Fuehrung (Bot-Tank fuehrt, Rest folgt) an/aus; die einzelnen Bot-Flags unten gelten fuer Gruppen ohne Bot-Tank
    _groupCfg[group->GetGUID()].Enabled = enable ? 1 : 0;

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
    // Zone-Kampf von Raid-Bossen (alle Spieler der Instanz "im Kampf") zaehlt nicht: nur ein echtes Ziel/Angreifer haelt den Clear auf.
    if (bot->GetVictim() || !bot->getAttackers().empty() || bot->IsNonMeleeSpellCast(false))
        return;

    if (Creature* corpse = FindNearestLootableCorpse(bot, BOT_DUNGEON_CLEAR_LOOT_RADIUS))
    {
        BotLootTarget(accountId, corpse->GetSpawnId());
        return; // ein Schritt pro Tick - naechster Schritt (Trash/Boss-Suche) beim naechsten Tick
    }

    // Trash auf dem Weg hat Vorrang vor dem Weiterlaufen zum Boss - "auf dem Weg toeten", nicht dran
    // vorbeilaufen und im Ruecken stehen lassen.
    uint8 pullMode = 0; // 0 normal, 1 pack, 2 leeroy, 3 combo (siehe BotMgr.h, GroupBotConfig)
    if (Group* cg = bot->GetGroup())
    {
        auto c = _groupCfg.find(cg->GetGUID());
        if (c != _groupCfg.end())
            pullMode = c->second.Mode;
    }

    Creature* trashTarget = pullMode == 2 ? nullptr : FindNearestAggroableTrash(bot, BOT_DUNGEON_CLEAR_TRASH_AGGRO_RADIUS); // Leeroy: Gegner ignorieren
    if (trashTarget)
    {
        bool needGather = pullMode == 1;
        if (pullMode == 3)
        {
            uint32 packSize = 0;
            for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
            {
                Creature* other = pair.second;
                if (other && other->IsInWorld() && other->IsAlive() && other->IsHostileTo(bot) && other->GetDistance(trashTarget) <= 15.0f)
                    ++packSize;
            }
            needGather = packSize >= 3;
        }
        if (needGather && !IsGroupGathered(bot, 20.0f))
        {
            itr->second.GatherWaitMs += BOT_DUNGEON_CLEAR_TICK_MS;
            if (itr->second.GatherWaitMs < 30000)
                return; // auf Nachzuegler/Heiler-Mana warten, nach 30 s trotzdem pullen
        }
        itr->second.GatherWaitMs = 0;
    }
    if (Creature* trash = trashTarget)
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

    if ((pullMode == 1 || pullMode == 3) && bot->GetDistance(boss) <= 45.0f && !IsGroupGathered(bot, 20.0f))
    {
        itr->second.GatherWaitMs += BOT_DUNGEON_CLEAR_TICK_MS;
        if (itr->second.GatherWaitMs < 30000)
            return; // vor dem Boss sammeln
    }

    if (bot->GetDistance(boss) <= BOT_DUNGEON_CLEAR_ARRIVAL_DISTANCE)
    {
        itr->second.GatherWaitMs = 0;
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


// ---------------------------------------------------------------------------------------------------
// OI-051 (04.10.2026): Bot-Ausbau auf eine beliebige Stufe. Ein Bot wird als Stufe-1-Charakter angelegt;
// ProvisionBot() bringt ihn auf die gewuenschte Stufe (1-110) mit Spezialisierung, Talenten der Stufe und
// zur Stufe passender Ausruestung (Pool-Bands 1/20/50/80/100/110, Qualitaetsstufe dauerhaft pro Bot).
// Zauber/Faehigkeiten kommen aus Player::GiveLevel() (LearnDefaultSkills, LearnSpecializationSpells).
// Bewusst nur AUFWAERTS (Level senken wuerde erlernte Zauber stehen lassen) - fuer eine niedrigere Stufe
// einen neuen Bot anlegen. Liefert eine Zusammenfassung (Zauberzahl, Ilvl, Rotationsabdeckung) zurueck.
// ---------------------------------------------------------------------------------------------------
bool BotMgr::ProvisionBot(uint32 accountId, uint8 targetLevel, uint32 specId, uint8 role, uint16 targetIlvl, std::string& outSummary)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
    {
        outSummary = "keine Bot-Session (erst '.bottest login' ausfuehren)";
        return false;
    }

    Player* player = itr->second.Session->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        outSummary = "Bot ist nicht in der Welt";
        return false;
    }
    // Ein toter oder kaempfender Bot wird vor dem Ausbau wiederbelebt bzw. aus dem Kampf genommen.
    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
    }
    if (player->IsInCombat())
        player->CombatStop(true);

    uint8 const maxLevel = uint8(std::min<uint32>(110, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
    if (targetLevel < 1 || targetLevel > maxLevel)
    {
        outSummary = "Stufe muss zwischen 1 und " + std::to_string(maxLevel) + " liegen";
        return false;
    }

    uint8 const oldLevel = player->getLevel();
    if (targetLevel < oldLevel)
    {
        outSummary = "Stufe " + std::to_string(oldLevel) + " ist hoeher als das Ziel " + std::to_string(targetLevel)
            + " - Provisionierung geht nur aufwaerts (neuen Bot anlegen)";
        return false;
    }

    // 1. Stufe anheben (Zauber/Faehigkeiten/Talentreihen der Stufe kommen aus GiveLevel())
    if (targetLevel > oldLevel)
    {
        player->GiveLevel(targetLevel);
        player->SetUInt32Value(PLAYER_XP, 0);
    }

    // 2. Spezialisierung (ab Stufe 10): angegebene oder die Standard-Spezialisierung der Klasse
    ChrSpecializationEntry const* spec = nullptr;
    // Rolle (1 Tank, 2 Heiler, 3 Schaden) -> erste passende Spezialisierung der Klasse
    if (!specId && role >= 1 && role <= 3)
    {
        for (uint32 i = 0; i < sChrSpecializationStore.GetNumRows(); ++i)
        {
            ChrSpecializationEntry const* candidate = sChrSpecializationStore.LookupEntry(i);
            if (candidate && candidate->ClassID == int8(player->getClass()) && candidate->Role == int8(role - 1)
                && (!spec || candidate->OrderIndex < spec->OrderIndex))
                spec = candidate;
        }
        if (!spec)
        {
            outSummary = "diese Klasse hat keine Spezialisierung fuer die gewuenschte Rolle";
            return false;
        }
        specId = spec->ID;
        spec = nullptr;
    }
    if (specId)
    {
        spec = sChrSpecializationStore.LookupEntry(specId);
        if (!spec || spec->ClassID != player->getClass())
        {
            outSummary = "Spezialisierung " + std::to_string(specId) + " gehoert nicht zur Klasse des Bots";
            return false;
        }
        if (targetLevel >= MIN_SPECIALIZATION_LEVEL)
            player->ActivateTalentGroup(spec);
    }
    uint32 const currentSpecId = player->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID);

    // 3. Talente: je freigeschalteter Reihe ein Talent (Spalte nach Bot-GUID gestreut), spezifisch fuer die Spec
    uint32 talentsLearned = 0;
    uint32 const tiers = player->GetUInt32Value(PLAYER_FIELD_MAX_TALENT_TIERS);
    uint32 const guidLow = uint32(player->GetGUID().GetCounter());
    for (uint32 tier = 0; tier < tiers && tier < MAX_TALENT_TIERS; ++tier)
    {
        bool hasTalentInTier = false;
        for (uint32 c = 0; c < MAX_TALENT_COLUMNS && !hasTalentInTier; ++c)
            for (TalentEntry const* t : sDB2Manager.GetTalentsByPosition(player->getClass(), tier, c))
                if (player->HasTalent(t->ID, player->GetActiveTalentGroup()))
                    hasTalentInTier = true;
        if (hasTalentInTier)
            continue;

        for (uint32 attempt = 0; attempt < MAX_TALENT_COLUMNS; ++attempt)
        {
            uint32 column = (guidLow + tier + attempt) % MAX_TALENT_COLUMNS;
            bool done = false;
            for (TalentEntry const* t : sDB2Manager.GetTalentsByPosition(player->getClass(), tier, column))
            {
                if (t->SpecID && t->SpecID != currentSpecId)
                    continue;
                int32 cooldownSpell = 0;
                if (player->LearnTalent(t->ID, &cooldownSpell) == TALENT_LEARN_OK)
                {
                    ++talentsLearned;
                    done = true;
                    break;
                }
            }
            if (done)
                break;
        }
    }

    // 4. Ausruestung: alte Ausruestung entfernen, dann zum aktuellen Level-Band aus dem Pool neu ausruesten
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    // Frisch eingeloggte/teleportierte Bots tragen UNIT_STATE_STUNNED (Login-/Teleport-Zustand ohne Client-Ack):
    // Player::CanEquipItem lehnt dann JEDES Item mit EQUIP_ERR_GENERIC_STUNNED ab. Ohne echte Betaeubung (Aura)
    // den Zustand fuer den Ausruestungsschritt loeschen.
    if (player->HasUnitState(UNIT_STATE_STUNNED) && !player->HasAuraType(SPELL_AURA_MOD_STUN))
        player->ClearUnitState(UNIT_STATE_STUNNED);
    // Rucksack leeren (Reste fehlgeschlagener Ausruestungsversuche; Bots besitzen nichts Wertvolles)
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    g_botPoolMinIlvl = targetIlvl;
    bool gearOk = EquipBotFromPool(accountId);
    g_botPoolMinIlvl = 0;
    // Reste dieses Laufs (nicht ausruestbare Items) wieder aus dem Rucksack entfernen
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    // 5. Heilen, speichern
    player->SetFullHealth();
    player->SetFullPower(player->GetPowerType());
    player->SaveToDB();

    // 6. Zusammenfassung / Rotationsabdeckung
    uint32 rotationKnown = 0, rotationTotal = 0;
    if (BotSpecRotation const* rotation = GetOrResolveSpecRotation(currentSpecId))
    {
        for (BotRotationStep const& step : rotation->Priority)
        {
            ++rotationTotal;
            if (FindKnownSpellIdByName(player, step.SpellName, rotation->SpellFamily, step.ResolvedSpellId))
                ++rotationKnown;
        }
    }

    // Befuellte Ruestungs-/Waffenslots (ohne Hemd 3 und Wappenrock 18) - zeigt Luecken in der Pool-Ausruestung
    uint32 filledSlots = 0, countedSlots = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;
        ++countedSlots;
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++filledSlots;
    }

    std::ostringstream out;
    out << "Stufe " << uint32(oldLevel) << " -> " << uint32(player->getLevel()) << ", Spec " << currentSpecId
        << ", Talente neu " << talentsLearned << ", bekannte Zauber " << player->GetSpellMap().size()
        << ", Slots " << filledSlots << "/" << countedSlots
        << ", Ilvl ausgeruestet " << std::fixed << std::setprecision(1) << player->GetAverageItemLevelEquipped()
        << (gearOk ? "" : " (Ausruestung: kein Slot befuellt)") << ", Rotation " << rotationKnown << "/" << rotationTotal
        << " Schritte bekannt";
    outSummary = out.str();
    TC_LOG_INFO("scripts.bots", "BotMgr::ProvisionBot: Account %u - %s", accountId, outSummary.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------------
// OI-051 Schritt 4: Startpunkt nach Stufe. Waehlt zufaellig einen Questgeber, dessen Quests zur Stufe des
// Bots passen (MinLevel Stufe-1..Stufe+1), die Volk/Klasse des Bots zulassen (AllowableRaces/-Classes, damit
// Fraktion und Klasse stimmen) und der auf einem Kontinent steht (nicht in einer Phase). Dorthin wird der
// Bot teleportiert. So stehen Bots der Stufen 1-110 in sinnvollen Gebieten statt alle am Startpunkt.
// ---------------------------------------------------------------------------------------------------
bool BotMgr::PlaceBotByLevel(uint32 accountId, std::string& outSummary)
{
    auto itr = _botSessions.find(accountId);
    Player* player = (itr != _botSessions.end() && itr->second.Session) ? itr->second.Session->GetPlayer() : nullptr;
    if (!player || !player->IsInWorld())
    {
        outSummary = "Bot ist nicht in der Welt";
        return false;
    }

    uint32 const level = player->getLevel();
    uint32 const raceMask = player->getRaceMask();
    uint32 const classMask = player->getClassMask();

    QueryResult result = WorldDatabase.PQuery(
        "SELECT c.map, c.position_x, c.position_y, c.position_z, c.orientation, c.id "
        "FROM creature_queststarter qs JOIN quest_template q ON q.ID = qs.quest JOIN creature c ON c.id = qs.id "
        "LEFT JOIN quest_template_addon a ON a.ID = q.ID "
        "WHERE q.MinLevel BETWEEN %u AND %u AND (q.AllowableRaces = 0 OR q.AllowableRaces = -1 OR (q.AllowableRaces & %u) <> 0) "
        "AND (a.AllowableClasses IS NULL OR a.AllowableClasses = 0 OR (a.AllowableClasses & %u) <> 0) "
        "AND c.map IN (0, 1, 530, 571, 870, 1116, 1220) AND c.PhaseId = 0 ORDER BY RAND() LIMIT 1",
        level > 1 ? level - 1 : 0, level + 1, raceMask, classMask);

    if (!result)
    {
        outSummary = "kein passender Questgeber fuer Stufe " + std::to_string(level) + " (Volk/Klasse) gefunden";
        return false;
    }

    Field* fields = (*result).Fetch();
    uint32 const mapId = fields[0].GetUInt16();
    float const x = fields[1].GetFloat(), y = fields[2].GetFloat(), z = fields[3].GetFloat() + 0.5f, o = fields[4].GetFloat();
    bool ok = TeleportBot(accountId, mapId, x, y, z, o);

    std::ostringstream out;
    out << "Stufe " << level << " -> Map " << mapId << " (" << std::fixed << std::setprecision(0) << x << ", " << y << ", " << z
        << "), Questgeber-Entry " << fields[5].GetUInt32() << (ok ? "" : " - Teleport fehlgeschlagen");
    outSummary = out.str();
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// OI-023 (04.10.2026): LFR mit vorgebauter Bot-Raidgruppe (siehe BotMgr.h). Idee aus dem Konzept des 3.3.5-Moduls
// (bedarfsgetriebene Anmeldung nur dort, wo ein echter Spieler wartet), erweitert um Rollen-Luecken-Rechnung und eine
// vorgebaute Gruppe als EIN Queue-Eintrag (vermeidet die Matcher-Last von 24 Einzel-Bots und Rollenueberschuss).
// ---------------------------------------------------------------------------------------------------
namespace
{
    uint8 BotLfgRoleMask(Player const* player)
    {
        if (ChrSpecializationEntry const* spec = sChrSpecializationStore.LookupEntry(player->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID)))
        {
            if (spec->Role == 0)
                return lfg::PLAYER_ROLE_TANK;
            if (spec->Role == 1)
                return lfg::PLAYER_ROLE_HEALER;
        }
        return lfg::PLAYER_ROLE_DAMAGE;
    }
}

bool BotMgr::QueueBotRaidGroup(uint32 dungeonId, uint32 teamId, uint8 realTanks, uint8 realHealers, uint8 realDamage, std::string& outSummary)
{
    using namespace lfg;

    LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(dungeonId);
    if (!dungeon || dungeon->subtype != LFG_SUBTYPE_LFR)
    {
        outSummary = "kein LFR-Fluegel";
        return false;
    }

    lfg::LfgQueueRoleCount const roleCount = LFGMgr::GetRoleCountByQueueId(dungeonId);
    int const need[3] =
    {
        std::max(0, int(roleCount.maxTanks) - int(realTanks)),
        std::max(0, int(roleCount.maxHealers) - int(realHealers)),
        std::max(0, int(roleCount.maxDamages) - int(realDamage))
    };

    struct Candidate { uint32 Account; Player* Bot; };
    std::vector<Candidate> pool[3]; // 0 Tank, 1 Heiler, 2 Schaden
    uint32 rejNotInWorld = 0, rejFiller = 0, rejDeadGroupMap = 0, rejTeamLevel = 0, rejIlvl = 0, rejLfgState = 0;
    for (auto& [accountId, entry] : _botSessions)
    {
        if (!entry.Session || entry.State != BotCharacterState::STATE_IN_WORLD)
        {
            ++rejNotInWorld;
            continue;
        }
        if (_lfgFillerBotAccountIds.count(accountId) || _lfgTestRealAccounts.count(accountId))
        {
            ++rejFiller;
            continue;
        }
        Player* bot = entry.Session->GetPlayer();
        if (bot && bot->IsInWorld() && !bot->GetGroup() && (!bot->IsAlive() || bot->GetMap()->Instanceable()))
        {
            // Pool bots are saved as ghosts or inside an instance (199 of 253 were dead/ghost, 22 on map 1520) and never became candidates.
            // Revive them and move them to their faction's capital; they qualify at a later demand tick (spec Raid-Pool-Konten).
            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
            }
            if (bot->GetTeamId() == TEAM_ALLIANCE)
                bot->TeleportTo(0, -8833.4f, 628.6f, 94.0f, 1.1f);
            else
                bot->TeleportTo(1, 1629.4f, -4373.4f, 31.3f, 3.5f);
            ++rejDeadGroupMap;
            continue;
        }
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetGroup() || bot->GetMap()->Instanceable())
        {
            ++rejDeadGroupMap;
            continue;
        }
        if (bot->GetTeamId() != TeamId(teamId) || bot->getLevel() < dungeon->minlevel)
        {
            ++rejTeamLevel;
            continue;
        }
        if (bot->GetAverageItemLevelEquipped() < float(dungeon->requiredItemLevel))
        {
            ++rejIlvl;
            continue;
        }
        if (sLFGMgr->GetState(bot->GetGUID()) != LFG_STATE_NONE)
        {
            ++rejLfgState;
            continue;
        }
        uint8 const mask = BotLfgRoleMask(bot);
        pool[mask == PLAYER_ROLE_TANK ? 0 : (mask == PLAYER_ROLE_HEALER ? 1 : 2)].push_back({ accountId, bot });
    }

    // Role gaps: switch damage bots of classes that have a healer/tank specialization (random specs left the pool with 0-2 healers of 253 bots)
    for (int r : { 1, 0 })
    {
        int missing = need[r] - int(pool[r].size());
        for (size_t i = 0; i < pool[2].size() && missing > 0;)
        {
            Player* b = pool[2][i].Bot;
            ChrSpecializationEntry const* target = nullptr;
            for (ChrSpecializationEntry const* s : sChrSpecializationStore)
                if (s->ClassID == b->getClass() && int(s->Role) == r)
                {
                    target = s;
                    break;
                }
            if (!target)
            {
                ++i;
                continue;
            }
            b->ActivateTalentGroup(target);
            pool[r].push_back(pool[2][i]);
            pool[2].erase(pool[2].begin() + i);
            --missing;
        }
    }

    std::ostringstream have;
    have << "Kandidaten Tank " << pool[0].size() << "/" << need[0] << ", Heiler " << pool[1].size() << "/" << need[1]
         << ", Schaden " << pool[2].size() << "/" << need[2] << " (Stufe >= " << uint32(dungeon->minlevel) << ", Ilvl >= " << dungeon->requiredItemLevel << ")"
         << " | abgelehnt: nicht in Welt " << rejNotInWorld << ", Fueller/Test " << rejFiller << ", tot/Gruppe/Instanz " << rejDeadGroupMap
         << ", Fraktion/Stufe " << rejTeamLevel << ", Ilvl " << rejIlvl << ", LFG-Zustand " << rejLfgState;
    for (int r = 0; r < 3; ++r)
        if (int(pool[r].size()) < need[r])
        {
            outSummary = "zu wenige geeignete Bots: " + have.str();
            return false;
        }

    // zufaellig je Rolle auswaehlen
    std::vector<Candidate> members[3];
    for (int r = 0; r < 3; ++r)
    {
        std::vector<Candidate>& src = pool[r];
        for (int i = 0; i < need[r]; ++i)
        {
            uint32 idx = urand(0, uint32(src.size() - 1));
            members[r].push_back(src[idx]);
            src.erase(src.begin() + idx);
        }
    }

    // Anfuehrer: bevorzugt ein Schadens-Bot, sonst Heiler, sonst Tank
    Candidate leader{ 0, nullptr };
    for (int r : { 2, 1, 0 })
        if (!members[r].empty())
        {
            leader = members[r].front();
            members[r].erase(members[r].begin());
            break;
        }
    if (!leader.Bot)
    {
        outSummary = "keine Mitglieder noetig (Rollenluecke 0)";
        return false;
    }

    Group* group = new Group();
    if (!group->Create(leader.Bot))
    {
        delete group;
        outSummary = "Group::Create fehlgeschlagen";
        return false;
    }
    sGroupMgr->AddGroup(group);
    group->ConvertToRaid();

    LfrBotGroup tracked;
    tracked.QueueId = dungeonId;
    tracked.Team = teamId;
    tracked.Created = time(nullptr);
    tracked.Accounts.push_back(leader.Account);
    std::vector<std::pair<Candidate, uint8>> others;
    for (int r = 0; r < 3; ++r)
        for (Candidate const& m : members[r])
        {
            if (!group->AddMember(m.Bot))
            {
                TC_LOG_ERROR("scripts.bots", "BotMgr::QueueBotRaidGroup: Group::AddMember(Account %u) fehlgeschlagen - uebersprungen.", m.Account);
                continue;
            }
            tracked.Accounts.push_back(m.Account);
            others.push_back({ m, BotLfgRoleMask(m.Bot) });
        }
    group->BroadcastGroupUpdate();
    tracked.GroupGuid = group->GetGUID();

    // In die Raidfinder-Warteschlange: der Anfuehrer meldet die Gruppe an, danach beantworten die uebrigen Bots den Rollencheck.
    LfgDungeonSet dungeons;
    dungeons.insert(dungeonId);
    sLFGMgr->JoinLfg(leader.Bot, BotLfgRoleMask(leader.Bot), dungeons);
    ObjectGuid const gguid = group->GetGUID();
    for (auto const& [m, mask] : others)
        sLFGMgr->UpdateRoleCheck(gguid, m.Bot->GetGUID(), mask);

    LfgState const state = sLFGMgr->GetState(gguid);
    for (uint32 acc : tracked.Accounts)
        _lfgFillerBotAccountIds.insert(acc);

    std::ostringstream out;
    out << "Raidgruppe mit " << tracked.Accounts.size() << " Bots fuer LFR-Fluegel " << dungeonId << " (" << dungeon->name << ") gebaut, LFG-Zustand der Gruppe "
        << uint32(state) << " (1 Rollencheck, 2 Warteschlange); " << have.str();
    outSummary = out.str();
    _lfrBotGroups.push_back(tracked);
    TC_LOG_INFO("scripts.bots", "BotMgr::QueueBotRaidGroup: %s", outSummary.c_str());
    return true;
}

void BotMgr::HandleLfrDemand()
{
    using namespace lfg;
    struct Demand { uint8 Tanks = 0, Healers = 0, Damage = 0; };
    std::map<std::pair<uint32, uint32>, Demand> demand; // (Fluegel, Fraktion) -> wartende echte Spieler nach Rolle

    for (uint8 team = TEAM_ALLIANCE; team <= TEAM_HORDE; ++team)
    {
        LfgQueueContainer const& queues = sLFGMgr->GetQueuesForTeam(team);
        for (auto const& [queueId, queue] : queues)
        {
            LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(queueId);
            if (!dungeon || dungeon->subtype != LFG_SUBTYPE_LFR)
                continue;
            for (auto const& [candidateGuid, data] : queue.GetQueueDataStore())
                for (auto const& [memberGuid, role] : data.roles)
                {
                    if (IsBotPlayerGuid(memberGuid))
                        continue;
                    Demand& d = demand[{ queueId, uint32(team) }];
                    if (role & PLAYER_ROLE_TANK)
                        ++d.Tanks;
                    else if (role & PLAYER_ROLE_HEALER)
                        ++d.Healers;
                    else
                        ++d.Damage;
                }
        }
    }

    // vorhandene Bot-Gruppen pflegen: Match zustande gekommen -> nicht mehr verfolgen; Spieler weg -> Gruppe aufloesen
    time_t const now = time(nullptr);
    for (auto itr = _lfrBotGroups.begin(); itr != _lfrBotGroups.end();)
    {
        LfgState const state = sLFGMgr->GetState(itr->GroupGuid);
        bool const realWaiting = demand.count({ itr->QueueId, itr->Team }) > 0;
        bool remove = false;
        if (state == LFG_STATE_DUNGEON || state == LFG_STATE_FINISHED_DUNGEON)
            remove = true;
        else if (state == LFG_STATE_NONE && now - itr->Created > 30)
            remove = true; // hat sich selbst aufgeloest (z. B. Match -> neue LFG-Gruppe)
        else if (!realWaiting && now - itr->Created > 60 && (state == LFG_STATE_QUEUED || state == LFG_STATE_ROLECHECK))
        {
            sLFGMgr->LeaveLfg(itr->GroupGuid);
            if (Group* group = sGroupMgr->GetGroupByGUID(itr->GroupGuid))
                group->Disband();
            for (uint32 acc : itr->Accounts)
                _lfgFillerBotAccountIds.erase(acc);
            TC_LOG_INFO("scripts.bots", "BotMgr::HandleLfrDemand: Bot-Raidgruppe fuer Fluegel %u aufgeloest (kein Spieler wartet mehr).", itr->QueueId);
            remove = true;
        }
        itr = remove ? _lfrBotGroups.erase(itr) : std::next(itr);
    }

    // neuer Bedarf: pro (Fluegel, Fraktion) eine Gruppe, hoechstens eine pro Takt, mit Abkuehlzeit nach Fehlschlag
    for (auto const& [key, d] : demand)
    {
        bool tracked = false;
        for (LfrBotGroup const& g : _lfrBotGroups)
            if (g.QueueId == key.first && g.Team == key.second)
                tracked = true;
        if (tracked)
            continue;
        uint64 const failKey = (uint64(key.first) << 8) | key.second;
        auto fail = _lfrFailUntil.find(failKey);
        if (fail != _lfrFailUntil.end() && fail->second > now)
            continue;

        std::string summary;
        if (!QueueBotRaidGroup(key.first, key.second, d.Tanks, d.Healers, d.Damage, summary))
        {
            _lfrFailUntil[failKey] = now + 60;
            // zu wenige Bots eingeloggt -> offline gehaltene Pool-Bots dieser Fraktion nachladen (ein spaeterer Versuch baut die Gruppe)
            if (uint32 started = EnsureLfrPoolOnline(key.second, 30))
                TC_LOG_INFO("scripts.bots", "BotMgr::HandleLfrDemand: %u Raid-Pool-Bots (Fraktion %u) werden eingeloggt.", started, key.second);
            TC_LOG_INFO("scripts.bots", "BotMgr::HandleLfrDemand: Fluegel %u (Fraktion %u): keine Bot-Raidgruppe moeglich - %s", key.first, key.second, summary.c_str());
        }
        break;
    }
}

bool BotMgr::TestQueueBotAsRealPlayer(uint32 accountId, uint32 dungeonId, uint8 roleMask, std::string& outSummary)
{
    using namespace lfg;
    Player* bot = GetBotPlayer(accountId);
    if (!bot || !bot->IsInWorld())
    {
        outSummary = "Bot ist nicht in der Welt";
        return false;
    }
    if (bot->GetGroup())
    {
        outSummary = "Bot ist in einer Gruppe";
        return false;
    }
    _lfgTestRealAccounts.insert(accountId);
    _lfgFillerBotAccountIds.insert(accountId); // nur damit AdvanceLfgFillerBots den Vorschlag fuer den fehlenden Client annimmt
    LfgDungeonSet dungeons;
    dungeons.insert(dungeonId);
    sLFGMgr->JoinLfg(bot, roleMask, dungeons);
    LfgState const state = sLFGMgr->GetState(bot->GetGUID());
    std::ostringstream out;
    out << "Bot '" << bot->GetName() << "' als wartender 'echter Spieler' fuer Fluegel " << dungeonId << " angemeldet, LFG-Zustand " << uint32(state)
        << " (3 = in der Warteschlange)";
    outSummary = out.str();
    return state != LFG_STATE_NONE;
}

std::string BotMgr::LfrStatus() const
{
    std::ostringstream out;
    out << "Vorgebaute LFR-Bot-Gruppen: " << _lfrBotGroups.size();
    for (LfrBotGroup const& g : _lfrBotGroups)
        out << " | Fluegel " << g.QueueId << " Fraktion " << g.Team << " Bots " << g.Accounts.size() << " Zustand "
            << uint32(sLFGMgr->GetState(g.GroupGuid)) << " Alter " << (time(nullptr) - g.Created) << " s";
    out << " | Testkonten als echte Spieler: " << _lfgTestRealAccounts.size() << " | Fueller-Bots: " << _lfgFillerBotAccountIds.size();
    return out.str();
}

uint32 BotMgr::EnsureLfrPoolOnline(uint32 teamId, uint32 maxLogins)
{
    if (teamId > 1)
        return 0;

    if (!_lfrPoolLoaded)
    {
        _lfrPoolLoaded = true;
        // einmaliger, synchroner Lesezugriff (Konten-Praefix LFRBOT, Fraktion ueber das Volk des ersten Charakters)
        // Healer-capable classes (priest, paladin, shaman, monk, druid) are interleaved 1:3 with the others, so any batch of 30 logins
        // contains healers; a plain account-id order delivered only 2 of 5 needed healers per batch and the group never formed
        std::vector<uint32> healerAccounts[2], otherAccounts[2];
        if (QueryResult result = CharacterDatabase.Query("SELECT ch.`account`, ch.`race`, MIN(ch.`class`) FROM `characters` ch JOIN `auth`.`account` a ON a.`id` = ch.`account` "
            "WHERE a.`username` LIKE 'LFRBOT%' GROUP BY ch.`account`, ch.`race` ORDER BY ch.`account`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const accountId = fields[0].GetUInt32();
                uint8 const race = fields[1].GetUInt8();
                uint8 const classId = fields[2].GetUInt8();
                bool const healerCapable = classId == CLASS_PRIEST || classId == CLASS_PALADIN || classId == CLASS_SHAMAN || classId == CLASS_MONK || classId == CLASS_DRUID;
                uint8 const team = Player::TeamForRace(race) == HORDE ? 1 : 0;
                (healerCapable ? healerAccounts[team] : otherAccounts[team]).push_back(accountId);
            } while (result->NextRow());
        }
        for (uint8 team = 0; team < 2; ++team)
        {
            size_t h = 0, o = 0;
            while (h < healerAccounts[team].size() || o < otherAccounts[team].size())
            {
                if (h < healerAccounts[team].size())
                    _lfrPoolAccounts[team].push_back(healerAccounts[team][h++]);
                for (int i = 0; i < 3 && o < otherAccounts[team].size(); ++i)
                    _lfrPoolAccounts[team].push_back(otherAccounts[team][o++]);
            }
        }
        TC_LOG_INFO("scripts.bots", "BotMgr::EnsureLfrPoolOnline: Raid-Pool geladen: Allianz %u, Horde %u Konten.",
            uint32(_lfrPoolAccounts[0].size()), uint32(_lfrPoolAccounts[1].size()));
    }

    uint32 started = 0;
    for (uint32 accountId : _lfrPoolAccounts[teamId])
    {
        if (started >= maxLogins)
            break;
        auto itr = _botSessions.find(accountId);
        if (itr != _botSessions.end() && (itr->second.State == BotCharacterState::STATE_IN_WORLD || itr->second.State != BotCharacterState::STATE_UNINITIALIZED))
            continue; // bereits online oder gerade im Login
        if (RequestBotLoginExistingAccount(accountId))
            ++started;
    }
    return started;
}
// Raid-/Dungeon-Verhalten (04.10.2026, Plan aus RAIDBOTS_RECHERCHE.md, Schritte 2, 7, 8): ~1 s-Tick fuer Bots auf Instanzkarten.
//  - Wipe: tote Bots werden nach 15 s Geist (HandleBotDeath) und am Friedhof wiederbelebt (ReviveBotAtGraveyard); ein Heiler kann vorher
//    per Rezz-Zauber helfen (ProcessBotCombatAI). Danach laufen Folgen/Dungeon-Clear automatisch wieder an.
//  - Folgen (Schritt 8): ist ein ECHTER Spieler in der Gruppe auf derselben Karte, folgen die Bots ihm (Teleport bei > 80 yd).
//  - Dungeon-Clear (Schritt 2): in reinen Bot-Gruppen wird der Modus automatisch aktiviert, sobald die Gruppe auf einer Instanzkarte steht.
void BotMgr::ProcessBotGroupInstance(uint32 accountId, uint32 diff)
{
    auto itr = _botSessions.find(accountId);
    if (itr == _botSessions.end() || !itr->second.Session)
        return;

    BotSessionEntry& e = itr->second;
    e.GroupTickAccumMs += diff;
    if (e.GroupTickAccumMs < 1000)
        return;
    uint32 const elapsed = e.GroupTickAccumMs;
    e.GroupTickAccumMs = 0;

    Player* bot = e.Session->GetPlayer();
    if (!bot || !bot->IsInWorld())
        return;

    Map* map = bot->GetMap();
    Group* group = bot->GetGroup();
    if (!map->IsDungeon() || !group)
    {
        e.DungeonClearAuto = false;
        e.DeadMs = 0;
        return;
    }

    // --- Wipe-Behandlung ---
    if (!bot->IsAlive())
    {
        e.DeadMs += elapsed;
        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            ReviveBotAtGraveyard(accountId);
            e.DeadMs = 0;
            e.DungeonClearAuto = false;
        }
        else if (e.DeadMs >= 15000)
        {
            bool groupFighting = false;
            for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
                if (Player* m = gr->GetSource())
                    if (m->IsAlive() && m->IsInCombat())
                        groupFighting = true;
            if (!groupFighting)
                HandleBotDeath(accountId);
        }
        return;
    }
    e.DeadMs = 0;

    // Diagnose: der Gruppenfuehrer-Bot schreibt alle 15 s eine Zusammenfassung der Gruppe (Lage im Raid ohne Client nachvollziehbar)
    // (der erste lebende Bot der Gruppe schreibt - ist der Fuehrer tot, bliebe das Log sonst stumm)
    Player* statusWriter = nullptr;
    for (GroupReference* gr = group->GetFirstMember(); gr && !statusWriter; gr = gr->next())
        if (Player* m = gr->GetSource())
            if (m->IsAlive() && m->IsInWorld() && m->GetMapId() == bot->GetMapId() && IsBotPlayerGuid(m->GetGUID()))
                statusWriter = m;
    if (bot == statusWriter)
    {
        e.PositionMoveCooldownMs += elapsed;
        if (e.PositionMoveCooldownMs >= 15000)
        {
            e.PositionMoveCooldownMs = 0;
            uint32 alive = 0, dead = 0, fighting = 0, clearing = 0, hasTarget = 0;
            for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
                if (Player* m = gr->GetSource())
                {
                    if (!m->IsAlive()) { ++dead; continue; }
                    ++alive;
                    if (m->IsInCombat()) ++fighting;
                    if (m->GetVictim()) ++hasTarget;
                    uint32 const acc = GetBotAccountIdByGuid(m->GetGUID());
                    if (acc && IsDungeonClearModeActive(acc)) ++clearing;
                }
            Unit* v = bot->GetVictim();
            TC_LOG_INFO("scripts.bots", "BotMgr::RaidStatus: Karte %u, Gruppe %u Bots: %u lebend, %u tot, %u im Kampf, %u mit Ziel, %u im Dungeon-Clear; Leiter bei (%.0f, %.0f, %.0f), Ziel '%s' (Entfernung %.0f).",
                map->GetId(), group->GetMembersCount(), alive, dead, fighting, hasTarget, clearing, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                v ? v->GetName().c_str() : "-", v ? bot->GetDistance(v) : 0.0f);
            // Diagnose Teil 2: Angreifer des Leiters und feindliche Gegner im Umkreis von 60 yd
            std::ostringstream nearList;
            uint32 listed = 0;
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c || !c->IsInWorld() || !c->IsAlive() || !c->IsHostileTo(bot) || bot->GetDistance(c) > 60.0f)
                    continue;
                if (listed++ < 6)
                    nearList << " [" << c->GetName() << " e" << c->GetEntry() << " d" << uint32(bot->GetDistance(c)) << " hp" << uint32(c->GetHealthPct()) << "%" << (c->IsInCombat() ? " Kampf" : "") << " Opfer:" << (c->GetVictim() ? c->GetVictim()->GetName() : std::string("-")) << "]";
            }
            TC_LOG_INFO("scripts.bots", "BotMgr::RaidStatus: Leiter hat %u Angreifer, %u feindliche Gegner <= 60 yd:%s",
                uint32(bot->getAttackers().size()), listed, nearList.str().c_str());

            // Diagnose Teil 3: wie viele Bots stehen in Reichweite des am meisten verletzten kaempfenden Gegners und wirken gerade
            Creature* foe = nullptr;
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (c && c->IsInWorld() && c->IsAlive() && c->IsInCombat() && c->IsHostileTo(bot) && bot->GetDistance(c) <= 80.0f
                    && (!foe || c->GetMaxHealth() > foe->GetMaxHealth()))
                    foe = c;
            }
            if (foe)
            {
                uint32 in10 = 0, in40 = 0, casting = 0, noLos = 0, tanksOnFoe = 0;
                for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
                    if (Player* m = gr->GetSource())
                        if (m->IsAlive())
                        {
                            float const d = m->GetDistance(foe);
                            if (d <= 10.0f) ++in10;
                            if (d <= 40.0f) ++in40;
                            if (m->IsNonMeleeSpellCast(false)) ++casting;
                            if (!m->IsWithinLOSInMap(foe)) ++noLos;
                            if (foe->GetVictim() == m) ++tanksOnFoe;
                        }
                TC_LOG_INFO("scripts.bots", "BotMgr::RaidStatus: Hauptgegner '%s' hp %.1f%%: %u Bots <= 10 yd, %u <= 40 yd, %u wirken gerade, %u ohne Sichtlinie.",
                    foe->GetName().c_str(), foe->GetHealthPct(), in10, in40, casting, noLos);
            }
        }
    }

    // --- Fuehrung bestimmen ---
    // Echter Spieler (naechster/Gruppenleiter, auch der Test-Bot zaehlt hier als Bot) und ob er Tank ist (Spec-Rolle 0).
    Player* real = nullptr;
    for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
    {
        Player* m = gr->GetSource();
        if (!m || !m->IsInWorld() || GetBotAccountIdByGuid(m->GetGUID()) != 0 || m->GetMapId() != bot->GetMapId() || !m->IsAlive())
            continue;
        if (!real || m->GetGUID() == group->GetLeaderGUID())
            real = m;
    }
    auto isTankSpec = [](Player const* p)
    {
        ChrSpecializationEntry const* spec = sChrSpecializationStore.LookupEntry(p->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID));
        return spec && spec->Role == 0;
    };

    auto cfgItr = _groupCfg.find(group->GetGUID());
    GroupBotConfig const* cfg = cfgItr != _groupCfg.end() ? &cfgItr->second : nullptr;
    Player* leadTank = ResolveLeadTank(group, cfg);
    bool const leadChosen = cfg && !cfg->Lead.IsEmpty();
    bool const enabled = cfg && cfg->Enabled != -1 ? cfg->Enabled == 1 : (real == nullptr);

    auto stopOwnClear = [&]()
    {
        if (e.DungeonClearActive && e.DungeonClearAuto)
        {
            e.DungeonClearActive = false;
            e.DungeonClearAuto = false;
        }
    };
    auto followTarget = [&](Player* target)
    {
        // "im Kampf" allein haelt nicht auf: Raid-Bosse setzen per Zone-Kampf ALLE Spieler der Instanz in den Kampfstatus (auch weit
        // entfernte). Nur ein echtes Ziel oder echte Angreifer stoppen das Folgen.
        if (e.DungeonClearActive || !target || target == bot || bot->GetVictim() || !bot->getAttackers().empty())
            return;
        float const dist = bot->GetDistance(target);
        if (dist > 80.0f)
            TeleportBot(accountId, target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() + 0.5f, target->GetOrientation());
        else if (dist > 8.0f && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            StartBotFollow(accountId, target->GetGUID());
    };

    // 1. Ein echter Tank in der Gruppe fuehrt, solange kein Bot-Tank ausdruecklich gewaehlt wurde: Bots folgen ihm, kein eigener Clear.
    if (real && isTankSpec(real) && !leadChosen)
    {
        stopOwnClear();
        followTarget(real);
        return;
    }

    // 2. Ein Bot-Tank fuehrt (reine Bot-Gruppe, "!dc on" oder "!dc lead <Bot>"): nur er laeuft den Dungeon-Clear, alle anderen folgen ihm.
    if (leadTank && enabled)
    {
        if (bot == leadTank)
        {
            if (!e.DungeonClearActive)
            {
                e.DungeonClearAuto = true;
                e.DungeonClearActive = true;
                TC_LOG_INFO("scripts.bots", "BotMgr::ProcessBotGroupInstance: Bot-Tank %u fuehrt die Gruppe auf Karte %u (Modus %u).",
                    accountId, map->GetId(), cfg ? uint32(cfg->Mode) : 0u);
            }
        }
        else
        {
            e.DungeonClearActive = false;
            e.DungeonClearAuto = false;
            followTarget(leadTank);
        }
        return;
    }

    // 3. Echter Nicht-Tank (oder kein Lead aktiv): Bots folgen dem Spieler, bis ihm per Addon ein Bot-Tank die Fuehrung gegeben wird.
    if (real)
    {
        stopOwnClear();
        followTarget(real);
        return;
    }

    // 4. Reine Bot-Gruppe ohne Tank: jeder Bot laeuft den Dungeon-Clear selbst (alter Behelf, Schritt 2)
    if (!e.DungeonClearActive && !e.DungeonClearAuto && !(cfg && cfg->Enabled == 0))
    {
        e.DungeonClearAuto = true;
        e.DungeonClearActive = true;
        TC_LOG_INFO("scripts.bots", "BotMgr::ProcessBotGroupInstance: Account %u - reine Bot-Gruppe ohne Tank auf Instanzkarte %u, Dungeon-Clear automatisch gestartet.",
            accountId, map->GetId());
    }
}

// Fuehrender Bot-Tank: gewaehlter Bot (cfg->Lead), sonst der erste lebende Bot-Tank der Gruppe auf derselben Karte
Player* BotMgr::ResolveLeadTank(Group* group, GroupBotConfig const* cfg) const
{
    if (!group)
        return nullptr;
    Player* fallback = nullptr;
    for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
    {
        Player* m = gr->GetSource();
        if (!m || !m->IsInWorld() || !m->IsAlive() || GetBotAccountIdByGuid(m->GetGUID()) == 0)
            continue;
        if (cfg && !cfg->Lead.IsEmpty() && m->GetGUID() == cfg->Lead)
            return m;
        ChrSpecializationEntry const* spec = sChrSpecializationStore.LookupEntry(m->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID));
        if (!fallback && spec && spec->Role == 0)
            fallback = m;
    }
    return (cfg && !cfg->Lead.IsEmpty()) ? fallback : fallback;
}

bool BotMgr::IsGroupGathered(Player* lead, float radius) const
{
    Group* group = lead->GetGroup();
    if (!group)
        return true;
    uint32 nearCount = 0, total = 0;
    for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
    {
        Player* m = gr->GetSource();
        if (!m || m == lead || !m->IsInWorld() || !m->IsAlive() || m->GetMapId() != lead->GetMapId())
            continue;
        ++total;
        if (lead->GetDistance(m) <= radius)
            ++nearCount;
        // Heiler mit weniger als 50 % Mana: erst rasten lassen
        if (m->GetPowerType() == POWER_MANA && GetBotRole(GetBotAccountIdByGuid(m->GetGUID())) == BotRole::Healer && m->GetPowerPct(POWER_MANA) < 50.0f)
            return false;
    }
    return total == 0 || nearCount * 100 >= total * 80;
}

std::string BotMgr::SetGroupLead(Player* requester, std::string const& botName)
{
    Group* group = requester ? requester->GetGroup() : nullptr;
    if (!group)
        return "[AshDC] Du bist in keiner Gruppe.";
    for (GroupReference* gr = group->GetFirstMember(); gr; gr = gr->next())
    {
        Player* m = gr->GetSource();
        if (!m || GetBotAccountIdByGuid(m->GetGUID()) == 0 || _stricmp(m->GetName().c_str(), botName.c_str()) != 0)
            continue;
        ChrSpecializationEntry const* spec = sChrSpecializationStore.LookupEntry(m->GetUInt32Value(PLAYER_FIELD_CURRENT_SPEC_ID));
        if (!spec || spec->Role != 0)
            return "[AshDC] " + m->GetName() + " ist kein Tank.";
        GroupBotConfig& cfg = _groupCfg[group->GetGUID()];
        cfg.Lead = m->GetGUID();
        cfg.Enabled = 1;
        static char const* const modeNames[] = { "normal", "pack", "leeroy", "combo" };
        return "[AshDC] Fuehrung: " + m->GetName() + ", Modus " + modeNames[cfg.Mode] + ", an.";
    }
    return "[AshDC] Kein Bot mit dem Namen '" + botName + "' in deiner Gruppe.";
}

std::string BotMgr::SetGroupMode(Player* requester, std::string const& modeName)
{
    Group* group = requester ? requester->GetGroup() : nullptr;
    if (!group)
        return "[AshDC] Du bist in keiner Gruppe.";
    uint8 mode;
    if (modeName == "normal") mode = 0;
    else if (modeName == "pack") mode = 1;
    else if (modeName == "leeroy") mode = 2;
    else if (modeName == "combo") mode = 3;
    else return "[AshDC] Unbekannter Modus (normal, pack, leeroy, combo).";
    _groupCfg[group->GetGUID()].Mode = mode;
    return "[AshDC] Pull-Modus: " + modeName + ".";
}

std::string BotMgr::GroupLeadStatus(Player* requester)
{
    Group* group = requester ? requester->GetGroup() : nullptr;
    if (!group)
        return "[AshDC] Du bist in keiner Gruppe.";
    auto itr = _groupCfg.find(group->GetGUID());
    GroupBotConfig const* cfg = itr != _groupCfg.end() ? &itr->second : nullptr;
    Player* lead = ResolveLeadTank(group, cfg);
    static char const* const names[] = { "normal", "pack", "leeroy", "combo" };
    return std::string("[AshDC] Fuehrung: ") + (lead ? lead->GetName() : std::string("-")) + ", Modus " + names[cfg ? cfg->Mode : 0]
        + (cfg && cfg->Enabled == 1 ? ", an" : (cfg && cfg->Enabled == 0 ? ", aus" : ", automatisch")) + ".";
}
uint32 BotMgr::SummonGroupBots(Player* me, uint32& moved)
{
    moved = 0;
    Group* group = me ? me->GetGroup() : nullptr;
    if (!group)
        return 0;
    uint32 total = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == me || !IsBotPlayerGuid(member->GetGUID()))
            continue;
        ++total;
        uint32 const accountId = GetBotAccountIdByGuid(member->GetGUID());
        float const angle = frand(0.0f, 6.2831853f);
        float const dist = frand(1.0f, 4.0f);
        if (accountId && TeleportBot(accountId, me->GetMapId(), me->GetPositionX() + std::cos(angle) * dist,
            me->GetPositionY() + std::sin(angle) * dist, me->GetPositionZ() + 0.5f, me->GetOrientation()))
            ++moved;
    }
    return total;
}
