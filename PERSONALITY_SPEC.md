# Spec: Bot Personalities (alt bots, party dialogue, memory)

Status, 30 October 2026, on branch `feat/personality-phase1`:

- **Phase 1 done**: built against the CoA core and tested in game.
- **Phase 2 done**: the alts talk.
- **Phase 3 done** (31 October, branch `feat/personality-2.8.3`, on CoA Bots 2.8.3): long-term memory as in 5.2,
  summarized with a cheaper `SummaryModel`. Also from phase 4: whisper chats (`_whisper.md`), and a worn-phrase
  detector that lists each alt's overused words in the prompt. On the SquidBots repack the bot server runs from `CoA-Bots\Core\`, so files go
  in `CoA-Bots\Core\personalities\`.

Phase 2 differs from the text below in these points:

- **HTTP and JSON**: Boost.Beast and Boost.JSON (header-only), with OpenSSL. Both are already in every AzerothCore
  Boost, so nothing is vendored. On Windows, OpenSSL is given the Windows trusted root certificates.
- **API version**: requests send `anthropic-version: 2026-10-21`, which rejects `temperature`/`top_p`/`top_k`.
  `fallbacks: "default"` is sent only for the models that take it, so never for Claude Haiku 5.5.
- **Settings**: they live in `PersonalityConfig` (`PersonalityMgr.h`), not in `PlayerbotAIConfig`. A new setting
  rebuilds only the personality files, and `.reload config` reads them all again.
- **Voice**: a `# Voice` heading in the file sets it to `character` (the default) or `player`. With `player`, the
  alt chats like a real person at the keyboard, not in roleplay.
- **Delivery**: alts say their lines from their own `OnPlayerUpdate`. `PlayerbotAI.cpp` has no second seam.
- **Stats**: `.playerbots personality stats` shows the calls, the tokens (with what the cache saved) and the last
  error. Branch base: `coa` (v2.8.2 in progress), fork `sensitivepigeon/mod-playerbots-person`.

Decisions so far:

- One personality **file per character**, written by people.
- **Player alt bots only.**
- **Hosted API first**: Anthropic.
- Alts talk **mostly in party chat**, to the player and **to each other**, while doing content, and **only while a real
  player is in the party**.
- No trade, world, general, LFG or guild chat.
- **Memory of circumstances**: short-term while playing, long-term between sessions.
- **One player per party** for now. The alts know that player's character and **talk to them by name**.
- **Files are edited by hand on this server** (this project only). A UI for editing comes after the feature works
  (phase 5).

## 1. What this is

You write a file for one of your alts. When you group with that alt and play, it talks in party chat:

- it answers what you say;
- it reacts to what happens (a boss dies, someone dies, a wipe, a good drop, a level up);
- it banters with your other alts during downtime.

It also remembers what you did together, last night included.

| Rule | Meaning |
|---|---|
| Opt-in per alt | No file for a character = that bot behaves exactly as today. No AI call, ever. |
| Off by default | `AiPlayerbot.Personality.Enable = 0`. The compiled default is off too (the README warns that an old `playerbots.conf` silently keeps old values, so new keys fall back to the compiled defaults). |
| Alts only | `botAI->IsAltBot()` must be true. Random bots, selfbots and bots with no real player in the group never get a personality. |
| Party only | Output goes to party (or raid) chat. Whispers from the owner get a 1:1 reply as a secondary path. Nothing else. |
| Exactly one player present | The group must hold **exactly one** real player (`GetRealPlayersInGroup().size() == 1`), and the alts must be that player's. Alts never chat to an empty room, so no tokens are spent with nobody reading. A second real player joining makes them go quiet until that player leaves. |
| Never blocks the server | No network or file I/O on a map or world thread. A slow or dead API only means quieter bots. |

Non-goals for v1: random bots with AI, personalities that change combat behaviour, guild chat, voice, a web editor.

## 2. Findings in the fork that shape the design

