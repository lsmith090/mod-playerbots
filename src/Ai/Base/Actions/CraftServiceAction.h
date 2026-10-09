/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTSERVICEACTION_H
#define PLAYERBOTS_CRAFTSERVICEACTION_H

#include <string>
#include <vector>

#include "Action.h"

class Player;
class PlayerbotAI;
class SpellInfo;

// Random bots offer their crafts to players (agent-wow local patch, #87). Off unless
// Playerbots.RandomBotCraftService = 1. The wording is fixed: the pilots' client parses it.
namespace CraftService
{
bool Enabled();

struct Recipe
{
    uint32 skill;
    uint32 rank;  // the skill at which it turns yellow (SkillLineAbility's MinSkillLineRank is 1 for all)
    SpellInfo const* spell;
};

// The bot's craft recipes in one crafting skill, or in all of them (skill 0), hardest first
std::vector<Recipe> Recipes(Player* bot, uint32 skill = 0);

// "LFW <Prof>: [r] [r] [r]", the recipe firstSpell first when the bot knows it; empty without recipes
std::string Advert(Player* bot, uint32 skill, uint32 firstSpell = 0);

// A player's whisper or channel line in one of the fixed forms, as the bot's queued command
// "crafter <verb> ..."; empty for anything else. Runs ahead of the bot's security levels.
std::string Command(Player* bot, uint32 type, std::string const& text, Player* from);

// The two have talked (either side answered the other's advert) within RandomBotCraftTalkMinutes
bool Talking(Player* bot, Player* player);

// A trade window event from a player the bot is talking with, under the craft service's rules
bool OnTradeStatus(PlayerbotAI* botAI, Player* trader, uint32 status);

// The trader's items changed: set a paid enchant on the item in the last slot. True when the craft service
// handled the change.
bool OnTradeItems(PlayerbotAI* botAI, Player* trader);
}  // namespace CraftService

// A random bot standing in a capital posts its LFW line in Trade - City
class SuggestCraftAction : public Action
{
public:
    SuggestCraftAction(PlayerbotAI* botAI) : Action(botAI, "suggest craft") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The bot's side of the conversation: the queued "crafter <verb> ..." commands
class CraftServiceAction : public Action
{
public:
    CraftServiceAction(PlayerbotAI* botAI) : Action(botAI, "crafter") {}

    bool Execute(Event event) override;
};

#endif
