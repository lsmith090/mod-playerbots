/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SURFACEACTION_H
#define PLAYERBOTS_SURFACEACTION_H

#include "MovementActions.h"
#include "Trigger.h"

class PlayerbotAI;

// A random bot swims up for air (agent-wow local patch). Off unless Playerbots.RandomBotSurfaceBreath is N > 0: with
// N seconds of breath left beyond the swim up, a bot under water swims straight up to the surface, ahead of everything
// else, and out of combat it waits there until half its breath is back. A careless bot (BotTraits) leaves late, and
// some of those drown, as players do. Nothing else in the module watches the breath: a bot sent to a spawn on the sea
// floor walked there and stood until it drowned.
namespace Breath
{
// Playerbots.RandomBotSurfaceBreath: the seconds of breath left at which a random bot swims up; 0 is off
int32 Knob();
}  // namespace Breath

// The core keeps the breath timer to itself, so the trigger keeps its own count: a second used for each second under
// water, ten given back for each second above, as Player::HandleDrowning does.
class OutOfBreathTrigger : public Trigger
{
public:
    OutOfBreathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "out of breath") {}

    bool IsActive() override;
};

class SurfaceAction : public MovementAction
{
public:
    SurfaceAction(PlayerbotAI* botAI) : MovementAction(botAI, "surface") {}

    bool Execute(Event event) override;
    bool isPossible() override;
};

#endif
