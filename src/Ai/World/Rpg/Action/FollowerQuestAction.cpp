/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowerQuestAction.h"

#include "Event.h"
#include "GroupChat.h"
#include "Playerbots.h"

bool FollowerQuestGiverTrigger::IsActive()
{
    return bot->IsAlive() && !bot->IsInCombat() && !bot->IsNonMeleeSpellCast(false) && GroupChat::Follows(bot);
}

bool FollowerQuestGiverAction::Execute(Event /*event*/) { return SearchQuestGiverAndAcceptOrReward(); }
