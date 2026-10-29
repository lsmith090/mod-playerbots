/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGAUCTIONACTION_H
#define PLAYERBOTS_NEWRPGAUCTIONACTION_H

#include "Action.h"

class Player;
class PlayerbotAI;

// agent-wow local patch: a random bot takes part in the auction house. Off unless Playerbots.RandomBotAuctionBuy = 1.
// Listing is patch 2's (RandomBotAuctionSell), and only ever happened when a bot's wander chanced on an auctioneer:
// 4 of 1,504 auctions were a bot's on 2026-10-13, and 32 auction mails sat in bots' mailboxes with nobody to take
// them. With this a bot goes to an auctioneer now and then, buys the best piece of gear it can afford there, and
// takes its auction mail at a mailbox: what its sales fetched, what it won and what came back unsold.
namespace BotAuction
{
// "last said" holds the next time the bot may set out for an auctioneer (no new header for a timestamp)
inline constexpr char const* NEXT_RUN = "auction next run";
// a bot under this level has nothing to sell and nothing to spend
inline constexpr uint32 MIN_LEVEL = 6;
// and it sets out with at least this much copper, or this many pieces worth listing
inline constexpr uint32 MIN_COPPER = 1000;
inline constexpr uint32 MIN_PIECES = 3;

// Playerbots.RandomBotAuctionBuy
bool KeyOn();
// the key is on and the bot is a random bot without a master
bool Enabled(PlayerbotAI* botAI);
// auction mail that has arrived, with money or items in it
bool HasMail(Player* bot);
// unbound pieces of uncommon quality or better in its bags: what an auctioneer is worth a walk for
uint32 Pieces(Player* bot);
}  // namespace BotAuction

// agent-wow local patch (#87 step 2, Logan's rulings 2026-10-22 12:14-13:02Z): the artisans' trade session. A random bot
// with a crafting profession, or a gatherer carrying goods, with something to do (a skill recipe to buy reagents for or
// to craft, goods to list) goes to the nearest town with an auction house on its map (patch 26's search, 3,000 yd),
// else the nearest inn town for crafting only, and stays there up to 15 minutes: patch 2 lists, it bids out reagents,
// the earned trainer trains, it crafts, patch 26 takes its auction mail. Drawn at weight 20 (WanderNpc's) among the
// statuses, at most every 2 hours. Off unless Playerbots.RandomBotTradeSession (read once, PlayerbotAIConfig).
namespace BotTrade
{
// "last said" values (no new header for a timestamp): when the next session may start, when this one ends (0 when none
// runs), and when this session's auction house visit was made
inline constexpr char const* NEXT_SESSION = "trade next session";
inline constexpr char const* SESSION_END = "trade session end";
inline constexpr char const* SHOPPED = "trade session shopped";
// Playerbots.RpgStatusProbWeight.WanderNpc's default
inline constexpr uint32 WEIGHT = 20;

// the key is on and the bot is a random bot without a master
bool Enabled(PlayerbotAI* botAI);
// a session is under way
bool Active(PlayerbotAI* botAI);
// the cooldown has passed (a bot's first one is drawn between 0 and 2 h, so the key turned on sends no wave to town)
bool Due(PlayerbotAI* botAI);
// pick the town and start walking there; false when there is nothing to do or no town in reach
bool Start(PlayerbotAI* botAI);
// at each idle in a session: the auctioneer's visit, then rest in town while there is work; false once it ends
bool Stay(PlayerbotAI* botAI);
}  // namespace BotTrade

// A random bot with money or goods walks to the nearest friendly auctioneer on its map: GO_CAMP to the spawn, then
// WANDER_NPC with the auctioneer as its visit, where patch 2 lists and AhBuyAction buys.
class NewRpgGoAuctioneerAction : public Action
{
public:
    NewRpgGoAuctioneerAction(PlayerbotAI* botAI) : Action(botAI, "new rpg go auctioneer") {}

    bool Execute(Event event) override;
};

// At the auctioneer its visit chose, the bot buys out the one auction that improves what it wears the most, for no
// more than half its money. The bid is its own CMSG_AUCTION_PLACE_BID, queued for the world thread.
class AhBuyAction : public Action
{
public:
    AhBuyAction(PlayerbotAI* botAI) : Action(botAI, "ah buy") {}

    bool Execute(Event event) override;
};

// At the auctioneer in a trade session, the bot bids out the reagents of its next skill recipe of each profession, a few
// crafts' worth, cheapest a unit first, from any seller but itself and its own account, with its own money.
class AhBuyReagentsAction : public Action
{
public:
    AhBuyReagentsAction(PlayerbotAI* botAI) : Action(botAI, "ah buy reagents") {}

    bool Execute(Event event) override;
};

// A random bot with auction mail and a mailbox in sight makes the mailbox its visit.
class NewRpgGoMailboxAction : public Action
{
public:
    NewRpgGoMailboxAction(PlayerbotAI* botAI) : Action(botAI, "new rpg go mailbox") {}

    bool Execute(Event event) override;
};

// At the mailbox the bot takes the money and the items of its auction mail: CMSG_MAIL_TAKE_MONEY and
// CMSG_MAIL_TAKE_ITEM, queued for the world thread.
class TakeAuctionMailAction : public Action
{
public:
    TakeAuctionMailAction(PlayerbotAI* botAI) : Action(botAI, "take auction mail") {}

    bool Execute(Event event) override;
};

#endif
