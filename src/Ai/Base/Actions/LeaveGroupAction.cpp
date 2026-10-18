/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaveGroupAction.h"
#include "Event.h"
#include "GroupChat.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

bool LeaveGroupAction::Execute(Event event)
{
    Player* player = event.getOwner();
    if (player == botAI->GetMaster())
        return Leave();

    return false;
}

bool PartyCommandAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 operation;
    std::string member;

    p >> operation >> member;

    if (operation != PARTY_OP_LEAVE)
        return false;
    // Only leave if master has left the party, and randombot cannot set new master.
    Player* master = GetMaster();
    if (master && member == master->GetName())
    {
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            Player* newMaster = botAI->FindNewMaster();
            if (newMaster || bot->InBattleground())
            {
                botAI->SetMaster(newMaster);
                return false;
            }
        }
        return Leave();
    }
    return false;
}

bool UninviteAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    if (p.GetOpcode() == CMSG_GROUP_UNINVITE)
    {
        p.rpos(0);
        std::string memberName;
        p >> memberName;

        // player not found
        if (!normalizePlayerName(memberName))
        {
            return false;
        }

        if (bot->GetName() == memberName)
            return Leave();
    }

    if (p.GetOpcode() == CMSG_GROUP_UNINVITE_GUID)
    {
        p.rpos(0);
        ObjectGuid guid;
        p >> guid;

        if (bot->GetGUID() == guid)
            return Leave();
    }

    return false;
}

bool LeaveGroupAction::Leave()
{
    if (!botAI)
        return false;

    Player* master = botAI -> GetMaster();
    if (master)
        botAI->TellMaster(
            PlayerbotTextMgr::instance().GetBotTextOrDefault("goodbye", "Goodbye!", {}),
            PLAYERBOT_SECURITY_TALK);

    botAI->LeaveOrDisbandGroup();
    return true;
}

bool LeaveFarAwayAction::Execute(Event /*event*/)
{
    // agent-wow: why a random bot parted was nowhere; 32,000 joins a day ended and nothing said how
    Player* leader = botAI->GetGroupLeader();
    if (leader && GroupChat::Enabled() && sRandomPlayerbotMgr.IsRandomBot(bot))
        LOG_INFO("playerbots", "[group] {} leaves its group: {} ({:.0f} yd and {} levels from {})", bot->GetName(), why,
                 bot->GetMapId() == leader->GetMapId() ? bot->GetDistance2d(leader) : -1.0f,
                 std::abs(int32(leader->GetLevel()) - int32(bot->GetLevel())), leader->GetName());

    // allow bot to leave party when they want
    return Leave();
}

bool LeaveFarAwayAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (!bot->GetGroup())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Player* trueMaster = botAI->GetMaster();
    if (!groupLeader || (bot == groupLeader && !IsSelfBot(bot)))
        return false;

    PlayerbotAI* groupLeaderBotAI = nullptr;
    if (groupLeader)
        groupLeaderBotAI = GET_PLAYERBOT_AI(groupLeader);
    if (groupLeader && !groupLeaderBotAI)
        return false;

    if (trueMaster && !GET_PLAYERBOT_AI(trueMaster))
        return false;

    if (botAI->IsAltBot() &&
        (!groupLeaderBotAI || IsSelfBot(groupLeader)))  // Don't leave when an altbot is grouped under a regular real player or a SelfBot.
        return false;

    if (botAI->GetGrouperType() == GrouperType::SOLO)
    {
        why = "a loner";
        return true;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");

    if (dCount > 9)
    {
        why = "too many deaths";
        return true;
    }

    if (dCount > 4 && !botAI->HasGameClientMaster())
    {
        why = "too many deaths";
        return true;
    }

    if (bot->GetGuildId() == groupLeader->GetGuildId())
    {
        if (bot->GetLevel() > groupLeader->GetLevel() + 5)
        {
            if (AI_VALUE(bool, "should get money"))
                return false;
        }
    }

    if (abs(int32(groupLeader->GetLevel() - bot->GetLevel())) > 4)
    {
        why = "levels apart";
        return true;
    }

    // agent-wow: a bot that just joined on a quest's line is still walking over to its leader
    if (GroupChat::Holds(bot))
        return false;

    if (bot->GetMapId() != groupLeader->GetMapId() || bot->GetDistance2d(groupLeader) >= 2 * sPlayerbotAIConfig.RpgDistance)
    {
        why = "far away";
        return true;
    }

    return false;
}
