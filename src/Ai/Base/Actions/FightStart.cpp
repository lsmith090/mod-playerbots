/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FightStart.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

#include "Config.h"
#include "NewRpgAuctionAction.h"
#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestBots.h"

namespace
{
time_t const WINDOW = 2;   // s after the first hit in which the mobs on the bot are counted
time_t const CAP = 60;     // s after which a fight still going gets its closing line
// Mobs called by a neighbour come after CreatureFamilyAssistanceDelay (2 s) and their run: they show in `most`, not in
// the 2 s count.
float const RANGE = 30.0f; // a mob farther than this is not on the bot (FleeBack's RANGE)

struct Fight  // what is known at the first hit
{
    time_t at = 0;
    std::string head;  // everything up to the count of mobs
    size_t two = 0;    // the mobs on the bot by the end of the first 2 s
    size_t most = 0;   // the most seen on it since
};

struct Seen
{
    bool combat = false;  // was in combat at the last update
    bool pending = false; // the opening line is not yet written: the first 2 s are not in
    bool open = false;    // the opening line is out and the fight is not over
    time_t last = 0;      // when the last line began
    Fight fight;
};

std::mutex lock;  // bots of different maps update on different threads
std::map<ObjectGuid, Seen> seen;  // ponytail: never pruned; one small entry per bot ever seen

int32 Knob() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotFightLog", 0); }

char const* ClassName(uint8 c)
{
    static char const* const names[] = {"?",      "warrior", "paladin", "hunter", "rogue", "priest",
                                        "dk",     "shaman",  "mage",    "warlock", "?",    "druid"};
    return c < 12 ? names[c] : "?";
}

char const* RankName(uint32 rank)
{
    switch (rank)
    {
        case CREATURE_ELITE_NORMAL: return "normal";
        case CREATURE_ELITE_ELITE: return "elite";
        case CREATURE_ELITE_RAREELITE: return "rare elite";
        case CREATURE_ELITE_WORLDBOSS: return "boss";
        case CREATURE_ELITE_RARE: return "rare";
    }

    return "?";
}

char const* RpgName(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE: return "idle";
        case RPG_GO_GRIND: return "go grind";
        case RPG_GO_CAMP: return "go camp";
        case RPG_WANDER_RANDOM: return "wander random";
        case RPG_WANDER_NPC: return "wander npc";
        case RPG_DO_QUEST: return "do quest";
        case RPG_TRAVEL_FLIGHT: return "travel flight";
        case RPG_REST: return "rest";
        case RPG_OUTDOOR_PVP: return "outdoor pvp";
        case RPG_DO_GATHER: return "do gather";
        default: return "?";
    }
}

// The mobs on the bot within range
size_t On(Player* bot)
{
    size_t on = 0;
    for (Unit* attacker : bot->getAttackers())
        if (attacker->ToCreature() && attacker->IsAlive() && attacker->GetVictim() == bot &&
            bot->GetDistance(attacker) < RANGE)
            ++on;

    return on;
}

// The first mob: the one the bot is hitting if it is fighting the bot (the bot began the fight), else the nearest on it.
// A bot walking to its grind target may have it selected while another mob comes at it: that one is the first.
Creature* First(Player* bot, bool& pulled)
{
    Unit* victim = bot->GetVictim();
    pulled = victim && victim->ToCreature() && (victim->GetVictim() == bot || victim->IsInCombatWith(bot));
    if (pulled)
        return victim->ToCreature();

    Creature* nearest = nullptr;
    for (Unit* attacker : bot->getAttackers())
        if (Creature* mob = attacker->ToCreature())
            if (mob->IsAlive() && (!nearest || bot->GetDistance(mob) < bot->GetDistance(nearest)))
                nearest = mob;

    return nearest;
}

