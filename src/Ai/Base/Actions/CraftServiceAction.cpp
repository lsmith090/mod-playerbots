/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftServiceAction.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <mutex>

#include "Channel.h"
#include "ChannelMgr.h"
#include "ChatHelper.h"
#include "Config.h"
#include "DBCStores.h"
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
                         SKILL_LEATHERWORKING, SKILL_TAILORING};

// agent-wow (#87 step 2): the skill from which a bot advertises a craft. By 50 a tailor makes Linen Bags (its trainer's
// rank 45) and a smith its stones (25), the first things a customer orders; 600 adverts a day came from bots at 1
uint32 const ADVERT_SKILL = 50;

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
    time_t next = 0;     // no new order before then
    uint32 order = 0;    // the enchant agreed on
    uint32 tip = 0;      // in copper
    bool paid = false;   // trade 1 is done: the bot holds the mats and the tip
    ObjectGuid warned;   // the last-slot item the bot already explained a refusal for
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

std::string Link(uint32 spell)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    return info ? ChatHelper::FormatSpell(info) : "";
}

std::string Refusal(uint32 spell, char const* reason)
{
    std::string const link = spell ? Link(spell) : "";
    return std::string("No order") + (link.empty() ? "" : " " + link) + ": " + reason;
}

// The mats the core will take from the bot's bags when it casts the enchant
std::map<uint32, uint32> Mats(SpellInfo const* spell)
{
    std::map<uint32, uint32> mats;
    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        if (spell->Reagent[i] > 0 && spell->ReagentCount[i])
            mats[spell->Reagent[i]] += spell->ReagentCount[i];

    return mats;
}

// The tools the core checks at the cast even for the trade's triggered spell: a rod, a hammer
bool HasTools(Player* bot, SpellInfo const* spell)
{
    for (uint8 i = 0; i < 2; ++i)
        if ((spell->Totem[i] && !bot->HasItemCount(spell->Totem[i])) ||
            (spell->TotemCategory[i] && !bot->HasItemTotemCategory(spell->TotemCategory[i])))
            return false;

    return true;
}

