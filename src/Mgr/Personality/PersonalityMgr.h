/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYMGR_H
#define PLAYERBOTS_PERSONALITYMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "PersonalityPrompt.h"
#include "PersonalityProvider.h"
#include "PersonalityText.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
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

// Bot personalities (PERSONALITY_SPEC.md).
//
// A player's own character played as a bot (an alt, see CoaMissionMgr::IsMissionBot) has a personality
// when a file <AiPlayerbot.Personality.Dir>/<CharacterName>.md exists. While exactly one real player
// groups with such alts, their party has a scene: who is there, and what happened lately. When the player
// talks in party, something notable happens or the party is idle for a while, one API call writes the
// next lines of the alts, and each alt says its own in party chat.
//
// Threads: hooks run on the map thread of the player they are about and only add to the scene under a
// lock; the members of a scene are read from the real player's own update, on the same map, as
// CoaGroupTelemetry does, and the exchanges are decided there too. Each alt says its lines from its own
// update. Files and API calls run on two worker threads, never on a map or world thread.
// Lock order: scenesLock or whispersLock (never both), then filesLock, budgetLock, statsLock; configLock last.
enum class PersonalityFileState : uint8
{
    None,     // never asked for
    Loading,  // on the worker
    Missing,  // no file for this character
    Loaded
};

// AiPlayerbot.Personality.*, read at startup and on ".reload config". Kept out of PlayerbotAIConfig so that a
// new setting here rebuilds these files only.
struct PersonalityConfig
{
    bool enable = false;
    std::string dir = "personalities";
    uint32 maxFileBytes = 4096;
    uint32 sceneEvents = 40;
    uint32 lootQuality = 3;

    std::string provider = "none";  // none | anthropic
    std::string apiKeyEnv = "ANTHROPIC_API_KEY";
    std::string apiKeySource;       // where the key came from, for ".playerbots personality stats"; never the key
    PersonalityApiSettings api;

    uint32 maxLines = 3;
    uint32 eventChance = 35;
    uint32 idleBanterSec = 120;
    uint32 idleChance = 25;
    uint32 exchangeCooldownSec = 20;
    uint32 maxExchangesPerHour = 40;
    uint32 dailyCallsPerAccount = 400;
    uint32 maxInFlight = 4;

    // Long-term memory: at the end of a chapter, one call with SummaryModel writes <Name>.memory.md.
    bool memory = true;
    std::string summaryModel = "claude-haiku-5-5";
    uint32 chapterEvents = 25;
    uint32 memoryMaxBullets = 30;

    // The alts talk: a provider and a key. Without, the scene is kept and nothing is sent.
    bool Talks() const { return enable && provider == "anthropic" && !api.apiKey.empty(); }
    bool Remembers() const { return Talks() && memory; }
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

    void LoadConfig();
    std::shared_ptr<PersonalityConfig const> Config() const;
    // The frame of the prompt, <Dir>/_frame.md, read on the worker; empty: the built-in one.
    void RequestFrame();
    std::shared_ptr<std::string const> Frame() const;
    bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
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
    void OnPlayerUpdate(Player* player, uint32 diff);  // real players: the scene; alts: their lines
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
    std::vector<std::string> DescribeStats() const;
    std::string StatusLine(Player* bot) const;
    std::vector<std::string> RecentMemories(Player* bot, size_t count) const;
    void ForgetMemories(Player* bot);  // the file becomes <Name>.memory.md.bak

private:
    struct FileEntry
    {
        PersonalityFileState state = PersonalityFileState::None;
        std::string name;
        std::string path;
        PersonalityText text;
        std::vector<std::string> memories;  // <Name>.memory.md, oldest first
    };

    // What may start an exchange, the stronger first: the player talking beats an event, which beats a quiet moment.
    enum class TriggerKind : uint8
    {
        None,
        Idle,
        Event,
        PlayerTalk
    };

    struct Trigger
    {
        TriggerKind kind = TriggerKind::None;
        std::string text;
        uint32 sinceMs = 0;
        uint32 notBeforeMs = 0;
    };

    struct QueuedLine
    {
        ObjectGuid::LowType guid = 0;
        std::string name;
        std::string text;
        uint32 dueMs = 0;
        bool keepInCombat = false;  // the first answer to the player still comes when a fight starts
    };

    struct Scene
    {
        ObjectGuid::LowType player = 0;
        std::string playerName;
        uint32 account = 0;    // the player's: the daily budget counts per account
        std::string language;  // of the lines, from AiPlayerbot.BotTextLocale or the player's client
        std::unordered_map<ObjectGuid::LowType, std::string> roster;  // the alts with a personality, on any map
        std::vector<PersonalityCard> cards;  // the player first, then the alts of the roster on the player's map
        SceneLog log{40};      // sized from AiPlayerbot.Personality.SceneEvents when the scene starts
        bool active = false;  // one real player and at least one alt with a personality, last time we looked
        bool inCombat = false;
        bool allDead = false;
        uint32 instanceMap = 0;  // the dungeon, raid or battleground the player is in, 0 in the open world
        std::string instanceName;
        uint32 lastSeenMs = 0;

        Trigger trigger;
        bool inFlight = false;     // an API call for this party is on the worker
        uint32 lastExchangeMs = 0;
        uint32 quietSinceMs = 0;   // out of combat with nothing said since
        std::deque<uint32> exchangesThisHour;
        std::deque<QueuedLine> lines;
        std::deque<std::string> recentSaid;  // "Gidaeus: ...", shown to the model so that it does not repeat itself
        std::unordered_map<std::string, std::deque<std::string>> saidBy;  // each alt's own last lines: worn phrases
        std::vector<std::string> chapter;    // what happened since the last memories were written, "[21:04] ..."
        bool talkedOutSaid = false;
    };

