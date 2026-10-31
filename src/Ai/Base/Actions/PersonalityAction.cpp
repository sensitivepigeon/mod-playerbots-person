/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PersonalityAction.h"

#include "Event.h"
#include "PersonalityMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

bool PersonalityAction::Execute(Event event)
{
    Player* requester = event.getOwner();

    std::string param = event.getParam();
    std::transform(param.begin(), param.end(), param.begin(), [](unsigned char c) { return std::tolower(c); });
    std::istringstream in(param);
    std::string word;
    in >> word;

    // Any other word stays unanswered: "personality" also starts sentences said in party chat.
    if (word != "?" && word != "reload" && word != "memory" && word != "forget")
        return true;

    // Only the owner, about one of the owner's own characters; the other bots of the party stay quiet.
    if (!PersonalityMgr::IsPersonalityAlt(bot, requester))
        return true;

    if (!sPersonalityMgr.Enabled())
    {
        botAI->TellMasterNoFacing(sPersonalityMgr.StatusLine(bot));
        return true;
    }

    if (word == "reload")
    {
        sPersonalityMgr.RequestFile(bot->GetGUID().GetCounter(), bot->GetName(), true);
        botAI->TellMasterNoFacing("Reading " + bot->GetName() + ".md again. Whisper \"personality ?\" in a moment.");
        return true;
    }

    // The last five memories of <Name>.memory.md, oldest first.
    if (word == "memory")
    {
        std::vector<std::string> const memories = sPersonalityMgr.RecentMemories(bot, 5);
        if (memories.empty())
            botAI->TellMasterNoFacing("No memories yet: they are written at the end of a dungeon, a party or a session.");
        for (std::string const& memory : memories)
            botAI->TellMasterNoFacing("Memory: " + memory);
        return true;
    }

    if (word == "forget")
    {
        sPersonalityMgr.ForgetMemories(bot);
        botAI->TellMasterNoFacing("Forgetting it all. The old memories are kept in " + bot->GetName() + ".memory.md.bak.");
        return true;
    }

    // The first look reads the file: an alt that never was in a scene has not been read yet.
    sPersonalityMgr.RequestFile(bot->GetGUID().GetCounter(), bot->GetName(), false);
    botAI->TellMasterNoFacing(sPersonalityMgr.StatusLine(bot));
    return true;
}
