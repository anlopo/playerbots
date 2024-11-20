
#include "playerbot/playerbot.h"
#include "SayAction.h"
#include "playerbot/PlayerbotTextMgr.h"
#include "Chat/ChannelMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/AiFactory.h"
#include <regex>
#include <boost/algorithm/string.hpp>
#include "playerbot/PlayerbotLLMInterface.h"

using namespace ai;

std::unordered_set<std::string> noReplyMsgs = {
  "join", "leave", "follow", "attack", "pull", "flee", "reset", "reset ai",
  "all ?", "talents", "talents list", "talents auto", "talk", "stay", "stats",
  "who", "items", "leave", "join", "repair", "summon", "nc ?", "co ?", "de ?",
  "dead ?", "follow", "los", "guard", "do accept invitation", "stats", "react ?",
  "reset strats", "home",
};
std::unordered_set<std::string> noReplyMsgParts = { "+", "-","@" , "follow target", "focus heal", "boost target", "buff target", "revive target", "cast ", "accept [", "e [", "destroy [", "go zone" };

std::unordered_set<std::string> noReplyMsgStarts = { "e ", "accept ", "cast ", "destroy " };

SayAction::SayAction(PlayerbotAI* ai) : Action(ai, "say"), Qualified()
{
}

bool SayAction::Execute(Event& event)
{
    std::string text = "";
    std::map<std::string, std::string> placeholders;
    Unit* target = AI_VALUE(Unit*, "tank target");
    if (!target) target = AI_VALUE(Unit*, "current target");

    // set replace std::strings
    if (target) placeholders["<target>"] = target->GetName();
    placeholders["<randomfaction>"] = IsAlliance(bot->getRace()) ? "Alliance" : "Horde";
    if (qualifier == "low ammo" || qualifier == "no ammo")
    {
        Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (pItem)
        {
            switch (pItem->GetProto()->SubClass)
            {
            case ITEM_SUBCLASS_WEAPON_GUN:
                placeholders["<ammo>"] = "bullets";
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                placeholders["<ammo>"] = "arrows";
                break;
            }
        }
    }

    if (bot->IsInWorld())
    {
        if (AreaTableEntry const* area = GetAreaEntryByAreaID(sServerFacade.GetAreaId(bot)))
            placeholders["<subzone>"] = area->area_name[0];
    }

    // set delay before next say
    time_t lastSaid = AI_VALUE2(time_t, "last said", qualifier);
    uint32 nextTime = time(0) + urand(1, 30);
    ai->GetAiObjectContext()->GetValue<time_t>("last said", qualifier)->Set(nextTime);

    Group* group = bot->GetGroup();
    if (group)
    {
        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            PlayerbotAI* memberAi = member->GetPlayerbotAI();
            if (memberAi) members.push_back(member);
        }

        uint32 count = members.size();
        if (count > 1)
        {
            for (uint32 i = 0; i < count * 5; i++)
            {
                int i1 = urand(0, count - 1);
                int i2 = urand(0, count - 1);

                Player* item = members[i1];
                members[i1] = members[i2];
                members[i2] = item;
            }
        }

        int index = 0;
        for (std::vector<Player*>::iterator i = members.begin(); i != members.end(); ++i)
        {
            PlayerbotAI* memberAi = (*i)->GetPlayerbotAI();
            if (memberAi)
                memberAi->GetAiObjectContext()->GetValue<time_t>("last said", qualifier)->Set(nextTime + (20 * ++index) + urand(1, 15));
        }
    }

    // load text based on chance
    if (!sPlayerbotTextMgr.GetBotText(qualifier, text, placeholders))
        return false;

    if (text.find("/y ") == 0)
        bot->Yell(text.substr(3), (bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH));
    else
        bot->Say(text, (bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH));

    return true;
}


