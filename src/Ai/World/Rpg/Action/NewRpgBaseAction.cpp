/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "FleeBackAction.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestGate.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpeechForms.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "WorldSession.h"
#include "TestBots.h"
#include "TravelMgr.h"
#include "G3D/Vector2.h"

namespace
{
std::mutex pathLock;  // bots of different maps update on different threads
std::map<ObjectGuid, time_t> pathLogged;  // ponytail: never pruned; one time_t per bot ever seen
// agent-wow (#2954): each bot's path-danger holds on the way to its present goal (Playerbots.RandomBotPathDangerHoldTries)
std::map<ObjectGuid, std::pair<WorldPosition, uint32>> pathHolds;  // ponytail: never pruned, as pathLogged
// agent-wow (#2954): when the ladder walk last picked a goal for each bot (Playerbots.RandomBotLadderWalk)
std::map<ObjectGuid, uint32> ladderPicked;  // ponytail: never pruned, as pathLogged
// agent-wow (#3159): each bot's stuck give-ups at one spot (patch 52): where the first was, when, and how many
std::map<ObjectGuid, std::tuple<WorldPosition, time_t, uint32>> stuckSpots;  // ponytail: never pruned, as pathLogged
// agent-wow (#3159): when each bot was last moved unseen (Playerbots.RandomBotRelocate)
std::map<ObjectGuid, time_t> relocated;  // ponytail: never pruned, as pathLogged

// agent-wow: the first mob that would aggro the bot on a walk through these points (Playerbots.RandomBotPathDanger).
// The walk is sampled every 8 yd for its first 120 yd: farther than that the bot sees nothing. A bot that already
// stands where a mob would aggro it is left to the flee.
Creature* DangerOnWalk(PlayerbotAI* botAI, Player* bot, std::vector<G3D::Vector3> const& points)
{
    float const step = 8.0f, horizon = 120.0f;
    if (FleeBack::Aggro(botAI, bot, bot->GetPositionX(), bot->GetPositionY()))
        return nullptr;

    G3D::Vector3 from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    float walked = 0.0f;
    for (G3D::Vector3 const& to : points)
    {
        float const len = (to - from).magnitude();
        for (float d = step; d < len + step && walked + d < horizon + step; d += step)
        {
            G3D::Vector3 const at = from + (to - from) * (std::min(d, len) / std::max(len, 0.01f));
            if (Creature* mob = FleeBack::Aggro(botAI, bot, at.x, at.y))
                return mob;
        }

        walked += len;
        from = to;
        if (walked >= horizon)
            break;
    }

    return nullptr;
}

// agent-wow: the level gate on a quest's creatures (Playerbots.RandomBotQuestGate). 76% of the random bots' deaths were
// to a mob above the bot, and the grind pick's level cap (GrindMaxLevelAbove) never sees the mobs a quest sends a bot
// among. Quest level + 3 (IsQuestCapableDoing) says nothing of the creatures a quest wants.
struct Source
{
    uint8 normalMin = 255;  // lowest minlevel among its rank-0 droppers
    bool normal = false;    // has a rank-0 dropper
};

std::atomic<bool> sourcesReady{false};
std::unordered_map<uint32, Source> sources;  // written once, by the read's thread, before sourcesReady

// Items quests ask for, by the creatures that drop them: the lowest-level normal dropper is the one a bot may pick; an
// item only elites drop is an elite quest for a solo bot. The quest items are a derived table, joined: MySQL
// materializes it once (0.15 s on the live world, 1,911 items), where an `IN (SELECT ... UNION ...)` ran once per loot
// row. MAX_EXECUTION_TIME ends the read after 10 s whatever its plan.
void ReadQuestGateItems()
{
    uint32 const start = getMSTime();
    QueryResult result = WorldDatabase.Query(
        "SELECT /*+ MAX_EXECUTION_TIME(10000) */ l.Item, MIN(IF(ct.`rank` = 0, ct.minlevel, 255)), MAX(ct.`rank` = 0) "
        "FROM (SELECT RequiredItemId1 AS Item FROM quest_template WHERE RequiredItemId1 > 0 "
        "UNION SELECT RequiredItemId2 FROM quest_template WHERE RequiredItemId2 > 0 "
        "UNION SELECT RequiredItemId3 FROM quest_template WHERE RequiredItemId3 > 0 "
        "UNION SELECT RequiredItemId4 FROM quest_template WHERE RequiredItemId4 > 0 "
        "UNION SELECT RequiredItemId5 FROM quest_template WHERE RequiredItemId5 > 0 "
        "UNION SELECT RequiredItemId6 FROM quest_template WHERE RequiredItemId6 > 0) q "
        "JOIN creature_loot_template l ON l.Item = q.Item JOIN creature_template ct ON ct.lootid = l.Entry "
        "GROUP BY l.Item");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            Source& source = sources[fields[0].Get<uint32>()];
            source.normalMin = fields[1].Get<uint8>();
            source.normal = fields[2].Get<uint8>() != 0;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "[quest] item sources: {} items in {} ms{}", sources.size(), GetMSTimeDiffToNow(start),
             result ? "" : "; the read failed, so items hold no quest back");
    sourcesReady.store(true, std::memory_order_release);
}

