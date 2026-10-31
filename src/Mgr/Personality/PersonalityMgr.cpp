/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PersonalityMgr.h"

#include "CoaMissionMgr.h"
#include "CoaSpecLookup.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Random.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
constexpr uint32 SampleEveryMs = 1000;                         // a look at the party, from the player's update
constexpr uint32 SceneKeepMs = 10 * MINUTE * IN_MILLISECONDS;  // a scene nobody looked at for this long goes
constexpr uint32 SweepEveryMs = 10 * IN_MILLISECONDS;
constexpr size_t ReadLimit = 1 << 20;                          // a personality file is never read past 1 MiB
constexpr int32 LineGiveUpMs = 15 * IN_MILLISECONDS;           // a line its speaker could not say by then is dropped
constexpr uint32 WorkerCount = 2;

std::filesystem::path Utf8Path(std::string const& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

std::string PathUtf8(std::filesystem::path const& path)
{
    std::u8string const text = path.u8string();
    return std::string(text.begin(), text.end());
}

// "21:04", server time.
std::string Stamp(time_t now)
{
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char text[8];
    std::strftime(text, sizeof(text), "%H:%M", &parts);
    return text;
}

// A name of the DBC in the server's locale, else the English one.
std::string DbcName(char const* const* names)
{
    LocaleConstant const locale = sWorld->GetDefaultDbcLocale();
    if (names[locale] && *names[locale])
        return names[locale];
    return names[LOCALE_enUS] ? names[LOCALE_enUS] : "";
}

std::string ZoneName(uint32 zoneId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
    return area ? DbcName(area->area_name) : "";
}

char const* QualityName(uint32 quality)
{
    static char const* const names[] = {"poor", "common", "uncommon", "rare", "epic", "legendary", "artifact", "heirloom"};
    return quality < std::size(names) ? names[quality] : "unknown";
}

// The language of the lines: AiPlayerbot.BotTextLocale, or the player's client when it is -1.
std::string LanguageName(uint32 locale)
{
    static char const* const names[] = {"English", "Korean", "French", "German", "Simplified Chinese",
                                        "Traditional Chinese", "Spanish", "Latin American Spanish", "Russian"};
    return locale < std::size(names) ? names[locale] : "English";
}

// "Sqazz, Thraxa and Brana"
std::string JoinNames(std::vector<std::string> const& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

bool IsPartyChat(uint32 type)
{
    return type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER || type == CHAT_MSG_RAID ||
           type == CHAT_MSG_RAID_LEADER;
}

// Read on the map thread of `member`: the real player's own, or an alt on the same map.
PersonalityCard MakeCard(Player* member, bool isPlayer)
{
    PersonalityCard card;
    card.guid = member->GetGUID().GetCounter();
    card.name = member->GetName();
    card.isPlayer = isPlayer;
    card.level = member->GetLevel();
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(member->getRace()))
        card.race = DbcName(race->name);
    if (ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(member->getClass()))
        card.className = DbcName(cls->name);
    if (IsCoaClass(member))
        if (CoaSpecStrategy const* spec = GetCoaSpecStrategyFor(member))
        {
            card.specName = spec->specName;
            card.role = spec->role == CoaSpecRole::Tank ? "tank" : spec->role == CoaSpecRole::Heal ? "healer" : "damage";
        }
    card.zone = ZoneName(member->GetZoneId());
    card.healthPct = uint32(member->GetHealthPct() + 0.5f);
    if (uint32 const maxMana = member->GetMaxPower(POWER_MANA))
        card.manaPct = member->GetPower(POWER_MANA) * 100 / maxMana;
    card.alive = member->IsAlive();
    card.inCombat = member->IsInCombat();
    return card;
}

PromptCard ToPromptCard(PersonalityCard const& card)
{
    PromptCard prompt;
    prompt.name = card.name;
    prompt.isPlayer = card.isPlayer;
    prompt.level = card.level;
    prompt.race = card.race;
    prompt.className = card.className;
    prompt.specName = card.specName;
    prompt.role = card.role;
    prompt.zone = card.zone;
    prompt.healthPct = card.healthPct;
    prompt.manaPct = card.manaPct;
    prompt.alive = card.alive;
    return prompt;
}

// Milliseconds from `then` to `now` on getMSTime()'s wrapping clock; negative when `then` is still ahead.
int32 MsSince(uint32 then, uint32 now) { return int32(now - then); }
}  // namespace

PersonalityMgr::~PersonalityMgr() { Stop(); }

