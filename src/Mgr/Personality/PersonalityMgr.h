/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYMGR_H
#define PLAYERBOTS_PERSONALITYMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "PersonalityText.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Creature;
class Group;
class Item;
class Player;
class Quest;

// Bot personalities (PERSONALITY_SPEC.md), phase 1: files and scenes, no AI yet.
//
// A player's own character played as a bot (an alt, see CoaMissionMgr::IsMissionBot) has a personality
// when a file <AiPlayerbot.Personality.Dir>/<CharacterName>.md exists. While exactly one real player
// groups with such alts, their party has a scene: who is there, and what happened lately.
//
// Threads: hooks run on the map thread of the player they are about and only add to the scene under a
// lock; the members of a scene are read from the real player's own update, on the same map, as
// CoaGroupTelemetry does. Files are read on a worker thread, never on a map or world thread.
enum class PersonalityFileState : uint8
{
    None,     // never asked for
    Loading,  // on the worker
    Missing,  // no file for this character
    Loaded
};

// One member of a scene, as the real player's update last saw it.
struct PersonalityCard
{
    ObjectGuid::LowType guid = 0;
    std::string name;
    bool isPlayer = false;  // the real player; every other card is an alt with a personality
    uint32 level = 0;
    std::string race;
    std::string className;
    std::string specName;  // the CoA specialization, empty for the nine base classes
    std::string role;      // "tank", "healer", "damage", empty when unknown
    std::string zone;
    uint32 healthPct = 0;
    uint32 manaPct = 0;
    bool alive = true;
    bool inCombat = false;
};

class PersonalityMgr
{
public:
    static PersonalityMgr& instance()
    {
        static PersonalityMgr instance;
        return instance;
    }

    void Stop();

    // An alt of this player, in the sense of the missions: one of the player's own characters, played as a bot.
    static bool IsPersonalityAlt(Player* bot, Player* player);

    // Files
    void RequestFile(ObjectGuid::LowType guid, std::string const& name, bool force);
    void ReloadAll();
    PersonalityFileState FileState(ObjectGuid::LowType guid) const;
    bool GetFile(ObjectGuid::LowType guid, PersonalityText& text, std::string& path) const;
    std::string FolderPath() const;

    // Hooks
    void OnPlayerUpdate(Player* player, uint32 diff);
    void OnUnparsedChat(Player* bot, Player* from, uint32 type, std::string const& text);
    void OnDied(Player* player, Creature* killer);
    void OnCreatureKill(Player* player, Creature* killed);
    void OnLoot(Player* player, Item* item);
    void OnZone(Player* player, uint32 zoneId);
    void OnMapChanged(Player* player);
    void OnLevelChanged(Player* player, uint8 oldLevel);
    void OnQuestComplete(Player* player, Quest const* quest);
    void OnGroupDisband(Group* group);
    void OnLogout(Player* player);
    void Update(uint32 diff);  // world thread

    // Commands
    std::vector<std::string> DescribeScene(Group* group) const;
    std::string StatusLine(Player* bot) const;

private:
    struct FileEntry
    {
        PersonalityFileState state = PersonalityFileState::None;
        std::string name;
        std::string path;
        PersonalityText text;
    };

    struct Scene
    {
        ObjectGuid::LowType player = 0;
        std::string playerName;
        std::unordered_map<ObjectGuid::LowType, std::string> roster;  // the alts with a personality, on any map
        std::vector<PersonalityCard> cards;  // the player first, then the alts of the roster on the player's map
        SceneLog log{40};      // sized from AiPlayerbot.Personality.SceneEvents when the scene starts
        bool active = false;  // one real player and at least one alt with a personality, last time we looked
        bool inCombat = false;
        bool allDead = false;
        uint32 instanceMap = 0;  // the dungeon, raid or battleground the player is in, 0 in the open world
        std::string instanceName;
        uint32 lastSeenMs = 0;
    };

    enum class NoteFrom : uint8
    {
        Anyone,     // any member of the party: a death or a boss kill is the party's, personality or not
        PlayerOnly  // the alts follow the player: their own copy of the event says nothing new
    };

    PersonalityMgr() = default;
    ~PersonalityMgr();

    // Adds to the scene of this party, if it has an active one. upgrade: replaces the text of the same
    // event noted within dedupSeconds instead of being dropped as a copy of it.
    void Note(Group* group, ObjectGuid::LowType who, NoteFrom from, std::string key, std::string text,
              int64 dedupSeconds, bool upgrade = false);

    void Post(std::function<void()> job);
    void WorkerLoop();
    void LoadFile(ObjectGuid::LowType guid, std::string name, std::string dir, uint32 maxBytes);  // worker thread

    mutable std::mutex filesLock;
    std::unordered_map<ObjectGuid::LowType, FileEntry> files;

    mutable std::mutex scenesLock;
    std::unordered_map<uint64, Scene> scenes;  // by group guid
    std::unordered_map<ObjectGuid::LowType, uint32> sampleTimers;
    uint32 sweepTimer = 0;  // world thread only

    std::mutex jobsLock;
    std::condition_variable jobsWake;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    bool stopping = false;
};

#define sPersonalityMgr PersonalityMgr::instance()

#endif