// The items once read, or nullptr while the read runs
std::unordered_map<uint32, Source> const* ItemSources()
{
    LoadQuestGateItems();
    return sourcesReady.load(std::memory_order_acquire) ? &sources : nullptr;
}

std::mutex gateLock;  // bots of different maps update on different threads
std::map<std::pair<ObjectGuid, uint32>, time_t> gateLogged;  // ponytail: never pruned; one time_t per bot and quest

// The first thing about the quest's creatures that makes it too much for the bot, or "" (Playerbots.RandomBotQuestGate
// levels, 0 off): a kill objective whose creature is that many levels above the bot (its lowest level), an elite one
// when the bot is solo, or an item only creatures that high (or only elites, when solo) drop.
std::string QuestGate(Player* bot, PlayerbotAI* botAI, Quest const* quest)
{
    int32 const levels = sConfigMgr->GetOption<int32>("Playerbots.RandomBotQuestGate", 0);
    if (levels <= 0 || !sRandomPlayerbotMgr.IsRandomBot(bot) || TestBots::IsTestBot(bot) ||
        !botAI->GetRealPlayersInGroup().empty())
        return "";

    bool const solo = !bot->GetGroup();
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const entry = quest->RequiredNpcOrGo[i];
        if (entry <= 0 || !quest->RequiredNpcOrGoCount[i])
            continue;

        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(uint32(entry));
        if (!ct)
            continue;

        if (int32(ct->minlevel) - int32(bot->GetLevel()) >= levels)
            return ct->Name + " (L" + std::to_string(ct->minlevel) + ")";

        if (solo && ct->rank > CREATURE_ELITE_NORMAL)
            return ct->Name + " (elite)";
    }

    auto const* items = ItemSources();
    for (uint8 i = 0; items && i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        auto it = quest->RequiredItemId[i] ? items->find(quest->RequiredItemId[i]) : items->end();
        if (it == items->end())
            continue;

        if (it->second.normal ? int32(it->second.normalMin) - int32(bot->GetLevel()) >= levels : solo)
            return "item " + std::to_string(quest->RequiredItemId[i]) + (it->second.normal ? " (dropped from L" +
                   std::to_string(it->second.normalMin) + ")" : " (elites only)");
    }

    return "";
}

