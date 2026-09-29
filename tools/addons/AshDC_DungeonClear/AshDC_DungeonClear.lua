-- AshDC_DungeonClear.lua
--
-- Eigenstaendig neu geschriebenes Steuer-Addon fuer den serverseitigen Bot-Dungeon-Clear-Modus
-- (siehe src/server/scripts/Custom/Bots/bot_dungeonclear_control.cpp und BotMgr::SetDungeonClearMode()).
-- Ideenreferenz: https://github.com/jrad7/mod-dungeon-clear-addon (WotLK 3.3.5, AGPL-3.0) - bewusst NICHT
-- kopiert (Lizenz-/Patch-Inkompatibilitaet, siehe README.md Abschnitt e)), sondern von Grund auf neu fuer
-- Client-Build 26972 (Legion 7.3.5) geschrieben und strikt auf das beschraenkt, was der Server aktuell
-- tatsaechlich anbietet (kein "so tun als ob" fuer noch nicht implementierte Server-Funktionen).
--
-- Der Server kennt ZWEI gleichwertige Steuerwege (siehe bot_dungeonclear_control.cpp-Kopfkommentar):
--   1. Ein Party-/Raid-Chat-Schluesselwort ("!dc on"/"!dc off"/"!dc status") - kein Addon-Kanal noetig,
--      funktioniert bereits ohne dieses Addon. Die Slash-Befehle unten senden GENAU diesen Text.
--   2. Eine an einen bestimmten Bot gerichtete Addon-Whisper-Nachricht mit eingebetteter Text-Markierung
--      ("ASHDC:CMD:ON"/"OFF"/"STATUS") - braucht den Namen eines Bots in der eigenen Gruppe als Ziel.
--
-- Dieses Addon bietet fuer beide Wege Slash-Befehle an und zeigt die (immer als normale sichtbare
-- System-/Whisper-Nachricht mit "[AshDC]"-Praefix zurueckkommende) Serverantwort zusaetzlich in einem
-- kleinen, verschiebbaren Status-Fenster an.

local ADDON_PREFIX = "ASHDC"
local STATUS_LINE_PATTERN_TOGGLE = "^%[AshDC%] Dungeon%-Clear%-Modus fuer (%d+) Bot%(s%) deiner Gruppe (%a+)%.?$"
local STATUS_LINE_PATTERN_QUERY  = "^%[AshDC%] Dungeon%-Clear aktiv bei (%d+) Bot%(s%) in deiner Gruppe%.?$"

-- ===================== Status-Fenster (rein informativ, keine Interaktion noetig) =====================

local statusFrame = CreateFrame("Frame", "AshDC_StatusFrame", UIParent, "BackdropTemplate")
statusFrame:SetSize(220, 50)
statusFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 250)
statusFrame:SetMovable(true)
statusFrame:EnableMouse(true)
statusFrame:RegisterForDrag("LeftButton")
statusFrame:SetScript("OnDragStart", statusFrame.StartMoving)
statusFrame:SetScript("OnDragStop", statusFrame.StopMovingOrSizing)
statusFrame:SetBackdrop({
    bgFile = "Interface/Tooltips/UI-Tooltip-Background",
    edgeFile = "Interface/Tooltips/UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 }
})
statusFrame:SetBackdropColor(0, 0, 0, 0.6)
statusFrame:Hide() -- erst sichtbar, sobald der erste Status eintrifft - kein leeres Fenster beim Login

local statusText = statusFrame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
statusText:SetPoint("CENTER", statusFrame, "CENTER", 0, 0)
statusText:SetText("AshDC: kein Status")

local function UpdateStatusFrame(botCount, isActive)
    local stateText = isActive and "|cff40ff40AN|r" or "|cffff4040AUS|r"
    statusText:SetText(string.format("AshDC Dungeon-Clear: %s\n(%d Bot(s))", stateText, botCount))
    statusFrame:Show()
end

-- ===================== Server-Antworten auswerten =====================
-- Beide Steuerwege beantworten Befehle als GEWOEHNLICHEN sichtbaren Chat-Text mit "[AshDC]"-Praefix
-- (Chat-Schluesselwort-Pfad: CHAT_MSG_SYSTEM ueber ChatHandler::PSendSysMessage; Addon-Whisper-Pfad:
-- CHAT_MSG_WHISPER von dem angesprochenen Bot) - bewusst KEIN echtes CHAT_MSG_ADDON-Event, siehe
-- bot_dungeonclear_control.cpp-Kopfkommentar fuer die Begruendung (Prefix wird vom Server-Hook nicht
-- durchgereicht). Dieses Addon erkennt beide Antwortformen ueber dasselbe Text-Muster.

local function HandleIncomingLine(text)
    local count, state = text:match(STATUS_LINE_PATTERN_TOGGLE)
    if count then
        UpdateStatusFrame(tonumber(count), state == "aktiviert")
        return
    end

    count = text:match(STATUS_LINE_PATTERN_QUERY)
    if count then
        UpdateStatusFrame(tonumber(count), tonumber(count) > 0)
    end
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_SYSTEM")
eventFrame:RegisterEvent("CHAT_MSG_WHISPER")
eventFrame:SetScript("OnEvent", function(_, _, message)
    HandleIncomingLine(message)
end)

-- ===================== Addon-Nachrichtenkanal (Weg 2) =====================

RegisterAddonMessagePrefix(ADDON_PREFIX)

local function SendBotCommand(botName, command)
    if not botName or botName == "" then
        print("|cffff4040AshDC:|r Bot-Name fehlt - Syntax: /ashdc whisper <on|off|status> <BotName>")
        return
    end
    SendAddonMessage(ADDON_PREFIX, "ASHDC:CMD:" .. command, "WHISPER", botName)
end

-- ===================== Chat-Schluesselwort-Weg (Weg 1, benoetigt KEIN Addon-Handshake) =====================

local function SendChatKeyword(command)
    local channel = IsInRaid() and "RAID" or (IsInGroup() and "PARTY" or nil)
    if not channel then
        print("|cffff4040AshDC:|r Du bist in keiner Gruppe - der Dungeon-Clear-Modus wirkt gruppenweit.")
        return
    end
    SendChatMessage("!dc " .. command, channel)
end

-- ===================== Slash-Befehle =====================

SLASH_ASHDC1 = "/ashdc"
SlashCmdList["ASHDC"] = function(msg)
    local args = {}
    for word in msg:gmatch("%S+") do
        table.insert(args, word:lower())
    end

    if args[1] == "on" or args[1] == "off" or args[1] == "status" then
        SendChatKeyword(args[1])
        return
    end

    if args[1] == "whisper" and (args[2] == "on" or args[2] == "off" or args[2] == "status") then
        SendBotCommand(args[3], args[2]:upper())
        return
    end

    print("|cff40ff40AshDC Dungeon-Clear-Steuerung|r")
    print("  /ashdc on|off|status               - Party-/Raid-Chat-Schluesselwort (fuer die ganze Gruppe)")
    print("  /ashdc whisper on|off|status <Bot>  - direkte Addon-Nachricht an einen Bot in der Gruppe")
end