void PersonalityMgr::LoadConfig()
{
    auto loaded = std::make_shared<PersonalityConfig>();
    PersonalityConfig& c = *loaded;
    std::string const p = "AiPlayerbot.Personality.";

    c.enable = sConfigMgr->GetOption<bool>(p + "Enable", false, false);
    c.dir = sConfigMgr->GetOption<std::string>(p + "Dir", "personalities", false);
    c.maxFileBytes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "MaxFileBytes", 4096, false), 256, 65536);
    c.sceneEvents = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "SceneEvents", 40, false), 5, 200);
    c.lootQuality = std::min<uint32>(sConfigMgr->GetOption<uint32>(p + "LootQuality", 3, false), 5);

    c.provider = PersonalityFoldAscii(sConfigMgr->GetOption<std::string>(p + "Provider", "none", false));
    c.api.host = sConfigMgr->GetOption<std::string>(p + "ApiHost", "api.anthropic.com", false);
    c.api.model = sConfigMgr->GetOption<std::string>(p + "Model", "claude-opus-5-5", false);
    c.api.effort = sConfigMgr->GetOption<std::string>(p + "Effort", "low", false);
    c.api.maxTokens = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "MaxTokens", 4096, false), 1024, 32000);
    c.api.timeoutMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "TimeoutMs", 30000, false), 5000, 120000);
    c.api.fallbacks = sConfigMgr->GetOption<bool>(p + "Fallbacks", true, false);

    // The key: an environment variable first, so that it never sits in a conf file that ends up in backups.
    c.apiKeyEnv = sConfigMgr->GetOption<std::string>(p + "ApiKeyEnv", "ANTHROPIC_API_KEY", false);
    if (!c.apiKeyEnv.empty())
        if (char const* value = std::getenv(c.apiKeyEnv.c_str()); value && *value)
        {
            c.api.apiKey = value;
            c.apiKeySource = "environment variable " + c.apiKeyEnv;
        }
    if (c.api.apiKey.empty())
    {
        c.api.apiKey = sConfigMgr->GetOption<std::string>(p + "ApiKey", "", false);
        if (!c.api.apiKey.empty())
            c.apiKeySource = p + "ApiKey";
    }

    c.maxLines = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "MaxLines", 3, false), 1, 6);
    c.eventChance = std::min<uint32>(sConfigMgr->GetOption<uint32>(p + "EventChance", 35, false), 100);
    c.idleBanterSec = std::max<uint32>(sConfigMgr->GetOption<uint32>(p + "IdleBanterSec", 120, false), 30);
    c.idleChance = std::min<uint32>(sConfigMgr->GetOption<uint32>(p + "IdleChance", 25, false), 100);
    c.exchangeCooldownSec = sConfigMgr->GetOption<uint32>(p + "ExchangeCooldownSec", 20, false);
    c.maxExchangesPerHour = sConfigMgr->GetOption<uint32>(p + "MaxExchangesPerHour", 40, false);
    c.dailyCallsPerAccount = sConfigMgr->GetOption<uint32>(p + "DailyCallsPerAccount", 400, false);
    c.maxInFlight = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(p + "MaxInFlight", 4, false), 1, 16);

    if (c.enable)
    {
        if (c.provider == "none")
            LOG_INFO("server.loading", "Bot personalities: on, scenes only (AiPlayerbot.Personality.Provider = none).");
        else if (c.provider != "anthropic")
            LOG_ERROR("server.loading", "Bot personalities: unknown provider \"{}\", the alts stay silent.", c.provider);
        else if (c.api.apiKey.empty())
            LOG_ERROR("server.loading", "Bot personalities: no API key in the environment variable {} nor in {}ApiKey, "
                      "the alts stay silent.", c.apiKeyEnv, p);
        else
            LOG_INFO("server.loading", "Bot personalities: on, {} ({}, effort {}), key from the {}.", c.provider,
                     c.api.model, c.api.effort.empty() ? "default" : c.api.effort, c.apiKeySource);
    }

    {
        std::lock_guard<std::mutex> guard(configLock);
        config = loaded;
    }
    enabled.store(c.enable, std::memory_order_relaxed);
    if (c.enable)
        RequestFrame();
}

std::shared_ptr<PersonalityConfig const> PersonalityMgr::Config() const
{
    std::lock_guard<std::mutex> guard(configLock);
    return config;
}

void PersonalityMgr::RequestFrame()
{
    std::string const dir = Config()->dir;
    Post([this, dir]() { LoadFrame(dir); });
}

std::shared_ptr<std::string const> PersonalityMgr::Frame() const
{
    std::lock_guard<std::mutex> guard(configLock);
    return frame;
}

void PersonalityMgr::LoadFrame(std::string dir)
{
    std::filesystem::path const file = Utf8Path(dir) / "_frame.md";
    std::string text;
    std::error_code error;
    if (std::filesystem::is_regular_file(file, error))
    {
        std::ifstream in(file, std::ios::binary);
        std::string raw(64 * 1024, '\0');  // a frame is a page, not a book
        in.read(raw.data(), std::streamsize(raw.size()));
        raw.resize(size_t(in.gcount()));
        text = PersonalityTrim(PersonalityStripComments(std::move(raw)));
    }

    if (text.empty())
        LOG_INFO("playerbots", "Bot personalities: no {}, the built-in frame is used.", PathUtf8(file));
    else
        LOG_INFO("playerbots", "Bot personalities: frame read from {} ({} bytes).", PathUtf8(file), text.size());

    std::lock_guard<std::mutex> guard(configLock);
    frame = std::make_shared<std::string const>(std::move(text));
}

bool PersonalityMgr::IsPersonalityAlt(Player* bot, Player* player) { return CoaMissionMgr::IsMissionBot(bot, player); }

void PersonalityMgr::Stop()
{
    {
        std::lock_guard<std::mutex> guard(jobsLock);
        stopping = true;
        jobs.clear();
    }
    jobsWake.notify_all();
    // A call on its way ends at AiPlayerbot.Personality.TimeoutMs at the latest.
    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
    workers.clear();
}

void PersonalityMgr::Post(std::function<void()> job)
{
    std::lock_guard<std::mutex> guard(jobsLock);
    if (stopping)
        return;
    while (workers.size() < WorkerCount)
        workers.emplace_back(&PersonalityMgr::WorkerLoop, this);
    jobs.push_back(std::move(job));
    jobsWake.notify_one();
}

void PersonalityMgr::WorkerLoop()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> guard(jobsLock);
            jobsWake.wait(guard, [this]() { return stopping || !jobs.empty(); });
            if (stopping)
                return;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
    }
}

