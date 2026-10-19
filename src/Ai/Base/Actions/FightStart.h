/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FIGHTSTART_H
#define PLAYERBOTS_FIGHTSTART_H

class PlayerbotAI;

// agent-wow local patch: one `[fight]` line in Playerbots.log when a random bot enters combat, so a death's first
// seconds are known and not only its last 9. Off unless Playerbots.RandomBotFightLog is N > 0: at most one line per
// bot per N s (30 is a start). A second, short line closes a fight that went on to matter (mobs came after the first
// 2 s, the bot ended under 25% hp, or it died). Changes nothing the bot does.
namespace FightStart
{
// At the bot's update: note a bot that has just entered combat, write its line once the first 2 s are in, and close it
void Update(PlayerbotAI* botAI);
}  // namespace FightStart

#endif
