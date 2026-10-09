/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionSellAction.h"

#include <algorithm>

#include "AuctionHouseMgr.h"
#include "Config.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "NewRpgInfo.h"
#include "Playerbots.h"

namespace
{
class AuctionableItemsVisitor : public IterateItemsVisitor
{
public:
    AuctionableItemsVisitor(AiObjectContext* context) : context(context) {}

    bool Visit(Item* item) override
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (item->IsSoulBound() || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM ||
            proto->Class == ITEM_CLASS_QUEST || proto->HasFlag(ITEM_FLAG_CONJURED) || !item->CanBeTraded() ||
            item->IsNotEmptyBag() || item->GetUInt32Value(ITEM_FIELD_DURATION) || !proto->SellPrice)
            return true;

        // ITEM_USAGE_AH: not wanted by this bot (no upgrade, no skill, no quest), quality >= common, unbound
        if (context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get() != ITEM_USAGE_AH)
            return true;

        items.push_back(item);
        return true;
    }

    std::vector<Item*> items;

private:
    AiObjectContext* context;
};
}  // namespace

bool AuctionSellAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    ObjectGuid const auctioneer = visit->npcOrGo;

    Creature* creature = bot->GetNPCIfCanInteractWith(auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
        return false;
    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
    if (!house)
        return false;

    AuctionableItemsVisitor visitor(context);
    IterateItems(&visitor);
    std::vector<Item*>& items = visitor.items;
    std::sort(items.begin(), items.end(), [](Item* a, Item* b)
              { return a->GetTemplate()->SellPrice * a->GetCount() > b->GetTemplate()->SellPrice * b->GetCount(); });

    uint32 const etime = MIN_AUCTION_TIME / MINUTE;  // 12 h, the smallest deposit
    float const multiplier = sConfigMgr->GetOption<float>("Playerbots.RandomBotAuctionPriceMultiplier", 1.0f);
    uint32 const maxListed = sConfigMgr->GetOption<uint32>("Playerbots.RandomBotAuctionSellMax", 3);
    uint32 listed = 0;
    uint32 deposits = 0;
    for (Item* item : items)
    {
        if (listed >= maxListed)
            break;

        ItemTemplate const* proto = item->GetTemplate();
        uint32 const count = item->GetCount();
        uint32 const vendor = proto->SellPrice * count;
        // a vendor sells an item for about four times what it pays for it
        uint64 const unit = proto->BuyPrice ? proto->BuyPrice : uint64(proto->SellPrice) * 4;
        uint64 const buyout = uint64(unit * count * multiplier * frand(0.85f, 1.15f));
        uint64 const bid = uint64(buyout * frand(0.6f, 0.8f));
        if (bid <= vendor || buyout > MAX_MONEY_AMOUNT)
            continue;

        uint32 const deposit = AuctionHouseMgr::GetAuctionDeposit(house, etime * MINUTE, item, count);
        if (!bot->HasEnoughMoney(deposits + deposit))
            continue;
        deposits += deposit;

        // CMSG_AUCTION_SELL_ITEM is PROCESS_THREADUNSAFE: queue it for the world thread, where the
        // handler checks, takes the deposit, moves the item into the auction house and saves
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_SELL_ITEM, 8 + 4 + 8 + 4 * 4);
        *packet << auctioneer << uint32(1) << item->GetGUID() << count << uint32(bid) << uint32(buyout) << etime;
        bot->GetSession()->QueuePacket(packet);

        ++listed;
        LOG_INFO("playerbots", "[AH sell] {} lists {}x{} at {}: bid {} buyout {} deposit {}", bot->GetName(), count,
                 proto->ItemId, creature->GetName(), bid, buyout, deposit);
    }

    return listed > 0;
}
