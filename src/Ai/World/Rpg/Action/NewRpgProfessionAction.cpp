/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgProfessionAction.h"

#include <algorithm>

#include "BudgetValues.h"
#include "Config.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellMgr.h"
#include "Trainer.h"

namespace
{
constexpr uint16 TRADE_SKILLS[] = {SKILL_ALCHEMY,     SKILL_BLACKSMITHING, SKILL_COOKING,       SKILL_ENCHANTING,
                                   SKILL_ENGINEERING, SKILL_FIRST_AID,     SKILL_FISHING,       SKILL_HERBALISM,
                                   SKILL_LEATHERWORKING, SKILL_MINING,     SKILL_SKINNING,      SKILL_TAILORING};

struct TrainerSpawn
{
    uint32 entry;
    uint16 mapId;
    float x, y, z;
};

// every spawn of a tradeskill trainer, gathered once
std::vector<TrainerSpawn> const& TradeskillTrainerSpawns()
{
    static std::vector<TrainerSpawn> const spawns = []
    {
        std::vector<TrainerSpawn> result;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(data.id);
            if (trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill)
                result.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
        }
        return result;
    }();
    return spawns;
}

// the trainer's spell that lifts the max of a trade skill the bot has at its cap (Journeyman, Expert, ...), if the
// core lets the trainer teach it now (level, skill rank, the two primaries) and it costs no more than budget
Trainer::Spell const* NextRank(Player* bot, Trainer::Trainer const* trainer, uint32 budget)
{
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (spell.MoneyCost > budget || !PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, &spell) ||
            !trainer->CanTeachSpell(bot, &spell))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spell.SpellId);
        if (!spellInfo)
            continue;

        std::vector<uint32> taught = {spell.SpellId};
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                taught.push_back(effect.TriggerSpell);

        for (uint32 spellId : taught)
        {
            SpellLearnSkillNode const* learnSkill = sSpellMgr->GetSpellLearnSkill(spellId);
            if (!learnSkill || !bot->HasSkill(learnSkill->skill))
                continue;

            uint16 const max = bot->GetPureMaxSkillValue(learnSkill->skill);
            if (learnSkill->maxvalue > max && bot->GetPureSkillValue(learnSkill->skill) >= max)
                return &spell;
        }
    }
    return nullptr;
}

bool HostileFaction(Player* bot, uint32 creatureEntry)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(creatureEntry);
    FactionTemplateEntry const* theirs = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
    FactionTemplateEntry const* ours = bot->GetFactionTemplateEntry();
    return !theirs || !ours || ours->IsHostileTo(*theirs) || theirs->IsHostileTo(*ours);
}
}  // namespace

namespace EarnedProfessions
{
bool Enabled(PlayerbotAI* botAI)
{
    // read here, not in PlayerbotAIConfig.h, so the patch leaves the module's hot headers alone
    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotEarnedProfessions", false) &&
           sRandomPlayerbotMgr.IsRandomBot(botAI->GetBot()) && !botAI->GetMaster();
}

Item* LearnableRecipe(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    for (Item* item : botAI->GetAiObjectContext()->GetValue<std::vector<Item*>>("inventory items", "recipe")->Get())
    {
        // Spells[0] is "Learning" (483) and Spells[1] the recipe; CanUseItem checks the skill rank
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Spells[0].SpellId == 483 && proto->Spells[1].SpellId && !bot->HasSpell(proto->Spells[1].SpellId) &&
            bot->CanUseItem(item) == EQUIP_ERR_OK)
            return item;
    }
    return nullptr;
}

bool CraftEnabled(PlayerbotAI* botAI)
{
    return sConfigMgr->GetOption<bool>("Playerbots.RandomBotCraftForSkill", false) &&
           sRandomPlayerbotMgr.IsRandomBot(botAI->GetBot()) && !botAI->GetMaster();
}