bool SayAction::isUseful()
{
    if (!ai->AllowActivity())
        return false;

    if (ai->HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return false;

    time_t lastSaid = AI_VALUE2(time_t, "last said", qualifier);
    return (time(0) - lastSaid) > 30;
}

void ChatReplyAction::ChatReplyDo(Player* bot, uint32 type, uint32 guid1, uint32 guid2, std::string msg, std::string chanName, std::string name)
{
    // if we're just commanding bots around, don't respond...
    // first one is for exact word matches
    if (noReplyMsgs.find(msg) != noReplyMsgs.end())
    {
        //ostringstream out;
        //out << "DEBUG ChatReplyDo decided to ignore exact blocklist match" << msg;
        //bot->Say(out.str(), LANG_UNIVERSAL);
        return;
    }

    // second one is for partial matches like + or - where we change strats
    if (std::any_of(noReplyMsgParts.begin(), noReplyMsgParts.end(), [&msg](const std::string& part) { return msg.find(part) != std::string::npos; }))
    {
        //ostringstream out;
        //out << "DEBUG ChatReplyDo decided to ignore partial blocklist match" << msg;
        //bot->Say(out.str(), LANG_UNIVERSAL);

        return;
    }

    if (std::any_of(noReplyMsgStarts.begin(), noReplyMsgStarts.end(), [&msg](const std::string& start) {
        return msg.find(start) == 0;  // Check if the start matches the beginning of msg
        }))
    {
        //ostringstream out;
        //out << "DEBUG ChatReplyDo decided to ignore start blocklist match" << msg;
        //bot->Say(out.str(), LANG_UNIVERSAL);
        return;
    }

    ChatChannelSource chatChannelSource = bot->GetPlayerbotAI()->GetChatChannelSource(bot, type, chanName);

    if ((boost::algorithm::istarts_with(msg, "LFG") || boost::algorithm::istarts_with(msg, "LFM"))
        && HandleLFGQuestsReply(bot, chatChannelSource, msg, name))
    {
        return;
    }

    if ((boost::algorithm::istarts_with(msg, "WTB"))
        && HandleWTBItemsReply(bot, chatChannelSource, msg, name))
    {
        return;
    }

    //toxic links
    if (boost::algorithm::istarts_with(msg, sPlayerbotAIConfig.toxicLinksPrefix)
        && (bot->GetPlayerbotAI()->GetChatHelper()->ExtractAllItemIds(msg).size() > 0 || bot->GetPlayerbotAI()->GetChatHelper()->ExtractAllQuestIds(msg).size() > 0))
    {
        HandleToxicLinksReply(bot, chatChannelSource, msg, name);
        return;
    }

    //thunderfury
    if (bot->GetPlayerbotAI()->GetChatHelper()->ExtractAllItemIds(msg).count(19019))
    {
        HandleThunderfuryReply(bot, chatChannelSource, msg, name);
        return;
    }

    if (bot->GetPlayerbotAI() && bot->GetPlayerbotAI()->HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) && chatChannelSource != ChatChannelSource::SRC_UNDEFINED)
    {
        Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, guid1));

        PlayerbotAI* ai = bot->GetPlayerbotAI();
        AiObjectContext* context = ai->GetAiObjectContext();

        std::string llmChannel;

        if (!sPlayerbotAIConfig.llmGlobalContext)
            llmChannel = ((chatChannelSource == ChatChannelSource::SRC_WHISPER) ? name : std::to_string(chatChannelSource));

        std::string llmContext = AI_VALUE(std::string, "manual string::llmcontext" + llmChannel);

        if (player && (player->isRealPlayer() || (sPlayerbotAIConfig.llmBotToBotChatChance && urand(0,99) < sPlayerbotAIConfig.llmBotToBotChatChance)))
        {
            std::string botName = bot->GetName();
            std::string playerName = player->GetName();

            std::map<std::string, std::string> placeholders;
            placeholders["<bot name>"] = botName;
            placeholders["<bot gender>"] = bot->getGender() == GENDER_MALE ? "male" : "female";
            placeholders["<bot level>"] = std::to_string(bot->GetLevel());
            placeholders["<bot class>"] = ai->GetChatHelper()->formatClass(bot->getClass());
            placeholders["<bot race>"] = ai->GetChatHelper()->formatRace(bot->getRace());
            placeholders["<player name>"] = playerName;
            placeholders["<player gender>"] = player->getGender() == GENDER_MALE ? "male" : "female";
            placeholders["<player level>"] = std::to_string(player->GetLevel());
            placeholders["<player class>"] = ai->GetChatHelper()->formatClass(player->getClass());
            placeholders["<player race>"] = ai->GetChatHelper()->formatRace(player->getRace());

#ifdef MANGOSBOT_ZERO
            placeholders["<expansion name>"] = "Vanilla";
#endif
#ifdef MANGOSBOT_ONE
            placeholders["<expansion name>"] = "The Burning Crusade";
#endif
#ifdef MANGOSBOT_TWO
            placeholders["<expansion name>"] = "Wrath of the Lich King";
#endif
            placeholders["<bot zone>"] = WorldPosition(bot).getAreaName();
            placeholders["<bot subzone>"] = WorldPosition(bot).getAreaOverride();

            switch (chatChannelSource)
            {
            case ChatChannelSource::SRC_WHISPER:
            {
                placeholders["<channel name>"] = "in private message";
                break;
            }
            case ChatChannelSource::SRC_SAY:
            {
                placeholders["<channel name>"] = "directly";
                break;
            }
            case ChatChannelSource::SRC_YELL:
            {
                placeholders["<channel name>"] = "with a yell";
                break;
            }
            case ChatChannelSource::SRC_PARTY:
            {
                placeholders["<channel name>"] = "in party chat";
                break;
            }
            case ChatChannelSource::SRC_GUILD:
            {
                placeholders["<channel name>"] = "in guild chat";
            }
            }

            placeholders["<player message>"] = msg;

            std::string startPattern, endPattern, splitPattern;
            startPattern = BOT_TEXT2(sPlayerbotAIConfig.llmResponseStartPattern, placeholders);
            endPattern = BOT_TEXT2(sPlayerbotAIConfig.llmResponseEndPattern, placeholders);
            splitPattern = BOT_TEXT2(sPlayerbotAIConfig.llmResponseSplitPattern, placeholders);

            std::map<std::string, std::string> jsonFill;
            jsonFill["<pre prompt>"] = BOT_TEXT2(sPlayerbotAIConfig.llmPrePrompt, placeholders);
            jsonFill["<prompt>"] = BOT_TEXT2(sPlayerbotAIConfig.llmPrompt, placeholders); 
            jsonFill["<post prompt>"] = BOT_TEXT2(sPlayerbotAIConfig.llmPostPrompt, placeholders);

            uint32 currentLength = jsonFill["<pre prompt>"].size() + jsonFill["<context>"].size() + jsonFill["<prompt>"].size() + llmContext.size();

            PlayerbotLLMInterface::LimitContext(llmContext, currentLength);

            jsonFill["<context>"] = llmContext;

            llmContext += " " + jsonFill["<prompt>"];

            std::string json = BOT_TEXT2(sPlayerbotAIConfig.llmApiJson, jsonFill);

            json = BOT_TEXT2(json, placeholders);

            uint32 type = CHAT_MSG_WHISPER;

            switch (chatChannelSource)
            {
            case ChatChannelSource::SRC_WHISPER:
            {
                type = CHAT_MSG_WHISPER;
                break;
            }
            case ChatChannelSource::SRC_SAY:
            {
                type = CHAT_MSG_SAY;
                break;
            }
            case ChatChannelSource::SRC_YELL:
            {
                type = CHAT_MSG_YELL;
                break;
            }
            case ChatChannelSource::SRC_PARTY:
            {
                type = CHAT_MSG_PARTY;
                break;
            }
            case ChatChannelSource::SRC_GUILD:
            {
                type = CHAT_MSG_GUILD;
            }
            }

            WorldSession* session = bot->GetSession();

            std::future<std::vector<WorldPacket>> futurePackets = std::async([type, playerName, json, startPattern, endPattern, splitPattern] {

                WorldPacket packet_template(CMSG_MESSAGECHAT);

                uint32 lang = LANG_UNIVERSAL;

                packet_template << type;
                packet_template << lang;

                if (type == CHAT_MSG_WHISPER)
                    packet_template << playerName;

                std::string response = PlayerbotLLMInterface::Generate(json);

                std::vector<std::string> lines = PlayerbotLLMInterface::ParseResponse(response, startPattern, endPattern, splitPattern);

                std::vector<WorldPacket> packets;
                for (auto& line : lines)
                {
                    WorldPacket packet(packet_template);
                    packet << line;
                    packets.push_back(packet);
                }

                return packets;  });

            ai->SendDelayedPacket(session, std::move(futurePackets));

        }
        else if(sPlayerbotAIConfig.llmBotToBotChatChance)
        {
            llmContext = llmContext + " " + bot->GetName() + ":" + msg;
            PlayerbotLLMInterface::LimitContext(llmContext, llmContext.size());
        }
        SET_AI_VALUE(std::string, "manual string::llmcontext" + llmChannel, llmContext);

        return;
    }

    SendGeneralResponse(bot, chatChannelSource, GenerateReplyMessage(bot, msg, guid1, name), name);
}

