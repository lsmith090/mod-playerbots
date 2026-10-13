/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgErrandsAction.h"

#include <algorithm>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
// a bot walks no farther than this for a repair: a longer way crosses zones above its level
constexpr float MAX_WALK = 1500.0f;

struct Spawn
{
    uint32 entry;
    uint16 mapId;
    float x, y, z;
};

// every spawn of a creature that repairs, gathered once
std::vector<Spawn> const& RepairSpawns()
{
    static std::vector<Spawn> const spawns = []
    {
        std::vector<Spawn> result;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
            if (creature && (creature->npcflag & UNIT_NPC_FLAG_REPAIR))
                result.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
        }
        return result;
    }();
    return spawns;
}

bool HostileFaction(Player* bot, uint32 creatureEntry)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(creatureEntry);
    FactionTemplateEntry const* theirs = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
    FactionTemplateEntry const* ours = bot->GetFactionTemplateEntry();
    return !theirs || !ours || ours->IsHostileTo(*theirs) || theirs->IsHostileTo(*ours);
}
}  // namespace

namespace BotErrands
{
bool KeyOn()
{
    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotEarnedGear", false);
}

bool Earns(Player* bot) { return KeyOn() && sRandomPlayerbotMgr.IsRandomBot(bot); }

bool Enabled(PlayerbotAI* botAI) { return Earns(botAI->GetBot()) && !botAI->GetMaster(); }

float Worst(Player* bot)
{
    float worst = 1.0f;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 const most = item ? item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) : 0;
        if (most)
            worst = std::min(worst, float(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) / float(most));
    }
    return worst;
}
}  // namespace BotErrands

bool NewRpgGoRepairAction::Execute(Event /*event*/)
{
    auto nextSearch = context->GetValue<time_t>("last said", BotErrands::NEXT_REPAIR_SEARCH);
    time_t const now = time(nullptr);

    Spawn const* nearest = nullptr;
    float nearestDist = 0.0f;
    for (Spawn const& spawn : RepairSpawns())
    {
        if (spawn.mapId != bot->GetMapId())
            continue;

        float const dist = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (dist > MAX_WALK || (nearest && dist >= nearestDist) || HostileFaction(bot, spawn.entry))
            continue;

        nearest = &spawn;
        nearestDist = dist;
    }

    if (!nearest)
    {
        nextSearch->Set(now + 10 * MINUTE);
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;
    if (Creature* repairer = bot->FindNearestCreature(nearest->entry, 60.0f))
    {
        info.ChangeToWanderNpc();
        std::get<NewRpgInfo::WanderNpc>(info.data).npcOrGo = repairer->GetGUID();
        nextSearch->Set(now + 2 * MINUTE);
    }
    else if (nearestDist < 30.0f)
    {
        // at the spawn and nobody there (dead, or a spawn of an event that isn't on)
        nextSearch->Set(now + 10 * MINUTE);
        return false;
    }
    else
    {
        info.ChangeToGoCamp(WorldPosition(nearest->mapId, nearest->x, nearest->y, nearest->z));
        nextSearch->Set(now + MINUTE);
    }

    LOG_INFO("playerbots", "[gear] {} goes to repairer {} ({:.0f} yd), its worst item at {:.0f}%, with {} copper",
             bot->GetName(), nearest->entry, nearestDist, BotErrands::Worst(bot) * 100.0f, bot->GetMoney());
    return true;
}

bool VendorErrandsAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* npc = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR);
    if (!npc)
        return false;

    bool const sells = npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR);
    uint32 const had = bot->GetMoney();

    // Junk first, so the rest is paid from what it fetched. Grey items only: upstream's "vendor" sale also takes what
    // the bot would list at the auction house (patch 2), and that is the stock other bots are to buy from.
    if (sells)
        botAI->DoSpecificAction("sell", Event("errands", "gray"), true);
    uint32 const afterSale = bot->GetMoney();

    float const worn = BotErrands::Worst(bot);
    if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && worn < 1.0f)
        botAI->DoSpecificAction("repair", Event(), true);
    uint32 const afterRepair = bot->GetMoney();

    // What it can use and pay for, by upstream's own measure (item usage, and its budget per kind of need); the buy
    // action equips an upgrade it bought
    if (sells)
        botAI->DoSpecificAction("buy", Event("errands", "vendor"), true);
    uint32 const left = bot->GetMoney();

    if (left == had && afterSale == had)
        return false;

    LOG_INFO("playerbots", "[gear] {} at {} ({}): sold for {}, repairs {} (worst item was at {:.0f}%), bought for {}, {} copper left",
             bot->GetName(), npc->GetName(), npc->GetEntry(), afterSale - had,
             afterSale > afterRepair ? afterSale - afterRepair : 0, worn * 100.0f,
             afterRepair > left ? afterRepair - left : 0, left);
    return true;
}
