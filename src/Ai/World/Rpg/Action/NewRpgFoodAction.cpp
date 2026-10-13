/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFoodAction.h"

#include <cmath>

#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
// a bot walks no farther than this for food: a longer way crosses zones above its level
constexpr float MAX_WALK = 1500.0f;

struct VendorSpawn
{
    uint32 entry;
    uint16 mapId;
    float x, y, z;
};

bool Edible(ItemTemplate const* proto, uint32 category)
{
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD ||
        proto->Spells[0].SpellCategory != category)
        return false;

    // beer, wine and rum share the drink category and restore nothing: their spell makes the drinker drunk
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
    return spell && !spell->HasEffect(SPELL_EFFECT_INEBRIATE);
}

// every spawn of a vendor that sells something to eat or drink for money, gathered once
std::vector<VendorSpawn> const& FoodVendorSpawns()
{
    static std::vector<VendorSpawn> const spawns = []
    {
        std::vector<VendorSpawn> result;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
            if (!items)
                continue;

            for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
            {
                VendorItem const* item = items->GetItem(slot);
                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                if (item && !item->ExtendedCost && (Edible(proto, BotFood::FOOD) || Edible(proto, BotFood::DRINK)))
                {
                    result.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
                    break;
                }
            }
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

// the vendor's slot with the best item of the category the bot can use and pay for one lot of (the highest required
// level, then the cheaper), or -1
int32 BestSlot(Player* bot, VendorItemData const* items, uint32 category)
{
    int32 best = -1;
    ItemTemplate const* bestProto = nullptr;
    for (uint32 slot = 0; items && slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!item || item->ExtendedCost || !Edible(proto, category) || proto->RequiredLevel > bot->GetLevel() ||
            (proto->RequiredSkill && !bot->HasSkill(proto->RequiredSkill)) || uint32(proto->BuyPrice) > bot->GetMoney())
            continue;

        if (bestProto && (proto->RequiredLevel < bestProto->RequiredLevel ||
                          (proto->RequiredLevel == bestProto->RequiredLevel && proto->BuyPrice >= bestProto->BuyPrice)))
            continue;

        best = int32(slot);
        bestProto = proto;
    }
    return best;
}

// the vendor sells something the bot is short of
bool SellsWhatIsShort(Player* bot, VendorItemData const* items)
{
    for (uint32 category : BotFood::Categories(bot))
        if (BotFood::Count(bot, category) < BotFood::LOW && BestSlot(bot, items, category) >= 0)
            return true;
    return false;
}
}  // namespace

namespace BotFood
{
bool KeyOn()
{
    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotBuyFood", false);
}

bool Enabled(PlayerbotAI* botAI)
{
    return KeyOn() && !botAI->HasCheat(BotCheatMask::food) && sRandomPlayerbotMgr.IsRandomBot(botAI->GetBot()) &&
           !botAI->GetMaster();
}

std::vector<uint32> Categories(Player* bot)
{
    if (bot->GetMaxPower(POWER_MANA) > 0)
        return {FOOD, DRINK};
    return {FOOD};
}

uint32 Count(Player* bot, uint32 category)
{
    // walked here, not read from "inventory items": that value is cached, and the buying loop counts after each lot
    uint32 count = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); item && Edible(item->GetTemplate(), category))
            count += item->GetCount();

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0; bag && slot < bag->GetBagSize(); ++slot)
            if (Item* item = bag->GetItemByPos(slot); item && Edible(item->GetTemplate(), category))
                count += item->GetCount();
    }
    return count;
}

bool Low(Player* bot)
{
    for (uint32 category : Categories(bot))
        if (Count(bot, category) < LOW)
            return true;
    return false;
}
}  // namespace BotFood

bool NewRpgGoFoodVendorAction::Execute(Event /*event*/)
{
    auto nextSearch = context->GetValue<time_t>("last said", BotFood::NEXT_VENDOR_SEARCH);
    time_t const now = time(nullptr);

    VendorSpawn const* nearest = nullptr;
    float nearestDist = 0.0f;
    for (VendorSpawn const& spawn : FoodVendorSpawns())
    {
        if (spawn.mapId != bot->GetMapId())
            continue;

        float const dist = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (dist > MAX_WALK || (nearest && dist >= nearestDist))
            continue;

        if (HostileFaction(bot, spawn.entry) ||
            !SellsWhatIsShort(bot, sObjectMgr->GetNpcVendorItemList(spawn.entry)))
            continue;

        nearest = &spawn;
        nearestDist = dist;
    }

    if (!nearest)
    {
        // no vendor in reach sells what it can use and pay for: look again once it has earned or walked some
        nextSearch->Set(now + 10 * MINUTE);
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;
    if (Creature* vendor = bot->FindNearestCreature(nearest->entry, 60.0f))
    {
        info.ChangeToWanderNpc();
        std::get<NewRpgInfo::WanderNpc>(info.data).npcOrGo = vendor->GetGUID();
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

    LOG_INFO("playerbots", "[food] {} goes to vendor {} ({:.0f} yd) with {} food, {} drink, {} copper", bot->GetName(),
             nearest->entry, nearestDist, BotFood::Count(bot, BotFood::FOOD), BotFood::Count(bot, BotFood::DRINK),
             bot->GetMoney());
    return true;
}

bool BuyOwnFoodAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* vendor = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR);
    VendorItemData const* vendorItems = vendor ? vendor->GetVendorItems() : nullptr;
    if (!vendorItems)
        return false;

    float const discount = bot->GetReputationPriceDiscount(vendor);
    uint32 bought = 0;
    for (uint32 category : BotFood::Categories(bot))
    {
        int32 const slot = BestSlot(bot, vendorItems, category);
        if (slot < 0)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItems->GetItem(slot)->item);
        uint32 const price = uint32(std::floor(proto->BuyPrice * discount));
        for (uint32 i = 0; i < 20 && BotFood::Count(bot, category) < BotFood::FULL; ++i)
        {
            if (bot->GetMoney() < price)
                break;

            uint32 const before = bot->GetItemCount(proto->ItemId);
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, proto->ItemId, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(proto->ItemId) <= before)
                break;
            ++bought;
        }
    }

    if (bought)
        LOG_INFO("playerbots", "[food] {} buys {} lots at {} ({}): {} food, {} drink, {} copper left", bot->GetName(),
                 bought, vendor->GetName(), vendor->GetEntry(), BotFood::Count(bot, BotFood::FOOD),
                 BotFood::Count(bot, BotFood::DRINK), bot->GetMoney());
    return bought > 0;
}
