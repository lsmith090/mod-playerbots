/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTBOTS_H
#define PLAYERBOTS_TESTBOTS_H

#include "Define.h"

class Player;

// Test bots (agent-wow local patch, #186): the characters of the accounts in Playerbots.TestBotAccounts count as
// random bots once a GM logs them in (".playerbots rndbot testlogin <name>"), but the manager never logs them in or
// out on its own, never randomizes, refreshes or teleports them, and they don't wander.
namespace TestBots
{
// Reads Playerbots.TestBotAccounts and adds those accounts to the random bot account list; at config load
void Load();

bool IsTestAccount(uint32 accountId);
bool IsTestBot(Player* bot);
}  // namespace TestBots

#endif
