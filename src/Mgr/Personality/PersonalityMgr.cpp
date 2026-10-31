/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PersonalityMgr.h"

#include "CoaMissionMgr.h"
#include "CoaSpecLookup.h"
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
#include "Timer.h"
#include "World.h"

#include <algorithm>
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
}  // namespace

PersonalityMgr::~PersonalityMgr() { Stop(); }

bool PersonalityMgr::IsPersonalityAlt(Player* bot, Player* player) { return CoaMissionMgr::IsMissionBot(bot, player); }

void PersonalityMgr::Stop()
{
    {
        std::lock_guard<std::mutex> guard(jobsLock);
        stopping = true;
        jobs.clear();
    }
    jobsWake.notify_all();
    if (worker.joinable())
        worker.join();
}

void PersonalityMgr::Post(std::function<void()> job)
{
    std::lock_guard<std::mutex> guard(jobsLock);
    if (stopping)
        return;
    if (!worker.joinable())
        worker = std::thread(&PersonalityMgr::WorkerLoop, this);
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

    std::string const dir = sPlayerbotAIConfig.personalityDir;
    uint32 const maxBytes = sPlayerbotAIConfig.personalityMaxFileBytes;
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
    std::error_code error;
    std::filesystem::path const absolute = std::filesystem::absolute(Utf8Path(sPlayerbotAIConfig.personalityDir), error);
    return error ? sPlayerbotAIConfig.personalityDir : PathUtf8(absolute);
}

void PersonalityMgr::OnPlayerUpdate(Player* player, uint32 diff)
{
    if (!sPlayerbotAIConfig.personalityEnable || !player || !player->GetSession() || player->GetSession()->IsBot() ||
        !IsRealPlayer(player))
        return;

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
    std::string const stamp = Stamp(now);
    Map* map = player->GetMap();

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
        Scene& scene = scenes[key];
        scene = Scene{};
        scene.player = playerGuid;
        scene.playerName = player->GetName();
        scene.log.SetCapacity(sPlayerbotAIConfig.personalitySceneEvents);

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
        scene.log.Add(now, stamp, "", JoinNames(names) + " are together" + (where.empty() ? "" : " in " + where), 0);
        found = scenes.find(key);
    }
    else
    {
        Scene& scene = found->second;
        for (auto const& [guid, name] : roster)
            if (!scene.roster.count(guid))
                scene.log.Add(now, stamp, "", name + " joined the party", 0);
        for (auto const& [guid, name] : scene.roster)
            if (!roster.count(guid))
                scene.log.Add(now, stamp, "", name + " left the party", 0);
    }

    Scene& scene = found->second;
    if (allDead && !scene.allDead)
        scene.log.Add(now, stamp, "wipe", "the party wiped" +
                      (scene.instanceName.empty() ? (cards.front().zone.empty() ? "" : " in " + cards.front().zone)
                                                  : " in " + scene.instanceName), 30);

    scene.log.SetCapacity(sPlayerbotAIConfig.personalitySceneEvents);
    scene.roster = std::move(roster);
    scene.cards = std::move(cards);
    scene.inCombat = inCombat;
    scene.allDead = allDead;
    scene.active = true;
    scene.lastSeenMs = getMSTime();
}

void PersonalityMgr::Note(Group* group, ObjectGuid::LowType who, NoteFrom from, std::string key, std::string text,
                          int64 dedupSeconds, bool upgrade)
{
    if (!sPlayerbotAIConfig.personalityEnable || !group)
        return;

    time_t const now = time(nullptr);
    std::string stamp = Stamp(now);

    std::lock_guard<std::mutex> guard(scenesLock);
    auto found = scenes.find(group->GetGUID().GetRawValue());
    if (found == scenes.end() || !found->second.active)
        return;
    Scene& scene = found->second;
    if (from == NoteFrom::PlayerOnly && who != scene.player)
        return;

    if (upgrade && scene.log.Upgrade(now, key, text, dedupSeconds))
        return;
    scene.log.Add(now, std::move(stamp), std::move(key), std::move(text), dedupSeconds);
}

