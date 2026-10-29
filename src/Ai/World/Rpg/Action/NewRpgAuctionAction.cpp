/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAuctionAction.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "BudgetValues.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameObject.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "NewRpgInfo.h"
#include "NewRpgProfessionAction.h"
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

std::vector<Spawn> GatherSpawns(uint32 npcFlag)
{
    std::vector<Spawn> result;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
        if (creature && (creature->npcflag & npcFlag))
            result.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
    }
    return result;
}

// every spawn of an auctioneer, gathered once
std::vector<Spawn> const& AuctioneerSpawns()
{
    static std::vector<Spawn> const spawns = GatherSpawns(UNIT_NPC_FLAG_AUCTIONEER);
    return spawns;
}

// agent-wow (#87 step 2): every spawn of an innkeeper, gathered once
std::vector<Spawn> const& InnkeeperSpawns()
{
    static std::vector<Spawn> const spawns = GatherSpawns(UNIT_NPC_FLAG_INNKEEPER);
    return spawns;
}

bool HostileFaction(Player* bot, uint32 creatureEntry)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(creatureEntry);
    FactionTemplateEntry const* theirs = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
    FactionTemplateEntry const* ours = bot->GetFactionTemplateEntry();
    return !theirs || !ours || ours->IsHostileTo(*theirs) || theirs->IsHostileTo(*ours);
}

// the nearest friendly spawn on the bot's map within MAX_WALK, or nullptr
Spawn const* Nearest(Player* bot, std::vector<Spawn> const& spawns, float& nearestDist)
{
    Spawn const* nearest = nullptr;
    for (Spawn const& spawn : spawns)
    {
        if (spawn.mapId != bot->GetMapId())
            continue;

        float const dist = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (dist > MAX_WALK || (nearest && dist >= nearestDist) || HostileFaction(bot, spawn.entry))
            continue;

        nearest = &spawn;
        nearestDist = dist;
    }
    return nearest;
}

// a few crafts' worth: enough to make the trip pay, little enough that one bot doesn't clear a seller's stock
constexpr uint32 CRAFTS = 5;
constexpr time_t SESSION = 15 * MINUTE;
constexpr time_t COOLDOWN = 2 * HOUR;
constexpr uint16 CRAFTING[] = {SKILL_ALCHEMY,     SKILL_BLACKSMITHING,  SKILL_COOKING,  SKILL_ENGINEERING,
                               SKILL_FIRST_AID,   SKILL_LEATHERWORKING, SKILL_TAILORING};
constexpr uint16 GATHERING[] = {SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING};

// The reagents to buy: for each crafting profession, the bot's first skill recipe it lacks reagents for, and of each
// reagent what CRAFTS crafts need beyond what it carries (item id, count, the recipe)
struct Want
{
    uint32 item;
    uint32 count;
    uint32 spell;
};

std::vector<Want> ShoppingList(Player* bot)
{
    std::vector<SpellInfo const*> recipes = EarnedProfessions::SkillRecipes(bot);
    std::sort(recipes.begin(), recipes.end(), [](SpellInfo const* a, SpellInfo const* b) { return a->Id < b->Id; });

    std::vector<Want> wants;
    for (uint16 skill : CRAFTING)
    {
        for (SpellInfo const* spellInfo : recipes)
        {
            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
            bool const ofSkill = std::any_of(bounds.first, bounds.second, [skill](auto const& row) { return row.second->SkillLine == skill; });
            if (!ofSkill || EarnedProfessions::HasReagents(bot, spellInfo))
                continue;

            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (spellInfo->Reagent[i] <= 0 || !spellInfo->ReagentCount[i])
                    continue;
                uint32 const need = spellInfo->ReagentCount[i] * CRAFTS;
                uint32 const held = bot->GetItemCount(spellInfo->Reagent[i]);
                if (held < need)
                    wants.push_back({uint32(spellInfo->Reagent[i]), need - held, spellInfo->Id});
            }
            break;  // one recipe a profession
        }
    }
    return wants;
}

// A gatherer's goods: trade goods patch 2 would list (item usage "AH"), counted by stack
uint32 Goods(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (std::none_of(std::begin(GATHERING), std::end(GATHERING), [bot](uint16 skill) { return bot->HasSkill(skill); }))
        return 0;

    uint32 goods = 0;
    auto count = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS && !item->IsSoulBound() &&
            botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get() == ITEM_USAGE_AH)
            ++goods;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        count(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                count(bag->GetItemByPos(slot));
    return goods;
}

bool HasCrafting(Player* bot)
{
    return std::any_of(std::begin(CRAFTING), std::end(CRAFTING), [bot](uint16 skill) { return bot->HasSkill(skill); });
}

