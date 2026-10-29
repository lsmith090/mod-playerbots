/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAuctionAction.h"

#include <map>
#include <mutex>
#include <vector>

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameObject.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// a capital lies beside its starting zone: farther than this is another zone's city
constexpr float MAX_WALK = 3000.0f;
constexpr float MAILBOX_SIGHT = 100.0f;
// a piece must beat what the bot wears by this much to be worth its money
constexpr float BETTER = 1.1f;

struct Spawn
{
    uint32 entry;
    uint16 mapId;
    float x, y, z;
};

// every spawn of an auctioneer, gathered once
std::vector<Spawn> const& AuctioneerSpawns()
{
    static std::vector<Spawn> const spawns = []
    {
        std::vector<Spawn> result;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
            if (creature && (creature->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
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

bool Piece(Item* item)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    return proto && proto->Quality >= ITEM_QUALITY_UNCOMMON && !item->IsSoulBound() &&
           proto->Bonding != BIND_WHEN_PICKED_UP && proto->Bonding != BIND_QUEST_ITEM && proto->SellPrice;
}
}  // namespace

namespace BotAuction
{
bool KeyOn()
{
    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotAuctionBuy", false);
}

bool Enabled(PlayerbotAI* botAI)
{
    return KeyOn() && sRandomPlayerbotMgr.IsRandomBot(botAI->GetBot()) && !botAI->GetMaster();
}

// agent-wow local patch (#2955): the auction mail waiting for the bot, as one number that changes when a letter comes or
// goes, money is taken or an item moves; 0 when there is none
uint64 Waiting(Player* bot, time_t now)
{
    uint64 waiting = 0;
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now ||
            (!mail->money && mail->items.empty()))
            continue;

        waiting = waiting * 1000003 + mail->messageID + mail->money;
        for (MailItemInfo const& item : mail->items)
            waiting = waiting * 31 + item.item_guid;
    }
    return waiting;
}

// What each bot's last take at a mailbox asked for, and when (Playerbots.RandomBotAuctionMailWait)
std::mutex takeLock;
std::map<ObjectGuid, std::pair<uint64, time_t>> lastTake;  // ponytail: never pruned; one entry per bot that took mail

bool HasMail(Player* bot)
{
    time_t const now = GameTime::GetGameTime().count();
    uint64 const waiting = Waiting(bot, now);
    if (!waiting)
        return false;

    // agent-wow local patch (#2955): a take the core refused (full bags, a unique item it already has) leaves the same
    // mail waiting, and the bot walked back for it every 60-90 s (Silyssae: 546 trips in 15.7 h, the same 14 items each
    // time). The same mail as its last take doesn't send it back until RandomBotAuctionMailWait seconds have passed;
    // new mail or money does at once. 0 goes back every time, as before.
    int32 const wait = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAuctionMailWait", 0);
    if (wait > 0)
    {
        std::lock_guard<std::mutex> guard(takeLock);
        auto const it = lastTake.find(bot->GetGUID());
        if (it != lastTake.end() && it->second.first == waiting && now - it->second.second < wait)
            return false;
    }
    return true;
}

uint32 Pieces(Player* bot)
{
    uint32 pieces = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Piece(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            ++pieces;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0; bag && slot < bag->GetBagSize(); ++slot)
            if (Piece(bag->GetItemByPos(slot)))
                ++pieces;
    }
    return pieces;
}
}  // namespace BotAuction

bool NewRpgGoAuctioneerAction::Execute(Event /*event*/)
{
    auto nextRun = context->GetValue<time_t>("last said", BotAuction::NEXT_RUN);
    time_t const now = time(nullptr);

    Spawn const* nearest = nullptr;
    float nearestDist = 0.0f;
    for (Spawn const& spawn : AuctioneerSpawns())
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
        nextRun->Set(now + 30 * MINUTE);
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;
    if (Creature* auctioneer = bot->FindNearestCreature(nearest->entry, 60.0f))
    {
        // There: patch 2 lists and AhBuyAction buys on this visit, and the next run is AhBuyAction's to set
        info.ChangeToWanderNpc();
        std::get<NewRpgInfo::WanderNpc>(info.data).npcOrGo = auctioneer->GetGUID();
        nextRun->Set(now + 5 * MINUTE);
    }
    else if (nearestDist < 30.0f)
    {
        nextRun->Set(now + 30 * MINUTE);
        return false;
    }
    else
    {
        info.ChangeToGoCamp(WorldPosition(nearest->mapId, nearest->x, nearest->y, nearest->z));
        nextRun->Set(now + MINUTE);
    }

    LOG_INFO("playerbots", "[AH] {} goes to auctioneer {} ({:.0f} yd) with {} copper and {} pieces to list",
             bot->GetName(), nearest->entry, nearestDist, bot->GetMoney(), BotAuction::Pieces(bot));
    return true;
}