// True when the gate holds the quest back. With RandomBotQuestGateAct 0 it only logs, once per bot and quest in 10 min.
bool QuestGated(Player* bot, PlayerbotAI* botAI, Quest const* quest, char const* what)
{
    std::string const why = QuestGate(bot, botAI, quest);
    if (why.empty())
        return false;

    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(gateLock);
        time_t& last = gateLogged[{bot->GetGUID(), quest->GetQuestId()}];
        if (now - last >= 600)
        {
            last = now;
            LOG_INFO("playerbots", "[quest] {} (L{}) {} quest {} ({}): {}", bot->GetName(), bot->GetLevel(),
                     sConfigMgr->GetOption<bool>("Playerbots.RandomBotQuestGateAct", false) ? what : "would skip",
                     quest->GetQuestId(), quest->GetTitle(), why);
        }
    }

    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotQuestGateAct", false);
}
}  // namespace

void LoadQuestGateItems()
{
    // ponytail: a detached thread; a worldserver stopping within the read's 0.2 s may meet a closed pool
    static std::once_flag once;
    std::call_once(once, [] { std::thread(ReadQuestGateItems).detach(); });
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    if (newDest)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // performance optimization
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // agent-wow: a walk through a mob that would aggro the bot is held, not walked (Playerbots.RandomBotPathDanger
    // levels, RandomBotPathDangerHold seconds; hold 0 only logs). The bot waits where it stands and tries again: a
    // patrol passes, a pack despawns. Held before the stuck check below, so waiting never counts as no progress.
    int32 const dangerLevels = sConfigMgr->GetOption<int32>("Playerbots.RandomBotPathDanger", 0);
    auto held = [&](std::vector<G3D::Vector3> const& points)
    {
        if (dangerLevels <= 0 || bot->IsInCombat())
            return false;

        Creature* mob = DangerOnWalk(botAI, bot, points);
        if (!mob)
            return false;

        int32 const hold = sConfigMgr->GetOption<int32>("Playerbots.RandomBotPathDangerHold", 0);
        time_t const now = time(nullptr);
        {
            std::lock_guard<std::mutex> guard(pathLock);
            time_t& last = pathLogged[bot->GetGUID()];
            if (now - last >= std::max(hold, 30))
            {
                last = now;
                LOG_INFO("playerbots", "[path] {} {} on its way to {:.0f}, {:.0f}: {} (L{}{}) {:.0f} yd off would aggro it",
                         bot->GetName(), hold > 0 ? "holds" : "would hold", dest.GetPositionX(), dest.GetPositionY(),
                         mob->GetName(), mob->GetLevel(),
                         mob->GetCreatureTemplate()->rank > CREATURE_ELITE_NORMAL ? " elite" : "", bot->GetDistance(mob));
            }
        }

        if (hold <= 0)
            return false;

        // agent-wow (#2954, step 3a): a mob that stands on the way (a rare at its spot, a camp) holds the bot again every
        // hold, for as long as it stands there: read 10-28/29 with hold 0, 47 walks stayed in sight of one for 2+ minutes
        // and 3 for 5+. After RandomBotPathDangerHoldTries holds on the way to one goal the bot gives that goal up and
        // picks another where it stands, as a player who sees the way blocked does. 0 holds without end, as before.
        int32 const tries = sConfigMgr->GetOption<int32>("Playerbots.RandomBotPathDangerHoldTries", 0);
        if (tries > 0)
        {
            uint32 holds;
            {
                std::lock_guard<std::mutex> guard(pathLock);
                std::pair<WorldPosition, uint32>& mine = pathHolds[bot->GetGUID()];
                if (mine.first != dest)
                    mine = {dest, 0};
                holds = ++mine.second;
            }
            if (holds > uint32(tries))
            {
                LOG_INFO("playerbots", "[path] {} gives up on {:.0f}, {:.0f} after {} holds: {} (L{}) stays on its way",
                         bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), tries, mob->GetName(), mob->GetLevel());
                {
                    std::lock_guard<std::mutex> guard(pathLock);
                    pathHolds.erase(bot->GetGUID());
                }
                bot->StopMoving();
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
        }

        bot->StopMoving();
        ForceToWait(uint32(hold) * IN_MILLISECONDS);
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        return true;
    };

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            // ponytail: a straight line to the committed point, not its spline's bends
            if (remaining > 10.0f)
            {
                held({G3D::Vector3(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ)});
                return true;
            }
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
    {
        // No meaningful progress toward dest for `stuckTime`: fall
        // back to teleporting directly so the bot can get on with
        // its RPG objective instead of oscillating indefinitely.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        // agent-wow local patch (Logan, 2026-10-29, #2954): this is a teleport under the 10-13 ruling (patch 27). With
        // Playerbots.RandomBotStuckStay a bot that makes no progress gives the goal up and picks another where it
        // stands, as a player who can't get somewhere does. Logged at info, so its rate is read.
        if (sConfigMgr->GetOption<bool>("Playerbots.RandomBotStuckStay", false))
        {
            LOG_INFO("playerbots", "[stuck] {} gives up on {:.0f}, {:.0f} after {}s with no progress - Zone: {} ({})",
                     bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), stuckTime / IN_MILLISECONDS,
                     bot->GetZoneId(), zone_name);
            {
                // agent-wow (#3159): give-ups within 200 yd of the first, inside an hour, count toward a relocate
                std::lock_guard<std::mutex> guard(pathLock);
                auto& [spot, since, count] = stuckSpots[bot->GetGUID()];
                time_t const now = time(nullptr);
                if (!count || now - since > HOUR || spot.GetMapId() != bot->GetMapId() || bot->GetExactDist(spot) > 200.0f)
                {
                    spot = WorldPosition(bot);
                    since = now;
                    count = 0;
                }
                ++count;
            }
            bot->StopMoving();
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(dest);
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        if (held({G3D::Vector3(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ())}))
            return true;

        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                if (held(path.GetPath()))
                    return true;

                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->rpgStatistic.questRewarded++;
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::CanInteractWithQuestGiver(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT: // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT: // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                float score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + 3 < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Elite quest and dungeon quest etc
    if (quest->GetType() != 0)
        return false;

    // now we only capable of doing solo quests
    if (quest->GetSuggestedPlayers() >= 2)
        return false;

    // agent-wow: the creatures it wants are too far above the bot (Playerbots.RandomBotQuestGate)
    if (QuestGated(bot, botAI, quest, "skips"))
        return false;

    return true;
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    // agent-wow: an incomplete quest the gate holds back is dropped at once, not only when the log is full: a bot that
    // took it at a lower level (or before the gate) would walk to creatures it was meant to leave. A bot that has
    // killed or looted some of it has put work in; it still drops, the same as the full-log pass below.
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || !QuestGated(bot, botAI, quest, "drops"))
            continue;

        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        botAI->rpgStatistic.questDropped++;
    }

    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) ||
            bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        if (quest->GetZoneOrSort() < 0 || (quest->GetZoneOrSort() > 0 && quest->GetZoneOrSort() != botZoneId))
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->rpgStatistic.questDropped++;
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    if (ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract(true, 80.0f))
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
        if (!object)
            return false;

        if (bot->CanInteractWithQuestGiver(object))
        {
            InteractWithNpcOrGameObjectForQuest(npcOrGo);
            ForceToWait(5000);
            return true;
        }
        return MoveWorldObjectTo(npcOrGo);
    }
    return false;
}