bool ChatReplyAction::HandleThunderfuryReply(Player* bot, ChatChannelSource chatChannelSource, std::string msg, std::string name)
{
    std::map<std::string, std::string> placeholders;
    ItemPrototype const* thunderfuryProto = sObjectMgr.GetItemPrototype(19019);
    placeholders["%thunderfury_link"] = bot->GetPlayerbotAI()->GetChatHelper()->formatItem(thunderfuryProto);

    std::string responseMessage = BOT_TEXT2("thunderfury_spam", placeholders);

    switch (chatChannelSource)
    {
        case ChatChannelSource::SRC_WORLD:
        {
            bot->GetPlayerbotAI()->SayToWorld(responseMessage);
            break;
        }
        case ChatChannelSource::SRC_GENERAL:
        {
            bot->GetPlayerbotAI()->SayToGeneral(responseMessage);
            break;
        }
    }

    bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));

    return true;
}

bool ChatReplyAction::HandleToxicLinksReply(Player* bot, ChatChannelSource chatChannelSource, std::string msg, std::string name)
{
    //quests
    std::vector<uint32> incompleteQuests;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            incompleteQuests.push_back(questId);
    }

    //items
    std::vector<Item*> botItems = bot->GetPlayerbotAI()->GetInventoryAndEquippedItems();

    //spells
    //?

    std::map<std::string, std::string> placeholders;

    placeholders["%random_inventory_item_link"] = botItems.size() > 0 ? bot->GetPlayerbotAI()->GetChatHelper()->formatItem(botItems[rand() % botItems.size()]) : BOT_TEXT("string_empty_link");
    placeholders["%prefix"] = sPlayerbotAIConfig.toxicLinksPrefix;

    if (incompleteQuests.size() > 0)
    {
        Quest const* quest = sObjectMgr.GetQuestTemplate(incompleteQuests[rand() % incompleteQuests.size()]);
        placeholders["%random_taken_quest_or_item_link"] = bot->GetPlayerbotAI()->GetChatHelper()->formatQuest(quest);
    }
    else
    {
        placeholders["%random_taken_quest_or_item_link"] = placeholders["%random_inventory_item_link"];
    }

    placeholders["%my_role"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot, AiFactory::GetPlayerSpecTab(bot));
    AreaTableEntry const* current_area = bot->GetPlayerbotAI()->GetCurrentArea();
    AreaTableEntry const* current_zone = bot->GetPlayerbotAI()->GetCurrentZone();
    placeholders["%area_name"] = current_area ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_area) : BOT_TEXT("string_unknown_area");
    placeholders["%zone_name"] = current_zone ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_zone) : BOT_TEXT("string_unknown_area");
    placeholders["%my_class"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot->getClass());
    placeholders["%my_race"] = bot->GetPlayerbotAI()->GetChatHelper()->formatRace(bot->getRace());
    placeholders["%my_level"] = std::to_string(bot->GetLevel());

    switch (chatChannelSource)
    {
        case ChatChannelSource::SRC_WORLD:
        {
            bot->GetPlayerbotAI()->SayToWorld(BOT_TEXT2("suggest_toxic_links", placeholders));
            break;
        }
        case ChatChannelSource::SRC_GENERAL:
        {
            bot->GetPlayerbotAI()->SayToGeneral(BOT_TEXT2("suggest_toxic_links", placeholders));
            break;
        }
        case ChatChannelSource::SRC_GUILD:
        {
            bot->GetPlayerbotAI()->SayToGuild(BOT_TEXT2("suggest_toxic_links", placeholders));
            break;
        }
    }

    bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 60));

    return true;
}

