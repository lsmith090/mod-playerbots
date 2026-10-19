/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FleeBackAction.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "BotTraits.h"
#include "CharmInfo.h"
#include "Config.h"
#include "FleeManager.h"
#include "Group.h"
#include "MoveSpline.h"
#include "Pet.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestBots.h"

namespace
{
struct Run  // where a flee is going, and until when it keeps that spot
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    time_t until = 0;
    time_t began = 0;     // when this fight's flee began
    bool stood = false;   // it ran its time with mobs still on it and turned to fight: no more fleeing this fight
    bool avoid = false;   // out of combat: it is stepping out of a strong mob's way
    bool pet = false;     // its pet was called off for the run
    ReactStates react = REACT_DEFENSIVE;  // and reacted like this before
};

std::mutex lock;  // bots of different maps update on different threads
std::map<ObjectGuid, std::deque<WorldPosition>> trails;  // ponytail: never pruned; a few positions per bot ever seen
std::map<ObjectGuid, Run> runs;

size_t const TRAIL = 6;         // positions kept
float const STEP = 8.0f;        // yd between two kept positions
float const RANGE = 30.0f;      // a mob farther than this is out of range: it no longer counts as on the bot
float const MIN_BACK = 15.0f;   // a kept position nearer than this is no way back
float const MAX_BACK = 80.0f;   // nor one farther than this
time_t const KEEP_RUN = 10;     // s a flee keeps its spot while running there

int32 Knob() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotFlee", 0); }

// Seconds a flee goes on with mobs still on the bot before it turns to fight; 0 keeps the flee as it was
int32 Commit() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotFleeCommit", 0); }

// A mob gives up 11 s after it last took damage, once 30 yd from home (Creature::CanCreatureAttack): a pet that goes
// on biting holds every mob on the bot. Called off, it follows.
void CallOff(Player* bot, Run& run)
{
    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive())
        return;

    run.pet = true;
    run.react = pet->GetReactState();
    pet->SetReactState(REACT_PASSIVE);
    pet->AttackStop();
    if (CharmInfo* info = pet->GetCharmInfo())
    {
        info->SetPlayerReactState(REACT_PASSIVE);
        info->SetIsCommandAttack(false);
        info->SetCommandState(COMMAND_FOLLOW);
    }

    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
}

void CallBack(Player* bot, Run const& run)
{
    Pet* pet = bot->GetPet();
    if (!run.pet || !pet)
        return;

    pet->SetReactState(run.react);
    if (CharmInfo* info = pet->GetCharmInfo())
        info->SetPlayerReactState(run.react);
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

// No mob but those on the bot would aggro at (x, y). ponytail: only the mobs the bot sees (possible targets no los,
// its sight distance); a kept position 80 yd back may have a pack the bot no longer sees.
bool Clear(PlayerbotAI* botAI, Player* bot, float x, float y)
{
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->GetVictim() == bot)
            continue;

        if (mob->GetDistance2d(x, y) < mob->GetAttackDistance(bot) + 5.0f)
            return false;
    }

    return true;
}

// Yards beyond a strong mob's aggro distance at which a bot steps out of its way; 0: strong mobs are mobs like any
int32 Avoid() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotAvoidElite", 0); }

// Levels above the bot at which a strong mob stays strong for a bot in a group (the group's "can fight elite" ignores
// levels); 0: a group fights elites as upstream decides
int32 EliteGap() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotEliteGap", 0); }

// A mob this bot can't fight alone: an elite, rare elite or boss that isn't far below it, or a rare three levels
// above. Old Icebeard (L11 elite) alone was 424 of 4,804 bot deaths in three days, on bots of level 5 to 9.
bool Strong(PlayerbotAI* botAI, Player* bot, Creature* mob)
{
    if (!mob || !mob->IsAlive() || mob->GetOwnerGUID() || !mob->IsHostileTo(bot))
        return false;

    uint32 const rank = mob->GetCreatureTemplate()->rank;
    int32 const above = int32(mob->GetLevel()) - int32(bot->GetLevel());
    if (rank == CREATURE_ELITE_NORMAL || above < (rank == CREATURE_ELITE_RARE ? 3 : -2))
        return false;

    // "can fight elite" is true for any bot in a group that is following its leader, whatever the levels: a party of
    // level 6 bots counted Old Icebeard (L11) as fightable and never stepped aside or ran (a quarter of his 274
    // victims, 10-16 to 10-19, were in a group). With EliteGap set, a mob that many levels above is strong in a group too.
    int32 const gap = EliteGap();
    if (gap > 0 && above >= gap)
        return true;

    return !botAI->GetAiObjectContext()->GetValue<bool>("can fight elite")->Get();
}