void PersonalityMgr::RequestFile(ObjectGuid::LowType guid, std::string const& name, bool force)
{
    {
        std::lock_guard<std::mutex> guard(filesLock);
        FileEntry& entry = files[guid];
        if (!force && entry.state != PersonalityFileState::None)
            return;
        // A file read again keeps its old text until the new one is in: the alt does not leave its scene meanwhile.
        entry.state = PersonalityFileState::Loading;
        entry.name = name;
    }

    std::shared_ptr<PersonalityConfig const> const cfg = Config();
    std::string const dir = cfg->dir;
    uint32 const maxBytes = cfg->maxFileBytes;
    Post([this, guid, name, dir, maxBytes]() { LoadFile(guid, name, dir, maxBytes); });
}

void PersonalityMgr::ReloadAll()
{
    std::vector<std::pair<ObjectGuid::LowType, std::string>> known;
    {
        std::lock_guard<std::mutex> guard(filesLock);
        for (auto const& [guid, entry] : files)
            known.emplace_back(guid, entry.name);
    }
    for (auto const& [guid, name] : known)
        RequestFile(guid, name, true);
    RequestFrame();
}

void PersonalityMgr::LoadFile(ObjectGuid::LowType guid, std::string name, std::string dir, uint32 maxBytes)
{
    namespace fs = std::filesystem;

    PersonalityText text;
    std::string path;
    std::error_code error;

    if (IsSafePersonalityName(name))
    {
        fs::path const folder = Utf8Path(dir);
        fs::path file = folder / Utf8Path(name + ".md");

        // Linux tells "thraxa.md" from "Thraxa.md": any case of the name will do.
        if (!fs::is_regular_file(file, error))
        {
            file.clear();
            std::string const wanted = PersonalityFoldAscii(name + ".md");
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
                if (it->is_regular_file(error) && PersonalityFoldAscii(PathUtf8(it->path().filename())) == wanted)
                {
                    file = it->path();
                    break;
                }
        }

        if (!file.empty())
        {
            std::ifstream in(file, std::ios::binary);
            std::string raw(ReadLimit, '\0');
            in.read(raw.data(), std::streamsize(raw.size()));
            raw.resize(size_t(in.gcount()));
            text = ParsePersonality(std::move(raw), maxBytes);
            path = PathUtf8(file);

            if (text.Empty())
                LOG_WARN("playerbots", "Personality file {} of {} says nothing: the alt has no personality.", path, name);
            else
                LOG_INFO("playerbots", "Personality of {} read from {} ({} bytes{}).", name, path, text.bytes,
                         text.truncated ? ", cut at AiPlayerbot.Personality.MaxFileBytes" : "");
        }
    }

    std::lock_guard<std::mutex> guard(filesLock);
    auto entry = files.find(guid);
    if (entry == files.end())
        return;  // the alt logged out meanwhile
    entry->second.state = text.Empty() ? PersonalityFileState::Missing : PersonalityFileState::Loaded;
    entry->second.path = path;
    entry->second.text = std::move(text);
}

PersonalityFileState PersonalityMgr::FileState(ObjectGuid::LowType guid) const
{
    std::lock_guard<std::mutex> guard(filesLock);
    auto entry = files.find(guid);
    return entry == files.end() ? PersonalityFileState::None : entry->second.state;
}

bool PersonalityMgr::GetFile(ObjectGuid::LowType guid, PersonalityText& text, std::string& path) const
{
    std::lock_guard<std::mutex> guard(filesLock);
    auto entry = files.find(guid);
    if (entry == files.end() || entry->second.text.Empty())
        return false;
    text = entry->second.text;
    path = entry->second.path;
    return true;
}

std::string PersonalityMgr::FolderPath() const
{
    std::string const dir = Config()->dir;
    std::error_code error;
    std::filesystem::path const absolute = std::filesystem::absolute(Utf8Path(dir), error);
    return error ? dir : PathUtf8(absolute);
}

