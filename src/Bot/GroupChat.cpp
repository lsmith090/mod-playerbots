/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GroupChat.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "ChatHelper.h"
#include "Config.h"
#include "Event.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestBots.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
struct Heard
{
    ObjectGuid from;
    bool whisper;
    std::string text;
};

struct Line  // a bot's own LFG/LFM
{
    uint32 quest = 0;
    time_t until = 0;
    uint8 invited = 0;
};

struct Answered  // the answers a speaker's line got
{
    time_t at = 0;
    uint8 answers = 0;
};

struct Answer  // a bot's last answer: whose invite it accepts, for which quest, and its cooldown
{
    ObjectGuid to;
    time_t at = 0;
    uint32 quest = 0;
};

struct Stay  // the group a bot joined on a quest's line: whose, for which quest, and when
{
    ObjectGuid leader;
    uint32 quest = 0;
    time_t at = 0;
};

std::mutex lock;  // Hear runs on the speaker's thread, the rest on the bot's
std::map<ObjectGuid, std::vector<Heard>> inbox;
std::map<ObjectGuid, Line> lines;
std::map<ObjectGuid, Answered> answered;
std::map<ObjectGuid, Answer> answers;
std::map<ObjectGuid, Stay> stays;

time_t const LINE_SECONDS = 180;
time_t const ANSWER_COOLDOWN = 120;
time_t const ACCEPT_SECONDS = 60;
uint8 const ANSWERS_PER_LINE = 2;
time_t const ARRIVE_SECONDS = 300;  // a joiner has this long to walk over before the leave for distance counts

// Yards within which a bot answers a line: near enough to walk to the speaker. 0: anywhere in the zone, as before,
// and nothing else of the hold below
float Near() { return sConfigMgr->GetOption<float>("Playerbots.RandomBotGroupNear", 0.0f); }

bool Form(std::string const& text, char const* form) { return text.rfind(form, 0) == 0; }

// Who joins: a random bot, never a test bot (it holds still where a test parks it)
bool Joins(Player* bot) { return sRandomPlayerbotMgr.IsRandomBot(bot) && !TestBots::IsTestBot(bot); }

void Whisper(PlayerbotAI* botAI, Player* to, Quest const* quest)
{
    botAI->Whisper("inv " + ChatHelper::FormatQuest(quest), to->GetName());
}

// Heard "LFG/LFM <quest>" in a channel: answer with "inv <quest>" when the bot can join
void AnswerLine(PlayerbotAI* botAI, Player* from, Quest const* quest)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || bot->GetGroup() || bot->GetGroupInvite() || bot->InBattleground() ||
        bot->GetMap()->Instanceable() || botAI->GetGrouperType() == GrouperType::SOLO)
        return;

    if (bot->GetZoneId() != from->GetZoneId() || std::abs(int32(bot->GetLevel()) - int32(from->GetLevel())) > 2)
        return;

    // 15,437 joins on 2026-10-13 ended with the quest turned in while grouped 16 times: a bot answered from across
    // the zone, and its own leave at 400 yd took it out before it could walk over.
    float const near = Near();
    if (near > 0.0f && (bot->GetMapId() != from->GetMapId() || bot->GetDistance2d(from) > near))
        return;

    // It holds the quest, or can take it: then only one its leader can share (Logan, 2026-10-13: not having the
    // quest matters only when it isn't shareable)
    uint32 const id = quest->GetQuestId();
    bool const holds = bot->GetQuestStatus(id) == QUEST_STATUS_INCOMPLETE;
    if (!holds && (!bot->CanTakeQuest(quest, false) || (near > 0.0f && !quest->HasFlag(QUEST_FLAGS_SHARABLE))))
        return;

    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = answers.find(bot->GetGUID());
        if (it != answers.end() && now < it->second.at + ANSWER_COOLDOWN)
            return;

        if (urand(0, 1))
            return;

        Answered& line = answered[from->GetGUID()];
        if (now > line.at + 30)
            line = {now, 0};
        if (line.answers >= ANSWERS_PER_LINE)
            return;

        ++line.answers;
        answers[bot->GetGUID()] = {from->GetGUID(), now, id};
    }

    Whisper(botAI, from, quest);
    LOG_INFO("playerbots", "[group] {} answers {}'s line for quest {}", bot->GetName(), from->GetName(), id);
}