void PersonalityMgr::OnUnparsedChat(Player* bot, Player* from, uint32 type, std::string const& text)
{
    // Only what the player says to the party: a whisper is for one alt, and channels are for strangers.
    if (!sPlayerbotAIConfig.personalityEnable || !bot || !from || !IsPartyChat(type) || !IsRealPlayer(from))
        return;

    std::string const line = PersonalityPlainChat(text);
    if (line.empty())
        return;

    // Every bot of the party hears it once: the copies are kept once.
    Note(bot->GetGroup(), from->GetGUID().GetCounter(), NoteFrom::PlayerOnly, "say:" + line,
         from->GetName() + ": \"" + line + "\"", 10);
}

void PersonalityMgr::OnDied(Player* player, Creature* killer)
{
    if (!player)
        return;
    // Two hooks see a death: the one that knows the killer says more.
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "death:" + std::to_string(player->GetGUID().GetCounter()),
         player->GetName() + (killer ? " died to " + killer->GetName() : " died"), 15, killer != nullptr);
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
         "the party killed " + killed->GetName() + (boss ? " (boss)" : " (rare)"), 60);
}

void PersonalityMgr::OnLoot(Player* player, Item* item)
{
    if (!player || !item)
        return;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Quality < sPlayerbotAIConfig.personalityLootQuality)
        return;
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone,
         "loot:" + std::to_string(item->GetGUID().GetCounter()),
         player->GetName() + " looted [" + proto->Name1 + "] (" + QualityName(proto->Quality) + ")", 30);
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
    if (!sPlayerbotAIConfig.personalityEnable || !player || !player->GetGroup() || !player->GetMap())
        return;

    Map* map = player->GetMap();
    time_t const now = time(nullptr);
    std::string const stamp = Stamp(now);

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
        scene.log.Add(now, stamp, "map:" + std::to_string(map->GetId()),
                      "entered " + scene.instanceName + " (" + kind + ")", 60);
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
         player->GetName() + " reached level " + level, 60);
}

void PersonalityMgr::OnQuestComplete(Player* player, Quest const* quest)
{
    if (!player || !quest)
        return;
    Note(player->GetGroup(), player->GetGUID().GetCounter(), NoteFrom::Anyone, "quest:" + std::to_string(quest->GetQuestId()),
         player->GetName() + " completed the quest " + quest->GetTitle(), 120);
}

void PersonalityMgr::OnGroupDisband(Group* group)
{
    if (!group)
        return;
    std::lock_guard<std::mutex> guard(scenesLock);
    scenes.erase(group->GetGUID().GetRawValue());
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
        it = getMSTimeDiff(it->second.lastSeenMs, now) > SceneKeepMs ? scenes.erase(it) : std::next(it);
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
                    (scene.instanceName.empty() ? "" : ", in " + scene.instanceName) + "):");
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
    lines.push_back("Recent (" + std::to_string(scene.log.Size()) + "):");
    for (std::string const& line : scene.log.Lines())
        lines.push_back("  " + line);
    return lines;
}

std::string PersonalityMgr::StatusLine(Player* bot) const
{
    if (!sPlayerbotAIConfig.personalityEnable)
        return "Personalities are off on this server (AiPlayerbot.Personality.Enable).";

    std::string const file = bot->GetName() + ".md";
    std::lock_guard<std::mutex> guard(filesLock);
    auto entry = files.find(bot->GetGUID().GetCounter());
    if (entry == files.end() || entry->second.state == PersonalityFileState::Loading)
        return "Reading my personality file, ask again in a moment.";
    if (entry->second.text.Empty())
        return "No personality: write " + sPlayerbotAIConfig.personalityDir + "/" + file +
               " on the server (start from _template.md).";

    PersonalityText const& text = entry->second.text;
    std::string who;
    if (std::string const* found = text.Find("Who I am"))
        who = found->substr(0, found->find('\n'));
    if (who.size() > 120)
        who = PersonalityPlainChat(who, 117) + "...";
    return "Personality: " + file + ", " + std::to_string(text.bytes) + " bytes" +
           (text.truncated ? " (cut, the file is too long)" : "") + (who.empty() ? "." : ". " + who);
}