std::vector<SpellInfo const*> SkillRecipes(Player* bot)
{
    std::vector<SpellInfo const*> recipes;
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !spellInfo->HasEffect(SPELL_EFFECT_CREATE_ITEM))
            continue;

        // a recipe of a trade skill below its max that still gives a point (not grey)
        bool raisesSkill = false;
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second && !raisesSkill; ++itr)
        {
            uint16 const skillId = itr->second->SkillLine;
            bool const tradeSkill =
                std::find(std::begin(TRADE_SKILLS), std::end(TRADE_SKILLS), skillId) != std::end(TRADE_SKILLS);
            raisesSkill = tradeSkill && bot->GetPureSkillValue(skillId) < bot->GetPureMaxSkillValue(skillId);
        }
        if (raisesSkill && ItemUsageValue::SpellGivesSkillUp(spellId, bot))
            recipes.push_back(spellInfo);
    }
    return recipes;
}

bool HasReagents(Player* bot, SpellInfo const* spellInfo)
{
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        if (spellInfo->Reagent[i] > 0 && spellInfo->ReagentCount[i] &&
            !bot->HasItemCount(spellInfo->Reagent[i], spellInfo->ReagentCount[i]))
            return false;
    return true;
}

bool CanCraftNow(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    for (SpellInfo const* spellInfo : SkillRecipes(bot))
        if (HasReagents(bot, spellInfo) && botAI->CanCastSpell(spellInfo->Id, bot, true))
            return true;
    return false;
}

bool AtCap(Player* bot)
{
    for (uint16 skillId : TRADE_SKILLS)
    {
        uint16 const max = bot->GetPureMaxSkillValue(skillId);
        if (max && bot->GetPureSkillValue(skillId) >= max)
            return true;
    }
    return false;
}
}  // namespace EarnedProfessions

bool NewRpgGoTrainerAction::Execute(Event /*event*/)
{
    auto nextSearch = context->GetValue<time_t>("last said", EarnedProfessions::NEXT_TRAINER_SEARCH);
    time_t const now = time(nullptr);
    uint32 const budget = AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::spells));

    TrainerSpawn const* nearest = nullptr;
    Trainer::Spell const* rank = nullptr;
    float nearestDist = 0.0f;
    for (TrainerSpawn const& spawn : TradeskillTrainerSpawns())
    {
        if (spawn.mapId != bot->GetMapId())
            continue;

        float const dist = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (nearest && dist >= nearestDist)
            continue;

        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(spawn.entry);
        Trainer::Spell const* spell = trainer ? NextRank(bot, trainer, budget) : nullptr;
        if (!spell || HostileFaction(bot, spawn.entry))
            continue;

        nearest = &spawn;
        rank = spell;
        nearestDist = dist;
    }

    if (!nearest)
    {
        // nothing to train here yet: too low a level, too little gold, or no trainer on this map
        nextSearch->Set(now + 30 * MINUTE);
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;
    if (Creature* trainer = bot->FindNearestCreature(nearest->entry, 60.0f))
    {
        info.ChangeToWanderNpc();
        std::get<NewRpgInfo::WanderNpc>(info.data).npcOrGo = trainer->GetGUID();
        nextSearch->Set(now + 2 * MINUTE);
    }
    else
    {
        info.ChangeToGoCamp(WorldPosition(nearest->mapId, nearest->x, nearest->y, nearest->z));
        nextSearch->Set(now + MINUTE);
    }

    LOG_INFO("playerbots", "[earned] {} goes to trainer {} for spell {} ({} copper, {:.0f} yd)", bot->GetName(),
             nearest->entry, rank->SpellId, rank->MoneyCost, nearestDist);
    return true;
}

bool RpgProfessionTrainAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* trainer = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_TRAINER);
    if (!trainer)
        return false;

    // TrainerAction teaches what the bot's selection offers and charges for it (random bots always learn)
    bot->SetSelection(trainer->GetGUID());
    bool const trained = botAI->DoSpecificAction("trainer", Event(), true);

    std::ostringstream skills;
    for (uint16 skillId : TRADE_SKILLS)
        if (uint16 const max = bot->GetPureMaxSkillValue(skillId))
            skills << " " << skillId << "=" << bot->GetPureSkillValue(skillId) << "/" << max;
    LOG_INFO("playerbots", "[earned] {} trains at {} ({}): {}; skills{}", bot->GetName(), trainer->GetName(),
             trainer->GetEntry(), trained ? "bought what it could afford" : "nothing to learn", skills.str());
    return trained;
}