void PersonalityMgr::OnPlayerUpdate(Player* player, uint32 diff)
{
    if (!Enabled() || !player || !player->GetSession())
        return;

    // A bot: its own lines, if there are any waiting.
    if (player->GetSession()->IsBot() || !IsRealPlayer(player))
    {
        SayLines(player);
        return;
    }

    ObjectGuid::LowType const playerGuid = player->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> guard(scenesLock);
        uint32& timer = sampleTimers[playerGuid];
        timer += diff;
        if (timer < SampleEveryMs)
            return;
        timer = 0;
    }

    Group* group = player->GetGroup();
    if (!group)
        return;

    std::shared_ptr<PersonalityConfig const> const cfg = Config();

    // Who is there. The alts of the roster may be on any map; only those on the player's map are read.
    uint32 realPlayers = 0;
    bool inCombat = false;
    bool allDead = true;
    std::unordered_map<ObjectGuid::LowType, std::string> roster;
    std::vector<PersonalityCard> cards{MakeCard(player, true)};
    std::vector<std::pair<ObjectGuid::LowType, std::string>> toRead;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        if (IsRealPlayer(member))
            ++realPlayers;

        bool const sameMap = member->IsInWorld() && member->GetMap() == player->GetMap();
        if (sameMap)
        {
            inCombat |= member->IsInCombat();
            allDead &= !member->IsAlive();
        }

        if (member == player || !IsPersonalityAlt(member, player))
            continue;

        ObjectGuid::LowType const guid = member->GetGUID().GetCounter();
        bool hasPersonality = false;
        {
            std::lock_guard<std::mutex> guard(filesLock);
            auto entry = files.find(guid);
            if (entry == files.end())
                toRead.emplace_back(guid, member->GetName());
            else
                hasPersonality = !entry->second.text.Empty();
        }
        if (!hasPersonality)
            continue;

        roster.emplace(guid, member->GetName());
        if (sameMap)
            cards.push_back(MakeCard(member, false));
    }

    for (auto const& [guid, name] : toRead)
        RequestFile(guid, name, false);

    bool const active = realPlayers == 1 && !roster.empty();
    uint64 const key = group->GetGUID().GetRawValue();
    time_t const now = time(nullptr);
    uint32 const nowMs = getMSTime();
    std::string const stamp = Stamp(now);
    Map* map = player->GetMap();

    int32 const textLocale = sPlayerbotAIConfig.botTextLocale;
    std::string const language =
        LanguageName(textLocale >= 0 ? uint32(textLocale) : uint32(player->GetSession()->GetSessionDbcLocale()));

    std::function<void()> job;
    {
        std::lock_guard<std::mutex> guard(scenesLock);
        auto found = scenes.find(key);
        if (!active)
        {
            if (found != scenes.end())
                found->second.active = false;
            return;
        }

        if (found == scenes.end() || found->second.player != playerGuid)
        {
            if (found != scenes.end())
                queuedLines -= uint32(found->second.lines.size());
            Scene& scene = scenes[key];
            scene = Scene{};
            scene.player = playerGuid;
            scene.playerName = player->GetName();
            scene.log.SetCapacity(cfg->sceneEvents);

            std::vector<std::string> names{player->GetName()};
            for (auto const& [guid, name] : roster)
                names.push_back(name);
            std::string where = cards.front().zone;
            if (map && map->Instanceable())
            {
                scene.instanceMap = map->GetId();
                scene.instanceName = map->GetMapName();
                where = scene.instanceName;
            }
            std::string const together = JoinNames(names) + " are together" + (where.empty() ? "" : " in " + where);
            scene.log.Add(now, stamp, "", together, 0);
            if (cfg->Talks())
                Offer(scene, TriggerKind::Event, together + ": the party just formed", 3000, nowMs);
            found = scenes.find(key);
        }
        else
        {
            Scene& scene = found->second;
            for (auto const& [guid, name] : roster)
                if (!scene.roster.count(guid))
                {
                    scene.log.Add(now, stamp, "", name + " joined the party", 0);
                    if (cfg->Talks() && roll_chance_i(int(cfg->eventChance)))
                        Offer(scene, TriggerKind::Event, name + " joined the party", 2000, nowMs);
                }
            for (auto const& [guid, name] : scene.roster)
                if (!roster.count(guid))
                    scene.log.Add(now, stamp, "", name + " left the party", 0);
        }

        Scene& scene = found->second;
        if (allDead && !scene.allDead)
        {
            std::string const wipe = "the party wiped" +
                (scene.instanceName.empty() ? (cards.front().zone.empty() ? "" : " in " + cards.front().zone)
                                            : " in " + scene.instanceName);
            if (scene.log.Add(now, stamp, "wipe", wipe, 30) && cfg->Talks())
                Offer(scene, TriggerKind::Event, wipe, 2000, nowMs);
        }

        scene.log.SetCapacity(cfg->sceneEvents);
        scene.roster = std::move(roster);
        scene.cards = std::move(cards);
        scene.inCombat = inCombat;
        scene.allDead = allDead;
        scene.active = true;
        scene.lastSeenMs = nowMs;
        scene.account = player->GetSession()->GetAccountId();
        scene.language = language;

        job = Direct(key, scene, *cfg, nowMs);
    }

    if (job)
        Post(std::move(job));
}

void PersonalityMgr::Offer(Scene& scene, TriggerKind kind, std::string text, uint32 delayMs, uint32 now)
{
    // A weaker reason never replaces a stronger one; a newer one of the same kind is the more relevant.
    if (kind < scene.trigger.kind)
        return;
    scene.trigger.kind = kind;
    scene.trigger.text = std::move(text);
    scene.trigger.sinceMs = now;
    scene.trigger.notBeforeMs = now + delayMs;
}

