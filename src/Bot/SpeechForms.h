/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPEECHFORMS_H
#define PLAYERBOTS_SPEECHFORMS_H

#include <string>

#include "ObjectGuid.h"

class Player;
class PlayerbotAI;
class WorldPosition;

// Random bots speak a few fixed forms a player can read and say back (agent-wow local patch). Off unless
// Playerbots.RandomBotSpeechForms = 1. The wording is fixed: the pilots' client parses it (scripts/clawd/speech.py).
//   "danger <mob name>": an elite or rare is on the bot, or a second mob is. Party chat when grouped; alone, the zone's
//                         General, once per bot per mob per Playerbots.RandomBotSpeechGeneralMinutes (10).
//   "need heal":         the bot is grouped and at critical health. Party chat only.
//   "going <place>":     the bot's wander sets out for a grind spot or a town. Party chat only. The place is the map's
//                         words: the sub-area, else the nearest game_tele place, else the zone; never coordinates.
// Heard in party from a member, bot or player: on "danger" a bot out of combat within 40 yd of the speaker attacks the
// mob on the speaker (else it holds where it is); on "need heal" a bot that knows a heal casts it on the speaker.
namespace SpeechForms
{
bool Enabled();

// A party line of the forms a bot acts on: "danger <name>" or "need heal"
bool Is(uint32 type, std::string const& text);

// A chat packet the bot received: true when it is one of the forms, queued for the bot's next update. Any thread.
bool Hear(Player* bot, uint32 type, ObjectGuid from, std::string const& text);

// The bot's turn, at its update: it says danger or need heal, and acts on what it heard
void Update(PlayerbotAI* botAI);

// The wander set out for a grind spot or a town: "going <place>" in party
void Going(PlayerbotAI* botAI, WorldPosition const& to);
}  // namespace SpeechForms

#endif