| Finding | Where | Consequence |
|---|---|---|
| `SayToParty` sends the packet **only to the real players** in the group. Other bots never "hear" it. | `src/Bot/PlayerbotAI.cpp:3510` | Bots can't react to each other's chat packets. Bot-to-bot dialogue has to be **scripted by one director**, which is actually what we want: no reply loops, and one API call for a whole exchange. |
| A player's party message is handed to every bot in the group once, in `OnPlayerCanUseChat(…, Group*)`. | `src/Script/Playerbots.cpp:281` | One clean place to record "the player said X", once per message, not once per bot. |
| Party text that isn't a command is parsed by each bot, then dropped. A whisper that isn't a command is dropped too. | `src/Bot/PlayerbotAI.cpp:835` (`HandleCommands`) | Command words (`follow`, `attack`…) must never reach the AI. Talk is whatever no bot parsed as a command. |
| An alt only listens to its master. | `src/Bot/PlayerbotAI.cpp:1476` | Strangers can't drive an alt's AI. |
| `IsAltBot()`, `GetRealPlayersInGroup()`, `IsRealPlayer()` exist. | `PlayerbotAI.cpp:5261`, `:5164` | The gates are one-liners. |
| Hooks already used: `OnPlayerJustDied`, `OnPlayerLootItem`, `OnPlayerUpdateZone`, `OnPlayerGiveXP`, `OnPlayerLogin`, `OnPlayerbotLogout`. | `src/Script/Playerbots.cpp` | Most "circumstance" events are already in reach. |
| Boss checks already exist (`IsDungeonBoss() \|\| isWorldBoss()`). | `src/Ai/Base/Value/AttackersValue.cpp:193` and others | Boss kills can be told apart from trash. |
| The per-bot key/value store is `varchar(255)` and expires. | `data/sql/playerbots/base/playerbots_random_bots.sql` | Not used. Files hold the personality and the memory. |
| No HTTP client or AI code in the module. | grep of `src/` | The provider hook is new. |

## 3. The personality file

### 3.1 Where

```
<worldserver dir>/personalities/          AiPlayerbot.Personality.Dir (one folder per realm: give each realm's conf its own)
  _template.md                            shipped from conf/personalities/_template.md.dist
  Thraxa.md                               written by people: the personality
  Thraxa.memory.md                        written by the server: long-term memory (people may edit it too)
  Brana.md
  Brana.memory.md
```

- The file name is the **character name**, matched case-insensitively. Accented names (é, ö…) are matched in UTF-8.
  Anything with a path character is rejected.
- **The file existing is the opt-in.** Delete it and the alt goes back to normal at its next login (or after a
  `reload`).
- The file is read when the bot logs in, on a worker thread, and cached. In-game `personality reload` (owner) or
  `.playerbots personality reload` (GM) re-reads it. Nothing watches the folder.
- Limit: `Personality.MaxFileBytes` (4096). Anything past it is cut, and the log says so once.

### 3.2 Format

Plain Markdown, with a few headings the server knows. Every heading is optional, and text outside a heading counts as
"Who I am". The text goes to the model as it is; the headings only help the prompt and the in-game `personality ?`.

```markdown
# Who I am
Thraxa, orc Venomancer. Raised by a dwarf smith in Ironforge after a caravan raid, so she's
the only orc anyone knows with a Dun Morogh accent. Proud, blunt, secretly soft.

# How I talk
Short sentences. Calls everyone "lad", even women. Swears by "Muradin's beard".
Never uses big words. Complains about elves.

# The others
Brana: my little sister in all but blood. I tease her constantly, I'd die for her.
Kelvarr: don't trust him. He stole my cheese in Westfall.

# Likes and dislikes
Loves cheese, forges, a good fight. Hates boats, elves, waiting.

# Never
Never talks about the real world. Never flirts.
```

`# The others` is what makes bot-to-bot dialogue good: each alt knows how it feels about the others in the party.
`# Never` lists hard limits the server puts last in that character's block.

### 3.3 In-game commands (whispered to your own alt)

| Whisper | Does |
|---|---|
| `personality ?` | Says whether a file is loaded (name, size, the first line of "Who I am"), or "none". |
| `personality reload` | Re-reads this alt's file and memory file. |
| `personality memory` | Whispers its last 5 long-term memories. |
| `personality forget` | Empties the memory file (keeps a `.bak`). |
| `personality quiet` / `personality chatty` | Turns its idle banter off or on until logout. It still answers you. |

Only the **owning account** may use these. The account of the bot's character must equal the sender's account.

## 4. When alts talk

One **Scene** per group, owned by `PersonalityMgr`. The scene does three things:

- it collects what happens;
- it decides when an exchange is worth an API call;
- it hands each line to the alt that should say it.

### 4.1 Gates (all must hold)

1. `Personality.Enable = 1` and a provider is configured.
2. The group holds exactly one real player, that player is the master of the personality alts, and the player is
   on the same map as at least one of them.
3. At least one member is an alt bot with a loaded personality file. Bot-to-bot lines need two.
4. The group is **not in combat**. Exception: `Personality.CombatBarks = 1` lets in one short line at a pull or a boss
   death. It's off by default.
5. Budgets aren't spent (§7).

### 4.2 Triggers

| Trigger | Source | What happens |
|---|---|---|
| **The player talks in party** (not a command) | `OnPlayerCanUseChat(Group)` records it. It counts as talk once the first alt's `HandleCommands` drops it as unparsed. | Always triggers an exchange, after a 2–5 s delay. Usually 1 alt answers, sometimes a second one chimes in. A message that names an alt makes that alt answer. |
| **The owner whispers the alt** (not a command) | `HandleCommands` drop branch | A 1:1 whisper reply from that alt alone. |
| **Notable event** | §5 | With chance `Personality.EventChance` (35%) once out of combat, an exchange of 1–3 lines about it. Wipes and first-time boss kills always qualify. |
| **Idle downtime** | Scene tick | When the group is out of combat for `Personality.IdleBanterSec` (120 s) with no exchange, there's a `Personality.IdleChance` (25%) chance of some banter between alts about the scene, the zone or each other. |

### 4.3 One call per exchange

The model writes the whole exchange in one response, as JSON (structured outputs):

```json
{ "lines": [
  { "speaker": "Thraxa", "text": "Muradin's beard, lad, that thing hit like a cart." },
  { "speaker": "Brana",  "text": "You stood in the fire again." },
  { "speaker": "Thraxa", "text": "...the fire stood in me." }
] }
```

- `speaker` must be an alt in the roster. Unknown speakers are dropped. An empty `lines` array means "nobody would
  say anything", which is valid.
- Max `Personality.MaxLines` (3) lines.
- Each line is sanitized (§8), then queued for **that alt** with a due time 2–6 s after the previous line, scaled by
  length, so it reads like people typing.
- Each alt drains its own lines in its own `UpdateAIInternal`, next to the existing `chatReplies` loop
  (`PlayerbotAI.cpp:741`), and sends them with `SayToParty` (`SayToRaid` in a raid). If the alt left, its lines are
  skipped. If combat starts, pending lines are dropped, except the first one of an exchange the player triggered.
- The exchange is written back into the scene log (§5.1), so the next exchange knows what was just said, by bots and
  player alike.

This is cheaper and calmer than one call per bot, and two bots can never ping-pong with each other forever.

## 5. Memory of circumstances

Three layers: short-term in memory, long-term in a file, and the API's prompt cache to make both cheap.

### 5.1 Scene log (short-term, per group, in memory)

A ring buffer of the last `Personality.SceneEvents` (40) entries, shared by all alts in the group:

```
[21:04] entered The Deadmines (Normal)
[21:09] killed Rhahk'Zor (boss)
[21:09] Player Sqazz: "nice one"
[21:09] Brana: "Easy. Next."
[21:16] Thraxa died (killed by Sneed's Shredder)
[21:17] wipe at Sneed
[21:22] killed Sneed (boss), second try
[21:22] Brana looted [Buzzer Blade] (rare)
[21:31] Thraxa reached level 20
```

Event sources:

| Event | Hook | Already used in the module? |
|---|---|---|
| Death (whose, killed by whom) | `OnPlayerJustDied` | yes |
| Loot of quality ≥ `Personality.LootQuality` (rare) | `OnPlayerLootItem` | yes |
| Zone change | `OnPlayerUpdateZone` | yes |
| Entered/left a dungeon or raid | `OnPlayerMapChanged` | add. Verify the name on jealous-sound's core. |
| Boss kill (`IsDungeonBoss() \|\| isWorldBoss()`) | `OnPlayerCreatureKill` | add. Verify. |
| Wipe | derived: every group member dead | — |
| Level up | `OnPlayerLevelChanged` | add. Verify. |
| Quest completed | `OnPlayerCompleteQuest` | add. Verify. |
| Party chat (player and alts) | `OnPlayerCanUseChat(Group)` + the exchanges themselves | yes |

