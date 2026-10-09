/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestBots.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "AccountMgr.h"
#include "Config.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "WorldSession.h"

namespace
{
std::mutex lock;  // written at config load, read from the map threads
std::unordered_set<uint32> accounts;
}  // namespace

void TestBots::Load()
{
    std::unordered_set<uint32> found;
    std::istringstream names(sConfigMgr->GetOption<std::string>("Playerbots.TestBotAccounts", ""));
    for (std::string name; std::getline(names, name, ',');)
    {
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }),
                   name.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
        if (name.empty())
            continue;

        uint32 const id = AccountMgr::GetId(name);
        if (!id)
        {
            LOG_ERROR("playerbots", "[testbots] no account {}", name);
            continue;
        }

        found.insert(id);
        if (!sPlayerbotAIConfig.IsInRandomAccountList(id))
            sPlayerbotAIConfig.RandomBotAccounts.push_back(id);
    }

    LOG_INFO("playerbots", "[testbots] {} test account(s)", found.size());
    std::lock_guard<std::mutex> guard(lock);
    accounts = std::move(found);
}

bool TestBots::IsTestAccount(uint32 accountId)
{
    std::lock_guard<std::mutex> guard(lock);
    return accounts.count(accountId) > 0;
}

bool TestBots::IsTestBot(Player* bot)
{
    return bot && bot->GetSession() && IsTestAccount(bot->GetSession()->GetAccountId());
}
