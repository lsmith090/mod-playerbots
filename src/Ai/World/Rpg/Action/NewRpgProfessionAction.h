/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGPROFESSIONACTION_H
#define PLAYERBOTS_NEWRPGPROFESSIONACTION_H

#include "Action.h"

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

#endif