std::function<void()> PersonalityMgr::Direct(uint64 key, Scene& scene, PersonalityConfig const& cfg, uint32 now)
{
    // Lines nobody could say: a speaker that never came by, or a fight that started meanwhile.
    while (!scene.lines.empty() && MsSince(scene.lines.front().dueMs, now) > LineGiveUpMs)
    {
        scene.lines.pop_front();
        --queuedLines;
    }
    if (scene.inCombat)
        for (auto it = scene.lines.begin(); it != scene.lines.end();)
            if (it->keepInCombat)
                ++it;
            else
            {
                it = scene.lines.erase(it);
                --queuedLines;
            }

    if (!cfg.Talks())
    {
        scene.trigger = Trigger{};
        return {};
    }

    // A quiet moment, now and then.
    if (scene.inCombat || scene.inFlight || !scene.lines.empty() || !scene.quietSinceMs)
        scene.quietSinceMs = now;
    else if (scene.trigger.kind == TriggerKind::None && MsSince(scene.quietSinceMs, now) >= int32(cfg.idleBanterSec * IN_MILLISECONDS))
    {
        scene.quietSinceMs = now;
        if (roll_chance_i(int(cfg.idleChance)))
        {
            // "Nothing is happening" alone leaves the model nothing but the descriptions to recite: a seed to start from.
            std::string text = "A quiet moment, nobody is fighting.";
            if (std::string const seed = IdleSeed(scene); !seed.empty())
                text += " An idea to start from, only if it fits: " + seed + ".";
            Offer(scene, TriggerKind::Idle, std::move(text), 0, now);
        }
    }

    Trigger& trigger = scene.trigger;
    if (trigger.kind == TriggerKind::None || scene.inFlight || !scene.lines.empty() || MsSince(trigger.notBeforeMs, now) < 0)
        return {};

    // After the fight: the player's words wait 30 s for it to end, an event 90 s, a quiet moment not at all.
    int32 const age = MsSince(trigger.sinceMs, now);
    if (scene.inCombat)
    {
        if (trigger.kind == TriggerKind::Idle || (trigger.kind == TriggerKind::Event && age > 90 * IN_MILLISECONDS) ||
            (trigger.kind == TriggerKind::PlayerTalk && age > 30 * IN_MILLISECONDS))
            trigger = Trigger{};
        return {};
    }

    int32 gap = int32(cfg.exchangeCooldownSec * IN_MILLISECONDS);
    if (trigger.kind == TriggerKind::PlayerTalk)
        gap /= 2;
    if (scene.lastExchangeMs && MsSince(scene.lastExchangeMs, now) < gap)
    {
        if (trigger.kind != TriggerKind::PlayerTalk && age > 60 * IN_MILLISECONDS)
            trigger = Trigger{};
        return {};
    }

    while (!scene.exchangesThisHour.empty() && MsSince(scene.exchangesThisHour.front(), now) > int32(HOUR * IN_MILLISECONDS))
        scene.exchangesThisHour.pop_front();
    if (cfg.maxExchangesPerHour && scene.exchangesThisHour.size() >= cfg.maxExchangesPerHour)
    {
        trigger = Trigger{};
        return {};
    }

    uint32 const today = uint32(time(nullptr) / DAY);
    std::pair<uint32, uint32>& daily = dailyCalls[scene.account];
    if (daily.first != today)
        daily = {today, 0};
    if (cfg.dailyCallsPerAccount && daily.second >= cfg.dailyCallsPerAccount)
    {
        // Said once, so that the player knows why the alts went quiet.
        if (trigger.kind == TriggerKind::PlayerTalk && !scene.talkedOutSaid && !scene.roster.empty())
        {
            auto const& [guid, name] = *scene.roster.begin();
            scene.lines.push_back({guid, name, "I'm talked out for today, " + scene.playerName + ". Tomorrow.", now + 1500, true});
            ++queuedLines;
            scene.talkedOutSaid = true;
        }
        trigger = Trigger{};
        return {};
    }

    if (callsInFlight.load() >= cfg.maxInFlight)
        return {};  // the next look tries again

    // What the model reads, copied now: the call runs on a worker, and the scene moves on meanwhile.
    PromptInput input;
    input.frame = *Frame();
    input.recentLines.assign(scene.recentSaid.begin(), scene.recentSaid.end());
    input.playerName = scene.playerName;
    input.language = scene.language;
    input.maxLines = cfg.maxLines;
    input.trigger = trigger.text;
    input.scene = scene.log.Lines();
    for (PersonalityCard const& card : scene.cards)
        input.cards.push_back(ToPromptCard(card));

    std::unordered_map<std::string, std::pair<ObjectGuid::LowType, std::string>> speakers;
    {
        std::lock_guard<std::mutex> guard(filesLock);
        for (auto const& [guid, name] : scene.roster)
        {
            auto entry = files.find(guid);
            if (entry == files.end() || entry->second.text.Empty())
                continue;
            input.characters.push_back({guid, name, entry->second.text});
            speakers[PersonalityFoldAscii(name)] = {guid, name};
        }
    }
    if (input.characters.empty())
    {
        trigger = Trigger{};
        return {};
    }

    bool const answersPlayer = trigger.kind == TriggerKind::PlayerTalk;
    trigger = Trigger{};
    scene.inFlight = true;
    scene.exchangesThisHour.push_back(now);
    ++daily.second;
    ++callsInFlight;
    {
        std::lock_guard<std::mutex> guard(statsLock);
        ++stats.calls;
    }

    PersonalityApiSettings const settings = cfg.api;
    uint32 const maxLines = cfg.maxLines;
    return [this, key, input = std::move(input), speakers = std::move(speakers), answersPlayer, settings, maxLines]()
    {
        PersonalityApiResult result =
            RequestPersonalityLines(settings, BuildPersonalitySystem(input), BuildPersonalityUser(input), maxLines);
        FinishExchange(key, speakers, answersPlayer, maxLines, std::move(result));
    };
}

std::string PersonalityMgr::IdleSeed(Scene const& scene) const
{
    // Most seeds point away from the descriptions, which the model already leans on too much: something new
    // about a companion, a question, a tease, the place, something that happened. One may be a line of a file.
    std::vector<std::string> seeds;
    std::vector<std::string> names;
    for (auto const& [guid, name] : scene.roster)
        names.push_back(name);
    std::sort(names.begin(), names.end());

    for (std::string const& name : names)
    {
        seeds.push_back(name + " brings up something about themselves that is not in their description: a memory, "
                        "an opinion, a plan, a small complaint");
        seeds.push_back(name + " asks " + scene.playerName + " something personal or silly");
        seeds.push_back(name + " teases " + (names.size() > 1 && name == names.front() ? names.back() : scene.playerName));
    }
    if (names.size() > 1)
        seeds.push_back(names.front() + " and " + names.back() + " disagree about something small");

    if (!scene.instanceName.empty())
        seeds.push_back("what they think of " + scene.instanceName);
    else if (!scene.cards.empty() && !scene.cards.front().zone.empty())
        seeds.push_back("what they think of " + scene.cards.front().zone);

    std::vector<std::string> events;
    for (SceneEntry const& entry : scene.log.Entries())
        if (!entry.key.empty() && entry.key.compare(0, 4, "say:") != 0)
            events.push_back(entry.text);
    if (!events.empty())
        seeds.push_back("looking back on this: " + events[urand(0, uint32(events.size() - 1))]);

    std::vector<std::string> fileLines;
    {
        std::lock_guard<std::mutex> guard(filesLock);
        for (auto const& [guid, name] : scene.roster)
        {
            auto entry = files.find(guid);
            if (entry == files.end())
                continue;
            for (char const* heading : {"Likes and dislikes", "The others"})
                if (std::string const* text = entry->second.text.Find(heading))
                    for (size_t start = 0; start < text->size();)
                    {
                        size_t end = text->find('\n', start);
                        if (end == std::string::npos)
                            end = text->size();
                        if (std::string line = PersonalityTrim(std::string_view(*text).substr(start, end - start)); !line.empty())
                            fileLines.push_back(name + ": " + line);
                        start = end + 1;
                    }
        }
    }
    if (!fileLines.empty())
        seeds.push_back(fileLines[urand(0, uint32(fileLines.size() - 1))]);

    return seeds.empty() ? std::string() : seeds[urand(0, uint32(seeds.size() - 1))];
}