// The bot stops crafting for skill while it holds a customer's mats (the earn-professions patches read this)
void Hold(PlayerbotAI* botAI, time_t until)
{
    botAI->GetAiObjectContext()->GetValue<time_t>("last said", "craft order hold")->Set(until);
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

// The profession's trade link, as the 3.3.5 client makes one (#289): every spell of the skill line in
// SkillLineAbility.dbc, by MinSkillLineRank then ID, one bit each, least significant first, six to a base64 digit.
// "" when the bot knows no rank spell of the line.
std::string CraftService::TradeLink(Player* bot, uint32 skill)
{
    std::vector<SkillLineAbilityEntry const*> rows;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        if (SkillLineAbilityEntry const* row = sSkillLineAbilityStore.LookupEntry(i))
            if (row->SkillLine == skill)
                rows.push_back(row);

    std::sort(rows.begin(), rows.end(), [](SkillLineAbilityEntry const* a, SkillLineAbilityEntry const* b)
              { return a->MinSkillLineRank != b->MinSkillLineRank ? a->MinSkillLineRank < b->MinSkillLineRank : a->ID < b->ID; });

    static char const digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string mask((rows.size() + 5) / 6, 'A');
    SpellInfo const* rank = nullptr;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (!bot->HasSpell(rows[i]->Spell))
            continue;

        mask[i / 6] = digits[(strchr(digits, mask[i / 6]) - digits) | (1 << (i % 6))];
        SpellInfo const* info = sSpellMgr->GetSpellInfo(rows[i]->Spell);
        if (info && info->Effects[0].Effect == SPELL_EFFECT_TRADE_SKILL && (!rank || info->Id > rank->Id))
            rank = info;
    }

    if (!rank)
        return "";

    std::ostringstream out;
    out << "|cffffd000|Htrade:" << rank->Id << ":" << bot->GetSkillValue(skill) << ":" << bot->GetMaxSkillValue(skill)
        << ":" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << bot->GetGUID().GetRawValue()
        << std::dec << ":" << mask << "|h[" << rank->SpellName[0] << "]|h|r";
    return out.str();
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

    // The whole list in one trade link, and the recipe asked for after it; three recipes when there is no link
    std::string const link = TradeLink(bot, skill);
    if (!link.empty())
    {
        out << " " << link;
        if (firstSpell && recipes[0].spell->Id == firstSpell)
            out << " " << ChatHelper::FormatSpell(recipes[0].spell);

        return out.str();
    }

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
    // agent-wow (#87 step 2): with the trade session's key on, a bot advertises a craft only from ADVERT_SKILL on; at
    // skill 1 it has nothing a customer would order (an answer to someone who asks, below, isn't gated)
    std::vector<uint32> skills;
    for (uint32 skill : crafts)
        if (bot->HasSkill(skill) && (!sPlayerbotAIConfig.RandomBotTradeSession || bot->GetSkillValue(skill) >= ADVERT_SKILL))
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

    if (lower.rfind("order ", 0) == 0)
    {
        uint32 const spell = SpellIn(text);
        out << "crafter order " << (spell ? spell : uint32(atoi(lower.c_str() + 6)));
        return out.str();
    }

    if (lower.rfind("cancel order", 0) == 0)
        return "crafter cancel";

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
    // Every status the craft service sees, and why it leaves one unanswered (#289)
    TradeData const* const seen = trader->GetTradeData();
    LOG_INFO("playerbots", "[craft] {} hears trade status {} from {} at {} (their side {}, {}c)", bot->GetName(),
             status, trader->GetName(), time(nullptr), !seen ? "closed" : seen->IsAccepted() ? "accepted" : "open",
             seen ? seen->GetMoney() : 0);
    auto quiet = [&](char const* why)
    {
        LOG_INFO("playerbots", "[craft] {} leaves {}'s trade status {} unanswered: {}", bot->GetName(),
                 trader->GetName(), status, why);
        return false;
    };

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
        return quiet("no window");

    if (!theirs->IsAccepted())
        return quiet("their side isn't accepted");

    // Nothing leaves the bot: its side of the window stays empty
    if (mine->GetMoney())
        return quiet("money on the bot's side");

    for (uint32 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
        if (mine->GetItem(TradeSlots(slot)))
            return quiet("an item on the bot's side");

    std::map<uint32, uint32> given;
    for (uint32 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        if (Item* item = theirs->GetItem(TradeSlots(slot)))
            given[item->GetEntry()] += item->GetCount();

    uint32 const gold = theirs->GetMoney();
    time_t const now = time(nullptr);
    time_t const talkFor = Conf("Playerbots.RandomBotCraftTalkMinutes", 10) * 60;
    Talk talk;
    {
        std::lock_guard<std::mutex> guard(lock);
        Talk& stored = talks[{bot->GetGUID(), trader->GetGUID()}];
        stored.until = now + talkFor;
        talk = stored;
    }

    if (talk.paid)  // the hold lasts as long as the talk
        Hold(botAI, now + talkFor);

    SpellInfo const* spell = talk.order ? sSpellMgr->GetSpellInfo(talk.order) : nullptr;
    if (spell && talk.paid)
    {
        // Trade 2: the item in the last slot, the enchant on the window, nothing traded but gold
        if (!given.empty())
        {
            Tell(bot, trader, Refusal(talk.order, "unpaid"));
            return false;
        }

        if (!theirs->GetItem(TRADE_SLOT_NONTRADED) || mine->GetSpell() != talk.order)
            return OnTradeItems(botAI, trader);  // sets it; the player accepts again once they see it

        if (!Accept(bot))
        {
            // The core's CheckCast refused at the accept (item level, reagents) and cleared the spell
            Tell(bot, trader, Refusal(talk.order, "failed"));
            WorldPacket p;
            bot->GetSession()->HandleCancelTradeOpcode(p);
            LOG_INFO("playerbots", "[craft] {} failed {} for {}", bot->GetName(), talk.order, trader->GetName());
            return false;
        }

        {
            std::lock_guard<std::mutex> guard(lock);
            Talk& stored = talks[{bot->GetGUID(), trader->GetGUID()}];
            stored.order = 0;
            stored.paid = false;
            stored.next = now + 30;
        }

        Hold(botAI, 0);
        Tell(bot, trader, "Done " + Link(talk.order));
        LOG_INFO("playerbots", "[craft] {} enchants for {}: {} (tip {}c)", bot->GetName(), trader->GetName(),
                 talk.order, talk.tip + gold);
        return true;
    }

    if (spell)
    {
        // Trade 1: at least the mats and the tip; the item in the last slot, if any, stays with the player
        bool paid = gold >= talk.tip;
        for (auto const& [entry, count] : Mats(spell))
            paid = paid && given[entry] >= count;

        if (!paid)
        {
            Tell(bot, trader, Refusal(talk.order, "unpaid"));
            return false;
        }

        if (!Accept(bot))
            return quiet("trade 1's accept left the window open");

        {
            std::lock_guard<std::mutex> guard(lock);
            Talk& stored = talks[{bot->GetGUID(), trader->GetGUID()}];
            stored.paid = true;
            stored.until = now + talkFor;
        }

        Hold(botAI, now + talkFor);
        Tell(bot, trader, "Paid " + Link(talk.order));
        LOG_INFO("playerbots", "[craft] {} is paid by {} for {}: {}c", bot->GetName(), trader->GetName(),
                 talk.order, gold);
        return true;
    }

    // No order: a tip, alone or with mats for the bot's recipes
    bool gift = gold || !given.empty();
    for (auto const& [entry, count] : given)
        gift = gift && IsReagent(bot, entry);

    if (!gift)
    {
        Tell(bot, trader, "No order: unknown");
        return false;
    }

    if (!Accept(bot))
        return quiet("the tip's accept left the window open");

    LOG_INFO("playerbots", "[craft] {} takes a tip of {}c and {} item kinds from {}", bot->GetName(), gold,
             given.size(), trader->GetName());
    return true;
}

bool CraftService::OnTradeItems(PlayerbotAI* botAI, Player* trader)
{
    Player* bot = botAI->GetBot();
    TradeData* mine = bot->GetTradeData();
    TradeData* theirs = trader->GetTradeData();
    Item* target = theirs ? theirs->GetItem(TRADE_SLOT_NONTRADED) : nullptr;
    if (!Enabled() || !mine || !target)
        return false;

    Talk talk;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = talks.find({bot->GetGUID(), trader->GetGUID()});
        if (it == talks.end() || it->second.until <= time(nullptr) || !it->second.order)
            return false;

        talk = it->second;
    }

    if (mine->GetSpell() == talk.order)  // already set: SetSpell sends the window again, which lands here
        return true;

    SpellInfo const* spell = sSpellMgr->GetSpellInfo(talk.order);
    char const* refusal = nullptr;
    if (!talk.paid)
        refusal = "unpaid";
    else if (!spell || !bot->HasSpell(talk.order) || !target->IsFitToSpellRequirements(spell))
        refusal = "slot";
    else if (!HasTools(bot, spell))
        refusal = "tool";
    else
        for (auto const& [entry, count] : Mats(spell))
            if (!bot->HasItemCount(entry, count))
                refusal = "mats";

    if (refusal)
    {
        bool tell = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            Talk& stored = talks[{bot->GetGUID(), trader->GetGUID()}];
            tell = stored.warned != target->GetGUID();
            stored.warned = target->GetGUID();
        }

        if (tell)
            Tell(bot, trader, Refusal(talk.order, refusal));

        return true;
    }

    mine->SetSpell(talk.order);
    LOG_INFO("playerbots", "[craft] {} puts {} on {}'s {}", bot->GetName(), talk.order, trader->GetName(),
             target->GetEntry());
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
    bool holding = false;
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

        holding = open && talk.paid;
    }

    if (holding)  // the hold lasts as long as the talk
        Hold(botAI, now + talkFor);

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

    if (verb == "order")
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(arg);
        std::vector<CraftService::Recipe> const recipes = CraftService::Recipes(bot);
        auto recipe = std::find_if(recipes.begin(), recipes.end(),
                                   [arg](CraftService::Recipe const& r) { return r.spell->Id == arg; });
        char const* refusal = nullptr;
        if (!spell || recipe == recipes.end())
            refusal = "unknown";
        else if (!spell->HasEffect(SPELL_EFFECT_ENCHANT_ITEM))
            refusal = "craft";
        else if (!HasTools(bot, spell))  // before any payment: the core would refuse the cast
            refusal = "tool";

        uint32 const tip = recipe == recipes.end() ? 0 :
            std::max<uint32>(100, recipe->rank * Conf("Playerbots.RandomBotCraftTipPerSkill", 2));
        if (!refusal)
        {
            std::lock_guard<std::mutex> guard(lock);
            Talk& talk = talks[{bot->GetGUID(), owner->GetGUID()}];
            if (talk.paid || now < talk.next)  // one order at a time, and a pause after each
                refusal = "busy";
            else
            {
                talk.order = arg;
                talk.tip = tip;
                talk.warned = ObjectGuid::Empty;
            }
        }

        if (refusal)
        {
            Tell(bot, owner, Refusal(spell ? arg : 0, refusal));
            LOG_INFO("playerbots", "[craft] {} refuses {}'s order {}: {}", bot->GetName(), owner->GetName(), arg,
                     refusal);
            return true;
        }

        // "Order [r]: [item] x<n>, [item] x<n>; tip <copper>c"
        std::ostringstream out;
        out << "Order " << Link(arg) << ":";
        char const* separator = " ";
        for (auto const& [entry, count] : Mats(spell))
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            {
                out << separator << ChatHelper::FormatItem(proto) << " x" << count;
                separator = ", ";
            }

        out << "; tip " << tip << "c";
        Tell(bot, owner, out.str());
        LOG_INFO("playerbots", "[craft] {} takes {}'s order: {}", bot->GetName(), owner->GetName(), out.str());
        return true;
    }

    if (verb == "cancel")
    {
        uint32 order = 0;
        bool paid = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            Talk& talk = talks[{bot->GetGUID(), owner->GetGUID()}];
            order = talk.order;
            paid = talk.paid;
            talk.order = 0;
            talk.paid = false;
        }

        if (paid)  // the mats and the tip stay with the bot
            Hold(botAI, 0);

        Tell(bot, owner, Refusal(order, "canceled"));
        LOG_INFO("playerbots", "[craft] {} cancels {}'s order {} (paid {})", bot->GetName(), owner->GetName(), order,
                 paid);
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