bool AhBuyAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* auctioneer = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_AUCTIONEER);
    AuctionHouseObject* house = auctioneer ? sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction()) : nullptr;
    if (!house)
        return false;

    // Here once: the next walk to an auctioneer is a while off, bought or not
    context->GetValue<time_t>("last said", BotAuction::NEXT_RUN)->Set(time(nullptr) + 2 * HOUR);

    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);

    // The auctions change on the world thread, between map updates, never during this one. The best piece is the one
    // that adds the most to what the bot wears, among those it can use and buy out with half its money.
    uint32 const budget = bot->GetMoney() / 2;
    AuctionEntry const* best = nullptr;
    float bestGain = 0.0f, bestNew = 0.0f, bestOld = 0.0f;
    uint32 seen = 0;
    for (auto const& [id, auction] : house->GetAuctions())
    {
        if (!auction || !auction->buyout || auction->buyout > budget || auction->owner == bot->GetGUID())
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            continue;

        ++seen;
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", proto->ItemId);
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;

        Item* item = sAuctionMgr->GetAItem(auction->item_guid);
        float const fresh = calculator.CalculateItem(proto->ItemId, item ? item->GetItemRandomPropertyId() : 0);
        uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
        Item* worn = slot != NULL_SLOT ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) : nullptr;
        float const old = worn ? calculator.CalculateItem(worn->GetEntry(), worn->GetItemRandomPropertyId()) : 0.0f;
        if (fresh <= old * BETTER || fresh - old <= bestGain)
            continue;

        best = auction;
        bestGain = fresh - old;
        bestNew = fresh;
        bestOld = old;
    }

    if (!best)
    {
        LOG_INFO("playerbots", "[AH buy] {} finds nothing better among {} pieces it could pay for at {}, with {} copper",
                 bot->GetName(), seen, auctioneer->GetName(), bot->GetMoney());
        return false;
    }

    // CMSG_AUCTION_PLACE_BID is PROCESS_THREADUNSAFE: queued for the world thread, where the handler checks the
    // auction still stands, takes the money and mails the item
    WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
    *packet << auctioneer->GetGUID() << best->Id << best->buyout;
    bot->GetSession()->QueuePacket(packet);

    LOG_INFO("playerbots", "[AH buy] {} buys out {} for {} at {}: {:.0f} over the {:.0f} it wears, {} copper before",
             bot->GetName(), best->item_template, best->buyout, auctioneer->GetName(), bestNew, bestOld, bot->GetMoney());
    return true;
}

bool NewRpgGoMailboxAction::Execute(Event /*event*/)
{
    GameObject* mailbox = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, MAILBOX_SIGHT);
    if (!mailbox)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    info.ChangeToWanderNpc();
    std::get<NewRpgInfo::WanderNpc>(info.data).npcOrGo = mailbox->GetGUID();
    LOG_INFO("playerbots", "[AH mail] {} goes to a mailbox {:.0f} yd away for its auction mail", bot->GetName(),
             bot->GetDistance(mailbox));
    return true;
}

bool TakeAuctionMailAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    GameObject* mailbox = visit ? bot->GetGameObjectIfCanInteractWith(visit->npcOrGo, GAMEOBJECT_TYPE_MAILBOX) : nullptr;
    if (!mailbox)
        return false;

    time_t const now = GameTime::GetGameTime().count();
    // agent-wow local patch (#2955): what this take asks for, kept so the same mail doesn't send the bot back at once
    if (int32 const wait = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAuctionMailWait", 0); wait > 0)
    {
        uint64 const waiting = BotAuction::Waiting(bot, now);
        std::lock_guard<std::mutex> guard(BotAuction::takeLock);
        auto& last = BotAuction::lastTake[bot->GetGUID()];
        if (last.first == waiting && waiting)
            LOG_INFO("playerbots", "[AH mail] {} finds the same auction mail its last visit couldn't take: waits {} s before the next trip",
                     bot->GetName(), wait);
        last = {waiting, now};
    }
    uint32 money = 0, items = 0;
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now)
            continue;

        if (mail->money)
        {
            WorldPacket* packet = new WorldPacket(CMSG_MAIL_TAKE_MONEY, 8 + 4);
            *packet << mailbox->GetGUID() << mail->messageID;
            bot->GetSession()->QueuePacket(packet);
            money += mail->money;
        }

        for (MailItemInfo const& item : mail->items)
        {
            WorldPacket* packet = new WorldPacket(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
            *packet << mailbox->GetGUID() << mail->messageID << item.item_guid;
            bot->GetSession()->QueuePacket(packet);
            ++items;
        }
    }

    if (!money && !items)
        return false;

    LOG_INFO("playerbots", "[AH mail] {} takes {} copper and {} items from its auction mail", bot->GetName(), money, items);
    return true;
}
