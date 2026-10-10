/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SpeechForms.h"

#include <cctype>
#include <map>
#include <mutex>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "Event.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestBots.h"
#include "TravelMgr.h"
#include "WorldPacket.h"

namespace
{
struct Heard
{
    ObjectGuid from;
    std::string text;
};

struct Said  // what the bot last said of its own fight
{
    size_t attackers = 0;
    ObjectGuid rare;
    time_t danger = 0;
    time_t heal = 0;
};

std::mutex lock;  // Hear runs on the speaker's thread, the rest on the bot's
std::map<ObjectGuid, std::vector<Heard>> inbox;
std::map<ObjectGuid, Said> said;
std::map<std::pair<ObjectGuid, uint32>, time_t> general;  // (bot, mob entry): when its danger was said in General

time_t const PARTY_EVERY = 15;  // one danger and one need heal in party per bot per 15 s
float const ASSIST_RANGE = 40.0f;

// The heals a bot casts on a member's "need heal", the first it can
char const* const HEALS[] = {"flash heal", "lesser heal", "heal",  "flash of light", "holy light",
                             "lesser healing wave", "healing wave", "regrowth", "healing touch"};

bool Speaks(Player* bot) { return sRandomPlayerbotMgr.IsRandomBot(bot) && !TestBots::IsTestBot(bot); }

bool Form(std::string const& text, char const* form) { return text.rfind(form, 0) == 0; }

bool Rare(Creature* mob)
{
    uint32 const rank = mob->GetCreatureTemplate()->rank;
    return rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE || rank == CREATURE_ELITE_WORLDBOSS ||
           rank == CREATURE_ELITE_RARE;
}

// In party chat to every member, bots included (SayToParty reaches only real players), and into Chat.log in
// chat_log.cpp's words: a bot's line never passes the chat handler's hooks
void Party(Player* bot, std::string const& text)
{
    Group* group = bot->GetGroup();
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, bot->GetGUID(), {}, text, CHAT_TAG_NONE,
                                 bot->GetName());
    group->BroadcastPacket(&data, false, -1, bot->GetGUID());
    LOG_INFO("chat.party", "Player {} tells party with leader {}: {}", bot->GetName(), group->GetLeaderName(), text);
}

void Danger(PlayerbotAI* botAI, Creature* mob)
{
    Player* bot = botAI->GetBot();
    std::string const text = "danger " + mob->GetName();
    if (bot->GetGroup())
    {
        Party(bot, text);
        LOG_INFO("playerbots", "[speech] {} says {} in party", bot->GetName(), text);
        return;
    }

    if (bot->GetMap()->Instanceable())
        return;

    time_t const now = time(nullptr);
    time_t const every = 60 * sConfigMgr->GetOption<int32>("Playerbots.RandomBotSpeechGeneralMinutes", 10);
    {
        std::lock_guard<std::mutex> guard(lock);
        time_t& at = general[{bot->GetGUID(), mob->GetEntry()}];
        if (at && now < at + every)
            return;
        at = now;
    }

    AreaTableEntry const* zone = botAI->GetCurrentZone();
    if (!zone || !botAI->SayToChannel(text, ChatChannelId::GENERAL))
        return;

    LOG_INFO("chat.channel", "Player {} tells channel General - {}: {}", bot->GetName(),
             PlayerbotAI::GetLocalizedAreaName(zone), text);
    LOG_INFO("playerbots", "[speech] {} says {} in General", bot->GetName(), text);
}

// Its own fight: danger when a second mob or an elite or rare comes on, need heal at critical health in a group
void Speak(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Said last;
    {
        std::lock_guard<std::mutex> guard(lock);
        last = said[bot->GetGUID()];
    }

    std::vector<Creature*> on;
    if (bot->IsAlive() && bot->IsInCombat())
        for (Unit* attacker : bot->getAttackers())
            if (Creature* mob = attacker->ToCreature())
                if (mob->IsAlive() && mob->GetVictim() == bot)
                    on.push_back(mob);

    Said now = last;
    now.attackers = on.size();
    time_t const t = time(nullptr);

    Creature* rare = nullptr;
    Creature* worst = nullptr;
    for (Creature* mob : on)
    {
        if (Rare(mob) && !rare)
            rare = mob;
        if (!worst || mob->GetLevel() > worst->GetLevel())
            worst = mob;
    }

    if (rare && rare->GetGUID() != last.rare)
        now.rare = rare->GetGUID();

    bool const pulled = (on.size() >= 2 && last.attackers < 2) || (rare && rare->GetGUID() != last.rare);
    if (pulled && t >= last.danger + PARTY_EVERY)
    {
        Danger(botAI, rare ? rare : worst);
        now.danger = t;
    }

    if (!on.empty() && bot->GetGroup() && bot->GetHealthPct() < sPlayerbotAIConfig.CriticalHealth &&
        t >= last.heal + PARTY_EVERY)
    {
        Party(bot, "need heal");
        LOG_INFO("playerbots", "[speech] {} says need heal in party at {:.0f}%", bot->GetName(), bot->GetHealthPct());
        now.heal = t;
    }

    if (on.empty())
        now.rare = ObjectGuid::Empty;

    std::lock_guard<std::mutex> guard(lock);
    said[bot->GetGUID()] = now;
}

