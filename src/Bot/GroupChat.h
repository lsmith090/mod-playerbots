/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GROUPCHAT_H
#define PLAYERBOTS_GROUPCHAT_H

#include <string>

#include "NonCombatStrategy.h"
#include "ObjectGuid.h"

class Group;
class Player;
class PlayerbotAI;

// Random bots group by asking in chat and answering, never by a blind invite (agent-wow local patch, #291). Off unless
// Playerbots.RandomBotGroupChat = 1. The wording is fixed: the pilots' client parses it.
//   "LFG <quest link>" in General: a leader-type bot alone asks for partners on a quest in its log
//   "LFM <quest link>" in General: a leader-type bot whose all-bot group has room offers a place
//   "inv <quest link>" whispered: the answer, "invite me". A bot invites only a random bot that answered its own line
//   of the last 3 minutes; a player who answers gets "inv <quest link>" back, so the player invites and leads.
// A random bot answers an LFG/LFM line in any channel from a bot or a player of its side in its zone:
// alone, not a solo-type bot, within 2 levels, with the quest in its log or able to take it; one time in two, at
// most 2 answers a line, one answer every 2 minutes.
namespace GroupChat
{
bool Enabled();

// Every member is one of the bot manager's random bots: a player, an altbot, an addclass or an offline member is not
bool AllRandomBots(Group* group);

// One of the fixed forms: an "inv" whisper or an "LFG"/"LFM" channel line, with a quest link
bool Is(uint32 type, std::string const& text);

// A chat packet the bot received: true when it is one of the fixed forms, queued for the bot's next update. Any thread.
bool Hear(Player* bot, uint32 type, ObjectGuid from, std::string const& text);

// The bot's turn, at its update: what it heard is answered, invited or accepted; a dead random bot leaves its group
void Update(PlayerbotAI* botAI);

// A leader-type bot's LFG (alone) or LFM (leading an all-bot group with room) in General: true when said
bool Ask(PlayerbotAI* botAI);

// The bot leads an all-bot group with room and may offer a place in it
bool CanOffer(PlayerbotAI* botAI);

// The bot joined a group on a quest's line a moment ago and is still walking over: it doesn't leave for distance yet
// (Playerbots.RandomBotGroupNear)
bool Holds(Player* bot);

// The bot follows another random bot in an all-bot group, and the group hold is on (Playerbots.RandomBotGroupNear)
bool Follows(Player* bot);
}  // namespace GroupChat

// "bot group": a follower in an all-bot group leaves when it falls 5 levels apart or 400 yards away (leave far away).
// No invite nearby or invite guild: grouping comes only from the chat above.
class BotGroupStrategy : public NonCombatStrategy
{
public:
    BotGroupStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "bot group"; }
    uint32 GetType() const override { return STRATEGY_TYPE_GENERIC; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
