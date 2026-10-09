/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftServiceAction.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "Channel.h"
#include "ChannelMgr.h"
#include "ChatHelper.h"
#include "Config.h"
#include "Event.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "SpellMgr.h"
#include "TradeData.h"

namespace
{
// Bots on different maps update on different threads: every map below is read and written under this lock
std::mutex lock;

struct Adverts
{
    time_t tried = 0;   // the last LFW attempt, posted or not
    time_t posted = 0;  // the last LFW actually said in Trade
};
std::map<ObjectGuid, Adverts> adverts;  // by bot
time_t factionPosted[3] = {0, 0, 0};    // by TeamId: one LFW per faction per RandomBotCraftAdvertGap

uint32 const crafts[] = {SKILL_ALCHEMY,        SKILL_BLACKSMITHING, SKILL_ENCHANTING,    SKILL_ENGINEERING,
                         SKILL_LEATHERWORKING, SKILL_TAILORING,     SKILL_JEWELCRAFTING, SKILL_INSCRIPTION};

bool IsCraft(uint32 skill) { return std::find(std::begin(crafts), std::end(crafts), skill) != std::end(crafts); }

std::string SkillName(uint32 skill)
{
    SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skill);
    return entry ? entry->name[0] : "";
}

time_t Conf(char const* key, int32 fallback) { return sConfigMgr->GetOption<int32>(key, fallback); }

struct Talk
{
    time_t until = 0;    // the two have talked: the bot trades with this player until then
    time_t replied = 0;  // the bot's last answer to this player
    time_t listed = 0;   // the bot's last recipe list to this player
};
std::map<std::pair<ObjectGuid, ObjectGuid>, Talk> talks;  // by (bot, player)

struct Heard
{
    time_t at = 0;
    uint32 answers = 0;
};
std::map<ObjectGuid, Heard> heard;  // by asker: how many bots answered their last LF line

std::map<std::string, uint32> const professions = {
    {"alchemy", SKILL_ALCHEMY},
    {"alchemist", SKILL_ALCHEMY},
    {"blacksmithing", SKILL_BLACKSMITHING},
    {"blacksmith", SKILL_BLACKSMITHING},
    {"enchanting", SKILL_ENCHANTING},
    {"enchanter", SKILL_ENCHANTING},
    {"engineering", SKILL_ENGINEERING},
    {"engineer", SKILL_ENGINEERING},
    {"leatherworking", SKILL_LEATHERWORKING},
    {"leatherworker", SKILL_LEATHERWORKING},
    {"tailoring", SKILL_TAILORING},
    {"tailor", SKILL_TAILORING},
    {"jewelcrafting", SKILL_JEWELCRAFTING},
    {"jewelcrafter", SKILL_JEWELCRAFTING},
    {"inscription", SKILL_INSCRIPTION},
    {"scribe", SKILL_INSCRIPTION},
};

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

// The profession named by the word at pos ("enchanting", "enchanter:", ...), or 0
uint32 SkillAt(std::string const& lower, size_t pos)
{
    if (pos >= lower.size())
        return 0;

    auto it = professions.find(lower.substr(pos, lower.find_first_of(" :,", pos) - pos));
    return it == professions.end() ? 0 : it->second;
}

// "LF <Prof>..." → the profession's skill, or 0
uint32 ParseLF(std::string const& lower) { return lower.rfind("lf ", 0) == 0 ? SkillAt(lower, 3) : 0; }

// The first |Hspell:<id>| or |Henchant:<id>| link's id, or 0
uint32 SpellIn(std::string const& text)
{
    for (char const* tag : {"Hspell:", "Henchant:"})
        if (size_t pos = text.find(tag); pos != std::string::npos)
            return atoi(text.c_str() + pos + strlen(tag));

    return 0;
}