Hooks only **push a small struct under the scene mutex**. They never call the API and never touch another player.
One event seen by five group members is stored once, deduplicated by type + subject + second.

### 5.2 Memory file (long-term, per character, persisted)

`<Name>.memory.md` sits next to the personality file. The server writes it, and people can read and edit it:

```markdown
- 2026-10-29 Cleared the Deadmines with Brana and Sqazz. Died to Sneed's Shredder; Brana won't let me forget it.
- 2026-10-29 Brana got the Buzzer Blade. I wanted it.
- 2026-10-30 Hit level 20 in Westfall. Kelvarr still owes me cheese.
```

- **When it's written** (a "chapter end"): leaving a dungeon or raid, the group disbanding, the player logging out,
  or every `Personality.ChapterEvents` (25) scene entries.
- **How**: one summary call per chapter, with the scene log in and JSON out:
  `{"memories": {"Thraxa": ["..."], "Brana": ["..."]}}`. That's 0–2 bullets per alt, in that alt's own point of view,
  only what it would remember. The memories are appended to each file and written on a worker thread.
- **Cap**: `Personality.MemoryMaxBullets` (30). When over, one compaction call folds the 10 oldest bullets into 2–3.
  So the file stays small forever and old history fades the way real memory does.
- `personality forget` empties it. Editing it by hand works too: the next login or `reload` picks it up.
- Optional v2: in-memory "relationship lines" (Brana → Thraxa: "teases her about dying"). For v1 they live inside the
  bullets.

### 5.3 Prompt caching (what makes this affordable)

Every call is built as **stable prefix first, changing part last**:

```
system:  [frame]  [each alt's personality file]  [each alt's memory bullets]   <- cache_control breakpoint
user:    [scene log]  [roster cards: level, zone, health, mana, mood]  [the trigger]
```

- The prefix only changes when the roster changes or at a chapter end. Within a dungeon run it stays byte-identical,
  so every exchange after the first reads it from cache.
- The roster is **sorted by character guid** and files are cached as loaded. Never put a timestamp, a random value or
  an unordered map in the prefix; any of those silently breaks the cache.
- The minimum cacheable prefix on Claude Opus 5.5 and Claude Haiku 5.5 is 512 tokens. Two personality files plus the
  frame pass that. A prefix below it just doesn't cache, with no error.
- Use the default 5-minute TTL. Party chatter comes more often than every 5 minutes, and each read refreshes the
  timer. Cache reads on Opus 5.5 cost 5% of normal input.
- Check `usage.cache_read_input_tokens` in the debug log (§9). If it stays at 0 call after call, something in the
  prefix is changing.

## 6. The AI provider hook

### 6.1 Interface

```cpp
struct PersonalityRequest
{
    std::string system;            // stable prefix, cached
    std::string user;              // scene + trigger
    std::string jsonSchema;        // the shape of the answer (lines or memories)
    uint32 maxTokens;
};
struct PersonalityResult { bool ok; std::string json; std::string error; uint32 cachedTokens; };

class IPersonalityProvider
{
public:
    virtual ~IPersonalityProvider() = default;
    virtual PersonalityResult Complete(PersonalityRequest const& req) = 0;  // worker thread only
};
```

The providers are `none` (default) and **`anthropic` (phase 2, first)**, then `openai` (any compatible API) and
`ollama` (local) later, in that order. `openai` and `ollama` lack Anthropic's structured outputs and cache markers.
They get the schema in the prompt and a tolerant JSON parse.

### 6.2 Anthropic request (raw HTTP; there's no official C++ SDK)

`POST https://api.anthropic.com/v1/messages`, with these headers:

- `x-api-key`
- `content-type: application/json`
- `anthropic-version` (confirm the current value in the docs when you implement this)
- `anthropic-beta: server-side-fallback-2026-07-01`

Body:

```json
{
  "model": "claude-opus-5-5",
  "max_tokens": 4096,
  "output_config": {
    "effort": "low",
    "format": { "type": "json_schema", "schema": {
      "type": "object", "additionalProperties": false, "required": ["lines"],
      "properties": { "lines": { "type": "array", "maxItems": 3, "items": {
        "type": "object", "additionalProperties": false, "required": ["speaker", "text"],
        "properties": { "speaker": { "type": "string" }, "text": { "type": "string" } } } } } } }
  },
  "fallbacks": "default",
  "system": [ { "type": "text", "text": "<frame + files + memories>", "cache_control": { "type": "ephemeral" } } ],
  "messages": [ { "role": "user", "content": "<scene log + roster cards + trigger>" } ]
}
```

Rules from the current API:

- Don't send `temperature`/`top_p`/`top_k`. They return a 400 now.
- Don't send an assistant prefill.
- On Opus 5.5, thinking can't be disabled. Effort `low` keeps it short, and `max_tokens` has to cover the thinking
  too, which is why it's 4096 and not 300.
- Check `stop_reason` before reading anything. `refusal` or `max_tokens` means no lines, which is fine.
- The JSON is in the first `text` block. Skip `thinking` blocks.
- Verify that `fallbacks` and `output_config.format` work together on the first test call. If they don't, drop
  `fallbacks`.

**Model**: it's the admin's choice (`Personality.Model`). The default is `claude-opus-5-5`.
Rough cost per exchange, with about 1,500 cached prefix tokens, 500 fresh, and about 400 out including thinking:

| Model | Per exchange | Per hour at 30 exchanges |
|---|---|---|
| `claude-opus-5-5` | about $0.01 | about $0.30 |
| `claude-haiku-5-5` | about $0.0003 | under 1¢ |

These are estimates. Measure them with the debug log.

**API key**: `Personality.ApiKeyEnv = "ANTHROPIC_API_KEY"` (an environment variable) is the preferred way, so the key
stays out of conf files, backups and launcher bundles. `Personality.ApiKey` is the fallback. Never log the key.

