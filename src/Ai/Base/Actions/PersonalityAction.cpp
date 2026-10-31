/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PersonalityAction.h"

#include "Event.h"
#include "PersonalityMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

#include <algorithm>
#include <cctype>
#include <sstream>

bool PersonalityAction::Execute(Event event)
{
    Player* requester = event.getOwner();

    std::string param = event.getParam();
    std::transform(param.begin(), param.end(), param.begin(), [](unsigned char c) { return std::tolower(c); });
    std::istringstream in(param);
    std::string word;
    in >> word;

    // Any other word stays unanswered: "personality" also starts sentences said in party chat.
    if (word != "?" && word != "reload")
        return true;

    // Only the owner, about one of the owner's own characters; the other bots of the party stay quiet.
    if (!PersonalityMgr::IsPersonalityAlt(bot, requester))
        return true;

    if (!sPlayerbotAIConfig.personalityEnable)
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

    // The first look reads the file: an alt that never was in a scene has not been read yet.
    sPersonalityMgr.RequestFile(bot->GetGUID().GetCounter(), bot->GetName(), false);
    botAI->TellMasterNoFacing(sPersonalityMgr.StatusLine(bot));
    return true;
}