// Heard "inv <quest>" whispered: invite a random bot that answered the bot's own line; ask a player to invite
void TakeAnswer(PlayerbotAI* botAI, Player* from, Quest const* quest)
{
    Player* bot = botAI->GetBot();
    uint32 const id = quest->GetQuestId();
    time_t const now = time(nullptr);
    uint8 invited = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lines.find(bot->GetGUID());
        if (it == lines.end() || it->second.quest != id || now > it->second.until)
            return;
        invited = it->second.invited;
    }

    auto sent = [&]()
    {
        std::lock_guard<std::mutex> guard(lock);
        ++lines[bot->GetGUID()].invited;
    };

    if (IsRealPlayer(from))
    {
        // The player leads: a bot never invites a player. It accepts the player's invite at its next update.
        if (bot->GetGroup() || invited >= ANSWERS_PER_LINE)
            return;

        Whisper(botAI, from, quest);
        sent();
        {
            std::lock_guard<std::mutex> guard(lock);
            answers[bot->GetGUID()] = {from->GetGUID(), now, id};
        }
        LOG_INFO("playerbots", "[group] {} asks player {} to invite it for quest {}", bot->GetName(),
                 from->GetName(), id);
        return;
    }

    if (!Joins(from) || from->GetGroup())
        return;

    // Room: its size, less the members and this line's invites still pending (an invite of a bot that is still
    // alone forms the group only when accepted)
    Group* group = bot->GetGroup();
    uint8 const size = std::max<uint8>(2, uint8(botAI->GetGrouperType()));
    uint8 const members = group ? group->GetMembersCount() : 1;
    if (group && (group->GetLeaderGUID() != bot->GetGUID() || group->isRaidGroup()))
        return;
    if (invited >= ANSWERS_PER_LINE || members + invited >= size)
        return;

    WorldPacket p;
    p << from->GetName();
    p << uint32(0);  // roles
    bot->GetSession()->HandleGroupInviteOpcode(p);
    sent();
    LOG_INFO("playerbots", "[group] {} invites {} for quest {}", bot->GetName(), from->GetName(), id);
}

// Joined on a quest's line: take the quest from the leader if the bot lacks it, and remember what the group is for
void Joined(Player* bot, Answer const& answer)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(answer.quest);
    Player* leader = ObjectAccessor::FindConnectedPlayer(answer.to);
    if (!quest || !leader || Near() <= 0.0f)
        return;

    if (bot->GetQuestStatus(answer.quest) == QUEST_STATUS_NONE && quest->HasFlag(QUEST_FLAGS_SHARABLE) &&
        leader->GetQuestStatus(answer.quest) == QUEST_STATUS_INCOMPLETE && bot->CanTakeQuest(quest, false) &&
        bot->CanAddQuest(quest, false))
    {
        bot->AddQuestAndCheckCompletion(quest, leader);
        LOG_INFO("playerbots", "[group] {} takes quest {} from {}", bot->GetName(), answer.quest, leader->GetName());
    }

    std::lock_guard<std::mutex> guard(lock);
    stays[bot->GetGUID()] = {answer.to, answer.quest, time(nullptr)};
}
}  // namespace

bool GroupChat::Holds(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group || Near() <= 0.0f)
        return false;

    std::lock_guard<std::mutex> guard(lock);
    auto stay = stays.find(bot->GetGUID());
    return stay != stays.end() && stay->second.leader == group->GetLeaderGUID() &&
           time(nullptr) < stay->second.at + ARRIVE_SECONDS;
}

bool GroupChat::Follows(Player* bot)
{
    Group* group = bot->GetGroup();
    return group && Near() > 0.0f && Joins(bot) && group->GetLeaderGUID() != bot->GetGUID() && AllRandomBots(group);
}

bool GroupChat::Enabled() { return sConfigMgr->GetOption<bool>("Playerbots.RandomBotGroupChat", false); }

bool GroupChat::AllRandomBots(Group* group)
{
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
        if (!sRandomPlayerbotMgr.IsRandomBot(slot.guid.GetCounter()))
            return false;

    return true;
}

bool GroupChat::Is(uint32 type, std::string const& text)
{
    bool form = false;
    if (type == CHAT_MSG_WHISPER)
        form = Form(text, "inv ");
    else if (type == CHAT_MSG_CHANNEL)
        form = Form(text, "LFG ") || Form(text, "LFM ");

    return form && text.find("|Hquest:") != std::string::npos && Enabled();
}

bool GroupChat::Hear(Player* bot, uint32 type, ObjectGuid from, std::string const& text)
{
    if (!Is(type, text))
        return false;

    if (from == bot->GetGUID())
        return true;

    std::lock_guard<std::mutex> guard(lock);
    std::vector<Heard>& heard = inbox[bot->GetGUID()];
    if (heard.size() < 16)
        heard.push_back({from, type == CHAT_MSG_WHISPER, text});

    return true;
}

