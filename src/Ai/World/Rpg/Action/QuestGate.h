/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTGATE_H
#define PLAYERBOTS_QUESTGATE_H

// agent-wow local patch (Playerbots.RandomBotQuestGate): start the quest gate's one read of the items quests ask
// for, on a thread of its own, once. No map or world thread waits on it: a map thread holding that read froze the
// world for 5 min on 2026-10-19. Called at startup when the gate is on, and at the gate's first use; until the read
// is in, or if it fails, items hold no quest back (kill objectives still do).
void LoadQuestGateItems();

#endif
