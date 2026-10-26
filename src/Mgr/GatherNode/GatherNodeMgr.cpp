/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherNodeMgr.h"

#include <algorithm>
#include <cmath>

#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"

namespace
{
bool GetGatherSkillFromTemplate(GameObjectTemplate const* goInfo, uint32& skillId, uint32& reqSkillValue)
{
    if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
        return false;

    uint32 lockId = goInfo->GetLockId();
    if (!lockId)
        return false;

    LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
    if (!lockInfo)
        return false;

    for (uint8 j = 0; j < 8; ++j)
    {
        if (lockInfo->Type[j] != LOCK_KEY_SKILL)
            continue;

        uint32 skill = SkillByLockType(LockType(lockInfo->Index[j]));
        if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
        {
            skillId = skill;
            reqSkillValue = std::max(1u, lockInfo->Skill[j]);
            return true;
        }
    }

    return false;
}

// Local patch (agent-wow #2629): how near a spawn makes a node unsafe, and the cell size of the index that finds it.
constexpr float GATHER_THREAT_RADIUS = 30.0f;

uint64 ThreatCell(uint32 mapId, int32 cx, int32 cy)
{
    return (uint64(mapId) << 32) | (uint64(uint16(cx + 0x8000)) << 16) | uint16(cy + 0x8000);
}

int32 ThreatCellOf(float coord) { return int32(std::floor(coord / GATHER_THREAT_RADIUS)); }

// Read once per call, not per node: 0 is off.
uint32 SafeGap() { return sConfigMgr->GetOption<uint32>("Playerbots.RandomBotGatherSafeGap", 0); }
}

void GatherNodeMgr::Load()
{
    uint32 oldMSTime = getMSTime();
    uint32 count = 0;

    // Loot ids with at least one freely lootable row. Quest-gated
    // "gathering" chests (Cactus Apple, Serpentbloom, ...) carry a
    // herbalism/mining lock but ALL their loot rows are QuestRequired, so
    // bots without the quest can't harvest them - and they never despawn,
    // which makes them permanent live-node bait. Note that filtering on
    // gameobject_questitem instead would be wrong: standard herbs carry a
    // conditional quest item (Root Sample) on top of their normal loot.
    std::unordered_set<uint32> openLootIds;
    if (QueryResult result =
            WorldDatabase.Query("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE QuestRequired = 0"))
    {
        do
        {
            openLootIds.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }

    std::unordered_map<ObjectGuid::LowType, uint32> dbZoneIds;
    if (QueryResult result = WorldDatabase.Query("SELECT guid, zoneId FROM gameobject WHERE zoneId <> 0"))
    {
        do
        {
            dbZoneIds[(*result)[0].Get<uint32>()] = (*result)[1].Get<uint32>();
        } while (result->NextRow());
    }

    for (auto const& [spawnId, goData] : sObjectMgr->GetAllGOData())
    {
        // Instances are excluded: the state targets outdoor zone roaming.
        MapEntry const* mapEntry = sMapStore.LookupEntry(goData.mapid);
        if (!mapEntry || !mapEntry->IsContinent())
            continue;

        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(goData.id);

        uint32 skillId = 0;
        uint32 reqSkillValue = 0;
        if (!GetGatherSkillFromTemplate(goInfo, skillId, reqSkillValue))
            continue;

        if (!openLootIds.contains(goInfo->GetLootId()))
            continue;

        GatherNodeSpawn node;
        node.spawnId = spawnId;
        node.pos = WorldPosition(goData.mapid, goData.posX, goData.posY, goData.posZ);
        node.skillId = skillId;
        node.reqSkillValue = reqSkillValue;
        auto zoneItr = dbZoneIds.find(spawnId);
        uint32 zoneId = zoneItr != dbZoneIds.end()
                            ? zoneItr->second
                            : sMapMgr->GetZoneId(PHASEMASK_NORMAL, goData.mapid, goData.posX, goData.posY, goData.posZ);
        _nodes[goData.mapid][zoneId].push_back(std::move(node));
        ++count;
    }

    LOG_INFO("playerbots", ">> Loaded {} gather node spawns in {} ms", count, GetMSTimeDiffToNow(oldMSTime));

    LoadThreats();
}

// Local patch (agent-wow #2629): after #2277 sent bots to nodes (DoGather 15), random-bot deaths to a creature 3+
// levels above them within 25 yd of a node rose from 74 to 735 in two days. Each node keeps, per team, the highest
// level among the spawns within 30 yd that would attack that team's player: not a civilian, a trigger or a mob immune
// to players (patch 44's harmless), and hostile by its faction template. A node's own level check is then one compare.
void GatherNodeMgr::LoadThreats()
{
    uint32 oldMSTime = getMSTime();

    std::unordered_map<uint64, std::vector<GatherNodeSpawn*>> cells;
    uint32 total = 0;
    for (auto& [mapId, zones] : _nodes)
        for (auto& [zoneId, nodes] : zones)
            for (GatherNodeSpawn& node : nodes)
            {
                cells[ThreatCell(mapId, ThreatCellOf(node.pos.GetPositionX()), ThreatCellOf(node.pos.GetPositionY()))]
                    .push_back(&node);
                ++total;
            }

    // A human's and an orc's own templates stand for each team's players.
    FactionTemplateEntry const* players[2] = {sFactionTemplateStore.LookupEntry(1), sFactionTemplateStore.LookupEntry(2)};
    if (!players[0] || !players[1])
        return;

    float const radiusSq = GATHER_THREAT_RADIUS * GATHER_THREAT_RADIUS;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (cells.empty())
            break;

        for (uint32 entry : {data.id, data.id2, data.id3})
        {
            CreatureTemplate const* info = entry ? sObjectMgr->GetCreatureTemplate(entry) : nullptr;
            if (!info || info->HasFlagsExtra(CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER) ||
                ((info->unit_flags | data.unit_flags) & UNIT_FLAG_IMMUNE_TO_PC))
                continue;

            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction)
                continue;

            bool hostile[2] = {faction->IsHostileTo(*players[0]), faction->IsHostileTo(*players[1])};
            if (!hostile[0] && !hostile[1])
                continue;

            WorldPosition pos(data.mapid, data.posX, data.posY, data.posZ);
            int32 cx = ThreatCellOf(data.posX);
            int32 cy = ThreatCellOf(data.posY);
            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    auto itr = cells.find(ThreatCell(data.mapid, cx + dx, cy + dy));
                    if (itr == cells.end())
                        continue;

                    for (GatherNodeSpawn* node : itr->second)
                    {
                        if (node->pos.sqDistance(pos) > radiusSq)
                            continue;

                        for (uint8 team = 0; team < 2; ++team)
                            if (hostile[team])
                                node->threatLevel[team] = std::max(node->threatLevel[team], info->maxlevel);
                    }
                }
        }
    }

    uint32 threatened = 0;
    for (auto const& [cell, nodes] : cells)
        for (GatherNodeSpawn const* node : nodes)
            if (node->threatLevel[0] || node->threatLevel[1])
                ++threatened;

    LOG_INFO("playerbots", "[gather] {} of {} gather nodes have a hostile spawn within {} yd, in {} ms", threatened,
             total, GATHER_THREAT_RADIUS, GetMSTimeDiffToNow(oldMSTime));
}

