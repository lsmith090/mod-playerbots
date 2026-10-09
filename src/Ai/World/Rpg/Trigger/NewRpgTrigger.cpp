/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "Config.h"
#include "NewRpgProfessionAction.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Trainer.h"

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

bool ProfessionTrainingDueTrigger::IsActive()
{
    // not while questing, flying or already walking somewhere chosen
    switch (botAI->rpgInfo.GetStatus())
    {
        case RPG_IDLE:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_GO_GRIND:
        case RPG_REST:
            break;
        default:
            return false;
    }

    return !bot->IsInCombat() && EarnedProfessions::Enabled(botAI) && EarnedProfessions::AtCap(bot) &&
           AI_VALUE2(time_t, "last said", EarnedProfessions::NEXT_TRAINER_SEARCH) <= time(nullptr);
}

bool AtTrainerTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, trained or not
    if (visit->npcOrGo == lastTrainer && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!EarnedProfessions::Enabled(botAI))
        return false;

    Creature* creature = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_TRAINER);
    Trainer::Trainer const* trainer = creature ? sObjectMgr->GetTrainer(creature->GetEntry()) : nullptr;
    if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
        return false;

    lastTrainer = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool CraftForSkillTrigger::IsActive()
{
    switch (botAI->rpgInfo.GetStatus())
    {
        case RPG_IDLE:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_REST:
            break;
        default:
            return false;
    }

    time_t const now = time(nullptr);
    return bot->HasPlayerFlag(PLAYER_FLAGS_RESTING) && !bot->IsInCombat() && !bot->isMoving() &&
           !bot->IsNonMeleeSpellCast(false) && EarnedProfessions::CraftEnabled(botAI) &&
           AI_VALUE2(time_t, "last said", EarnedProfessions::NEXT_CRAFT_SEARCH) <= now &&
           AI_VALUE2(time_t, "last said", EarnedProfessions::CRAFT_ORDER_HOLD) <= now;
}

bool AtVendorTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, bought or not
    if (visit->npcOrGo == lastVendor && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    // reagents with RandomBotCraftForSkill, recipes with RandomBotEarnedProfessions
    if (!(EarnedProfessions::CraftEnabled(botAI) || EarnedProfessions::Enabled(botAI)) ||
        !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR))
        return false;

    lastVendor = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool LearnableRecipeTrigger::IsActive()
{
    return !bot->IsInCombat() && !bot->IsNonMeleeSpellCast(false) && EarnedProfessions::Enabled(botAI) &&
           EarnedProfessions::LearnableRecipe(botAI);
}
