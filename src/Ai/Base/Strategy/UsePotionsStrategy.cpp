/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UsePotionsStrategy.h"

class UsePotionsStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    UsePotionsStrategyActionNodeFactory() { creators["healthstone"] = &healthstone; }

private:
    static ActionNode* healthstone(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode("healthstone",
                              /*P*/ {},
                              /*A*/ { NextAction("healing potion") },
                              /*C*/ {});
    }
};

UsePotionsStrategy::UsePotionsStrategy(PlayerbotAI* botAI) : Strategy(botAI)
{
    actionNodeFactories.Add(new UsePotionsStrategyActionNodeFactory());
}

void UsePotionsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);

    // agent-wow: the healing potion outranks the rotation. At ACTION_MEDIUM_HEAL + 1 every class's attacks (rend on
    // attacker, battle shout, execute...) won the tick, and a bot died at 25% hp with ten potions in its bags. The mana
    // potion waits for low mana: it shares the potion cooldown, and drunk at medium mana it left no healing potion.
    triggers.push_back(new TriggerNode(
        "critical health", { NextAction("healthstone", ACTION_EMERGENCY + 2) }));
    triggers.push_back(
        new TriggerNode("low mana", { NextAction("mana potion", ACTION_EMERGENCY) }));
    // agent-wow: a random bot outnumbered, or at critical health with no potion it can drink now, runs back the way it
    // came (Playerbots.RandomBotFlee). Below the potion and the class emergencies (Lay on Hands 91, Divine Shield and
    // Ice Block 90), above every rotation action: a bot under three mobs stood and fought until dead.
    triggers.push_back(
        new TriggerNode("flee back", { NextAction("flee back", ACTION_EMERGENCY - 1) }));
    // agent-wow: a random bot under water with little breath left swims up (Playerbots.RandomBotSurfaceBreath),
    // ahead of the potion: drowning takes a fifth of its health a second. The same in UseFoodStrategy, out of combat.
    triggers.push_back(
        new TriggerNode("out of breath", { NextAction("surface", ACTION_EMERGENCY + 3) }));
}
