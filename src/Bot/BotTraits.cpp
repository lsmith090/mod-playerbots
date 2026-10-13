/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTraits.h"

#include "Config.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"

uint32 BotTraits::Care(Player* bot)
{
    // guids are handed out in a row: the multiplier (Knuth's) spreads neighbours over the range
    return ((bot->GetGUID().GetCounter() * 2654435761u) >> 16) % 100;
}

bool BotTraits::Careless(Player* bot)
{
    return sRandomPlayerbotMgr.IsRandomBot(bot) &&
           Care(bot) < sConfigMgr->GetOption<uint32>("Playerbots.RandomBotCarelessPercent", 0);
}