**HTTP**: vendor [cpp-httplib](https://github.com/yhirose/cpp-httplib) (one header, MIT, GPL-2 compatible) with
`CPPHTTPLIB_OPENSSL_SUPPORT`, using the OpenSSL the server already links and ships on Windows. There's no new DLL.
**Verify** that the module's CMake target sees the OpenSSL headers.

### 6.3 The frame (start of the system prompt)

```
You write party chat for characters in World of Warcraft (Conquest of Azeroth). The party is playing
right now with one player, {player}; the characters below are that player's companions.
Each character has a description written by the player and memories of past sessions.
Write what these characters would say next, in character, given the scene. Rules:
- 0 to 3 lines total, each under 180 characters, plain text, no asterisk emotes, no lists.
- Only characters listed under CHARACTERS may speak. Not every character speaks every time; silence is fine.
- They are people in the world, not assistants. They don't offer help, don't explain game mechanics,
  don't mention being bots or AI, and never give orders that sound like commands.
- {player} is the player's own character, travelling with them. Call {player} by name when it fits (not in
  every line), answer {player} directly when {player} spoke, and talk about {player} like a companion would.
- React to events naturally, briefly.
- Write in {language}.
PLAYER
{player}: level {level} {race} {coaClass} ({coaSpec})
CHARACTERS
## Thraxa (level 20 orc Venomancer, Tank)  <file contents>  MEMORIES <bullets>
## Brana  (level 19 dwarf Runemaster, Healer) <file contents> MEMORIES <bullets>
```

- `{language}` comes from `AiPlayerbot.BotTextLocale` (-1 = the master's client locale).
- Use the **CoA class and spec names** (the same source as `talents spec list`), never the base WotLK class.
- `{player}` is the player character's name (not the account name). It sits in the cached prefix because it doesn't
  change during a session.
- Level and spec live in the prefix, so a level-up changes it. That's rare enough. Health, mana and zone change
  often, so they go in the roster cards in the user turn.

## 7. Budgets

| Setting | Default | Purpose |
|---|---|---|
| `Personality.ExchangeCooldownSec` | 20 | minimum gap between exchanges per group (the player talking skips half of it) |
| `Personality.MaxExchangesPerHour` | 40 | per group |
| `Personality.DailyCallsPerAccount` | 400 | all calls (exchanges, summaries, whispers) per owner account, reset at the daily reset |
| `Personality.Workers` | 1 | worker threads |
| `Personality.QueueSize` | 32 | jobs waiting, global. When full, triggers are skipped, except the player talking. |
| `Personality.TimeoutMs` | 15000 | per request |

When an account hits its daily limit, its alts go quiet. The first alt to notice says one canned line ("I'm talked
out for today, lad."), once.

## 8. Reply sanitizing

- Strip `|` (WoW escape codes: `|c` colors, `|H` links). Otherwise the model could write a fake item or quest link.
- Fold newlines into spaces, trim, and drop surrounding quotes and a leading "Name:".
- Cut at 255 bytes, at a word boundary.
- Drop a line that a bot would parse as a command if a player typed it (`follow`, `attack`…). Run it through the same
  command check as player input.
- An empty line after this is dropped.

## 9. Threading and shutdown

```
map threads                         PersonalityMgr                         worker
───────────                         ──────────────                         ──────
hooks ── push event ──────────────▶ Scene (mutex)
each alt, on its own update:        tick: gates, budgets, decide
  publishes its own roster card ──▶   build request from the Scene copy ─▶ HTTPS ─▶ API
  drains its own due lines  ◀──────── per-alt line queues  ◀─────────────── parse
file loads, memory writes ────────────────────────────────────────────────▶ worker
```

- **Nobody reads another bot's `Player*`.** Each alt writes its own roster card (level, zone, health, mana, in
  combat) into the scene on its own update. The tick and the workers work only from that copy. Group members can be on
  different maps, so this is the only safe way.
- The scene tick runs from `WorldScript::OnUpdate` every 1 s. It only reads the scene and queues jobs.
- Debug: `Logger.playerbots.personality` logs each call's token usage (`input`, `cache_read`, `output`) and the lines
  it produced. It's off by default, in the same style as `CoaBots.log`.
- Shutdown: set a stop flag, wake the workers, join them within the request timeout, and flush pending memory writes.
  The server stop that 2.4 fixed must stay fast.

## 10. Config block (`conf/playerbots.conf.dist`)

```
###################################################################################################
# BOT PERSONALITIES (player alt bots only; opt-in: a file <Dir>/<CharacterName>.md)
#   Alts with a file talk in party chat while a real player is in the party, and remember sessions
#   in <CharacterName>.memory.md. Party chat of the group is sent to the AI provider.
AiPlayerbot.Personality.Enable = 0
AiPlayerbot.Personality.Dir = "personalities"
AiPlayerbot.Personality.Provider = "none"            # none | anthropic | openai | ollama
AiPlayerbot.Personality.BaseUrl = ""                 # empty = the provider's default
AiPlayerbot.Personality.Model = "claude-opus-5-5"    # e.g. claude-haiku-5-5
AiPlayerbot.Personality.ApiKeyEnv = "ANTHROPIC_API_KEY"
AiPlayerbot.Personality.ApiKey = ""
AiPlayerbot.Personality.MaxFileBytes = 4096
AiPlayerbot.Personality.MaxLines = 3
AiPlayerbot.Personality.EventChance = 35
AiPlayerbot.Personality.IdleBanterSec = 120
AiPlayerbot.Personality.IdleChance = 25
AiPlayerbot.Personality.CombatBarks = 0
AiPlayerbot.Personality.LootQuality = 3              # 3 rare, 4 epic
AiPlayerbot.Personality.SceneEvents = 40
AiPlayerbot.Personality.ChapterEvents = 25
AiPlayerbot.Personality.MemoryMaxBullets = 30
AiPlayerbot.Personality.ExchangeCooldownSec = 20
AiPlayerbot.Personality.MaxExchangesPerHour = 40
AiPlayerbot.Personality.DailyCallsPerAccount = 400
AiPlayerbot.Personality.Workers = 1
AiPlayerbot.Personality.QueueSize = 32
AiPlayerbot.Personality.TimeoutMs = 15000
```

Add the keys to `conf/playerbots.conf.settings.json` too if the launcher builds its settings UI from that file (check
this).

## 11. Files

New:

- `src/Mgr/Personality/PersonalityMgr.{h,cpp}`: file cache, scenes, gates, budgets, tick, line queues
- `src/Mgr/Personality/PersonalityPrompt.{h,cpp}`: prefix and user-turn builders, JSON schemas, sanitizer
- `src/Mgr/Personality/PersonalityMemory.{h,cpp}`: memory file read, append, compaction (worker thread)
- `src/Mgr/Personality/PersonalityProviders.{h,cpp}`: `none`, `anthropic`; later `openai`, `ollama`
- `src/Mgr/Personality/third_party/httplib.h`: vendored
- `src/Ai/Base/Actions/PersonalityAction.{h,cpp}`: `personality ?|reload|memory|forget|quiet|chatty`
- `conf/personalities/_template.md.dist`
- `changelog.d/2026xxxx-bot-personalities.md`
- `tests/personality_prompt.cpp`: the prefix is byte-stable across calls, roster order, sanitizer, schema parse,
  file-name matching

Seams in existing files (keep each one small so upstream merges stay easy):

1. `PlayerbotAI.cpp` `HandleCommands`: report unparsed party and whisper text to `PersonalityMgr`.
2. `PlayerbotAI.cpp` `UpdateAIInternal`: publish this alt's roster card and drain its due lines.
3. `Playerbots.cpp`: party-chat record in `OnPlayerCanUseChat(Group)`; events in the existing death, loot and zone
   hooks; new kill, map, level and quest hooks; file load at login; drop at logout and group leave; tick in
   `OnUpdate`; stop in `OnShutdown`.
4. `ChatTriggerContext.h` / `ChatActionContext.h`: register `personality`.
5. `PlayerbotAIConfig.{h,cpp}`: settings.

## 12. Conquest of Azeroth considerations

- **No core fork.** All the hooks listed are stock AzerothCore script hooks. Check that the four new ones exist with
  those names on jealous-sound's `main` before writing code.
- **Windows-first.** It uses header-only HTTP on OpenSSL that's already shipped, and files next to `worldserver.exe`.
  Players on a self-hosted realm can edit a `.md` in Notepad.
- **Several realms per install.** The launcher runs CoA, Wildcard and Reborn realms side by side. Give each realm's
  `playerbots.conf` its own `Personality.Dir`, or same-named characters on two realms share a file.
- **Custom classes.** Prompts name the CoA class, spec and role (tank, healer, DPS).
- **Locale.** CoA is commonly run in French. `BotTextLocale` sets the language of the lines.
- **Dungeon Clear and missions.** A Dungeon Clear run is a natural stream of events (boss after boss). An alt on a
  gathering mission has left the group, so it has no scene and stays silent.
- **Upstream churn.** Zyth45's `coa` branch merges almost daily. Keep everything in `src/Mgr/Personality/` and the
  seams to a few lines each.
- **Privacy.** With the feature on, party chat of groups with personality alts goes to the AI provider. Say so in the
  conf comment (done above) and tell your players.
- **Editing UI (phase 5).** The SquidBots Lite addon only sends chat commands a player could type, so a panel there
  fits the way this fork already works. See phase 5.

## 13. Phases (what's next)

1. **Build and run your fork as it is** on your CoA server. Check that an alt answers `co ?` and that you can group
   two alts. Everything else depends on this.
2. **Phase 1, files and scenes, no AI**: file loading, `personality ?|reload`, the scene log from the hooks, and
   roster cards. Add a debug command `.playerbots personality scene` that prints a group's scene log. You can test
   all of it in a dungeon with no API key.
3. **Phase 2, the Anthropic hook**: config, worker, provider, prompt builder, exchanges in party chat, budgets,
   sanitizer, the token-usage log. Check that `cache_read_input_tokens` climbs after the first exchange of a run.
4. **Phase 3, long-term memory**: chapter summaries into `.memory.md`, compaction, `personality memory|forget`.
5. **Phase 4, polish**: owner whispers, combat barks, `quiet`/`chatty`, `openai`/`ollama` providers.
6. **Phase 5, module-friendly editing UI** (once phases 1–4 work in play): edit a character's personality and read
   or prune its memories from inside the game. That means new `personality set/add` commands that write the file
   on a worker thread (owner only, size-capped, with a `.bak`), plus a panel in the SquidBots Lite addon that sends
   them. A personality is longer than one chat line, so the addon sends the text in numbered chunks. Then the
   feature can be offered to other servers.

## 14. Decided

- **One real player per party** for v1. Several players sharing a scene is deferred. The scene design doesn't rule it
  out, but the budgets, the prompt frame and the ownership checks assume one player.
- **The alts address the player's character by name** and know its race, class and spec.
- **Files are edited by hand on this server** until phase 5.
