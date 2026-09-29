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

/* ScriptData
Name: ollamachat_playerscript_chat
Comment: Ollama-Chat-Modul (Ideenreferenz mod-ollama-chat, siehe README Abschnitt e) und
         OllamaChatMgr.h-Kopfkommentar fuer die volle Begruendung/das Threading-Modell). Hookt
         PlayerScript::OnChat() (Whisper-Ueberladung, liefert Sender UND Empfaenger direkt) - reagiert
         NUR, wenn der Empfaenger ein Bot (BotMgr::IsBotPlayerGuid()) und der Sender KEIN Bot ist
         (verhindert Bot-antwortet-Bot-Endlosschleifen).
Name: ollamachat_worldscript_tick
Comment: Laedt die OllamaChat.*-Konfiguration bei Start/'.reload config' und leert JEDEN World-Tick die
         Antwort-Queue (OllamaChatMgr::DeliverPendingReplies()) - siehe dort fuer das
         Thread-Sicherheits-Modell (Player::Whisper() darf nur hier, auf dem World-Update-Thread,
         aufgerufen werden).
Category: custom
EndScriptData */

#include "ScriptMgr.h"
#include "Player.h"
#include "BotMgr.h"
#include "OllamaChatMgr.h"
#include "SharedDefines.h"

class ollamachat_playerscript_chat : public PlayerScript
{
    public:
        ollamachat_playerscript_chat() : PlayerScript("ollamachat_playerscript_chat") { }

        void OnChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (!sOllamaChatMgr->IsEnabled() || !receiver)
                return;

            // Kollisions-Fix (Runde 6, gefunden bei der modulweiten Durchsicht): eine an einen Bot
            // gerichtete ADDON-Whisper-Nachricht (lang==LANG_ADDON, siehe bot_dungeonclear_control.cpp)
            // loest DENSELBEN PlayerScript::OnChat()-Whisper-Hook aus wie eine normale Spieler-Whisper -
            // ohne diesen Filter wuerde z.B. "ASHDC:CMD:ON" versehentlich an die Ollama-LLM als
            // Chat-Text weitergereicht und eine sinnlose In-Charakter-Antwort erzeugen. Nur echte,
            // sichtbare Spieler-Whispers (lang!=LANG_ADDON) sollen eine Ollama-Antwort ausloesen.
            if (lang == LANG_ADDON)
                return;

            // Nur reagieren, wenn EXAKT der Empfaenger ein Bot ist und der Absender KEIN Bot - sonst
            // wuerden zwei Bots (oder ein Bot und sich selbst) sich gegenseitig unbegrenzt Whispers
            // schicken, sobald einer per Ollama antwortet (die Antwort selbst loest wieder OnChat() aus).
            if (!sBotMgr->IsBotPlayerGuid(receiver->GetGUID()) || sBotMgr->IsBotPlayerGuid(player->GetGUID()))
                return;

            sOllamaChatMgr->RequestBotReply(receiver, player, msg);
        }
};

class ollamachat_worldscript_tick : public WorldScript
{
    public:
        ollamachat_worldscript_tick() : WorldScript("ollamachat_worldscript_tick") { }

        void OnConfigLoad(bool /*reload*/) override
        {
            sOllamaChatMgr->LoadConfig();
        }

        void OnUpdate(uint32 /*diff*/) override
        {
            sOllamaChatMgr->DeliverPendingReplies();
        }
};

void AddSC_ollamachat_scripts()
{
    new ollamachat_playerscript_chat();
    new ollamachat_worldscript_tick();
}