void PersonalityMgr::FinishExchange(uint64 key,
                                    std::unordered_map<std::string, std::pair<ObjectGuid::LowType, std::string>> speakers,
                                    bool answersPlayer, uint32 maxLines, PersonalityApiResult result)
{
    --callsInFlight;

    {
        std::lock_guard<std::mutex> guard(statsLock);
        stats.inputTokens += result.inputTokens;
        stats.cacheReadTokens += result.cacheReadTokens;
        stats.cacheWriteTokens += result.cacheWriteTokens;
        stats.outputTokens += result.outputTokens;
        stats.lastLatencyMs = result.latencyMs;
        if (result.ok)
            ++stats.answered;
        else if (result.refused)
            ++stats.refused;
        else
            ++stats.failed;
        if (!result.ok)
            stats.lastError = result.error;
    }

    if (!result.ok)
        LOG_WARN("playerbots", "Bot personalities: the call failed ({} ms): {}", result.latencyMs, result.error);
    else
        LOG_INFO("playerbots.personality", "{} line(s) from {} in {} ms: {} in, {} from cache, {} cached, {} out",
                 result.lines.size(), result.model, result.latencyMs, result.inputTokens, result.cacheReadTokens,
                 result.cacheWriteTokens, result.outputTokens);

    uint32 const now = getMSTime();
    uint32 added = 0;
    {
        std::lock_guard<std::mutex> guard(scenesLock);
        auto found = scenes.find(key);
        if (found == scenes.end())
            return;  // the party is gone
        Scene& scene = found->second;
        scene.inFlight = false;
        scene.lastExchangeMs = now;
        scene.quietSinceMs = now;
        if (!result.ok)
            return;

        // Typed one after the other: a short pause, then a few seconds more for a longer line.
        uint32 due = now + urand(300, 1200);
        for (PersonalityLine const& line : result.lines)
        {
            if (added >= maxLines)
                break;
            auto speaker = speakers.find(PersonalityFoldAscii(PersonalityTrim(line.speaker)));
            if (speaker == speakers.end())
                continue;  // only the alts of the party speak
            std::string text = PersonalitySanitizeLine(line.text, speaker->second.second);
            if (text.empty())
                continue;
            uint32 const length = uint32(text.size());
            scene.lines.push_back({speaker->second.first, speaker->second.second, std::move(text), due,
                                   answersPlayer && added == 0});
            ++added;
            due += 1500 + 35 * length + urand(0, 1500);
        }
        queuedLines += added;
    }

    std::lock_guard<std::mutex> guard(statsLock);
    stats.lines += added;
}

void PersonalityMgr::SayLines(Player* bot)
{
    if (!queuedLines.load(std::memory_order_relaxed))
        return;
    Group* group = bot->GetGroup();
    if (!group)
        return;

    QueuedLine line;
    {
        std::lock_guard<std::mutex> guard(scenesLock);
        auto found = scenes.find(group->GetGUID().GetRawValue());
        if (found == scenes.end() || found->second.lines.empty())
            return;
        Scene& scene = found->second;
        QueuedLine& next = scene.lines.front();
        if (next.guid != bot->GetGUID().GetCounter() || MsSince(next.dueMs, getMSTime()) < 0)
            return;  // not this alt's turn, or not yet
        line = std::move(next);
        scene.lines.pop_front();
        --queuedLines;
        scene.recentSaid.push_back(line.name + ": " + line.text);
        while (scene.recentSaid.size() > 12)
            scene.recentSaid.pop_front();
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    if (group->isRaidGroup())
        botAI->SayToRaid(line.text);
    else
        botAI->SayToParty(line.text);

    // What was said is part of the scene: the next exchange knows it.
    Note(group, bot->GetGUID().GetCounter(), NoteFrom::Anyone, "", line.name + ": \"" + line.text + "\"", 0);
}

bool PersonalityMgr::Note(Group* group, ObjectGuid::LowType who, NoteFrom from, std::string key, std::string text,
                          int64 dedupSeconds, React react, bool upgrade)
{
    if (!Enabled() || !group)
        return false;

    time_t const now = time(nullptr);
    std::string stamp = Stamp(now);

    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = scenes.find(group->GetGUID().GetRawValue());
    if (found == scenes.end() || !found->second.active)
        return false;
    Scene& scene = found->second;
    if (from == NoteFrom::PlayerOnly && who != scene.player)
        return false;

    if (upgrade && scene.log.Upgrade(now, key, text, dedupSeconds))
        return false;
    std::string const reaction = react != React::None ? text : std::string();
    if (!scene.log.Add(now, std::move(stamp), std::move(key), std::move(text), dedupSeconds))
        return false;

    if (react != React::None)
    {
        std::shared_ptr<PersonalityConfig const> const cfg = Config();
        if (cfg->Talks() && (react == React::Always || roll_chance_i(int(cfg->eventChance))))
            Offer(scene, TriggerKind::Event, reaction, 1500, getMSTime());
    }
    return true;
}

void PersonalityMgr::OnUnparsedChat(Player* bot, Player* from, uint32 type, std::string const& text)
{
    // Only what the player says to the party: a whisper is for one alt, and channels are for strangers.
    if (!Enabled() || !bot || !from || !IsPartyChat(type) || !IsRealPlayer(from))
        return;

    std::string const line = PersonalityPlainChat(text);
    if (line.empty())
        return;

    // Every bot of the party hears it once: the copies are kept once, and answered once.
    Group* group = bot->GetGroup();
    if (!Note(group, from->GetGUID().GetCounter(), NoteFrom::PlayerOnly, "say:" + line,
              from->GetName() + ": \"" + line + "\"", 10))
        return;
    if (!Config()->Talks())
        return;

    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = scenes.find(group->GetGUID().GetRawValue());
    if (found == scenes.end())
        return;
    Scene& scene = found->second;

    std::string trigger = from->GetName() + " said in party: \"" + line + "\"";
    std::string const folded = PersonalityFoldAscii(line);
    for (auto const& [guid, name] : scene.roster)
        if (folded.find(PersonalityFoldAscii(name)) != std::string::npos)
        {
            trigger += " (to " + name + ")";
            break;
        }
    Offer(scene, TriggerKind::PlayerTalk, std::move(trigger), urand(2000, 4000), getMSTime());
}

void PersonalityMgr::OnDied(Player* player, Creature* killer)
{
    if (!player)
        return;
    // Two hooks see a death: the one that knows the killer says more.
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "death:" + std::to_string(player->GetGUID().GetCounter()),
         player->GetName() + (killer ? " died to " + killer->GetName() : " died"), 15, React::Chance, killer != nullptr);
}

