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
