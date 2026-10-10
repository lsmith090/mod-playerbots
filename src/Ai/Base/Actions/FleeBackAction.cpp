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

#include "Config.h"
#include "FleeManager.h"
#include "MoveSpline.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestBots.h"

namespace
{
struct Run  // where a flee is going, and until when it keeps that spot
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    time_t until = 0;
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
}  // namespace

void FleeBack::Remember(Player* bot)
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport() || Knob() <= 0 ||
        !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    std::lock_guard<std::mutex> guard(lock);
    auto run = runs.find(bot->GetGUID());
    if (run != runs.end())
    {
        LOG_INFO("playerbots", "[flee] {} got away at {:.0f}% hp", bot->GetName(), bot->GetHealthPct());
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
    return on >= size_t(n) || (on && bot->GetHealthPct() < sPlayerbotAIConfig.CriticalHealth);
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

    // Still running to its spot: keep going
    if (now < run.until && bot->GetExactDist2d(run.x, run.y) > 3.0f && !bot->movespline->Finalized())
        return true;

    // The oldest kept position far enough back that no other mob guards, else FleeManager's spot farthest from them
    // all. The spot taken and every newer one are dropped: they lie toward the pull, so the next leg goes on back or to
    // FleeManager, never forward again.
    size_t back = trail.size();
    for (size_t i = 0; i < trail.size(); ++i)
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

    if (back == trail.size() && !FleeManager(bot, RANGE, 0.0f, true).CalculateDestination(&x, &y, &z))
        return false;

    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT))
        return false;

    {
        std::lock_guard<std::mutex> guard(lock);
        runs[g] = {x, y, z, now + KEEP_RUN};
    }

    if (now >= run.until)  // a new flee, not the next leg of one
        LOG_INFO("playerbots", "[flee] {} flees {} mobs at {:.0f}% hp to {} {:.0f} yd away", bot->GetName(), On(bot),
                 bot->GetHealthPct(), back < trail.size() ? "where it came from" : "a scored spot", bot->GetExactDist2d(x, y));

    return true;
}
