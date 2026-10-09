/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgProfessionAction.h"

#include "BudgetValues.h"
#include "Config.h"
#include "Event.h"
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
                                   SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING,
                                   SKILL_SKINNING,    SKILL_TAILORING};

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