// 1: a group of random bots flees as one. 0: every member decides alone, as before
int32 GroupKnob() { return sConfigMgr->GetOption<int32>("Playerbots.RandomBotGroupFlee", 0); }

struct Call  // a group's call to flee: where, until when it stands, and who made it
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 map = 0;
    time_t until = 0;
    ObjectGuid caller;
};

std::map<ObjectGuid, Call> calls;  // by group; ponytail: never pruned, one per group ever seen
float const HERE = 40.0f;          // a living groupmate within this is in the fight with the bot
float const HEED = 150.0f;         // a call's spot farther than this is somebody else's fight
time_t const KEEP_CALL = 12;       // s a call stands: a mob's 11 s, and the caller's next leg renews it

// The bot and the living groupmates standing with it. Just the bot when it has no group or the key is off. Mates on
// the bot's map update on its thread.
std::vector<Player*> Party(Player* bot)
{
    std::vector<Player*> party{bot};
    Group* group = bot->GetGroup();
    if (!group || GroupKnob() <= 0)
        return party;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* mate = ref->GetSource();
        if (mate && mate != bot && mate->IsInWorld() && mate->IsAlive() && mate->GetMap() == bot->GetMap() &&
            bot->GetDistance(mate) < HERE)
            party.push_back(mate);
    }

    return party;
}

// The mobs on the party within range of the member they are on
size_t OnParty(std::vector<Player*> const& party)
{
    std::set<ObjectGuid> mobs;
    for (Player* member : party)
        for (Unit* attacker : member->getAttackers())
            if (attacker->ToCreature() && attacker->IsAlive() && attacker->GetVictim() == member &&
                member->GetDistance(attacker) < RANGE)
                mobs.insert(attacker->GetGUID());

    return mobs.size();
}

// The strong mob on any of the party, by this bot's measure
Creature* StrongOnParty(PlayerbotAI* botAI, Player* bot, std::vector<Player*> const& party)
{
    if (Avoid() <= 0)
        return nullptr;

    for (Player* member : party)
        for (Unit* attacker : member->getAttackers())
            if (attacker->GetVictim() == member && Strong(botAI, bot, attacker->ToCreature()))
                return attacker->ToCreature();

    return nullptr;
}

// The call standing for the bot's group, if it is near enough to be the bot's fight
bool Called(Player* bot, time_t now, Call* out = nullptr)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::lock_guard<std::mutex> guard(lock);
    auto call = calls.find(group->GetGUID());
    if (call == calls.end() || now >= call->second.until || call->second.map != bot->GetMapId() ||
        bot->GetExactDist2d(call->second.x, call->second.y) > HEED)
        return false;

    if (out)
        *out = call->second;

    return true;
}

// The strong mob about to notice the bot, if any: within its aggro distance and the margin, and not on the bot yet
Creature* StrongNear(PlayerbotAI* botAI, Player* bot)
{
    int32 const margin = Avoid();
    if (margin <= 0)
        return nullptr;

    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
    {
        Creature* mob = botAI->GetCreature(guid);
        if (mob && mob->GetVictim() != bot && bot->GetDistance(mob) < mob->GetAttackDistance(bot) + float(margin) &&
            Strong(botAI, bot, mob))
            return mob;
    }

    return nullptr;
}
}  // namespace

