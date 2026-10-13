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