/*
* @return true if message contained item ids
*/
bool ChatReplyAction::HandleWTBItemsReply(Player* bot, ChatChannelSource chatChannelSource, std::string msg, std::string name)
{
    auto messageItemIds = bot->GetPlayerbotAI()->GetChatHelper()->ExtractAllItemIds(msg);

    if (messageItemIds.empty())
    {
        return false;
    }

    std::set<uint32> matchingItemIds;

    for (auto messageItemId : messageItemIds)
    {
        if (bot->GetPlayerbotAI()->HasItemInInventory(messageItemId))
        {
            matchingItemIds.insert(messageItemId);
        }
    }

    if (!matchingItemIds.empty())
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%other_name"] = name;
        AreaTableEntry const* current_area = bot->GetPlayerbotAI()->GetCurrentArea();
        AreaTableEntry const* current_zone = bot->GetPlayerbotAI()->GetCurrentZone();
        placeholders["%area_name"] = current_area ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_area) : BOT_TEXT("string_unknown_area");
        placeholders["%zone_name"] = current_zone ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_zone) : BOT_TEXT("string_unknown_area");
        placeholders["%my_class"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot->getClass());
        placeholders["%my_race"] = bot->GetPlayerbotAI()->GetChatHelper()->formatRace(bot->getRace());
        placeholders["%my_level"] = std::to_string(bot->GetLevel());
        placeholders["%my_role"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot, AiFactory::GetPlayerSpecTab(bot));
        placeholders["%formatted_item_links"] = "";

        for (auto matchingItemId : matchingItemIds)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(matchingItemId);
            placeholders["%formatted_item_links"] += bot->GetPlayerbotAI()->GetChatHelper()->formatItem(proto, bot->GetPlayerbotAI()->GetInventoryItemsCountWithId(matchingItemId));
            placeholders["%formatted_item_links"] += " ";
        }

        switch (chatChannelSource)
        {
            case ChatChannelSource::SRC_WORLD:
            {
                //may reply to the same channel or whisper
                if (urand(0, 1))
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_channel", placeholders);
                    bot->GetPlayerbotAI()->SayToWorld(responseMessage);
                }
                else
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_whisper", placeholders);
                    bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                }
                break;
            }
            case ChatChannelSource::SRC_GENERAL:
            {
                //may reply to the same channel or whisper
                if (urand(0, 1))
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_channel", placeholders);
                    bot->GetPlayerbotAI()->SayToGeneral(responseMessage);
                }
                else
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_whisper", placeholders);
                    bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                }
                break;
            }
            case ChatChannelSource::SRC_TRADE:
            {
                //may reply to the same channel or whisper
                if (urand(0, 1))
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_channel", placeholders);
                    bot->GetPlayerbotAI()->SayToTrade(responseMessage);
                }
                else
                {
                    std::string responseMessage = BOT_TEXT2("response_wtb_items_whisper", placeholders);
                    bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                }
                break;
            }
        }
        bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 60));
    }

    return true;
}

