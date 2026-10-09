/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "Config.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

bool AtAuctioneerTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, listed or not
    if (visit->npcOrGo == lastAuctioneer && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    if (!sConfigMgr->GetOption<bool>("Playerbots.RandomBotAuctionSell", false) ||
        !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_AUCTIONEER))
        return false;

    lastAuctioneer = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}