void PersonalityMgr::OnCreatureKill(Player* player, Creature* killed)
{
    if (!player || !killed)
        return;
    CreatureTemplate const* info = killed->GetCreatureTemplate();
    bool const boss = killed->IsDungeonBoss() || killed->isWorldBoss();
    bool const rare = info && (info->rank == CREATURE_ELITE_RARE || info->rank == CREATURE_ELITE_RAREELITE);
    if (!boss && !rare)
        return;  // trash says nothing worth a line
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "kill:" + std::to_string(killed->GetGUID().GetCounter()),
         "the party killed " + killed->GetName() + (boss ? " (boss)" : " (rare)"), 60,
         boss ? React::Always : React::Chance);
}

void PersonalityMgr::OnLoot(Player* player, Item* item)
{
    if (!player || !item)
        return;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Quality < Config()->lootQuality)
        return;
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "loot:" + std::to_string(item->GetGUID().GetCounter()),
         player->GetName() + " looted [" + proto->Name1 + "] (" + QualityName(proto->Quality) + ")", 30, React::Chance);
}

void PersonalityMgr::OnZone(Player* player, uint32 zoneId)
{
    if (!player || !player->GetMap() || player->GetMap()->Instanceable())
        return;  // inside, the instance is the place
    std::string const zone = ZoneName(zoneId);
    if (zone.empty())
        return;
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::PlayerOnly, "zone:" + std::to_string(zoneId),
         "now in " + zone, 300);
}

void PersonalityMgr::OnMapChanged(Player* player)
{
    if (!Enabled() || !player || !player->GetGroup() || !player->GetMap())
        return;

    Map* map = player->GetMap();
    time_t const now = time(nullptr);
    std::string const stamp = Stamp(now);
    std::shared_ptr<PersonalityConfig const> const cfg = Config();

    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = scenes.find(player->GetGroup()->GetGUID().GetRawValue());
    if (found == scenes.end() || !found->second.active || found->second.player != player->GetGUID().GetCounter())
        return;
    Scene& scene = found->second;

    if (map->Instanceable())
    {
        if (scene.instanceMap == map->GetId())
            return;
        scene.instanceMap = map->GetId();
        scene.instanceName = map->GetMapName();
        char const* kind = map->IsRaid() ? "raid" : map->IsBattlegroundOrArena() ? "battleground" : "dungeon";
        std::string const entered = "entered " + scene.instanceName + " (" + kind + ")";
        if (scene.log.Add(now, stamp, "map:" + std::to_string(map->GetId()), entered, 60) && cfg->Talks() &&
            roll_chance_i(int(cfg->eventChance)))
            Offer(scene, TriggerKind::Event, "the party " + entered, 8000, getMSTime());  // once everyone is in
    }
    else if (scene.instanceMap)
    {
        scene.log.Add(now, stamp, "", "left " + scene.instanceName, 0);
        scene.instanceMap = 0;
        scene.instanceName.clear();
    }
}

void PersonalityMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (!player || player->GetLevel() <= oldLevel)
        return;
    std::string const level = std::to_string(player->GetLevel());
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "level:" + std::to_string(player->GetGUID().GetCounter()) + ":" + level,
         player->GetName() + " reached level " + level, 60, React::Always);
}

void PersonalityMgr::OnQuestComplete(Player* player, Quest const* quest)
{
    if (!player || !quest)
        return;
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone, "quest:" + std::to_string(quest->GetQuestId()),
         player->GetName() + " completed the quest " + quest->GetTitle(), 120, React::Chance);
}

void PersonalityMgr::OnGroupDisband(Group* group)
{
    if (!group)
        return;
    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = scenes.find(group->GetGUID().GetRawValue());
    if (found == scenes.end())
        return;
    queuedLines -= uint32(found->second.lines.size());
    scenes.erase(found);
}

void PersonalityMgr::OnLogout(Player* player)
{
    if (!player)
        return;
    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> guard(scenesLock);
        sampleTimers.erase(guid);
    }
    // The file is read again at the next login: what was edited meanwhile is taken.
    std::lock_guard<std::mutex> guard(filesLock);
    files.erase(guid);
}

