/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStrategy.h"

NewRpgStrategy::NewRpgStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> NewRpgStrategy::getDefaultActions()
{
    // the releavance should be greater than grind
    return {
        NextAction("new rpg status update", 11.0f)
    };
}

void NewRpgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "go grind status",
            {
                NextAction("new rpg go grind", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go camp status",
            {
                NextAction("new rpg go camp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander random status",
            {
                NextAction("new rpg wander random", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander npc status",
            {
                NextAction("new rpg wander npc", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at auctioneer",
            {
                NextAction("ah sell", 4.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "profession training due",
            {
                NextAction("new rpg go trainer", 3.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at trainer",
            {
                NextAction("rpg profession train", 4.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at vendor for trade",
            {
                NextAction("buy for trade", 4.0f)
            }
        )
    );
    // agent-wow local patch (RandomBotBuyFood): a bot low on food or drink sets out for a vendor ahead of its trainer,
    // and tops up at any vendor its wander reaches
    triggers.push_back(
        new TriggerNode(
            "food run due",
            {
                NextAction("new rpg go food vendor", 3.6f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at food vendor",
            {
                NextAction("buy own food", 4.0f)
            }
        )
    );
    // agent-wow local patch (RandomBotEarnedGear): a bot with worn gear sets out for a repairer, ahead of its food
    // run, and at any vendor or repairer its wander reaches it sells its junk, repairs and buys what it can use
    triggers.push_back(
        new TriggerNode(
            "repair run due",
            {
                NextAction("new rpg go repair", 3.7f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at errand vendor",
            {
                NextAction("vendor errands", 3.9f)
            }
        )
    );
    // agent-wow local patch (RandomBotAuctionBuy): a bot with money or goods goes to an auctioneer now and then, buys
    // the best gear it can afford there (patch 2 lists on the same visit), and takes its auction mail at a mailbox
    triggers.push_back(
        new TriggerNode(
            "auction run due",
            {
                NextAction("new rpg go auctioneer", 3.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at auctioneer to buy",
            {
                NextAction("ah buy", 3.95f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mailbox run due",
            {
                NextAction("new rpg go mailbox", 3.55f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "at mailbox",
            {
                NextAction("take auction mail", 4.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "can craft for skill",
            {
                NextAction("craft for skill", 3.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "learnable recipe",
            {
                NextAction("learn recipe", 4.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do quest status",
            {
                NextAction("new rpg do quest", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel flight status",
            {
                NextAction("new rpg travel flight", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "outdoor pvp status",
            {
                NextAction("new rpg outdoor pvp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do gather status",
            {
                NextAction("new rpg do gather", 3.0f)
            }
        )
    );
}

void NewRpgStrategy::InitMultipliers(std::vector<Multiplier*>&)
{
}
