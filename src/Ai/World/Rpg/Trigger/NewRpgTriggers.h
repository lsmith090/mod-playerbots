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

#endif