/*
* @return true if message contained quest ids
*/
bool ChatReplyAction::HandleLFGQuestsReply(Player* bot, ChatChannelSource chatChannelSource, std::string msg, std::string name)
{
    auto messageQuestIds = bot->GetPlayerbotAI()->GetChatHelper()->ExtractAllQuestIds(msg);

    if (messageQuestIds.empty())
    {
        return false;
    }

    auto botQuestIds = bot->GetPlayerbotAI()->GetAllCurrentQuestIds();

    std::set<uint32> matchingQuestIds;
    for (auto botQuestId : botQuestIds)
    {
        if (messageQuestIds.count(botQuestId) != 0)
        {
            matchingQuestIds.insert(botQuestId);
        }
    }

    if (!matchingQuestIds.empty())
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%other_name"] = name;
        AreaTableEntry const* current_area = bot->GetPlayerbotAI()->GetCurrentArea();
        AreaTableEntry const* current_zone = bot->GetPlayerbotAI()->GetCurrentZone();
        placeholders["%area_name"] = current_area ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_area) : BOT_TEXT("string_unknown_area");
        placeholders["%zone_name"] = current_zone ? bot->GetPlayerbotAI()->GetLocalizedAreaName(current_zone) : BOT_TEXT("string_unknown_area");
        placeholders["%my_class"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot->getClass());
        placeholders["%my_race"] = bot->GetPlayerbotAI()->GetChatHelper()->formatRace(bot->getRace());
        placeholders["%my_level"] = std::to_string(bot->GetLevel());
        placeholders["%my_role"] = bot->GetPlayerbotAI()->GetChatHelper()->formatClass(bot, AiFactory::GetPlayerSpecTab(bot));
        placeholders["%quest_links"] = "";
        for (auto matchingQuestId : matchingQuestIds)
        {
            Quest const* quest = sObjectMgr.GetQuestTemplate(matchingQuestId);
            placeholders["%quest_links"] += bot->GetPlayerbotAI()->GetChatHelper()->formatQuest(quest);
        }

        switch (chatChannelSource)
        {
            case ChatChannelSource::SRC_WORLD:
            {
                //may reply to the same channel or whisper
                if (urand(0, 1))
                {
                    std::string responseMessage = BOT_TEXT2(bot->GetGroup() ? "response_lfg_quests_channel_in_group" : "response_lfg_quests_channel", placeholders);
                    bot->GetPlayerbotAI()->SayToWorld(responseMessage);
                }
                else
                {
                    std::string responseMessage = BOT_TEXT2(bot->GetGroup() ? "response_lfg_quests_whisper_in_group" : "response_lfg_quests_whisper", placeholders);
                    bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                }
                break;
            }
            case ChatChannelSource::SRC_GENERAL:
            {
                //may reply to the same channel or whisper
                if (urand(0, 1))
                {
                    std::string responseMessage = BOT_TEXT2(bot->GetGroup() ? "response_lfg_quests_channel_in_group" : "response_lfg_quests_channel", placeholders);
                    bot->GetPlayerbotAI()->SayToGeneral(responseMessage);
                }
                else
                {
                    std::string responseMessage = BOT_TEXT2(bot->GetGroup() ? "response_lfg_quests_whisper_in_group" : "response_lfg_quests_whisper", placeholders);
                    bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                }
                break;
            }
            case ChatChannelSource::SRC_LOOKING_FOR_GROUP:
            {
                //do not reply to the chat
                //may whisper
                std::string responseMessage = BOT_TEXT2(bot->GetGroup() ? "response_lfg_quests_whisper_in_group" : "response_lfg_quests_whisper", placeholders);
                bot->GetPlayerbotAI()->Whisper(responseMessage, name);
                break;
            }
        }
        bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));
    }

    return true;
}

