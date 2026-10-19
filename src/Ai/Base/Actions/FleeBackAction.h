/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FLEEBACKACTION_H
#define PLAYERBOTS_FLEEBACKACTION_H

#include "MovementActions.h"
#include "Trigger.h"

class Creature;
class Player;
class PlayerbotAI;

// A random bot runs back the way it came (agent-wow local patch). Off unless Playerbots.RandomBotFlee is N > 0:
// with N or more mobs on it within 30 yd, or at critical health with a mob on it, the bot runs to the oldest of its
// last few positions out of combat that no other mob guards, else to the spot FleeManager scores farthest from every
// mob. It runs below the healing potion (UsePotionsStrategy): while a potion or healthstone can be drunk, that wins.
namespace FleeBack
{
// Out of combat, at the bot's update: keep its last few positions, and log a flee that got away
void Remember(Player* bot);

// A mob that would aggro the bot at (x, y): one the bot can see, hostile, alive, not on the bot, standing within its
// attack distance + 5 yd of the spot, that is Playerbots.RandomBotPathDanger or more levels above the bot or strong
// (an elite it can't fight). nullptr when the key is 0, or for a bot the flee leaves alone (pilot's party, test bot,
// instance, careless).
Creature* Aggro(PlayerbotAI* botAI, Player* bot, float x, float y);
}  // namespace FleeBack

class FleeBackTrigger : public Trigger
{
public:
    FleeBackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "flee back") {}

    bool IsActive() override;
};

class FleeBackAction : public MovementAction
{
public:
    FleeBackAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee back") {}

    bool Execute(Event event) override;
    bool isPossible() override;
};

#endif