bool CraftForSkillAction::Execute(Event /*event*/)
{
    std::vector<uint32> crafts;
    for (SpellInfo const* spellInfo : EarnedProfessions::SkillRecipes(bot))
    {
        // CanCastSpell ignores reagents (TRIGGERED_IGNORE_POWER_AND_REAGENT_COST), so they're counted first; then
        // tools, the spell focus, the cooldown
        if (EarnedProfessions::HasReagents(bot, spellInfo) && botAI->CanCastSpell(spellInfo->Id, bot, true))
            crafts.push_back(spellInfo->Id);
    }

    auto nextSearch = context->GetValue<time_t>("last said", EarnedProfessions::NEXT_CRAFT_SEARCH);
    if (crafts.empty())
    {
        nextSearch->Set(time(nullptr) + 5 * MINUTE);
        return false;
    }

    uint32 const spellId = crafts[urand(0, crafts.size() - 1)];
    // agent-wow (#87 step 2): every craft is logged at info while the trade session's key is on, for the day-after read
    if (sPlayerbotAIConfig.RandomBotTradeSession)
        LOG_INFO("playerbots", "[trade] {} crafts {} for skill", bot->GetName(), spellId);
    else
        LOG_DEBUG("playerbots", "[earned] {} crafts {} for skill", bot->GetName(), spellId);
    if (botAI->CastSpell(spellId, bot))
        return true;

    // the cast itself failed (a missing tool, say): try again later, not every tick
    nextSearch->Set(time(nullptr) + MINUTE);
    return false;
}

bool BuyForTradeAction::Execute(Event /*event*/)
{
    auto const* visit = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!visit)
        return false;

    Creature* vendor = bot->GetNPCIfCanInteractWith(visit->npcOrGo, UNIT_NPC_FLAG_VENDOR);
    VendorItemData const* vendorItems = vendor ? vendor->GetVendorItems() : nullptr;
    if (!vendorItems)
        return false;

    float const discount = bot->GetReputationPriceDiscount(vendor);
    uint32 bought = 0;
    for (uint32 slot = 0; slot < vendorItems->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = vendorItems->GetItem(slot);
        ItemTemplate const* proto = vendorItem ? sObjectMgr->GetItemTemplate(vendorItem->item) : nullptr;
        bool const recipe = proto && proto->Class == ITEM_CLASS_RECIPE;
        if (!proto || vendorItem->ExtendedCost || !(recipe ? EarnedProfessions::Enabled(botAI)
                                                           : EarnedProfessions::CraftEnabled(botAI)))
            continue;

        // "skill": a reagent of a known recipe that gives skill, with less than a stack in the bags; or a recipe the
        // bot can use (skill rank) and does not know
        if (AI_VALUE2(ItemUsage, "item usage", proto->ItemId) != ITEM_USAGE_SKILL)
            continue;

        uint32 const price = uint32(floor(proto->BuyPrice * discount));
        uint32 const stack = proto->GetMaxStackSize();
        for (uint32 i = 0; i < 20 && bot->GetItemCount(proto->ItemId) < stack; ++i)
        {
            if (AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::tradeskill)) < price)
                break;

            uint32 const before = bot->GetItemCount(proto->ItemId);
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, proto->ItemId, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(proto->ItemId) <= before)
                break;
            ++bought;
        }
    }

    if (bought)
        LOG_INFO("playerbots", "[earned] {} buys {} lots of reagents or recipes at {} ({})", bot->GetName(), bought,
                 vendor->GetName(), vendor->GetEntry());
    return bought > 0;
}

bool LearnRecipeAction::Execute(Event /*event*/)
{
    Item* recipe = EarnedProfessions::LearnableRecipe(botAI);
    if (!recipe)
        return false;

    uint32 const itemId = recipe->GetEntry();
    uint32 const spellId = recipe->GetTemplate()->Spells[1].SpellId;
    if (!UseItemAuto(recipe))
        return false;

    LOG_INFO("playerbots", "[earned] {} learns spell {} from recipe item {}", bot->GetName(), spellId, itemId);
    return true;
}