void FleeBack::Remember(Player* bot)
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport() || Knob() <= 0 ||
        !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    std::lock_guard<std::mutex> guard(lock);
    auto run = runs.find(bot->GetGUID());
    if (run != runs.end() && run->second.avoid)
    {
        // Stepping out of a strong mob's way was never a fight: no line, and the step is left to finish
        if (time(nullptr) < run->second.until && bot->GetExactDist2d(run->second.x, run->second.y) > 3.0f)
            return;

        runs.erase(run);
    }
    else if (run != runs.end())
    {
        LOG_INFO("playerbots", "[flee] {} {} at {:.0f}% hp", bot->GetName(),
                 run->second.stood ? "fought it out" : "got away", bot->GetHealthPct());
        CallBack(bot, run->second);
        runs.erase(run);
    }

    std::deque<WorldPosition>& trail = trails[bot->GetGUID()];
    if (!trail.empty() && trail.back().GetMapId() != bot->GetMapId())
        trail.clear();

    if (trail.empty() || trail.back().GetExactDist2d(bot) >= STEP)
        trail.push_back(WorldPosition(bot));

    if (trail.size() > TRAIL)
        trail.pop_front();
}

bool FleeBackTrigger::IsActive()
{
    int32 const n = Knob();
    if (n <= 0 || !bot->IsAlive() || !sRandomPlayerbotMgr.IsRandomBot(bot) || TestBots::IsTestBot(bot) ||
        bot->GetMap()->Instanceable() || bot->InBattleground() || !botAI->GetRealPlayersInGroup().empty())
        return false;  // a bot in a pilot's party stays with the pilot

    size_t const on = On(bot);
    bool const careless = BotTraits::Careless(bot);
    std::vector<Player*> const party = Party(bot);
    if (StrongOnParty(botAI, bot, party))
        return true;  // at any health: an elite's second hit is too late, on the bot or on a mate beside it

    if (!bot->IsInCombat() && Avoid() > 0 && !careless)
    {
        // A player watches a patrol come and steps aside. A careless one notices when it is on them.
        {
            std::lock_guard<std::mutex> guard(lock);
            auto run = runs.find(bot->GetGUID());
            if (run != runs.end() && run->second.avoid && time(nullptr) < run->second.until)
                return true;
        }

        return StrongNear(botAI, bot) != nullptr;
    }

    if (on && Commit() > 0)
    {
        std::lock_guard<std::mutex> guard(lock);
        auto run = runs.find(bot->GetGUID());
        if (run != runs.end() && run->second.began)
        {
            if (run->second.stood)
                return false;

            // A flee begun goes on while any mob is on the bot: turning on the last one, hurt, is how most flees
            // died. A careless bot still does.
            if (!careless)
                return true;
        }
    }

    // A group flees as one: a mate's call is the bot's too while it is in the fight. A careless bot fights on.
    if (party.size() > 1 && bot->IsInCombat() && !careless && Called(bot, time(nullptr)))
    {
        std::lock_guard<std::mutex> guard(lock);
        auto run = runs.find(bot->GetGUID());
        return run == runs.end() || !run->second.stood;
    }

    // Outnumbered is counted for the party: the mobs on all of it against its heads. Alone that is RandomBotFlee
    // mobs; two together stand against one more, and so on. A member with two mobs on it no longer runs off while
    // its mates fight them, which held those mobs on it for good.
    size_t const mobs = party.size() > 1 ? OnParty(party) : on;
    return mobs >= size_t(n) + party.size() - 1 || (on && bot->GetHealthPct() < sPlayerbotAIConfig.CriticalHealth);
}

bool FleeBackAction::isPossible() { return IsMovingAllowed(); }