// A link in an LF line names what the player wants: the bot answers only when it can make it.
// first is the recipe to show first (0 when the line carries no link).
bool Requested(Player* bot, uint32 skill, std::string const& text, uint32& first)
{
    first = 0;
    uint32 const spell = SpellIn(text);
    ItemIds const items = ChatHelper::parseItems(text);
    if (!spell && items.empty())
        return true;

    for (CraftService::Recipe const& recipe : CraftService::Recipes(bot, skill))
    {
        if (spell && recipe.spell->Id == spell)
            first = spell;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !spell; ++i)
            if (recipe.spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM &&
                items.count(recipe.spell->Effects[i].ItemType))
                first = recipe.spell->Id;

        if (first)
            return true;
    }

    return false;
}

bool IsReagent(Player* bot, uint32 item)
{
    for (CraftService::Recipe const& recipe : CraftService::Recipes(bot))
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (recipe.spell->Reagent[i] > 0 && uint32(recipe.spell->Reagent[i]) == item)
                return true;

    return false;
}

void Tell(Player* bot, Player* player, std::string const& text) { bot->Whisper(text, LANG_UNIVERSAL, player); }

std::string Busy()
{
    return PlayerbotTextMgr::instance().GetBotTextOrDefault("trade_busy_now", "I'm kind of busy now", {});
}

// The bot accepts the window; true when that completed the trade
bool Accept(Player* bot)
{
    WorldPacket p;
    p << uint32(0);
    bot->GetSession()->HandleAcceptTradeOpcode(p);
    return !bot->GetTradeData();
}
}  // namespace

bool CraftService::Enabled() { return sConfigMgr->GetOption<bool>("Playerbots.RandomBotCraftService", false); }

std::vector<CraftService::Recipe> CraftService::Recipes(Player* bot, uint32 skill)
{
    std::vector<Recipe> recipes;
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !(info->HasEffect(SPELL_EFFECT_CREATE_ITEM) || info->HasEffect(SPELL_EFFECT_ENCHANT_ITEM)))
            continue;

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            uint32 const line = it->second->SkillLine;
            if (IsCraft(line) && (!skill || line == skill) && bot->HasSkill(line))
            {
                recipes.push_back({line, it->second->TrivialSkillLineRankLow, info});
                break;
            }
        }
    }

    std::sort(recipes.begin(), recipes.end(), [](Recipe const& a, Recipe const& b)
              { return a.rank != b.rank ? a.rank > b.rank : a.spell->Id < b.spell->Id; });
    return recipes;
}

std::string CraftService::Advert(Player* bot, uint32 skill, uint32 firstSpell)
{
    std::vector<Recipe> recipes = Recipes(bot, skill);
    if (recipes.empty())
        return "";

    std::stable_partition(recipes.begin(), recipes.end(),
                          [firstSpell](Recipe const& r) { return r.spell->Id == firstSpell; });

    std::ostringstream out;
    out << "LFW " << SkillName(skill) << ":";
    for (size_t i = 0; i < recipes.size() && i < 3; ++i)
        out << " " << ChatHelper::FormatSpell(recipes[i].spell);

    return out.str();
}

bool SuggestCraftAction::isUseful()
{
    if (!CraftService::Enabled() || !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup())
        return false;

    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    if (!zone || !(zone->flags & AREA_FLAG_CAPITAL))
        return false;

    time_t const now = time(nullptr);
    std::lock_guard<std::mutex> guard(lock);
    return now >= adverts[bot->GetGUID()].tried + Conf("Playerbots.RandomBotCraftAdvertMinutes", 20) * 60 &&
           now >= factionPosted[bot->GetTeamId()] + Conf("Playerbots.RandomBotCraftAdvertGap", 60);
}

