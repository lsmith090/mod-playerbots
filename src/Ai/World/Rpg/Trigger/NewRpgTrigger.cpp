/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "Config.h"
#include "GameObject.h"
#include "NewRpgAuctionAction.h"
#include "NewRpgErrandsAction.h"
#include "NewRpgFoodAction.h"
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

    // agent-wow (#87 step 2): with the trade session's key on, a bot crafts at any rest, wherever it is (a recipe that
    // needs a focus still needs it: CraftForSkillAction's CanCastSpell)
    bool const resting = bot->HasPlayerFlag(PLAYER_FLAGS_RESTING) ||
                         (sPlayerbotAIConfig.RandomBotTradeSession && botAI->rpgInfo.GetStatus() == RPG_REST);
    time_t const now = time(nullptr);
    return resting && !bot->IsInCombat() && !bot->isMoving() &&
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

namespace
{
// not while questing, flying or already walking somewhere chosen
bool Idle(PlayerbotAI* botAI)
{
    switch (botAI->rpgInfo.GetStatus())
    {
        case RPG_IDLE:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_GO_GRIND:
        case RPG_REST:
            return true;
        default:
            return false;
    }
}
}  // namespace

bool AuctionRunDueTrigger::IsActive()
{
    return Idle(botAI) && !bot->IsInCombat() && BotAuction::Enabled(botAI) && bot->GetLevel() >= BotAuction::MIN_LEVEL &&
           AI_VALUE2(time_t, "last said", BotAuction::NEXT_RUN) <= time(nullptr) &&
           (bot->GetMoney() >= BotAuction::MIN_COPPER || BotAuction::Pieces(bot) >= BotAuction::MIN_PIECES);
}

bool AtAuctioneerToBuyTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, bought or not
    if (visit->npcOrGo == lastAuctioneer && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!BotAuction::Enabled(botAI) || !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_AUCTIONEER))
        return false;

    lastAuctioneer = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool AtAuctioneerForReagentsTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, bought or not
    if (visit->npcOrGo == lastAuctioneer && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!BotTrade::Active(botAI) || AI_VALUE2(time_t, "last said", BotTrade::SHOPPED) ||
        !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_AUCTIONEER))
        return false;

    lastAuctioneer = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool MailboxRunDueTrigger::IsActive()
{
    if (!Idle(botAI) || bot->IsInCombat() || !BotAuction::Enabled(botAI) || !BotAuction::HasMail(bot))
        return false;

    // already on its way to one
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (visit && visit->npcOrGo.IsGameObject())
        return false;

    return bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, 100.0f) != nullptr;
}

bool AtMailboxTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo.IsGameObject())
        return false;

    if (visit->npcOrGo == lastMailbox && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!BotAuction::Enabled(botAI) || !bot->GetGameObjectIfCanInteractWith(visit->npcOrGo, GAMEOBJECT_TYPE_MAILBOX))
        return false;

    lastMailbox = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool RepairRunDueTrigger::IsActive()
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

    // a repair costs money: with none there is nothing to walk for
    return !bot->IsInCombat() && BotErrands::Enabled(botAI) && bot->GetMoney() > 0 &&
           BotErrands::Worst(bot) < BotErrands::WORN &&
           AI_VALUE2(time_t, "last said", BotErrands::NEXT_REPAIR_SEARCH) <= time(nullptr);
}

bool AtErrandVendorTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, whatever came of it
    if (visit->npcOrGo == lastVendor && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!BotErrands::Enabled(botAI) ||
        !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR))
        return false;

    lastVendor = visit->npcOrGo;
    lastVisitStart = botAI->rpgInfo.startT;
    return true;
}

bool FoodRunDueTrigger::IsActive()
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

    return !bot->IsInCombat() && BotFood::Enabled(botAI) && BotFood::Low(bot) &&
           AI_VALUE2(time_t, "last said", BotFood::NEXT_VENDOR_SEARCH) <= time(nullptr);
}

bool AtFoodVendorTrigger::IsActive()
{
    auto* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit || !visit->npcOrGo)
        return false;

    // one try per visit, bought or not
    if (visit->npcOrGo == lastVendor && botAI->rpgInfo.startT == lastVisitStart)
        return false;

    if (!BotFood::Enabled(botAI) || !bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR))
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