// Who began it. ponytail: read from the state at the first update in combat, not from the packets: "its quest pull"
// is "its rpg status is do quest". The labels are counted by the parser, they are not a proof of cause. A social pull
// shows as a "its ... pull" with more mobs in `most` than in the 2 s count.
char const* By(PlayerbotAI* botAI, Player* bot, bool pulled)
{
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    if (pulled)
        return status == RPG_DO_QUEST ? "its quest pull"
               : (status == RPG_GO_GRIND || status == RPG_GO_CAMP || status == RPG_WANDER_RANDOM ||
                  status == RPG_IDLE)
                   ? "its grind pull"
                   : "its pull";

    return bot->isMoving() ? "aggro on the move" : "aggro standing";
}

Fight Start(PlayerbotAI* botAI, Player* bot, time_t now)
{
    bool pulled = false;
    Creature* const first = First(bot, pulled);
    bool const mana = bot->GetMaxPower(POWER_MANA) > 0;

    Fight fight;
    fight.at = now;
    fight.two = fight.most = On(bot);
    fight.head = fmt::format(
        "{} (L{} {}) starts at {:.0f}% hp, {}, {} potions, {}", bot->GetName(), bot->GetLevel(),
        ClassName(bot->getClass()), bot->GetHealthPct(),
        mana ? fmt::format("{:.0f}% mana", 100.0f * bot->GetPower(POWER_MANA) / bot->GetMaxPower(POWER_MANA))
             : std::string("no mana"),
        botAI->GetAiObjectContext()->GetValue<uint32>("item count", "healing potion")->Get(),
        By(botAI, bot, pulled));
    if (first)
        fight.head += fmt::format(": {} (entry {}, L{} {}) {:.0f} yd off", first->GetName(), first->GetEntry(),
                                  first->GetLevel(), RankName(first->GetCreatureTemplate()->rank),
                                  bot->GetDistance(first));
    else
        fight.head += ": nothing on it";

    fight.head += fmt::format("; rpg {}", RpgName(botAI->rpgInfo.GetStatus()));
    // agent-wow (#87 step 2): a fight on a trade session's walk or stay says so, for the deaths it costs
    if (BotTrade::Active(botAI))
        fight.head += "; trade session";
    return fight;
}
}  // namespace

void FightStart::Update(PlayerbotAI* botAI)
{
    int32 const hold = Knob();
    Player* bot = botAI->GetBot();
    if (hold <= 0 || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot) || TestBots::IsTestBot(bot))
        return;

    time_t const now = time(nullptr);
    bool const combat = bot->IsAlive() && bot->IsInCombat();
    std::lock_guard<std::mutex> guard(lock);
    Seen& s = seen[bot->GetGUID()];
    if (combat && !s.combat && now - s.last >= hold)
    {
        s.fight = Start(botAI, bot, now);
        s.pending = true;
        s.last = now;
    }

    s.combat = combat;
    Fight& f = s.fight;
    if (s.pending || s.open)
        f.most = std::max(f.most, combat ? On(bot) : 0);

    // The opening line, once the first 2 s are in: it is the one that is always written, even for a bot that dies
    // before the fight is over (a dead bot may be logged out before its next update)
    if (s.pending && (!combat || now - f.at >= WINDOW))
    {
        f.two = f.most;
        s.pending = false;
        s.open = true;
        LOG_INFO("playerbots", "[fight] {}; {} mobs on it within {} s", f.head, f.two, WINDOW);
    }

    // The closing line, only for a fight that went on to matter: mobs came after the first 2 s, the bot ended under
    // 25% hp, or it died
    if (s.open && (!combat || now - f.at >= CAP))
    {
        s.open = false;
        bool const alive = bot->IsAlive();
        if (!alive || f.most > f.two || bot->GetHealthPct() < 25.0f)
            LOG_INFO("playerbots", "[fight] {} {} after {} s{}, {} mobs on it at most", bot->GetName(),
                     !alive ? "died" : combat ? "is still fighting" : "got out", now - f.at,
                     alive ? fmt::format(" at {:.0f}% hp", bot->GetHealthPct()) : std::string(), f.most);
    }
}