void End(PlayerbotAI* botAI, char const* why)
{
    botAI->GetAiObjectContext()->GetValue<time_t>("last said", BotTrade::SESSION_END)->Set(0);
    LOG_INFO("playerbots", "[trade] {} ends its session: {}", botAI->GetBot()->GetName(), why);
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

namespace BotTrade
{
bool Enabled(PlayerbotAI* botAI)
{
    return sPlayerbotAIConfig.RandomBotTradeSession && sRandomPlayerbotMgr.IsRandomBot(botAI->GetBot()) &&
           !botAI->GetMaster();
}

bool Active(PlayerbotAI* botAI)
{
    return Enabled(botAI) && botAI->GetAiObjectContext()->GetValue<time_t>("last said", SESSION_END)->Get() != 0;
}

bool Due(PlayerbotAI* botAI)
{
    if (!Enabled(botAI) || Active(botAI) || botAI->GetBot()->GetGroup() || botAI->GetBot()->IsInCombat())
        return false;

    auto next = botAI->GetAiObjectContext()->GetValue<time_t>("last said", NEXT_SESSION);
    time_t const now = time(nullptr);
    if (!next->Get())
    {
        next->Set(now + urand(0, COOLDOWN));
        return false;
    }
    return next->Get() <= now;
}

bool Start(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    time_t const now = time(nullptr);

    bool const crafter = HasCrafting(bot);
    std::vector<Want> const wants = crafter ? ShoppingList(bot) : std::vector<Want>();
    bool const craftable = crafter && EarnedProfessions::CanCraftNow(botAI);
    uint32 const goods = Goods(botAI);

    float dist = 0.0f;
    Spawn const* town = (wants.empty() && !craftable && goods < BotAuction::MIN_PIECES)
                            ? nullptr
                            : Nearest(bot, AuctioneerSpawns(), dist);
    bool inn = false;
    if (!town && craftable)
    {
        town = Nearest(bot, InnkeeperSpawns(), dist);  // crafting only
        inn = true;
    }

    // nothing to do, or nowhere to do it: the next look is a cooldown away
    context->GetValue<time_t>("last said", NEXT_SESSION)->Set(now + COOLDOWN);
    if (!town)
        return false;

    context->GetValue<time_t>("last said", SESSION_END)->Set(now + SESSION);
    // an inn town has no auctioneer to visit: the visit counts as made
    context->GetValue<time_t>("last said", SHOPPED)->Set(inn ? now : 0);
    botAI->rpgInfo.ChangeToGoCamp(WorldPosition(town->mapId, town->x, town->y, town->z));
    LOG_INFO("playerbots",
             "[trade] {} starts a session at the {} of {} ({:.0f} yd): {} reagents to buy, {}, {} goods to list",
             bot->GetName(), inn ? "inn" : "auction house", town->entry, dist, wants.size(), craftable ? "a craft ready" : "no craft ready", goods);
    return true;
}

bool Stay(PlayerbotAI* botAI)
{
    if (!Active(botAI))
        return false;

    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    time_t const now = time(nullptr);
    if (now >= context->GetValue<time_t>("last said", SESSION_END)->Get())
    {
        End(botAI, "its 15 minutes are up");
        return false;
    }
    if (bot->GetGroup() || bot->IsInCombat())
    {
        End(botAI, bot->GetGroup() ? "it joined a group" : "it is in a fight");
        return false;
    }

    // the auctioneer first, once a session
    if (!context->GetValue<time_t>("last said", SHOPPED)->Get())
    {
        float dist = 0.0f;
        Spawn const* spawn = Nearest(bot, AuctioneerSpawns(), dist);
        Creature* auctioneer = spawn ? bot->FindNearestCreature(spawn->entry, 60.0f) : nullptr;
        if (!auctioneer)
        {
            End(botAI, "it didn't reach an auctioneer");
            return false;
        }
        botAI->rpgInfo.ChangeToWanderNpc();
        std::get<NewRpgInfo::WanderNpc>(botAI->rpgInfo.data).npcOrGo = auctioneer->GetGUID();
        return true;
    }

    if (!bot->HasPlayerFlag(PLAYER_FLAGS_RESTING))
    {
        End(botAI, "it isn't in town");
        return false;
    }

    // what keeps it in town: a craft it can make, or auction mail to take (the reagents it won)
    if (!EarnedProfessions::CanCraftNow(botAI) && !BotAuction::HasMail(bot))
    {
        End(botAI, "it has nothing left to do");
        return false;
    }

    botAI->rpgInfo.ChangeToRest();
    return true;
}
}  // namespace BotTrade

bool AhBuyReagentsAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* auctioneer = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_AUCTIONEER);
    AuctionHouseObject* house = auctioneer ? sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction()) : nullptr;
    if (!house)
        return false;

    context->GetValue<time_t>("last said", BotTrade::SHOPPED)->Set(time(nullptr));

    // its own money (upstream's "free money for" tradeskill), less each bid queued this visit: the money moves only when
    // the world thread runs the bid. No price ceiling (Logan, 10-22 12:42Z).
    uint32 budget = AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::tradeskill));
    uint32 const account = bot->GetSession()->GetAccountId();
    uint32 bids = 0, spent = 0;
    for (Want const& want : ShoppingList(bot))
    {
        // the core refuses a bid on one's own auction or one from the same account (HandleAuctionPlaceBid)
        std::vector<AuctionEntry const*> offers;
        for (auto const& [id, auction] : house->GetAuctions())
            if (auction && auction->item_template == want.item && auction->buyout && auction->itemCount &&
                auction->owner != bot->GetGUID() && sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) != account)
                offers.push_back(auction);

        std::sort(offers.begin(), offers.end(), [](AuctionEntry const* a, AuctionEntry const* b)
                  { return uint64(a->buyout) * b->itemCount < uint64(b->buyout) * a->itemCount; });

        uint32 got = 0;
        for (AuctionEntry const* auction : offers)
        {
            if (got >= want.count || auction->buyout > budget)
                break;

            WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
            *packet << auctioneer->GetGUID() << auction->Id << auction->buyout;
            bot->GetSession()->QueuePacket(packet);
            budget -= auction->buyout;
            spent += auction->buyout;
            got += auction->itemCount;
            ++bids;
            LOG_INFO("playerbots", "[trade] {} bids out {} x {} for {} copper ({} each) at {}, for spell {}",
                     bot->GetName(), auction->itemCount, want.item, auction->buyout, auction->buyout / auction->itemCount,
                     auctioneer->GetName(), want.spell);
        }
    }

    if (!bids)
        LOG_INFO("playerbots", "[trade] {} finds no reagents it can pay for at {}, with {} copper free for its trades",
                 bot->GetName(), auctioneer->GetName(), budget);
    return bids > 0;
}

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