ObjectGuid NewRpgBaseAction::ChooseNpcOrGameObjectToInteract(bool questgiverOnly, float distanceLimit)
{
    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");

    if (possibleTargets.empty() && possibleGameObjects.empty())
        return ObjectGuid();

    WorldObject* nearestObject = nullptr;
    for (ObjectGuid& guid : possibleTargets)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    for (ObjectGuid& guid : possibleGameObjects)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    if (nearestObject)
        return nearestObject->GetGUID();

    // No questgiver to accept or reward
    if (questgiverOnly)
        return ObjectGuid();

    if (possibleTargets.empty())
        return ObjectGuid();

    int idx = urand(0, possibleTargets.size() - 1);
    ObjectGuid guid = possibleTargets[idx];
    WorldObject* object = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
    if (!object)
        object = ObjectAccessor::GetGameObject(*bot, guid);

    if (object && object->IsInWorld())
    {
        return object->GetGUID();
    }
    return ObjectGuid();
}

bool NewRpgBaseAction::HasQuestToAcceptOrReward(WorldObject* object)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            return true;
        }
    }
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            return true;
        }
    }
    return false;
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

// agent-wow local patch (Logan, 2026-10-29, #2954): the ladder walk, in place of the teleport for level that patch 27
// turned off. A random bot that has outgrown its zone walks to the nearest hub on its map whose zone's band holds its
// level: go camp's own pick (the hubs the teleport chose from), without go camp's two limits, its own zone and 2,500 yd.
// Off unless Playerbots.RandomBotLadderWalk, and only while the path-danger hold is on (RandomBotPathDangerHold above
// 0): nothing else holds a long walk back from a mob far above the bot. A goal is picked at most every 20 minutes, so a
// goal given up (stuck, or held too often) isn't taken again at the next idle. A grouped bot keeps to its group.
// agent-wow local patch (Logan, 2026-10-29, #2954, his answer 1): the way off Teldrassil and back, as a night elf player
// goes. Darnassus' area trigger 527 takes a bot down to Rut'theran, where Vesprystus flies it to Auberdine; trigger 542
// takes it back up. Behind Playerbots.RandomBotTeldrassilLeg, inside the ladder walk's own conditions.
static uint32 constexpr TRIGGER_DOWN = 527, TRIGGER_UP = 542;
static float constexpr TREETOP_Z = 300.0f;  // Rut'theran stands at about 25 yd, the tree's top at about 1,300