bool SuggestCraftAction::Execute(Event /*event*/)
{
    std::vector<uint32> skills;
    for (uint32 skill : crafts)
        if (bot->HasSkill(skill))
            skills.push_back(skill);

    std::string const advert = skills.empty() ? "" : CraftService::Advert(bot, skills[urand(0, skills.size() - 1)]);

    // Trade - City is one channel for the whole faction, which bots join at login; the bot speaks in it only
    // from a capital (isUseful). ponytail: Channel::IsOn is private, so a bot that left it gets the core's
    // "not on channel" notice and the log line overstates; a public membership check would fix the log.
    Channel* trade = nullptr;
    if (ChannelMgr* channels = ChannelMgr::forTeam(bot->GetTeamId()))
        for (auto const& [name, channel] : channels->GetChannels())
            if (channel && channel->GetChannelId() == ChatChannelId::TRADE)
            {
                trade = channel;
                break;
            }

    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(lock);
        adverts[bot->GetGUID()].tried = now;
        if (advert.empty() || !trade)
            return false;

        adverts[bot->GetGUID()].posted = now;
        factionPosted[bot->GetTeamId()] = now;
    }

    trade->Say(bot->GetGUID(), advert, LANG_UNIVERSAL);
    LOG_INFO("playerbots", "[craft] {} advert in {}: {}", bot->GetName(), trade->GetName(), advert);
    return true;
}

std::string CraftService::Command(Player* bot, uint32 type, std::string const& text, Player* from)
{
    if (type != CHAT_MSG_WHISPER && type != CHAT_MSG_CHANNEL)
        return "";

    if (!bot || !from || from == bot || !IsRealPlayer(from) || !Enabled() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return "";

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || botAI->IsOpposing(from))
        return "";

    std::string const lower = Lower(text);
    uint32 const skill = ParseLF(lower);
    uint32 first = 0;
    std::ostringstream out;
    if (type == CHAT_MSG_CHANNEL)
    {
        // A player's "LF <Prof>": up to three bots in the asker's zone that can make it answer
        if (!skill || bot->GetZoneId() != from->GetZoneId() || !bot->HasSkill(skill) ||
            !Requested(bot, skill, text, first))
            return "";

        time_t const now = time(nullptr);
        std::lock_guard<std::mutex> guard(lock);
        if (heard.size() > 4096)
            heard.clear();

        Heard& line = heard[from->GetGUID()];
        if (now > line.at + 10)
            line = {now, 0};

        if (line.answers >= 3)
            return "";

        ++line.answers;
        out << "crafter heard " << skill << " " << first;
        return out.str();
    }

    if (skill)
    {
        Requested(bot, skill, text, first);
        out << "crafter lf " << skill << " " << first;
        return out.str();
    }

    if (lower.rfind("recipes", 0) == 0 && (lower.size() == 7 || lower[7] == ' '))
    {
        out << "crafter recipes " << SkillAt(lower, 8);
        return out.str();
    }

    return "";
}

bool CraftService::Talking(Player* bot, Player* player)
{
    if (!Enabled())
        return false;

    std::lock_guard<std::mutex> guard(lock);
    auto it = talks.find({bot->GetGUID(), player->GetGUID()});
    return it != talks.end() && it->second.until > time(nullptr);
}

