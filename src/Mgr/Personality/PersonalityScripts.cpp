/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// What a party of a player and alts with a personality goes through, handed to PersonalityMgr. Every hook
// returns at once while AiPlayerbot.Personality.Enable is 0.

#include "PersonalityMgr.h"

#include "Creature.h"
#include "Group.h"
#include "GroupScript.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"

namespace
{
class PersonalityPlayerScript : public PlayerScript
{
public:
    PersonalityPlayerScript()
        : PlayerScript("PersonalityPlayerScript",
                       {PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                        PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_UPDATE_ZONE,
                        PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
                        PLAYERHOOK_ON_LOGOUT})
    {
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnPlayerUpdate(player, diff);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnDied(player, nullptr);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnDied(killed, killer);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnCreatureKill(killer, killed);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnLoot(player, item);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnZone(player, newZone);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnMapChanged(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnLevelChanged(player, oldLevel);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (sPersonalityMgr.Enabled())
            sPersonalityMgr.OnQuestComplete(player, quest);
    }

    // Even with the feature off: a file read before it was turned off is forgotten.
    void OnPlayerLogout(Player* player) override { sPersonalityMgr.OnLogout(player); }
};

class PersonalityGroupScript : public GroupScript
{
public:
    PersonalityGroupScript() : GroupScript("PersonalityGroupScript", {GROUPHOOK_ON_DISBAND}) {}

    void OnDisband(Group* group) override { sPersonalityMgr.OnGroupDisband(group); }
};

class PersonalityWorldScript : public WorldScript
{
public:
    PersonalityWorldScript()
        : WorldScript("PersonalityWorldScript", {WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                                                 WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnBeforeWorldInitialized() override { sPersonalityMgr.LoadConfig(); }

    // ".reload config": the model, the budgets and the key are taken without a restart.
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            sPersonalityMgr.LoadConfig();
    }

    void OnUpdate(uint32 diff) override { sPersonalityMgr.Update(diff); }

    void OnShutdown() override { sPersonalityMgr.Stop(); }
};
}  // namespace

void AddSC_bot_personality()
{
    new PersonalityPlayerScript();
    new PersonalityGroupScript();
    new PersonalityWorldScript();
}
