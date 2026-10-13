/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTTRAITS_H
#define PLAYERBOTS_BOTTRAITS_H

#include "Define.h"

class Player;

// What a random bot is like, for its whole life (agent-wow local patch): a trait is read off the character's guid, so
// it is the same at every login and needs no table. Every rule that makes a bot take or avoid a risk reads the same
// trait, so one bot is careless in all of them and another careful in all of them, and a player who meets it twice
// meets the same character.
namespace BotTraits
{
// 0 (reckless) to 99 (careful)
uint32 Care(Player* bot);
// Playerbots.RandomBotCarelessPercent of the random bots are careless: those whose care is under it. 0: none is.
bool Careless(Player* bot);
}  // namespace BotTraits

#endif
