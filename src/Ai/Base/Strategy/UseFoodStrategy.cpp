/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseFoodStrategy.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

void UseFoodStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    // agent-wow: a random bot under water with little breath left swims up (Playerbots.RandomBotSurfaceBreath) and
    // catches its breath, ahead of everything else. The same in UsePotionsStrategy, in combat.
    triggers.push_back(new TriggerNode("out of breath", { NextAction("surface", ACTION_EMERGENCY + 3) }));
    if (botAI->HasCheat(BotCheatMask::food))
    {
        triggers.push_back(new TriggerNode("medium health", { NextAction("food", 3.0f) }));
        triggers.push_back(new TriggerNode("high mana", { NextAction("drink", 3.0f) }));
    }
    else
    {
        triggers.push_back(new TriggerNode("low health", { NextAction("food", 3.0f) }));
        triggers.push_back(new TriggerNode("low mana", { NextAction("drink", 3.0f) }));
    }
}
