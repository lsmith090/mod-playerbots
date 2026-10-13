/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGFOODACTION_H
#define PLAYERBOTS_NEWRPGFOODACTION_H

#include <vector>

#include "Action.h"

class Player;
class PlayerbotAI;

// agent-wow local patch: a random bot buys its own food and drink. Off unless Playerbots.RandomBotBuyFood = 1; it
// only matters once "food" is out of Playerbots.BotCheats (with the cheat a bot eats nothing it carries).
namespace BotFood
{
// the spell categories of an item that is eaten (11) or drunk (59)
inline constexpr uint32 FOOD = 11;
inline constexpr uint32 DRINK = 59;
// under LOW of either a bot sets out for a vendor; at any vendor it fills up to FULL (two stacks)
inline constexpr uint32 LOW = 5;
inline constexpr uint32 FULL = 40;
// "last said" holds the next time the bot looks for a food vendor (no new header for a timestamp)
inline constexpr char const* NEXT_VENDOR_SEARCH = "food next vendor search";

// Playerbots.RandomBotBuyFood
bool KeyOn();
// the key is on, the food cheat is off, and the bot is a random bot without a master
bool Enabled(PlayerbotAI* botAI);
// the categories this bot stocks: food, and drink if it has mana
std::vector<uint32> Categories(Player* bot);
// how many it carries of a category
uint32 Count(Player* bot, uint32 category);
// under LOW of a category it stocks
bool Low(Player* bot);
}  // namespace BotFood

// A random bot low on food or drink walks to the nearest friendly vendor on its map, within MAX_WALK, that sells some
// it can use and afford: GO_CAMP to the spawn, then WANDER_NPC with the vendor as its visit.
class NewRpgGoFoodVendorAction : public Action
{
public:
    NewRpgGoFoodVendorAction(PlayerbotAI* botAI) : Action(botAI, "new rpg go food vendor") {}

    bool Execute(Event event) override;
};

// A random bot at the vendor its WANDER_NPC visit chose buys, from its own money, the best food and drink it can use
// there, up to FULL of each.
class BuyOwnFoodAction : public Action
{
public:
    BuyOwnFoodAction(PlayerbotAI* botAI) : Action(botAI, "buy own food") {}

    bool Execute(Event event) override;
};

#endif
