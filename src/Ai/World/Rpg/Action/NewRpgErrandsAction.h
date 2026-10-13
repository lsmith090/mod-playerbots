/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGERRANDSACTION_H
#define PLAYERBOTS_NEWRPGERRANDSACTION_H

#include "Action.h"

class Player;
class PlayerbotAI;

// agent-wow local patch: a random bot earns its gold and its gear. Off unless Playerbots.RandomBotEarnedGear = 1.
// With it the factory hands a random bot no money and repairs nothing for free; the bot sells its junk, pays for its
// repairs and buys what it can use at the vendors it reaches, and walks to a repairer when its gear is worn. What it
// wears is then its starting outfit, its quest rewards, its loot and what it bought (with Playerbots.AutoUpgradeEquip
// = 0, which stops the outfit the factory hands out at every level).
namespace BotErrands
{
// the worst worn item under this share of its durability sends the bot to a repairer
inline constexpr float WORN = 0.3f;
// "last said" holds the next time the bot looks for a repairer (no new header for a timestamp)
inline constexpr char const* NEXT_REPAIR_SEARCH = "errands next repair search";

// Playerbots.RandomBotEarnedGear
bool KeyOn();
// the key is on and the bot is a random bot
bool Earns(Player* bot);
// and it has no master
bool Enabled(PlayerbotAI* botAI);
// the lowest share of durability left among what the bot wears; 1 when nothing it wears can break
float Worst(Player* bot);
}  // namespace BotErrands

// A random bot whose gear is worn walks to the nearest friendly repairer on its map: GO_CAMP to the spawn, then
// WANDER_NPC with the repairer as its visit.
class NewRpgGoRepairAction : public Action
{
public:
    NewRpgGoRepairAction(PlayerbotAI* botAI) : Action(botAI, "new rpg go repair") {}

    bool Execute(Event event) override;
};

// A random bot at the vendor or repairer its WANDER_NPC visit chose sells its junk, has its gear repaired and buys
// what it can use, all from its own money (upstream's sell, repair and buy actions, which new rpg never called).
class VendorErrandsAction : public Action
{
public:
    VendorErrandsAction(PlayerbotAI* botAI) : Action(botAI, "vendor errands") {}

    bool Execute(Event event) override;
};

#endif
