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
#include "Playerbots.h"
#include "SpellMgr.h"

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
