/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindTargetValue.h"

#include <map>
#include <mutex>
#include <set>

#include "Config.h"
#include "CreatureGroups.h"
#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "World.h"

namespace
{
// agent-wow: the other idle mobs within 15 yd of the unit, counted up to `stop`.
// ponytail: counts the other possible targets, not a fresh grid search; a mob out of the bot's sight range is missed.
uint32 PackCount(Unit* unit, GuidVector const& targets, uint32 stop)
{
    uint32 others = 0;
    for (ObjectGuid const guid : targets)
    {
        if (guid == unit->GetGUID())
            continue;

        Creature* other = ObjectAccessor::GetCreature(*unit, guid);
        if (!other || !other->IsAlive() || other->IsInCombat() || other->IsCritter())
            continue;

        if (other->GetDistance(unit) <= 15.0f && ++others >= stop)
            break;
    }

    return others;
}

bool InPack(Unit* unit, GuidVector const& targets, uint32 packSize) { return PackCount(unit, targets, packSize) >= packSize; }

// agent-wow: the mobs other than the pull that would come to it, by the three ways the core brings them:
// - called: within CreatureFamilyAssistanceRadius (10 yd) of the pull, in its line of sight, and what the core's own
//   AnyAssistCreatureInRangeCheck asks (Creature::CanAssistTo: same faction, answers calls, idle, hostile to the bot);
// - aggro: not called, but the bot, fighting at the pull, stands inside the mob's own aggro distance;
// - formation: a member of the pull's creature_formations group whose group_ai makes the group assist.
// ponytail: the bot is taken to fight where the pull stands, so a caster who stands off 25 yd is closer to some mobs and
// farther from others; a flee-call at low health (30 yd) and a patrol walking in are not counted.
struct Joiners
{
    uint32 called = 0, aggro = 0, formation = 0;
    uint32 Total() const { return called + aggro + formation; }
};

Joiners WouldJoin(PlayerbotAI* botAI, Player* bot, Creature* pull, GuidVector const& targets)
{
    Joiners joiners;
    std::set<ObjectGuid> counted{pull->GetGUID()};
    float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    for (ObjectGuid const guid : targets)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsEngaged() || mob->IsCritter() || mob->GetOwnerGUID() ||
            !counted.insert(guid).second)
            continue;

        float const d = mob->GetDistance(pull);
        if (d <= assist && mob->CanAssistTo(pull, bot) && pull->IsWithinLOSInMap(mob))
            ++joiners.called;
        else if (mob->IsHostileTo(bot) && d < mob->GetAttackDistance(bot) && pull->IsWithinLOSInMap(mob))
            ++joiners.aggro;
        else
            counted.erase(guid);
    }

    if (CreatureGroup* formation = pull->GetFormation())
    {
        auto const info = sFormationMgr->CreatureGroupMap.find(pull->GetSpawnId());
        bool const assists = info != sFormationMgr->CreatureGroupMap.end() &&
                             (info->second.groupAI & uint32(GroupAIFlags::GROUP_AI_FLAG_ASSIST_MASK));
        for (auto const& member : formation->GetMembers())
            if (assists && member.first && member.first->IsAlive() && !member.first->IsEngaged() &&
                counted.insert(member.first->GetGUID()).second)
                ++joiners.formation;
    }

    return joiners;
}