void GroupChat::Update(PlayerbotAI* botAI)
{
    if (!Enabled())
        return;

    Player* bot = botAI->GetBot();
    std::vector<Heard> heard;
    Answer pending;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = inbox.find(bot->GetGUID());
        if (it != inbox.end())
        {
            heard.swap(it->second);
            inbox.erase(it);
        }

        auto answer = answers.find(bot->GetGUID());
        if (answer != answers.end())
            pending = answer->second;
    }

    if (!Joins(bot))
        return;

    // One life (#66): the manager logs out only an ungrouped bot, and would revive a grouped one
    if (bot->isDead() && bot->GetGroup() && !bot->InBattleground())
    {
        LOG_INFO("playerbots", "[group] {} leaves its group: dead", bot->GetName());
        botAI->LeaveOrDisbandGroup();
        return;
    }

    // The invite of the one it answered, at once: the engine's packet trigger runs late on a passive bot (#289)
    if (Group* invite = bot->GetGroupInvite())
        if (invite->GetLeaderGUID() == pending.to && time(nullptr) < pending.at + ACCEPT_SECONDS &&
            botAI->DoSpecificAction("accept invitation", Event(), true) && bot->GetGroup())
        {
            LOG_INFO("playerbots", "[group] {} joins the group of {}", bot->GetName(),
                     bot->GetGroup()->GetLeaderName());
            Joined(bot, pending);
        }

    // What a stay was for, said once when it ends: the quest turned in or not, and how long together
    if (!bot->GetGroup())
    {
        Stay stay;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto it = stays.find(bot->GetGUID());
            if (it != stays.end())
            {
                stay = it->second;
                stays.erase(it);
            }
        }

        if (stay.quest)
            LOG_INFO("playerbots", "[group] {} is out of its group after {} s, quest {} {}", bot->GetName(),
                     time(nullptr) - stay.at, stay.quest,
                     bot->GetQuestRewardStatus(stay.quest) ? "turned in" :
                     bot->GetQuestStatus(stay.quest) == QUEST_STATUS_COMPLETE ? "done, not turned in" : "not done");
    }

    for (Heard const& line : heard)
    {
        Player* from = ObjectAccessor::FindConnectedPlayer(line.from);
        if (!from || botAI->IsOpposing(from))
            continue;

        std::set<uint32> const ids = ChatHelper::ExtractAllQuestIds(line.text);
        Quest const* quest = ids.empty() ? nullptr : sObjectMgr->GetQuestTemplate(*ids.begin());
        if (!quest)
            continue;

        if (line.whisper)
            TakeAnswer(botAI, from, quest);
        else
            AnswerLine(botAI, from, quest);
    }
}

bool GroupChat::CanOffer(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    GrouperType const type = botAI->GetGrouperType();
    return Enabled() && Joins(bot) && type >= GrouperType::LEADER_2 && group &&
           group->GetLeaderGUID() == bot->GetGUID() && !group->isRaidGroup() &&
           group->GetMembersCount() < uint8(type) && AllRandomBots(group);
}

bool GroupChat::Ask(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!Enabled() || !Joins(bot) || botAI->GetGrouperType() < GrouperType::LEADER_2)
        return false;

    bool const offer = bot->GetGroup();
    if (offer && !CanOffer(botAI))
        return false;

    std::vector<uint32> quests;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 id = bot->GetQuestSlotQuestId(slot))
            if (bot->GetQuestStatus(id) == QUEST_STATUS_INCOMPLETE)
                quests.push_back(id);

    Quest const* quest = quests.empty() ? nullptr : sObjectMgr->GetQuestTemplate(quests[urand(0, quests.size() - 1)]);
    if (!quest || !botAI->SayToChannel((offer ? "LFM " : "LFG ") + ChatHelper::FormatQuest(quest),
                                                ChatChannelId::GENERAL))
        return false;

    {
        std::lock_guard<std::mutex> guard(lock);
        lines[bot->GetGUID()] = {quest->GetQuestId(), time(nullptr) + LINE_SECONDS, 0};
    }

    LOG_INFO("playerbots", "[group] {} says {} for quest {}", bot->GetName(), offer ? "LFM" : "LFG",
             quest->GetQuestId());
    return true;
}

void BotGroupStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("random", { NextAction("leave far away", 4.0f) }));
    // agent-wow: a follower of a bot leader accepts and turns in at quest givers itself (Playerbots.RandomBotGroupNear)
    triggers.push_back(new TriggerNode("follower quest giver", { NextAction("follower quest giver", 4.0f) }));
}