bool ChatReplyAction::SendGeneralResponse(Player* bot, ChatChannelSource chatChannelSource, std::string responseMessage, std::string name)
{
    // send responds
    switch (chatChannelSource)
    {
    case ChatChannelSource::SRC_WORLD:
    {
        //may reply to the same channel or whisper
        bot->GetPlayerbotAI()->SayToWorld(responseMessage);
        break;
    }
    case ChatChannelSource::SRC_GENERAL:
    {
        //may reply to the same channel or whisper
        //bot->GetPlayerbotAI()->SayToGeneral(responseMessage);
        bot->GetPlayerbotAI()->Whisper(responseMessage, name);
        break;
    }
    case ChatChannelSource::SRC_TRADE:
    {
        //do not reply to the chat
        //may whisper
        break;
    }
    case ChatChannelSource::SRC_LOCAL_DEFENSE:
    {
        //may reply to the same channel or whisper
        bot->GetPlayerbotAI()->SayToLocalDefense(responseMessage);
        break;
    }
    case ChatChannelSource::SRC_WORLD_DEFENSE:
    {
        //may reply only if rank 11+ for MANGOSBOT_ZERO, may always reply for others
        //may whisper
        break;
    }
    case ChatChannelSource::SRC_LOOKING_FOR_GROUP:
    {
        //do not reply to the chat
        //may whisper
        break;
    }
    case ChatChannelSource::SRC_GUILD_RECRUITMENT:
    {
        //do not reply to the chat
        //may whisper
        break;
    }
    case ChatChannelSource::SRC_WHISPER:
    {
        bot->GetPlayerbotAI()->Whisper(responseMessage, name);
        break;
    }
    case ChatChannelSource::SRC_SAY:
    {
        bot->GetPlayerbotAI()->Say(responseMessage);
        break;
    }
    case ChatChannelSource::SRC_YELL:
    {
        bot->GetPlayerbotAI()->Yell(responseMessage);
        break;
    }
    case ChatChannelSource::SRC_GUILD:
    {
        bot->GetPlayerbotAI()->SayToGuild(responseMessage);
        break;
    }
    default:
        break;
    }
    bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));

    return true;
}