bool FleeBackAction::Execute(Event /*event*/)
{
    time_t const now = time(nullptr);
    ObjectGuid const g = bot->GetGUID();
    Run run;
    std::deque<WorldPosition> trail;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = runs.find(g);
        if (it != runs.end())
            run = it->second;
        trail = trails[g];
    }

    bool const fight = bot->IsInCombat();
    std::vector<Player*> const party = Party(bot);
    Creature* const strong = fight ? StrongOnParty(botAI, bot, party) : StrongNear(botAI, bot);
    if (fight && run.avoid)  // it stepped aside and was caught all the same: this is a flee from here
        run.avoid = false, run.until = 0;

    // It has run long enough for any mob to give up, and they haven't: running on only takes hits in the back. Never
    // from a strong mob: turning on one is death either way.
    int32 const commit = Commit();
    if (commit > 0 && run.began && now - run.began >= commit && !strong)
    {
        LOG_INFO("playerbots", "[flee] {} turns to fight {} mobs at {:.0f}% hp after {} s", bot->GetName(), On(bot),
                 bot->GetHealthPct(), now - run.began);
        CallBack(bot, run);
        std::lock_guard<std::mutex> guard(lock);
        runs[g].stood = true;
        return false;
    }

    // Still running to its spot: keep going
    if (now < run.until && bot->GetExactDist2d(run.x, run.y) > 3.0f && !bot->movespline->Finalized())
        return true;

    if (!fight && !strong)  // the step aside is done and nothing strong is near
        return false;

    // The oldest kept position far enough back that no other mob guards, else FleeManager's spot farthest from them
    // all. The spot taken and every newer one are dropped: they lie toward the pull, so the next leg goes on back or to
    // FleeManager, never forward again.
    // In a group, a mate's standing call comes first: everyone runs to the caller's spot, so the group stays one
    // group and nobody is left hitting the mobs that chase the others.
    Call call;
    bool const heeds = fight && party.size() > 1 && Called(bot, now, &call) && call.caller != g &&
                       bot->GetExactDist2d(call.x, call.y) > 5.0f;
    size_t back = trail.size();
    for (size_t i = 0; !heeds && i < trail.size(); ++i)
    {
        float const d = bot->GetExactDist2d(trail[i]);
        if (d >= MIN_BACK && d <= MAX_BACK && Clear(botAI, bot, trail[i].GetPositionX(), trail[i].GetPositionY()))
        {
            back = i;
            break;
        }
    }

    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (back < trail.size())
    {
        x = trail[back].GetPositionX(), y = trail[back].GetPositionY(), z = trail[back].GetPositionZ();
        std::lock_guard<std::mutex> guard(lock);
        std::deque<WorldPosition>& kept = trails[g];
        kept.erase(kept.begin() + std::min(back, kept.size()), kept.end());
    }

    if (heeds)
        x = call.x, y = call.y, z = call.z;
    else if (back == trail.size() && !FleeManager(bot, RANGE, 0.0f, true).CalculateDestination(&x, &y, &z))
        return false;

    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT))
        return false;

    // The bot's own spot becomes its group's call when the reason is the group's: outnumbered together, or a strong
    // mob on any of them. Critical health alone is one member stepping back while its mates finish the mob.
    size_t const onParty = OnParty(party);
    bool const calling = fight && party.size() > 1 && !heeds &&
                         (strong || onParty >= size_t(Knob()) + party.size() - 1);
    if (calling)
    {
        std::lock_guard<std::mutex> guard(lock);
        Call& mine = calls[bot->GetGroup()->GetGUID()];
        bool const fresh = now >= mine.until;
        if (fresh || mine.caller == g)
        {
            mine.x = x, mine.y = y, mine.z = z, mine.map = bot->GetMapId(), mine.until = now + KEEP_CALL, mine.caller = g;
            if (fresh)
                LOG_INFO("playerbots", "[flee] {} calls its group of {} here to flee {} mobs", bot->GetName(),
                         party.size(), onParty);
        }
    }

    {
        Run next = run;
        next.x = x, next.y = y, next.z = z, next.until = now + KEEP_RUN;
        next.avoid = !fight;
        if (fight && !next.began)
        {
            next.began = now;
            if (commit > 0)
                CallOff(bot, next);
        }

        std::lock_guard<std::mutex> guard(lock);
        runs[g] = next;
    }

    std::string const from =
        strong ? fmt::format(" {} (L{}{})", strong->GetName(), strong->GetLevel(),
                             strong->GetCreatureTemplate()->rank == CREATURE_ELITE_RARE ? " rare" : " elite")
               : "";
    if (!fight && now >= run.until)
        LOG_INFO("playerbots", "[flee] {} steps out of the way of{} {:.0f} yd off, to {} {:.0f} yd away", bot->GetName(),
                 from, strong ? bot->GetDistance(strong) : 0.0f,
                 back < trail.size() ? "where it came from" : "a scored spot", bot->GetExactDist2d(x, y));
    else if (now >= run.until)  // a new flee, not the next leg of one
        LOG_INFO("playerbots", "[flee] {} flees {} mobs at {:.0f}% hp to {} {:.0f} yd away{}{}", bot->GetName(), On(bot),
                 bot->GetHealthPct(),
                 heeds ? "its group's spot" : back < trail.size() ? "where it came from" : "a scored spot",
                 bot->GetExactDist2d(x, y), strong ? ", from" : "", from);

    return true;
}
