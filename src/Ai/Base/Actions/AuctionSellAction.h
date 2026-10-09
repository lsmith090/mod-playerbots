/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONSELLACTION_H
#define PLAYERBOTS_AUCTIONSELLACTION_H

#include "InventoryAction.h"

class PlayerbotAI;

// A random bot at its roleplay auctioneer lists its unbound, AH-worthy bag items
// through its own session's CMSG_AUCTION_SELL_ITEM (agent-wow local patch).
class AuctionSellAction : public InventoryAction
{
public:
    AuctionSellAction(PlayerbotAI* botAI) : InventoryAction(botAI, "ah sell") {}

    bool Execute(Event event) override;
};

#endif
