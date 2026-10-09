/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGPROFESSIONACTION_H
#define PLAYERBOTS_NEWRPGPROFESSIONACTION_H

#include "Action.h"
#include "UseItemAction.h"

class Item;
class Player;
class PlayerbotAI;

// agent-wow (#87): a random bot earns its professions. Off unless Playerbots.RandomBotEarnedProfessions = 1.
namespace EarnedProfessions
{
// the key is on, and the bot is a random bot without a master
bool Enabled(PlayerbotAI* botAI);
// a trade skill of the bot sits at its max
bool AtCap(Player* bot);
// "last said" holds the next time the bot looks for a trainer (no new header for a timestamp)
inline constexpr char const* NEXT_TRAINER_SEARCH = "earned next trainer search";

// a recipe item in the bags that teaches a spell the bot lacks and its skill allows now, or nullptr
Item* LearnableRecipe(PlayerbotAI* botAI);

// Playerbots.RandomBotCraftForSkill is on, and the bot is a random bot without a master
bool CraftEnabled(PlayerbotAI* botAI);
// "last said" holds the next time the bot looks for something to craft
inline constexpr char const* NEXT_CRAFT_SEARCH = "earned next craft search";
// "last said" holds until when a customer's mats sit in the bot's bags (#87 piece 8 sets it): no crafting till then
inline constexpr char const* CRAFT_ORDER_HOLD = "craft order hold";
}  // namespace EarnedProfessions

// A random bot whose trade skill sits at its cap walks to the nearest friendly trainer on its map that teaches the
// next rank it can afford: GO_CAMP to the spawn, then WANDER_NPC with the trainer as its visit.
class NewRpgGoTrainerAction : public Action
{
public:
    NewRpgGoTrainerAction(PlayerbotAI* botAI) : Action(botAI, "new rpg go trainer") {}

    bool Execute(Event event) override;
};

// A random bot at the profession trainer its WANDER_NPC visit chose buys what it can learn there (TrainerAction).
class RpgProfessionTrainAction : public Action
{
public:
    RpgProfessionTrainAction(PlayerbotAI* botAI) : Action(botAI, "rpg profession train") {}

    bool Execute(Event event) override;
};

// A random bot standing in a city or an inn crafts a known recipe that still raises its skill, with reagents and tools
// it carries and the focus (anvil, forge, fire) beside it.
class CraftForSkillAction : public Action
{
public:
    CraftForSkillAction(PlayerbotAI* botAI) : Action(botAI, "craft for skill") {}

    bool Execute(Event event) override;
};

// A random bot at the vendor its WANDER_NPC visit chose buys, from its "free money for" tradeskill, what item usage
// calls "skill": the reagents its skill-up recipes need (thread, vials, flux; RandomBotCraftForSkill), up to a stack
// each, and recipes it can use and lacks (RandomBotEarnedProfessions).
class BuyForTradeAction : public Action
{
public:
    BuyForTradeAction(PlayerbotAI* botAI) : Action(botAI, "buy for trade") {}

    bool Execute(Event event) override;
};

// A random bot learns from a recipe item in its bags once its skill allows it (#87).
class LearnRecipeAction : public UseItemAction
{
public:
    LearnRecipeAction(PlayerbotAI* botAI) : UseItemAction(botAI, "learn recipe", true) {}

    bool Execute(Event event) override;
};

#endif