// The bot has reached one of the two triggers' spots (go camp's arrival): it walks through, and the core moves it.
bool NewRpgBaseAction::TakeIslandTrigger(Player* bot, WorldPosition const& reached)
{
    for (uint32 const id : {TRIGGER_DOWN, TRIGGER_UP})
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(id);
        if (!at || at->map != reached.GetMapId() || reached.GetExactDist(at->x, at->y, at->z) > 1.0f ||
            bot->GetDistance(at->x, at->y, at->z) > at->radius)
            continue;

        LOG_INFO("playerbots", "[ladder] {} (L{}) walks through area trigger {}", bot->GetName(), bot->GetLevel(), id);
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << id;
        packet.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return true;
    }
    return false;
}

WorldPosition NewRpgBaseAction::SelectLadderHubPos()
{
    if (!sConfigMgr->GetOption<bool>("Playerbots.RandomBotLadderWalk", false) ||
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotPathDangerHold", 0) <= 0 || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        bot->GetGroup())
        return WorldPosition();

    {
        std::lock_guard<std::mutex> guard(pathLock);
        auto const it = ladderPicked.find(bot->GetGUID());
        if (it != ladderPicked.end() && GetMSTimeDiffToNow(it->second) < 20 * MINUTE * IN_MILLISECONDS)
            return WorldPosition();
    }

    uint32 const zone = bot->GetZoneId();
    if (sConfigMgr->GetOption<bool>("Playerbots.RandomBotTeldrassilLeg", false) && bot->GetMapId() == 1 &&
        (zone == AREA_TELDRASSIL || zone == AREA_DARNASSUS))
    {
        // the island's band is Teldrassil's, Darnassus' too (a capital has none of its own)
        bool const outgrown = sTravelMgr.OutgrownZone(bot, AREA_TELDRASSIL), below = bot->GetPositionZ() < TREETOP_Z;
        uint32 const trigger = outgrown && !below ? TRIGGER_DOWN : !outgrown && below ? TRIGGER_UP : 0;
        if (AreaTrigger const* at = trigger ? sObjectMgr->GetAreaTrigger(trigger) : nullptr)
        {
            {
                std::lock_guard<std::mutex> guard(pathLock);
                ladderPicked[bot->GetGUID()] = getMSTime();
            }
            LOG_INFO("playerbots", "[ladder] {} (L{}) walks to area trigger {} to leave the {}", bot->GetName(),
                     bot->GetLevel(), trigger, below ? "shore for the tree" : "tree");
            return WorldPosition(at->map, at->x, at->y, at->z);
        }
        if (outgrown && below)
            return WorldPosition();  // at Rut'theran: the flight, which RandomChangeStatus takes
    }

    if (!sTravelMgr.OutgrownZone(bot))
        return WorldPosition();

    WorldPosition dest{};
    float nearest = FLT_MAX;
    for (WorldLocation const& loc : sTravelMgr.GetTravelHubs(bot))
    {
        if (loc.GetMapId() != bot->GetMapId())
            continue;

        // an outgrown zone's own hubs aren't listed at this level: every hub here is in another zone
        float const dis = bot->GetExactDist(loc);
        if (dis < 50.0f || dis >= nearest)
            continue;

        nearest = dis;
        dest = loc;
    }

    if (dest != WorldPosition())
    {
        {
            std::lock_guard<std::mutex> guard(pathLock);
            ladderPicked[bot->GetGUID()] = getMSTime();
        }
        LOG_INFO("playerbots", "[ladder] {} (L{}) has outgrown zone {} and walks {:.0f} yd to the hub at {:.0f}, {:.0f}",
                 bot->GetName(), bot->GetLevel(), bot->GetZoneId(), nearest, dest.GetPositionX(), dest.GetPositionY());
    }
    return dest;
}

// agent-wow (#3159): the nearest player to `pos` on `mapId` (GMs counted, seen or not), FLT_MAX if none. The list is
// RandomPlayerbotMgr's of every player that isn't a random bot: people, pilots and their own bots.
static float NearestPlayer(uint32 mapId, Position const& pos)
{
    float nearest = FLT_MAX;
    for (Player* player : sRandomPlayerbotMgr.GetPlayers())
        if (player && player->IsInWorld() && player->GetMapId() == mapId)
            nearest = std::min(nearest, player->GetExactDist2d(pos));
    return nearest;
}

// agent-wow local patch (Logan's ruling, 2026-10-29 18:56Z, #3159): an unseen move for a stranded or stuck random bot,
// which narrows the 10-13 no-teleport ruling. Stranded: its level is 3 or more below where its zone's band starts,
// outside a starting zone (their bands start at 5). Stuck: patch 52 gave up 3 times within 200 yd in an hour, so the
// bot can't leave the spot (one give-up is a goal past a wall). It moves only ungrouped, out of a fight, on foot, with
// no player within 150 yd of where it stands or of the hub, and at most once every Playerbots.RandomBotRelocate hours.
// The hub is go camp's list for its level and faction: the nearest on its map, else the first on another. A level-up
// only raises a bot toward its band, so it never starts a move.
bool NewRpgBaseAction::RelocateStranded()
{
    uint32 const hours = sPlayerbotAIConfig.RandomBotRelocate;
    if (!hours || !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup() || bot->IsInCombat() || !bot->IsAlive() ||
        bot->GetTransport() || bot->IsInFlight())
        return false;

    uint32 const bandLow = sTravelMgr.ZoneBandLow(bot->GetZoneId());
    bool const stranded = bandLow > 5 && bot->GetLevel() + 3 <= bandLow;
    bool stuck = false;
    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(pathLock);
        auto const last = relocated.find(bot->GetGUID());
        if (last != relocated.end() && now - last->second < time_t(hours) * HOUR)
            return false;
        auto const spot = stuckSpots.find(bot->GetGUID());
        stuck = spot != stuckSpots.end() && std::get<2>(spot->second) >= 3 && now - std::get<1>(spot->second) <= HOUR;
    }
    if (!stranded && !stuck)
        return false;

    float const here = NearestPlayer(bot->GetMapId(), *bot);
    if (here < 150.0f)
        return false;

    WorldLocation const* dest = nullptr;
    float destNear = FLT_MAX, best = FLT_MAX;
    std::vector<WorldLocation> const hubs = sTravelMgr.GetTravelHubs(bot);
    for (WorldLocation const& hub : hubs)
    {
        float const dis = hub.GetMapId() == bot->GetMapId() ? bot->GetExactDist(hub) : FLT_MAX / 2;
        if (dis < 50.0f || dis >= best)
            continue;
        float const there = NearestPlayer(hub.GetMapId(), hub);
        if (there < 150.0f)
            continue;
        best = dis;
        dest = &hub;
        destNear = there;
    }
    if (!dest)
        return false;

    {
        std::lock_guard<std::mutex> guard(pathLock);
        relocated[bot->GetGUID()] = now;
        stuckSpots.erase(bot->GetGUID());
    }
    float const nearest = std::min(here, destNear);
    LOG_INFO("playerbots", "[relocate] {} (L{}) zone {} -> hub ({:.0f}, {:.0f}, map {}), {}: {}", bot->GetName(),
             bot->GetLevel(), bot->GetZoneId(), dest->GetPositionX(), dest->GetPositionY(), dest->GetMapId(),
             stranded ? "stranded" : "stuck",
             nearest == FLT_MAX ? std::string("no player on either map")
                                : fmt::format("{:.0f} yd from nearest player", nearest));
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(dest->GetMapId(), dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ(),
                    dest->GetOrientation());
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    // agent-wow local patch (#3144): a flight the bot can't pay for isn't picked. ActivateTaxiPathTo refuses it
    // (ERR_TAXINOTENOUGHMONEY), and the bot picked the same one again at its next idle: Booty Bay's 630c a leg against
    // the 1-48c of the level 5s stranded there. The reputation discount is left out, so a bot just short of the full
    // price isn't offered the flight.
    if (sConfigMgr->GetOption<bool>("Playerbots.RandomBotBoatBand", false))
        std::erase_if(availablePaths, [this](std::vector<uint32> const& nodes)
        {
            uint32 total = 0;
            for (size_t i = 1; i < nodes.size(); ++i)
            {
                uint32 pathId = 0, cost = 0;
                sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], pathId, cost);
                total += cost;
            }
            return total > bot->GetMoney();
        });
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    // agent-wow local patch (#2954): an outgrown bot walks the ladder before it draws a status (at a level-up too: the
    // bot is idle once its fight or its goal ends)
    // agent-wow local patch (#3159): a stranded or stuck bot is moved unseen before anything else
    if (RelocateStranded())
        return true;

    WorldPosition ladder = SelectLadderHubPos();
    if (ladder != WorldPosition())
    {
        botAI->rpgInfo.ChangeToGoCamp(ladder);
        SpeechForms::Going(botAI, ladder);
        return true;
    }

    // agent-wow local patch (#2954): an outgrown night elf down at Rut'theran flies on, by Vesprystus
    if (sConfigMgr->GetOption<bool>("Playerbots.RandomBotTeldrassilLeg", false) &&
        sConfigMgr->GetOption<bool>("Playerbots.RandomBotLadderWalk", false) && sRandomPlayerbotMgr.IsRandomBot(bot) &&
        !bot->GetGroup() && bot->GetZoneId() == AREA_TELDRASSIL && bot->GetPositionZ() < TREETOP_Z &&
        sTravelMgr.OutgrownZone(bot))
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            LOG_INFO("playerbots", "[ladder] {} (L{}) flies on from Rut'theran", bot->GetName(), bot->GetLevel());
            botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
            return true;
        }
    }

    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            break;
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                SpeechForms::Going(botAI, pos);  // agent-wow local patch
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                SpeechForms::Going(botAI, pos);  // agent-wow local patch
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            botAI->rpgInfo.ChangeToDoGather();
            return true;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