    struct Stats
    {
        uint32 calls = 0;
        uint32 answered = 0;
        uint32 failed = 0;
        uint32 refused = 0;
        uint32 lines = 0;
        uint32 summaries = 0;
        uint32 memoriesWritten = 0;
        uint32 whispers = 0;
        uint64 inputTokens = 0;
        uint64 cacheReadTokens = 0;
        uint64 cacheWriteTokens = 0;
        uint64 outputTokens = 0;
        uint32 lastLatencyMs = 0;
        std::string lastError;
    };

    // A private conversation of the player with one alt (PersonalityPrompt.h, DefaultWhisperFrame).
    struct WhisperChat
    {
        std::string name;    // the alt
        std::string player;  // its owner, who whispers
        uint32 account = 0;
        std::string language;
        std::deque<std::string> history;   // "Pigeon: ...", "Sylanar: ...", the last 20
        std::deque<std::string> said;      // the alt's own last lines: worn phrases
        std::vector<std::string> chapter;  // what its memories are written from, at its logout
        std::deque<QueuedLine> lines;
        bool inFlight = false;
        bool pending = false;  // the player whispered while it was answering: answered next
        uint32 lastCallMs = 0;
    };

    enum class NoteFrom : uint8
    {
        Anyone,     // any member of the party: a death or a boss kill is the party's, personality or not
        PlayerOnly  // the alts follow the player: their own copy of the event says nothing new
    };

    enum class React : uint8
    {
        None,
        Chance,  // AiPlayerbot.Personality.EventChance
        Always
    };

    PersonalityMgr() = default;
    ~PersonalityMgr();

    // Adds to the scene of this party, if it has an active one; false when it was a copy of an entry already
    // there. upgrade: replaces the text of the same event noted within dedupSeconds instead.
    bool Note(Group* group, ObjectGuid::LowType who, NoteFrom from, std::string key, std::string text,
              int64 dedupSeconds, React react = React::None, bool upgrade = false);
    // scenesLock held
    bool Log(Scene& scene, time_t now, std::string stamp, std::string key, std::string text, int64 dedupSeconds);
    void EndChapter(Scene& scene, std::string reason, PersonalityConfig const& config, bool always);
    void Offer(Scene& scene, TriggerKind kind, std::string text, uint32 delayMs, uint32 now);
    std::function<void()> Direct(uint64 key, Scene& scene, PersonalityConfig const& config, uint32 now);
    std::string IdleSeed(Scene const& scene) const;
    void SayLines(Player* bot);
    void FinishExchange(uint64 key, std::unordered_map<std::string, std::pair<ObjectGuid::LowType, std::string>> speakers,
                        bool answersPlayer, uint32 maxLines, PersonalityApiResult result);

    void Post(std::function<void()> job);
    void WorkerLoop();
    void LoadFile(ObjectGuid::LowType guid, std::string name, std::string dir, uint32 maxBytes);  // worker thread
    void LoadFrame(std::string dir);                                                              // worker thread
    void SaveMemories(ObjectGuid::LowType guid, std::string name, std::vector<std::string> added, std::string dir,
                      std::string language, PersonalityApiSettings summary, uint32 maxBullets);  // worker thread
    void Account(PersonalityApiResult const& result);                                           // the stats of a call
    void PostMemories(MemoryInput input,
                      std::unordered_map<std::string, std::pair<ObjectGuid::LowType, std::string>> owners,
                      PersonalityConfig const& cfg);
    bool DailyRoom(uint32 account, uint32 limit);  // budgetLock
    void SpendDaily(uint32 account);

    // Whisper chats; the bot's own thread unless said otherwise.
    void OnWhisper(Player* bot, Player* from, std::string const& text);
    void DispatchWhisper(Player* bot);
    void FinishWhisper(ObjectGuid::LowType guid, PersonalityApiResult result);  // worker thread
    void WhisperUpdate(Player* bot);
    void EndWhisperChapter(WhisperChat& chat, PersonalityConfig const& cfg);   // whispersLock held
    std::mutex memoryLock;  // one memory file written at a time (workers only)

    mutable std::mutex configLock;
    std::shared_ptr<PersonalityConfig const> config = std::make_shared<PersonalityConfig>();
    std::shared_ptr<std::string const> frame = std::make_shared<std::string>();
    std::shared_ptr<std::string const> whisperFrame = std::make_shared<std::string>();
    std::atomic<bool> enabled{false};

    mutable std::mutex filesLock;
    std::unordered_map<ObjectGuid::LowType, FileEntry> files;

    mutable std::mutex scenesLock;
    std::unordered_map<uint64, Scene> scenes;  // by group guid
    std::unordered_map<ObjectGuid::LowType, uint32> sampleTimers;
    std::mutex budgetLock;
    std::unordered_map<uint32, std::pair<uint32, uint32>> dailyCalls;  // account: (day, calls that day); budgetLock

    std::mutex whispersLock;
    std::unordered_map<ObjectGuid::LowType, WhisperChat> whispers;  // by alt guid
    std::atomic<uint32> whisperWork{0};  // whisper lines waiting and chats waiting to answer: alts look only then
    uint32 sweepTimer = 0;  // world thread only
    std::atomic<uint32> queuedLines{0};  // lines waiting in any scene: alts look only when there are some
    std::atomic<uint32> callsInFlight{0};

    mutable std::mutex statsLock;
    Stats stats;

    std::mutex jobsLock;
    std::condition_variable jobsWake;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> workers;  // two: a file read never waits behind an API call
    bool stopping = false;
};

#define sPersonalityMgr PersonalityMgr::instance()

#endif