bool CraftService::OnTradeStatus(PlayerbotAI* botAI, Player* trader, uint32 status)
{
    Player* bot = botAI->GetBot();
    if (status == TRADE_STATUS_BEGIN_TRADE)
    {
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, trader, sPlayerbotAIConfig.SightDistance))
            bot->SetFacingToObject(trader);

        WorldPacket p;
        bot->GetSession()->HandleBeginTradeOpcode(p);
        return true;
    }

    TradeData* mine = bot->GetTradeData();
    TradeData* theirs = trader->GetTradeData();
    if (!mine || !theirs)
        return false;

    if (status != TRADE_STATUS_TRADE_ACCEPT && !(status == TRADE_STATUS_BACK_TO_TRADE && theirs->IsAccepted()))
        return false;

    // Nothing leaves the bot: its side of the window stays empty
    if (mine->GetMoney())
        return false;

    for (uint32 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
        if (mine->GetItem(TradeSlots(slot)))
            return false;

    std::map<uint32, uint32> given;
    for (uint32 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        if (Item* item = theirs->GetItem(TradeSlots(slot)))
            given[item->GetEntry()] += item->GetCount();

    uint32 const gold = theirs->GetMoney();

    // A tip, alone or with mats for the bot's recipes
    bool gift = gold || !given.empty();
    for (auto const& [entry, count] : given)
        gift = gift && IsReagent(bot, entry);

    if (!gift)
    {
        Tell(bot, trader, "No order: unknown");
        return false;
    }

    if (!Accept(bot))
        return false;

    LOG_INFO("playerbots", "[craft] {} takes a tip of {}c and {} item kinds from {}", bot->GetName(), gold,
             given.size(), trader->GetName());
    return true;
}

bool CraftServiceAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner || !CraftService::Enabled())
        return false;

    std::istringstream in(event.getParam());
    std::string verb;
    uint32 arg = 0, arg2 = 0;
    in >> verb >> arg >> arg2;

    time_t const now = time(nullptr);
    time_t const talkFor = Conf("Playerbots.RandomBotCraftTalkMinutes", 10) * 60;
    bool open = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (talks.size() > 4096)
            for (auto it = talks.begin(); it != talks.end();)
                it = it->second.until < now ? talks.erase(it) : std::next(it);

        Talk& talk = talks[{bot->GetGUID(), owner->GetGUID()}];
        if (now < talk.replied + 2)  // one answer per pair per 2 s: no loops
            return false;

        if (verb == "heard" && talk.until > now)  // already talking: one answer to their LF is enough
            return false;

        if (verb == "recipes" && talk.until > now && now < talk.listed + 30)  // one list per pair per 30 s
            return false;

        if (verb == "recipes")
            talk.listed = now;

        // The ceremony: the bot answers the player's LF line, or the player answers the bot's advert
        auto advert = adverts.find(bot->GetGUID());
        bool const advertised = advert != adverts.end() && now < advert->second.posted + talkFor;
        open = talk.until > now || verb == "heard" || (verb == "lf" && advertised);
        talk.replied = now;
        if (open)
            talk.until = now + talkFor;
    }

    if (!open)
    {
        Tell(bot, owner, Busy());
        LOG_INFO("playerbots", "[craft] {} turns away {}: {} without a talk", bot->GetName(), owner->GetName(), verb);
        return true;
    }

    if (verb == "heard" || verb == "lf")
    {
        std::string const advert = bot->HasSkill(arg) ? CraftService::Advert(bot, arg, arg2) : "";
        Tell(bot, owner, advert.empty() ? Busy() : advert);
        LOG_INFO("playerbots", "[craft] {} answers {}'s {}: {}", bot->GetName(), owner->GetName(),
                 verb == "lf" ? "whisper" : "LF line", advert);
        return true;
    }

    if (verb == "recipes")
    {
        // "Recipes <Prof> <i>/<n>: [r] [r] [r]", every page, at most 10 a profession (its 30 best)
        std::vector<uint32> skills;
        for (uint32 skill : crafts)
            if ((!arg || arg == skill) && bot->HasSkill(skill))
                skills.push_back(skill);

        if (arg && skills.empty())
            skills.push_back(arg);

        for (uint32 skill : skills)
        {
            std::vector<CraftService::Recipe> const recipes = CraftService::Recipes(bot, skill);
            uint32 const pages = std::min<uint32>((recipes.size() + 2) / 3, 10);
            if (!pages)
                Tell(bot, owner, "Recipes " + SkillName(skill) + " 0/0:");

            for (uint32 page = 0; page < pages; ++page)
            {
                std::ostringstream out;
                out << "Recipes " << SkillName(skill) << " " << page + 1 << "/" << pages << ":";
                for (uint32 i = page * 3; i < recipes.size() && i < page * 3 + 3; ++i)
                    out << " " << ChatHelper::FormatSpell(recipes[i].spell);

                Tell(bot, owner, out.str());
            }

            LOG_INFO("playerbots", "[craft] {} lists {} {} recipes for {}", bot->GetName(), recipes.size(),
                     SkillName(skill), owner->GetName());
        }

        return true;
    }

    return false;
}
