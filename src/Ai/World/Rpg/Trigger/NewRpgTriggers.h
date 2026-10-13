/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGTRIGGERS_H
#define PLAYERBOTS_NEWRPGTRIGGERS_H

#include "NewRpgStrategy.h"
#include "Trigger.h"

class NewRpgStatusTrigger : public Trigger
{
public:
    NewRpgStatusTrigger(PlayerbotAI* botAI, NewRpgStatus status = RPG_IDLE)
        : Trigger(botAI, "new rpg status"), status(status)
    {
    }
    bool IsActive() override;

protected:
    NewRpgStatus status;
};

// A random bot stands at the auctioneer its WANDER_NPC visit chose: active once per visit
class AtAuctioneerTrigger : public Trigger
{
public:
    AtAuctioneerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at auctioneer") {}
    bool IsActive() override;

private:
    ObjectGuid lastAuctioneer;
    uint32 lastVisitStart{0};
};

// A random bot with a trade skill at its cap, between quests, is due to look for its trainer (#87)
class ProfessionTrainingDueTrigger : public Trigger
{
public:
    ProfessionTrainingDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "profession training due", 10) {}
    bool IsActive() override;
};

// A random bot standing still in a city or an inn, out of combat, may craft for skill (#87)
class CraftForSkillTrigger : public Trigger
{
public:
    CraftForSkillTrigger(PlayerbotAI* botAI) : Trigger(botAI, "can craft for skill", 5) {}
    bool IsActive() override;
};

// A random bot stands at the vendor its WANDER_NPC visit chose: active once per visit (#87)
class AtVendorTrigger : public Trigger
{
public:
    AtVendorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at vendor for trade") {}
    bool IsActive() override;

private:
    ObjectGuid lastVendor;
    uint32 lastVisitStart{0};
};

// A random bot low on food or drink, between quests, is due to look for a vendor (agent-wow: RandomBotBuyFood)
// agent-wow local patch (RandomBotAuctionBuy): the bot has money or goods for an auctioneer and may set out for one
class AuctionRunDueTrigger : public Trigger
{
public:
    AuctionRunDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auction run due", 30) {}
    bool IsActive() override;
};

// agent-wow local patch (RandomBotAuctionBuy): the bot stands at the auctioneer its wander chose, once a visit
class AtAuctioneerToBuyTrigger : public Trigger
{
public:
    AtAuctioneerToBuyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at auctioneer to buy") {}
    bool IsActive() override;

private:
    ObjectGuid lastAuctioneer;
    uint32 lastVisitStart{0};
};

// agent-wow local patch (RandomBotAuctionBuy): the bot has auction mail and a mailbox in sight
class MailboxRunDueTrigger : public Trigger
{
public:
    MailboxRunDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mailbox run due", 30) {}
    bool IsActive() override;
};

// agent-wow local patch (RandomBotAuctionBuy): the bot stands at the mailbox its visit chose, once a visit
class AtMailboxTrigger : public Trigger
{
public:
    AtMailboxTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at mailbox") {}
    bool IsActive() override;

private:
    ObjectGuid lastMailbox;
    uint32 lastVisitStart{0};
};

// agent-wow local patch (RandomBotEarnedGear): the bot's gear is worn and it may look for a repairer
class RepairRunDueTrigger : public Trigger
{
public:
    RepairRunDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "repair run due", 10) {}
    bool IsActive() override;
};

// agent-wow local patch (RandomBotEarnedGear): the bot stands at a vendor or repairer its wander chose, once a visit
class AtErrandVendorTrigger : public Trigger
{
public:
    AtErrandVendorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at errand vendor") {}
    bool IsActive() override;

private:
    ObjectGuid lastVendor;
    uint32 lastVisitStart{0};
};

class FoodRunDueTrigger : public Trigger
{
public:
    FoodRunDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "food run due", 10) {}
    bool IsActive() override;
};

// A random bot stands at a vendor its WANDER_NPC visit chose: active once per visit (agent-wow: RandomBotBuyFood)
class AtFoodVendorTrigger : public Trigger
{
public:
    AtFoodVendorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at food vendor") {}
    bool IsActive() override;

private:
    ObjectGuid lastVendor;
    uint32 lastVisitStart{0};
};

// A random bot out of combat carries a recipe item it can learn now (#87)
class LearnableRecipeTrigger : public Trigger
{
public:
    LearnableRecipeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "learnable recipe", 30) {}
    bool IsActive() override;
};

// A random bot stands at the profession trainer its WANDER_NPC visit chose: active once per visit (#87)
class AtTrainerTrigger : public Trigger
{
public:
    AtTrainerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "at trainer") {}
    bool IsActive() override;

private:
    ObjectGuid lastTrainer;
    uint32 lastVisitStart{0};
};

#endif
