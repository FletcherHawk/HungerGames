/*
 * Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#include "Common.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChannelMgr.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Language.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "Util.h"
#include "ScriptMgr.h"
#include "AccountMgr.h"
#include "HG_Game.h"
#include "BattlegroundMgr.h"
#include <iomanip>

void WorldSession::HandleMessagechatOpcode(WorldPacket& recvData)
{
    uint32 type;
    uint32 lang;

    recvData >> type;
    recvData >> lang;

    if (type >= MAX_CHAT_MSG_TYPE)
    {
        TC_LOG_ERROR("network", "CHAT: Wrong message type received: %u", type);
        recvData.rfinish();
        return;
    }

    if (lang == LANG_UNIVERSAL && type != CHAT_MSG_AFK && type != CHAT_MSG_DND)
    {
        TC_LOG_ERROR("network", "CMSG_MESSAGECHAT: Possible hacking-attempt: %s tried to send a message in universal language", GetPlayerInfo().c_str());
        SendNotification(LANG_UNKNOWN_LANGUAGE);
        recvData.rfinish();
        return;
    }

    Player* sender = GetPlayer();

    //TC_LOG_DEBUG("CHAT: packet received. type %u, lang %u", type, lang);

    // prevent talking at unknown language (cheating)
    LanguageDesc const* langDesc = GetLanguageDescByID(lang);
    if (!langDesc)
    {
        SendNotification(LANG_UNKNOWN_LANGUAGE);
        recvData.rfinish();
        return;
    }

    if (langDesc->skill_id != 0 && !sender->HasSkill(langDesc->skill_id))
    {
        // also check SPELL_AURA_COMPREHEND_LANGUAGE (client offers option to speak in that language)
        Unit::AuraEffectList const& langAuras = sender->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE);
        bool foundAura = false;
        for (Unit::AuraEffectList::const_iterator i = langAuras.begin(); i != langAuras.end(); ++i)
        {
            if ((*i)->GetMiscValue() == int32(lang))
            {
                foundAura = true;
                break;
            }
        }
        if (!foundAura)
        {
            SendNotification(LANG_NOT_LEARNED_LANGUAGE);
            recvData.rfinish();
            return;
        }
    }
    if (lang == LANG_ADDON)
    {
        // LANG_ADDON is only valid for the following message types
        std::string msg = "";
        switch (type)
        {
        case CHAT_MSG_PARTY:
        case CHAT_MSG_RAID:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_BATTLEGROUND:
        case CHAT_MSG_WHISPER:
            // Do not handle stuff here

            // Disabled addon channel?
            if (!sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL))
                return;
            break;
        default:
            TC_LOG_ERROR("network", "Player %s (GUID: %u) sent a chatmessage with an invalid language/message type combination",
                         GetPlayer()->GetName().c_str(), GetPlayer()->GetGUIDLow());

            recvData.rfinish();
            return;
        }
    }
    // LANG_ADDON should not be changed nor be affected by flood control
    else
    {
        // send in universal language if player in .gmon mode (ignore spell effects)
        if (sender->IsGameMaster())
            lang = LANG_UNIVERSAL;
        else
        {
            Unit::AuraEffectList const& ModLangAuras = sender->GetAuraEffectsByType(SPELL_AURA_MOD_LANGUAGE);
            if (!ModLangAuras.empty())
                lang = ModLangAuras.front()->GetMiscValue();
            else if (HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT))
                lang = LANG_UNIVERSAL;
            else
            {
                switch (type)
                {
                case CHAT_MSG_PARTY:
                case CHAT_MSG_PARTY_LEADER:
                case CHAT_MSG_RAID:
                case CHAT_MSG_RAID_LEADER:
                case CHAT_MSG_RAID_WARNING:
                    // allow two side chat at group channel if two side group allowed
                    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
                        lang = LANG_UNIVERSAL;
                    break;
                case CHAT_MSG_GUILD:
                case CHAT_MSG_OFFICER:
                    // allow two side chat at guild channel if two side guild allowed
                    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
                        lang = LANG_UNIVERSAL;
                    break;
                }
            }
        }

        if (!sender->CanSpeak())
        {
            std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
            SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
            recvData.rfinish(); // Prevent warnings
            return;
        }

        if (type != CHAT_MSG_AFK && type != CHAT_MSG_DND)
            sender->UpdateSpeakTime();
    }

    if (sender->HasAura(1852) && type != CHAT_MSG_WHISPER)
    {
        SendNotification(GetTrinityString(LANG_GM_SILENCE), sender->GetName().c_str());
        recvData.rfinish();
        return;
    }

    std::string to, channel, msg;
    bool ignoreChecks = false;
    switch (type)
    {
    case CHAT_MSG_SAY:
    case CHAT_MSG_EMOTE:
    case CHAT_MSG_YELL:
    case CHAT_MSG_PARTY:
    case CHAT_MSG_PARTY_LEADER:
    case CHAT_MSG_GUILD:
    case CHAT_MSG_OFFICER:
    case CHAT_MSG_RAID:
    case CHAT_MSG_RAID_LEADER:
    case CHAT_MSG_RAID_WARNING:
    case CHAT_MSG_BATTLEGROUND:
    case CHAT_MSG_BATTLEGROUND_LEADER:
        recvData >> msg;
        break;
    case CHAT_MSG_WHISPER:
        recvData >> to;
        recvData >> msg;
        break;
    case CHAT_MSG_CHANNEL:
        recvData >> channel;
        recvData >> msg;
        break;
    case CHAT_MSG_AFK:
    case CHAT_MSG_DND:
        recvData >> msg;
        ignoreChecks = true;
        break;
    }

    if (!ignoreChecks)
    {
        if (msg.empty())
            return;

        if (ChatHandler(this).ParseCommands(msg.c_str()))
            return;

        if (lang != LANG_ADDON)
        {
            // Strip invisible characters for non-addon messages
            if (sWorld->getBoolConfig(CONFIG_CHAT_FAKE_MESSAGE_PREVENTING))
                stripLineInvisibleChars(msg);

            if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_SEVERITY) && !ChatHandler(this).isValidChatMessage(msg.c_str()))
            {
                TC_LOG_ERROR("network", "Player %s (GUID: %u) sent a chatmessage with an invalid link: %s", GetPlayer()->GetName().c_str(),
                             GetPlayer()->GetGUIDLow(), msg.c_str());

                if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_KICK))
                    KickPlayer();

                return;
            }
        }
    }

    switch (type)
    {
    case CHAT_MSG_SAY:
    case CHAT_MSG_EMOTE:
    case CHAT_MSG_YELL:
    {
        // Prevent cheating
        if (!sender->IsAlive())
            return;

        if (sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ))
        {
            SendNotification(GetTrinityString(LANG_SAY_REQ), sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ));
            return;
        }

        if (type == CHAT_MSG_SAY)
            sender->Say(msg, lang);
        else if (type == CHAT_MSG_EMOTE)
            sender->TextEmote(msg);
        else if (type == CHAT_MSG_YELL)
            sender->Yell(msg, lang);
    } break;
    case CHAT_MSG_WHISPER:
    {
        if (!normalizePlayerName(to))
        {
            SendPlayerNotFoundNotice(to);
            break;
        }

        Player* receiver = sObjectAccessor->FindPlayerByName(to);
        if (!receiver || (!receiver->isAcceptWhispers() && receiver->GetSession()->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) && !receiver->IsInWhisperWhiteList(sender->GetGUID())))
        {
            SendPlayerNotFoundNotice(to);
            return;
        }
        if (!sender->IsGameMaster() && sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) && !receiver->IsInWhisperWhiteList(sender->GetGUID()))
        {
            SendNotification(GetTrinityString(LANG_WHISPER_REQ), sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
            return;
        }

        if (GetPlayer()->GetTeam() != receiver->GetTeam() && !HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT) && !receiver->IsInWhisperWhiteList(sender->GetGUID()))
        {
            SendWrongFactionNotice();
            return;
        }

        if (GetPlayer()->HasAura(1852) && !receiver->IsGameMaster())
        {
            SendNotification(GetTrinityString(LANG_GM_SILENCE), GetPlayer()->GetName().c_str());
            return;
        }

        // If player is a Gamemaster and doesn't accept whisper, we auto-whitelist every player that the Gamemaster is talking to
        // We also do that if a player is under the required level for whispers.
        if (receiver->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) ||
                (HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) && !sender->isAcceptWhispers() && !sender->IsInWhisperWhiteList(receiver->GetGUID())))
            sender->AddWhisperWhiteList(receiver->GetGUID());

        GetPlayer()->Whisper(msg, lang, receiver->GetGUID());

        if (lang == LANG_ADDON)
            OnPlayerAddonMessage(sender, msg);
    } break;
    case CHAT_MSG_PARTY:
    case CHAT_MSG_PARTY_LEADER:
    {
        // if player is in battleground, he cannot say to battleground members by /p
        Group* group = GetPlayer()->GetOriginalGroup();
        if (!group)
        {
            group = sender->GetGroup();
            if (!group || group->isBGGroup())
                return;
        }

        if (type == CHAT_MSG_PARTY_LEADER && !group->IsLeader(sender->GetGUID()))
            return;

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, ChatMsg(type), Language(lang), sender, NULL, msg);
        group->BroadcastPacket(&data, false, group->GetMemberGroup(GetPlayer()->GetGUID()));
    } break;
    case CHAT_MSG_GUILD:
    {
        if (GetPlayer()->GetGuildId())
        {
            if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
            {
                sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, guild);

                guild->BroadcastToGuild(this, false, msg, lang == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
            }
        }
    } break;
    case CHAT_MSG_OFFICER:
    {
        if (GetPlayer()->GetGuildId())
        {
            if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
            {
                sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, guild);

                guild->BroadcastToGuild(this, true, msg, lang == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
            }
        }
    } break;
    case CHAT_MSG_RAID:
    {
        // if player is in battleground, he cannot say to battleground members by /ra
        Group* group = GetPlayer()->GetOriginalGroup();
        if (!group)
        {
            group = GetPlayer()->GetGroup();
            if (!group || group->isBGGroup() || !group->isRaidGroup())
                return;
        }

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID, Language(lang), sender, NULL, msg);
        group->BroadcastPacket(&data, false);
    } break;
    case CHAT_MSG_RAID_LEADER:
    {
        // if player is in battleground, he cannot say to battleground members by /ra
        Group* group = GetPlayer()->GetOriginalGroup();
        if (!group)
        {
            group = GetPlayer()->GetGroup();
            if (!group || group->isBGGroup() || !group->isRaidGroup() || !group->IsLeader(sender->GetGUID()))
                return;
        }

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_LEADER, Language(lang), sender, NULL, msg);
        group->BroadcastPacket(&data, false);
    } break;
    case CHAT_MSG_RAID_WARNING:
    {
        Group* group = GetPlayer()->GetGroup();
        if (!group || !group->isRaidGroup() || !(group->IsLeader(GetPlayer()->GetGUID()) || group->IsAssistant(GetPlayer()->GetGUID())) || group->isBGGroup())
            return;

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        //in battleground, raid warning is sent only to players in battleground - code is ok
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, Language(lang), sender, NULL, msg);
        group->BroadcastPacket(&data, false);
    } break;
    case CHAT_MSG_BATTLEGROUND:
    {
        //battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
        Group* group = GetPlayer()->GetGroup();
        if (!group || !group->isBGGroup())
            return;

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, Language(lang), sender, NULL, msg);
        group->BroadcastPacket(&data, false);
    } break;
    case CHAT_MSG_BATTLEGROUND_LEADER:
    {
        // battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
        Group* group = GetPlayer()->GetGroup();
        if (!group || !group->isBGGroup() || !group->IsLeader(GetPlayer()->GetGUID()))
            return;

        sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND_LEADER, Language(lang), sender, NULL, msg);;
        group->BroadcastPacket(&data, false);
    } break;
    case CHAT_MSG_CHANNEL:
    {
        if (!HasPermission(rbac::RBAC_PERM_SKIP_CHECK_CHAT_CHANNEL_REQ))
        {
            if (sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ))
            {
                SendNotification(GetTrinityString(LANG_CHANNEL_REQ), sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ));
                return;
            }
        }

        if (ChannelMgr* cMgr = ChannelMgr::forTeam(sender->GetTeam()))
        {
            if (Channel* chn = cMgr->GetChannel(channel, sender))
            {
                sScriptMgr->OnPlayerChat(sender, type, lang, msg, chn);
                chn->Say(sender->GetGUID(), msg.c_str(), lang);
            }
            break;
        }
    } break;
    case CHAT_MSG_AFK:
    {
        if (!sender->IsInCombat())
        {
            if (sender->isAFK())                       // Already AFK
            {
                if (msg.empty())
                    sender->ToggleAFK();               // Remove AFK
                else
                    sender->autoReplyMsg = msg;        // Update message
            }
            else                                        // New AFK mode
            {
                sender->autoReplyMsg = msg.empty() ? GetTrinityString(LANG_PLAYER_AFK_DEFAULT) : msg;

                if (sender->isDND())
                    sender->ToggleDND();

                sender->ToggleAFK();
            }

            sScriptMgr->OnPlayerChat(sender, type, lang, msg);
            break;
        }
        break;
    }
    case CHAT_MSG_DND:
    {
        if (sender->isDND())                           // Already DND
        {
            if (msg.empty())
                sender->ToggleDND();                   // Remove DND
            else
                sender->autoReplyMsg = msg;            // Update message
        }
        else                                            // New DND mode
        {
            sender->autoReplyMsg = msg.empty() ? GetTrinityString(LANG_PLAYER_DND_DEFAULT) : msg;

            if (sender->isAFK())
                sender->ToggleAFK();

            sender->ToggleDND();
        }

        sScriptMgr->OnPlayerChat(sender, type, lang, msg);
        break;
    }
    default:
        TC_LOG_ERROR("network", "CHAT: unknown message type %u, lang: %u", type, lang);
        break;
    }
}

void WorldSession::OnPlayerAddonMessage(Player* sender, std::string& msg)
{
	// Do not handle stupidly short or long addon messages
	unsigned int length = msg.length();
	if (length < 5 || length > 256)
		return;
	// Split message by \t
	std::string first = "";
	uint32 i = 0;
	for (; i < msg.length(); ++i)
	{
		char c = msg[i];
		if (c == '\t')
			break;
		first.push_back(c);
	}
	std::string second = msg.substr(++i).c_str();

	// Debug
	TC_LOG_INFO("server.debug", "[DEBUG] From %s: %s, %s", sender->GetName().c_str(), first.c_str(), second.c_str());

	// Handle message
	if (first.compare("MAINMENU") == 0)
	{
		// Handle second GetTheGamesAvailable
        if (second.compare("GetTheGamesAvailable") == 0
            && sBattlegroundMgr->bgDataStore.find(BATTLEGROUND_HG_1) != sBattlegroundMgr->bgDataStore.end())
		{
			// Send: GAMES-icon-gamename...
			std::stringstream str;

			str << "GAMES";

			for (auto& pair : sBattlegroundMgr->bgDataStore[BATTLEGROUND_HG_1].m_Battlegrounds)
			{
				BattlegroundStatus status = pair.second->GetStatus();
				if (status != STATUS_WAIT_LEAVE)
				{
					str << (status >= STATUS_WAIT_JOIN ? "-2-" : "-1-");
                    str << ((HG_Game *)pair.second)->GetGameName();
				}
			}

			SendAddonMessage(sender, str.str(), 2);
		}
	}
	else if (first.compare("CREATEGAME") == 0)
	{
		// second = game name
		// Handle second contains game name
		if (second.length() < 3)
		{
			sWorld->SendServerMessage(SERVER_MSG_STRING, "Game name is too short!", sender);
			return;
		}
		// Filter characters that could cause bugs
		for (unsigned int i = 0; i < second.length(); ++i)
			if (second[i] == '-')
				second[i] = '_';
		// Check game name doesn't already exist
		for (auto& pair : sBattlegroundMgr->bgDataStore[BATTLEGROUND_HG_1].m_Battlegrounds)
		{
			HG_Game* temp = (HG_Game*)pair.second;
            if (!temp->HasPlayer(sender->GetGUID())) //wat? what has that to do with game name?
			{
				sender->LeaveBattleground(true);
				break;
			}
		}
		// Add BG
		HG_Game* temp = new HG_Game();
		temp->AddPlayer(sender);
		temp->SetHost(sender->GetGUID());
		temp->SetGameName(second, sender->GetGUID());
		temp->SetTypeID(BATTLEGROUND_HG_1);
		temp->SetInstanceID(temp->GetGUID());
		sBattlegroundMgr->AddBattleground(temp);
	}
	else if (first.compare("PLRSLB") == 0)
	{
		// Filter characters that could cause bugs
		for (uint32 i = 0; i < second.length(); ++i)
			if (second[i] == '-')
				second[i] = '_';

		for (auto& pair : sBattlegroundMgr->bgDataStore[BATTLEGROUND_HG_1].m_Battlegrounds)
		{
			HG_Game* temp = (HG_Game*)pair.second;
			if (temp->GetGameName().compare(second) == 0)
			{
				SendAddonMessage(sender, temp->getPlayerNameListStr(), 1);
				return;
			}
		}	
	}
	else if (first.compare("JoinGame") == 0)
	{
		// Filter characters that could cause bugs
		for (uint32 i = 0; i < second.length(); ++i)
		if (second[i] == '-')
			second[i] = '_';
		// Retrieve which game is being requested for
		for (auto& pair : sBattlegroundMgr->bgDataStore[BATTLEGROUND_HG_1].m_Battlegrounds)
		{
			HG_Game* temp = (HG_Game*)pair.second;
			if (temp->GetGameName().compare(second) == 0)
			{
				if (!temp->HasPlayer(sender->GetGUID()))
					temp->AddPlayer(sender);
				else
					sWorld->SendServerMessage(SERVER_MSG_STRING, "Cheat detected, failed to add to game.", sender);
				return;
			}
		}
		sWorld->SendServerMessage(SERVER_MSG_STRING, "Something went wrong trying to join this game!", sender);
	}
	else if (first.compare("SelectTalents") == 0)
	{
		if (second.length() < 8)
			return;
		int32 perks[4];
		std::string talents[4];
		// retrieve talents
		talents[0] = second.substr(0, 2);
		talents[1] = second.substr(2, 2);
		talents[2] = second.substr(4, 2);
		talents[3] = second.substr(6, 2);
		for (int32 i = 0; i < 4; ++i)
		{
			// verify them
			if (!isdigit(talents[i][0]) || !isdigit(talents[i][1]))
				return;
			// Set selected perk
			perks[i] = atoi(talents[i].c_str());
			sender->SetSelectedPerk(i, perks[i]);
		}
		// Save to database
		QueryResult result = CharacterDatabase.PQuery("SELECT COUNT(*) FROM `character_perks` WHERE `GUID` = '%u'", sender->GetGUIDLow());
		uint64 rows = result->Fetch()[0].GetUInt64();
		if (rows == 0)
		{
			CharacterDatabase.DirectPExecute("INSERT INTO `character_perks` VALUES ('%u', '%d', '%d', '%d', '%d')",
				sender->GetGUIDLow(), perks[0], perks[1], perks[2], perks[3]);
		}
		else if (rows == 1)
		{
			CharacterDatabase.DirectPExecute("UPDATE `character_perks` SET `perk1`='%d',`perk2`='%d',`perk3`='%d',`perk4`='%d' WHERE `GUID` = '%u'",
				perks[0], perks[1], perks[2], perks[3], sender->GetGUIDLow());
		}
		else
		{
            TC_LOG_INFO("server.error", "[ERROR]: Character %s has multiple perk records in the database.", sender->GetName().c_str());
		}
	}
}

void WorldSession::SendAddonMessage(Player* player, std::string message, uint32 packet)
{
	uint32 splitLength = 240;
	uint32 splits = ceil((float)message.length() / (float)splitLength);
	uint32 counter = 1;
	for (uint32 i = 0; i < message.length(); i += splitLength)
	{
		std::stringstream send;
		send << std::setfill('0') << std::setw(3) << packet;
		send << std::setw(2) << counter;
		send << std::setw(2) << splits;
        send << message.substr(i, std::min(splitLength, (uint32)(message.length() - i)));
		counter = counter + 1;

		WorldPacket* data = new WorldPacket();
		uint32 messageLength = send.str().length() + 1;
		data->Initialize(SMSG_MESSAGECHAT, 100);
		*data << (uint8)CHAT_MSG_SYSTEM;
		*data << LANG_ADDON;
		*data << player->GetGUID();
		*data << uint32(0);
		*data << player->GetGUID();
		*data << messageLength;
		*data << send.str().c_str();
		*data << uint8(0);
		player->GetSession()->SendPacket(data);

		TC_LOG_INFO("server.debug", "[DEBUG] Sent: %s", send.str().c_str());
	}
}

void WorldSession::HandleEmoteOpcode(WorldPacket& recvData)
{
    if (!GetPlayer()->IsAlive() || GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        return;

    uint32 emote;
    recvData >> emote;
    sScriptMgr->OnPlayerEmote(GetPlayer(), emote);
    GetPlayer()->HandleEmoteCommand(emote);
}

namespace Trinity
{
    class EmoteChatBuilder
    {
        public:
            EmoteChatBuilder(Player const& player, uint32 text_emote, uint32 emote_num, Unit const* target)
                : i_player(player), i_text_emote(text_emote), i_emote_num(emote_num), i_target(target) { }

            void operator()(WorldPacket& data, LocaleConstant loc_idx)
            {
                std::string const name(i_target ? i_target->GetNameForLocaleIdx(loc_idx) : "");
                uint32 namlen = name.size();

                data.Initialize(SMSG_TEXT_EMOTE, 20 + namlen);
                data << i_player.GetGUID();
                data << uint32(i_text_emote);
                data << uint32(i_emote_num);
                data << uint32(namlen);
                if (namlen > 1)
                    data << name;
                else
                    data << uint8(0x00);
            }

        private:
            Player const& i_player;
            uint32        i_text_emote;
            uint32        i_emote_num;
            Unit const*   i_target;
    };
}                                                           // namespace Trinity

void WorldSession::HandleTextEmoteOpcode(WorldPacket& recvData)
{
    if (!GetPlayer()->IsAlive())
        return;

    if (!GetPlayer()->CanSpeak())
    {
        std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
        SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
        return;
    }

    uint32 text_emote, emoteNum;
    uint64 guid;

    recvData >> text_emote;
    recvData >> emoteNum;
    recvData >> guid;

    sScriptMgr->OnPlayerTextEmote(GetPlayer(), text_emote, emoteNum, guid);

    EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(text_emote);
    if (!em)
        return;

    uint32 emote_anim = em->textid;

    switch (emote_anim)
    {
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        case EMOTE_ONESHOT_NONE:
            break;
        default:
            // Only allow text-emotes for "dead" entities (feign death included)
            if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
                break;
            GetPlayer()->HandleEmoteCommand(emote_anim);
            break;
    }

    Unit* unit = ObjectAccessor::GetUnit(*_player, guid);

    CellCoord p = Trinity::ComputeCellCoord(GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::EmoteChatBuilder emote_builder(*GetPlayer(), text_emote, emoteNum, unit);
    Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder > emote_do(emote_builder);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder > > emote_worker(GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), emote_do);
    TypeContainerVisitor<Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder> >, WorldTypeMapContainer> message(emote_worker);
    cell.Visit(p, message, *GetPlayer()->GetMap(), *GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));

    GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DO_EMOTE, text_emote, 0, unit);

    //Send scripted event call
    if (unit && unit->GetTypeId() == TYPEID_UNIT && ((Creature*)unit)->AI())
        ((Creature*)unit)->AI()->ReceiveEmote(GetPlayer(), text_emote);
}

void WorldSession::HandleChatIgnoredOpcode(WorldPacket& recvData)
{
    uint64 iguid;
    uint8 unk;
    //TC_LOG_DEBUG("network", "WORLD: Received CMSG_CHAT_IGNORED");

    recvData >> iguid;
    recvData >> unk;                                       // probably related to spam reporting

    Player* player = ObjectAccessor::FindPlayer(iguid);
    if (!player || !player->GetSession())
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_IGNORED, LANG_UNIVERSAL, _player, _player, GetPlayer()->GetName());
    player->GetSession()->SendPacket(&data);
}

void WorldSession::HandleChannelDeclineInvite(WorldPacket &recvPacket)
{
    TC_LOG_DEBUG("network", "Opcode %u", recvPacket.GetOpcode());
}

void WorldSession::SendPlayerNotFoundNotice(std::string const& name)
{
    WorldPacket data(SMSG_CHAT_PLAYER_NOT_FOUND, name.size()+1);
    data << name;
    SendPacket(&data);
}

void WorldSession::SendPlayerAmbiguousNotice(std::string const& name)
{
    WorldPacket data(SMSG_CHAT_PLAYER_AMBIGUOUS, name.size()+1);
    data << name;
    SendPacket(&data);
}

void WorldSession::SendWrongFactionNotice()
{
    WorldPacket data(SMSG_CHAT_WRONG_FACTION, 0);
    SendPacket(&data);
}

void WorldSession::SendChatRestrictedNotice(ChatRestrictionType restriction)
{
    WorldPacket data(SMSG_CHAT_RESTRICTED, 1);
    data << uint8(restriction);
    SendPacket(&data);
}
