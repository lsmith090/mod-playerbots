/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurfaceAction.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "BotTraits.h"
#include "Config.h"
#include "GridTerrainData.h"
#include "MoveSpline.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

namespace
{
// Each bot's count: the seconds of breath used, when the count was last brought up to date, and whether the bot is on
// its way up or catching its breath. Kept here, every number from zero: the "last said" value this was kept in starts
// at its creation time less 120 s, so every bot began "rising" with 1.8 billion seconds used, and only a fight cleared
// it (from 2026-10-14 08:05 UTC, when RandomBotSurfaceBreath went on, until a fight, every random bot stood still).
struct Count
{
    time_t used = 0;
    time_t seen = 0;
    bool rising = false;
};

std::mutex lock;
std::map<ObjectGuid, Count> counts;  // never pruned: three numbers per bot ever seen

time_t Used(Player* bot)
{
    std::lock_guard<std::mutex> guard(lock);
    auto count = counts.find(bot->GetGUID());
    return count == counts.end() ? 0 : count->second.used;
}

// the seconds a bot can stay under water, as Player::getMaxTimer(BREATH_TIMER) has them
time_t Lungs(Player* bot)
{
    return time_t(sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER) / IN_MILLISECONDS *
                  bot->GetTotalAuraMultiplier(SPELL_AURA_MOD_WATER_BREATHING));
}

// the seconds of breath left at which this bot leaves. A careful one adds the swim up to the knob. A careless one
// (BotTraits) waits for Playerbots.RandomBotCarelessBreath whatever the depth, and in a fight for its last second:
// deep down, or with a mob on it, that is too late, as it is for a player who watches the fight and not the bar.
time_t LeavesAt(Player* bot, int32 margin)
{
    if (BotTraits::Careless(bot))
        return bot->IsInCombat() ? 0 : sConfigMgr->GetOption<int32>("Playerbots.RandomBotCarelessBreath", 8);

    LiquidData const& liquid = bot->GetLiquidData();
    float const depth = liquid.Level > INVALID_HEIGHT ? std::max(0.0f, liquid.Level - bot->GetPositionZ()) : 0.0f;
    return margin + time_t(depth / bot->GetSpeed(MOVE_SWIM));
}
}  // namespace

namespace Breath
{
int32 Knob()
{
    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    return sConfigMgr->GetOption<int32>("Playerbots.RandomBotSurfaceBreath", 0);
}
}  // namespace Breath

bool OutOfBreathTrigger::IsActive()
{
    int32 const margin = Breath::Knob();
    if (margin <= 0 || !bot->IsAlive() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    std::lock_guard<std::mutex> guard(lock);
    Count& count = counts[bot->GetGUID()];

    time_t const now = time(nullptr);
    time_t const passed = count.seen ? now - count.seen : 0;
    count.seen = now;

    bool const under = bot->IsUnderWater() && !bot->HasWaterBreathingAura();
    time_t const lungs = Lungs(bot);
    time_t const spent =
        under ? std::min(lungs, count.used + passed) : std::max<time_t>(0, count.used - 10 * passed);
    count.used = spent;

    if (under && (count.rising || lungs - spent <= LeavesAt(bot, margin)))
    {
        count.rising = true;
        return true;
    }

    if (!count.rising)
        return false;

    // up: in a fight it goes straight back to it; out of one it waits until half of what it may use is back
    if (bot->IsInCombat() || spent * 2 <= lungs - margin)
    {
        count.rising = false;
        return false;
    }
    return true;
}

bool SurfaceAction::isPossible() { return IsMovingAllowed(); }

bool SurfaceAction::Execute(Event /*event*/)
{
    // at the surface, catching its breath
    if (!bot->IsUnderWater())
        return true;

    LiquidData const& liquid = bot->GetLiquidData();
    if (liquid.Level <= INVALID_HEIGHT)
        return false;

    // a yard under the level: the head is out, and the bot is still in the water and swims
    float const z = liquid.Level - 1.0f;
    float const depth = z - bot->GetPositionZ();
    bot->SetSwim(true);
    if (MoveTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), z, false, false, false, true,
               MovementPriority::MOVEMENT_FORCED))
    {
        LOG_INFO("playerbots", "[breath] {} swims up {:.0f} yd for air with {} s of breath left{}{}", bot->GetName(),
                 depth, Lungs(bot) - Used(bot), bot->IsInCombat() ? ", in a fight" : "",
                 BotTraits::Careless(bot) ? ", careless" : "");
        return true;
    }

    // already on its way up
    return !bot->movespline->Finalized();
}