std::string ChatReplyAction::GenerateReplyMessage(Player* bot, std::string incomingMessage, uint32 guid1, std::string name)
{
    ChatReplyType replyType = REPLY_NOT_UNDERSTAND; // default not understand

    std::string respondsText = "";

    // Chat Logic
    int32 verb_pos = -1;
    int32 verb_type = -1;
    int32 is_quest = 0;
    bool found = false;
    std::stringstream text(incomingMessage);
    std::string segment;
    std::vector<std::string> word;
    while (std::getline(text, segment, ' '))
    {
        word.push_back(segment);
    }

    for (uint32 i = 0; i < 15; i++)
    {
        if (word.size() < i)
            word.push_back("");
    }

    if (incomingMessage.find("?") != std::string::npos)
        is_quest = 1;
    if (word[0].find("what") != std::string::npos)
        is_quest = 2;
    else if (word[0].find("who") != std::string::npos)
        is_quest = 3;
    else if (word[0] == "when")
        is_quest = 4;
    else if (word[0] == "where")
        is_quest = 5;
    else if (word[0] == "why")
        is_quest = 6;

    // Responds
    for (uint32 i = 0; i < 8; i++)
    {
        // blame gm with chat tag
        if (Player* plr = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid1)))
        {
            if (plr->isGMChat())
            {
                replyType = REPLY_ADMIN_ABUSE;
                found = true;
                break;
            }
        }

        if (word[i] == "hi" || word[i] == "hey" || word[i] == "hello" || word[i] == "wazzup")
        {
            replyType = REPLY_HELLO;
            found = true;
            break;
        }

        if (verb_type < 4)
        {
            if (word[i] == "am" || word[i] == "are" || word[i] == "is")
            {
                verb_pos = i;
                verb_type = 2; // present
                if (verb_pos == 0)
                    is_quest = 1;
            }
            else if (word[i] == "will")
            {
                verb_pos = i;
                verb_type = 3; // future
            }
            else if (word[i] == "was" || word[i] == "were")
            {
                verb_pos = i;
                verb_type = 1; // past
            }
            else if (word[i] == "shut" || word[i] == "noob")
            {
                if (incomingMessage.find(bot->GetName()) == std::string::npos)
                {
                    continue; // not react
                    uint32 rnd = urand(0, 2);
                    std::string msg = "";
                    if (rnd == 0)
                        msg = "sorry %s, ill shut up now";
                    if (rnd == 1)
                        msg = "ok ok %s";
                    if (rnd == 2)
                        msg = "fine, i wont talk to you anymore %s";

                    msg = std::regex_replace(msg, std::regex("%s"), name);
                    respondsText = msg;
                    found = true;
                    break;
                }
                else
                {
                    replyType = REPLY_GRUDGE;
                    found = true;
                    break;
                }
            }
        }
    }
    if (verb_type < 4 && is_quest && !found)
    {
        switch (is_quest)
        {
        case 2:
        {
            uint32 rnd = urand(0, 3);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "i dont know what";
                break;
            case 1:
                msg = "i dont know %s";
                break;
            case 2:
                msg = "who cares";
                break;
            case 3:
                msg = "afraid that was before i was around or paying attention";
                break;
            }

            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 3:
        {
            uint32 rnd = urand(0, 4);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "nobody";
                break;
            case 1:
                msg = "we all do";
                break;
            case 2:
                msg = "perhaps its you, %s";
                break;
            case 3:
                msg = "dunno %s";
                break;
            case 4:
                msg = "is it me?";
                break;
            }

            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 4:
        {
            uint32 rnd = urand(0, 6);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "soon perhaps %s";
                break;
            case 1:
                msg = "probably later";
                break;
            case 2:
                msg = "never";
                break;
            case 3:
                msg = "what do i look like, a psychic?";
                break;
            case 4:
                msg = "a few minutes, maybe an hour ... years?";
                break;
            case 5:
                msg = "when? good question %s";
                break;
            case 6:
                msg = "dunno %s";
                break;
            }

            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 5:
        {
            uint32 rnd = urand(0, 6);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "really want me to answer that?";
                break;
            case 1:
                msg = "on the map?";
                break;
            case 2:
                msg = "who cares";
                break;
            case 3:
                msg = "afk?";
                break;
            case 4:
                msg = "none of your buisiness where";
                break;
            case 5:
                msg = "yeah, where?";
                break;
            case 6:
                msg = "dunno %s";
                break;
            }

            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 6:
        {
            uint32 rnd = urand(0, 6);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "dunno %s";
                break;
            case 1:
                msg = "why? just because %s";
                break;
            case 2:
                msg = "why is the sky blue?";
                break;
            case 3:
                msg = "dont ask me %s, im just a bot";
                break;
            case 4:
                msg = "your asking the wrong person";
                break;
            case 5:
                msg = "who knows?";
                break;
            case 6:
                msg = "dunno %s";
                break;
            }
            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        default:
        {
            switch (verb_type)
            {
            case 1:
            {
                uint32 rnd = urand(0, 3);
                std::string msg = "";

                switch (rnd)
                {
                case 0:
                    msg = "its true, " + word[verb_pos + 1] + " " + word[verb_pos] + " " + word[verb_pos + 2] + " " + word[verb_pos + 3] + " " + word[verb_pos + 4] + " " + word[verb_pos + 4];
                    break;
                case 1:
                    msg = "ya %s but thats in the past";
                    break;
                case 2:
                    msg = "nah, but " + word[verb_pos + 1] + " will " + word[verb_pos + 3] + " again though %s";
                    break;
                case 3:
                    msg = "afraid that was before i was around or paying attention";
                    break;
                }
                msg = std::regex_replace(msg, std::regex("%s"), name);
                respondsText = msg;
                found = true;
                break;
            }
            case 2:
            {
                uint32 rnd = urand(0, 6);
                std::string msg = "";

                switch (rnd)
                {
                case 0:
                    msg = "its true, " + word[verb_pos + 1] + " " + word[verb_pos] + " " + word[verb_pos + 2] + " " + word[verb_pos + 3] + " " + word[verb_pos + 4] + " " + word[verb_pos + 5];
                    break;
                case 1:
                    msg = "ya %s thats true";
                    break;
                case 2:
                    msg = "maybe " + word[verb_pos + 1] + " " + word[verb_pos] + " " + word[verb_pos + 2] + " " + word[verb_pos + 3] + " " + word[verb_pos + 4] + " " + word[verb_pos + 5];
                    break;
                case 3:
                    msg = "dunno %s";
                    break;
                case 4:
                    msg = "i dont think so %s";
                    break;
                case 5:
                    msg = "yes";
                    break;
                case 6:
                    msg = "no";
                    break;
                }
                msg = std::regex_replace(msg, std::regex("%s"), name);
                respondsText = msg;
                found = true;
                break;
            }
            case 3:
            {
                uint32 rnd = urand(0, 8);
                std::string msg = "";

                switch (rnd)
                {
                case 0:
                    msg = "dunno %s";
                    break;
                case 1:
                    msg = "beats me %s";
                    break;
                case 2:
                    msg = "how should i know %s";
                    break;
                case 3:
                    msg = "dont ask me %s, im just a bot";
                    break;
                case 4:
                    msg = "your asking the wrong person";
                    break;
                case 5:
                    msg = "what do i look like, a psychic?";
                    break;
                case 6:
                    msg = "sure %s";
                    break;
                case 7:
                    msg = "i dont think so %s";
                    break;
                case 8:
                    msg = "maybe";
                    break;
                }
                msg = std::regex_replace(msg, std::regex("%s"), name);
                respondsText = msg;
                found = true;
                break;
            }
            }
        }
        }
    }
    else if (!found)
    {
        switch (verb_type)
        {
        case 1:
        {
            uint32 rnd = urand(0, 2);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "yeah %s, the key word being " + word[verb_pos] + " " + word[verb_pos + 1];
                break;
            case 1:
                msg = "ya %s but thats in the past";
                break;
            case 2:
                msg = word[verb_pos - 1] + " will " + word[verb_pos + 1] + " again though %s";
                break;
            }
            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 2:
        {
            uint32 rnd = urand(0, 2);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "%s, what do you mean " + word[verb_pos + 1] + "?";
                break;
            case 1:
                msg = "%s, what is a " + word[verb_pos + 1] + "?";
                break;
            case 2:
                msg = "yeah i know " + word[verb_pos ? verb_pos - 1 : verb_pos + 1] + " is a " + word[verb_pos + 1];
                break;
            }
            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        case 3:
        {
            uint32 rnd = urand(0, 1);
            std::string msg = "";

            switch (rnd)
            {
            case 0:
                msg = "are you sure thats going to happen %s?";
                break;
            case 1:
                msg = "%s, what will happen %s?";
                break;
            case 2:
                msg = "are you saying " + word[verb_pos - 1] + " will " + word[verb_pos + 1] + " " + word[verb_pos + 2] + " %s?";
                break;
            }
            msg = std::regex_replace(msg, std::regex("%s"), name);
            respondsText = msg;
            found = true;
            break;
        }
        }
    }

    if (!found)
    {
        // Name Responds
        if (incomingMessage.find(bot->GetName()) != std::string::npos)
        {
            replyType = REPLY_NAME;
            found = true;
        }
        else // Does not understand
        {
            replyType = REPLY_NOT_UNDERSTAND;
            found = true;
        }
    }

    // load text if needed
    if (respondsText.empty())
    {
        respondsText = BOT_TEXT2(replyType, name);
    }

    if (respondsText.size() > 255)
    {
        respondsText.resize(255);
    }

    return respondsText;

}

bool ChatReplyAction::isUseful()
{
    return !ai->HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT);
}