void PersonalityMgr::Update(uint32 diff)
{
    sweepTimer += diff;
    if (sweepTimer < SweepEveryMs)
        return;
    sweepTimer = 0;

    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(scenesLock);
    for (auto it = scenes.begin(); it != scenes.end();)
    {
        if (getMSTimeDiff(it->second.lastSeenMs, now) <= SceneKeepMs || it->second.inFlight)
        {
            ++it;
            continue;
        }
        queuedLines -= uint32(it->second.lines.size());
        it = scenes.erase(it);
    }
}

std::vector<std::string> PersonalityMgr::DescribeScene(Group* group) const
{
    std::vector<std::string> lines;
    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = group ? scenes.find(group->GetGUID().GetRawValue()) : scenes.end();
    if (found == scenes.end())
    {
        lines.push_back("No scene for this party: it needs exactly one real player and an alt with a personality file.");
        return lines;
    }

    Scene const& scene = found->second;
    lines.push_back("Scene of " + scene.playerName + "'s party (" + (scene.active ? "active" : "paused") +
                    (scene.inCombat ? ", in combat" : "") +
                    (scene.instanceName.empty() ? "" : ", in " + scene.instanceName) +
                    (scene.inFlight ? ", writing lines" : "") + "):");
    for (PersonalityCard const& card : scene.cards)
    {
        std::string line = "  " + card.name + (card.isPlayer ? " (player)" : "") + ": level " +
                           std::to_string(card.level) + " " + card.race + " " + card.className;
        if (!card.specName.empty())
            line += " (" + card.specName + (card.role.empty() ? "" : ", " + card.role) + ")";
        if (!card.zone.empty())
            line += " in " + card.zone;
        line += ", " + std::to_string(card.healthPct) + "% health";
        if (card.manaPct)
            line += ", " + std::to_string(card.manaPct) + "% mana";
        if (!card.alive)
            line += ", dead";
        lines.push_back(line);
    }
    for (auto const& [guid, name] : scene.roster)
    {
        bool const here = std::any_of(scene.cards.begin(), scene.cards.end(),
                                      [guid = guid](PersonalityCard const& card) { return card.guid == guid; });
        if (!here)
            lines.push_back("  " + name + ": in the party, on another map");
    }
    if (scene.trigger.kind != TriggerKind::None)
        lines.push_back("Next to answer: " + scene.trigger.text);
    if (!scene.lines.empty())
        lines.push_back(std::to_string(scene.lines.size()) + " line(s) waiting to be said");
    lines.push_back("Recent (" + std::to_string(scene.log.Size()) + "):");
    for (std::string const& line : scene.log.Lines())
        lines.push_back("  " + line);
    return lines;
}

std::vector<std::string> PersonalityMgr::DescribeStats() const
{
    std::shared_ptr<PersonalityConfig const> const cfg = Config();
    std::vector<std::string> lines;
    std::string state = !cfg->enable ? "off (AiPlayerbot.Personality.Enable = 0)"
                        : cfg->Talks() ? "talking: " + cfg->provider + ", " + cfg->api.model + ", effort " +
                                             (cfg->api.effort.empty() ? std::string("default") : cfg->api.effort) +
                                             ", key from the " + cfg->apiKeySource
                        : cfg->provider == "none" ? std::string("scenes only (Provider = none)")
                                                  : "silent: no API key (environment variable " + cfg->apiKeyEnv + ")";
    lines.push_back("Bot personalities: " + state);

    std::lock_guard<std::mutex> guard(statsLock);
    lines.push_back("Calls since start: " + std::to_string(stats.calls) + " (" + std::to_string(stats.answered) +
                    " answered, " + std::to_string(stats.failed) + " failed, " + std::to_string(stats.refused) +
                    " declined), " + std::to_string(stats.lines) + " line(s) said or waiting");
    uint64 const input = stats.inputTokens + stats.cacheReadTokens + stats.cacheWriteTokens;
    lines.push_back("Tokens: " + std::to_string(input) + " in (" + std::to_string(stats.cacheReadTokens) +
                    " read from cache, " + std::to_string(stats.cacheWriteTokens) + " written to it), " +
                    std::to_string(stats.outputTokens) + " out; last call " + std::to_string(stats.lastLatencyMs) + " ms");
    if (!stats.lastError.empty())
        lines.push_back("Last error: " + stats.lastError);
    return lines;
}

std::string PersonalityMgr::StatusLine(Player* bot) const
{
    if (!Enabled())
        return "Personalities are off on this server (AiPlayerbot.Personality.Enable).";

    std::string const file = bot->GetName() + ".md";
    std::lock_guard<std::mutex> guard(filesLock);
    auto entry = files.find(bot->GetGUID().GetCounter());
    if (entry == files.end() || entry->second.state == PersonalityFileState::Loading)
        return "Reading my personality file, ask again in a moment.";
    if (entry->second.text.Empty())
        return "No personality: write " + Config()->dir + "/" + file + " on the server (start from _template.md).";

    PersonalityText const& text = entry->second.text;
    std::string who;
    if (std::string const* found = text.Find("Who I am"))
        who = found->substr(0, found->find('\n'));
    if (who.size() > 110)
        who = PersonalityPlainChat(who, 107) + "...";
    return "Personality: " + file + ", " + std::to_string(text.bytes) + " bytes, voice " +
           (PersonalityVoiceOf(text) == PersonalityVoice::Player ? "player" : "character") +
           (text.truncated ? " (cut, the file is too long)" : "") + (who.empty() ? "." : ". " + who);
}
