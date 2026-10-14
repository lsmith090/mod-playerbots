/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOLLOWERQUESTACTION_H
#define PLAYERBOTS_FOLLOWERQUESTACTION_H

#include "NewRpgBaseAction.h"
#include "Trigger.h"

class PlayerbotAI;

// agent-wow local patch: a random bot that follows another random bot talks to quest givers itself. A follower has
// no new rpg; its `quest` strategy answers only the packets of a master's game client (gossip hello, complete quest),
// and a bot leader has no client: it talks to a quest giver by direct calls. So a follower never accepted a quest and
// never turned one in, whatever it killed beside its leader. Under Playerbots.RandomBotGroupNear.
class FollowerQuestGiverTrigger : public Trigger
{
public:
    FollowerQuestGiverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "follower quest giver", 5) {}

    bool IsActive() override;
};

// The follower uses new rpg's own search: the nearest quest giver within 80 yd with a quest to give it or to take
// from it. In reach, it accepts and turns in; else it walks over, and follows its leader again after.
class FollowerQuestGiverAction : public NewRpgBaseAction
{
public:
    FollowerQuestGiverAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "follower quest giver") {}

    bool Execute(Event event) override;
};

#endif