bool GatherNodeMgr::IsUsable(PlayerbotAI* botAI, Player* bot, GatherNodeSpawn const& node, uint32 safeGap)
{
    if (!botAI->HasSkill(SkillType(node.skillId)))
        return false;

    // Local patch (agent-wow #2629): no node with a spawn near it more than safeGap levels above the bot.
    TeamId team = bot->GetTeamId();
    if (safeGap && team < TEAM_NEUTRAL && node.threatLevel[team] > bot->GetLevel() + safeGap)
        return false;

    return bot->GetSkillValue(node.skillId) >= node.reqSkillValue;
}

std::vector<GatherNodeSpawn> const* GatherNodeMgr::GetZoneNodes(uint32 mapId, uint32 zoneId) const
{
    auto mapItr = _nodes.find(mapId);
    if (mapItr == _nodes.end())
        return nullptr;

    auto zoneItr = mapItr->second.find(zoneId);
    return zoneItr != mapItr->second.end() ? &zoneItr->second : nullptr;
}

bool GatherNodeMgr::HasUsableNodes(Player* bot)
{
    std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId());
    if (!nodes)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    uint32 safeGap = SafeGap();
    for (GatherNodeSpawn const& node : *nodes)
        if (IsUsable(botAI, bot, node, safeGap))
            return true;

    return false;
}

GatherNodeSpawn const* GatherNodeMgr::GetNextNode(Player* bot, std::unordered_set<ObjectGuid::LowType> const& visited)
{
    std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId());
    if (!nodes)
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);

    std::vector<std::pair<float, GatherNodeSpawn const*>> candidates;
    uint32 safeGap = SafeGap();
    for (GatherNodeSpawn const& node : *nodes)
    {
        if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node, safeGap))
            continue;

        // Don't route to spawn points we can already see to be empty.
        // Evaluated fresh on every selection so respawns come back into
        // the pool naturally.
        if (IsVerifiablyDown(map, node.pos, node.spawnId))
            continue;

        candidates.push_back({botPos.sqDistance(node.pos), &node});
    }

    if (candidates.empty())
        return nullptr;

    uint32 nearestCount = std::min<uint32>(4, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + nearestCount, candidates.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

    return candidates[urand(0, nearestCount - 1)].second;
}

GameObject* GatherNodeMgr::FindLiveNode(Map* map, ObjectGuid::LowType spawnId)
{
    auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second->isSpawned() && it->second->GetGoState() == GO_STATE_READY)
            return it->second;

    return nullptr;
}

bool GatherNodeMgr::IsVerifiablyDown(Map* map, WorldPosition const& pos, ObjectGuid::LowType spawnId)
{
    // An unloaded grid means "unknown", not "not up".
    if (!map->IsGridLoaded(pos.GetPositionX(), pos.GetPositionY()))
        return false;

    return !FindLiveNode(map, spawnId);
}

GatherNodeSpawn const* GatherNodeMgr::GetNearestLiveNode(Player* bot,
                                                         std::unordered_set<ObjectGuid::LowType> const& visited,
                                                         float radius)
{
    auto mapItr = _nodes.find(bot->GetMapId());
    if (mapItr == _nodes.end())
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);
    GatherNodeSpawn const* nearest = nullptr;
    float nearestDistSq = radius * radius;
    uint32 safeGap = SafeGap();
    // All zone buckets of the map: this query is deliberately not
    // zone-scoped, and a radius this small prunes nearly everything on the
    // cheap distance check.
    for (auto const& [zoneId, nodes] : mapItr->second)
    {
        for (GatherNodeSpawn const& node : nodes)
        {
            float distSq = botPos.sqDistance(node.pos);
            if (distSq > nearestDistSq)
                continue;

            if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node, safeGap))
                continue;

            if (!map->IsGridLoaded(node.pos.GetPositionX(), node.pos.GetPositionY()))
                continue;

            if (!FindLiveNode(map, node.spawnId))
                continue;

            nearest = &node;
            nearestDistSq = distSq;
        }
    }

    return nearest;
}