// A member's "danger <name>": a bot out of combat near the speaker attacks what is on it, the named mob first
void Assist(PlayerbotAI* botAI, Player* from, std::string const& name)
{
    Player* bot = botAI->GetBot();
    if (bot->IsInCombat() || !bot->IsAlive() || bot->GetDistance(from) > ASSIST_RANGE)
        return;  // it holds: a bot too far away doesn't run into the pull

    Unit* target = nullptr;
    for (Unit* attacker : from->getAttackers())
        if (attacker->IsAlive() && bot->IsValidAttackTarget(attacker) && (!target || attacker->GetName() == name))
            target = attacker;

    if (!target)
        return;

    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    if (botAI->DoSpecificAction("attack", Event(), true))
        LOG_INFO("playerbots", "[speech] {} answers {}'s danger: attacks {}", bot->GetName(), from->GetName(),
                 target->GetName());
}

// A member's "need heal": a bot that knows a heal casts it on the speaker
void Heal(PlayerbotAI* botAI, Player* from)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !from->IsAlive())
        return;

    for (char const* heal : HEALS)
        if (botAI->CanCastSpell(heal, from) && botAI->CastSpell(heal, from))
        {
            LOG_INFO("playerbots", "[speech] {} answers {}'s need heal: {}", bot->GetName(), from->GetName(), heal);
            return;
        }
}

// The map's words for a place: the sub-area, else the nearest game_tele place on the map ("NorthshireValley" said
// "Northshire Valley", as answer.lua says it), else the zone
std::string Place(WorldPosition to)
{
    AreaTableEntry const* area = to.getArea();
    if (area && area->zone)
        return area->area_name[0];

    GameTele const* nearest = nullptr;
    float best = 0.0f;
    for (auto const& [id, tele] : sObjectMgr->GetGameTeleMap())
    {
        if (tele.mapId != to.GetMapId())
            continue;
        float const dx = tele.position_x - to.GetPositionX(), dy = tele.position_y - to.GetPositionY();
        if (!nearest || dx * dx + dy * dy < best)
        {
            nearest = &tele;
            best = dx * dx + dy * dy;
        }
    }

    if (!nearest)
        return area ? area->area_name[0] : "";

    std::string name;
    for (size_t i = 0; i < nearest->name.size(); ++i)
    {
        if (i && std::islower(nearest->name[i - 1]) && std::isupper(nearest->name[i]))
            name += ' ';
        name += nearest->name[i];
    }
    return name;
}
}  // namespace

bool SpeechForms::Enabled() { return sConfigMgr->GetOption<bool>("Playerbots.RandomBotSpeechForms", false); }

bool SpeechForms::Is(uint32 type, std::string const& text)
{
    return (type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER) &&
           (text == "need heal" || (Form(text, "danger ") && text.size() > 7)) && Enabled();
}

bool SpeechForms::Hear(Player* bot, uint32 type, ObjectGuid from, std::string const& text)
{
    if (!Is(type, text))
        return false;

    if (from == bot->GetGUID())
        return true;

    std::lock_guard<std::mutex> guard(lock);
    std::vector<Heard>& heard = inbox[bot->GetGUID()];
    if (heard.size() < 8)
        heard.push_back({from, text});

    return true;
}

void SpeechForms::Update(PlayerbotAI* botAI)
{
    if (!Enabled())
        return;

    Player* bot = botAI->GetBot();
    std::vector<Heard> heard;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = inbox.find(bot->GetGUID());
        if (it != inbox.end())
        {
            heard.swap(it->second);
            inbox.erase(it);
        }
    }

    if (!Speaks(bot))
        return;

    Speak(botAI);

    for (Heard const& line : heard)
    {
        Player* from = ObjectAccessor::FindConnectedPlayer(line.from);
        if (!from || !bot->GetGroup() || !bot->GetGroup()->IsMember(line.from) || from->GetMap() != bot->GetMap())
            continue;

        if (line.text == "need heal")
            Heal(botAI, from);
        else
            Assist(botAI, from, line.text.substr(7));
    }
}

void SpeechForms::Going(PlayerbotAI* botAI, WorldPosition const& to)
{
    Player* bot = botAI->GetBot();
    if (!Enabled() || !Speaks(bot) || !bot->GetGroup())
        return;

    std::string const place = Place(to);
    if (place.empty())
        return;

    Party(bot, "going " + place);
    LOG_INFO("playerbots", "[speech] {} says going {} in party", bot->GetName(), place);
}