std::mutex joinLogLock;  // bots of different maps update on different threads
std::map<ObjectGuid, std::pair<ObjectGuid, time_t>> joinLogged;  // by bot: the last pick logged and when; ponytail: never pruned
}  // namespace

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = nullptr;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(uint32 assistCount)
{
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        return unit;
    }

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");
    if (targets.empty())
        return nullptr;

    float distance = 0;
    Unit* result = nullptr;
    std::unordered_map<uint32, bool> needForQuestMap;

    // agent-wow knobs, read here so PlayerbotAIConfig.h stays untouched (a whole-module recompile).
    // GrindMaxLevelAbove: the most levels a grind target may stand above the bot (upstream: 4).
    // GrindPackSize: a solo bot skips a mob with this many other idle mobs within 15 yd of it (0: off).
    static int32 const maxLevelAbove = sConfigMgr->GetOption<int32>("Playerbots.GrindMaxLevelAbove", 4);
    static uint32 const packSize = sConfigMgr->GetOption<uint32>("Playerbots.GrindPackSize", 0);
    // agent-wow: GrindJoinSize replaces the pack rule: a solo bot skips a mob that this many others would come to
    // (called, in aggro distance, or in its formation), 0: the pack rule stands. GrindJoinSizeLow is the number for a
    // bot below level 10 (0: GrindJoinSize). GrindJoinLog: seconds between two [grind] lines per bot (0: none).
    static uint32 const joinSize = sConfigMgr->GetOption<uint32>("Playerbots.GrindJoinSize", 0);
    static uint32 const joinLow = sConfigMgr->GetOption<uint32>("Playerbots.GrindJoinSizeLow", 0);
    static int32 const joinLog = sConfigMgr->GetOption<int32>("Playerbots.GrindJoinLog", 0);
    uint32 const join = bot->GetLevel() < 10 && joinLow ? joinLow : joinSize;

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
            continue;

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            continue;

        if (!bot->isHonorOrXPTarget(unit))
            continue;

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
            continue;

        if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
            continue;

        // if (!bot->InBattleground() && master && master->GetDistance(unit) >= sPlayerbotAIConfig.GrindDistance &&
        // !sRandomPlayerbotMgr.IsRandomBot(bot)) continue;

        // Bots in bot-groups no have a more limited range to look for grind target
        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > sPlayerbotAIConfig.LootDistance)
        {
            if (botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                botAI->TellMaster(chat->FormatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        if (!bot->InBattleground() && (int)unit->GetLevel() - (int)bot->GetLevel() > maxLevelAbove &&
            !unit->GetGUID().IsPlayer())
            continue;

        if (Creature* creature = unit->ToCreature())
            if (CreatureTemplate const* CreatureTemplate = creature->GetCreatureTemplate())
                if (CreatureTemplate->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
                    continue;

        if (!bot->IsWithinLOSInMap(unit))
        {
            continue;
        }

        // agent-wow: a careful player doesn't pull a mob standing in a pack; the others come along (social aggro).
        if (join && !bot->InBattleground() && !group && unit->ToCreature())
        {
            if (WouldJoin(botAI, bot, unit->ToCreature(), targets).Total() >= join)
                continue;
        }
        else if (packSize && !bot->InBattleground() && !group && unit->ToCreature() &&
                 InPack(unit, targets, packSize))
            continue;

        bool inactiveGrindStatus = botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM && botAI->rpgInfo.GetStatus() != RPG_IDLE;

        float aggroRange = 30.0f;
        if (unit->ToCreature())
            aggroRange = std::min(30.0f, unit->ToCreature()->GetAggroRange(bot) + 10.0f);
        bool outOfAggro = unit->ToCreature() && bot->GetDistance(unit) > aggroRange;
        if (inactiveGrindStatus && outOfAggro)
        {
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);

            if (!needForQuestMap[unit->GetEntry()])
                continue;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                if (!member || !member->IsAlive())
                    continue;

                float d = member->GetDistance(unit);
                if (!result || d < distance)
                {
                    distance = d;
                    result = unit;
                }
            }
        }
        else
        {
            float newdistance = bot->GetDistance(unit);
            if (!result || (newdistance < distance))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    // agent-wow: what the pick would bring, beside what the pack rule counts, to read against the [fight] lines
    if (joinLog > 0 && result && result->ToCreature() && !group && !bot->InBattleground())
    {
        time_t const now = time(nullptr);
        std::lock_guard<std::mutex> guard(joinLogLock);
        auto& last = joinLogged[bot->GetGUID()];
        if (last.first != result->GetGUID() && now - last.second >= joinLog)
        {
            last = {result->GetGUID(), now};
            Joiners const j = WouldJoin(botAI, bot, result->ToCreature(), targets);
            LOG_INFO("playerbots",
                     "[grind] {} (L{}) picks {} (entry {}, L{}) {:.0f} yd off; {} would join ({} called, {} in aggro, "
                     "{} in its formation); {} others within 15 yd",
                     bot->GetName(), bot->GetLevel(), result->GetName(), result->GetEntry(), result->GetLevel(),
                     bot->GetDistance(result), j.Total(), j.called, j.aggro, j.formation,
                     PackCount(result, targets, 99));
        }
    }

    return result;
}

bool GrindTargetValue::needForQuest(Unit* target)
{
    QuestStatusMap& questMap = bot->getQuestStatusMap();
    for (auto& quest : questMap)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest.first);
        if (!questTemplate)
            continue;

        uint32 questId = questTemplate->GetQuestId();
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);

        if (status == QUEST_STATUS_INCOMPLETE)
        {
            QuestStatusData const* questStatus = &bot->getQuestStatusMap()[questId];

            if (questTemplate->GetQuestLevel() > bot->GetLevel() + 5)
                continue;

            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; j++)
            {
                int32 entry = questTemplate->RequiredNpcOrGo[j];

                if (entry && entry > 0)
                {
                    int required = questTemplate->RequiredNpcOrGoCount[j];
                    int available = questStatus->CreatureOrGOCount[j];

                    if (required && available < required && target->GetEntry() == uint32(entry))
                        return true;
                }
            }
        }
    }

    if (CreatureTemplate const* data = sObjectMgr->GetCreatureTemplate(target->GetEntry()))
    {
        if (uint32 lootId = data->lootid)
        {
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot))
            {
                return true;
            }
        }
    }

    return false;
}

uint32 GrindTargetValue::GetTargetingPlayerCount(Unit* unit)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    uint32 count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if ((botAI && *botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!botAI && member->GetTarget() == unit->GetGUID()))
            ++count;
    }

    return count;
}
