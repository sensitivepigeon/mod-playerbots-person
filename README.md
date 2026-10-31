> [!IMPORTANT]
> **Branch `feat/personality-2.8.3`: bot personalities.** This branch is CoA Bots 2.8.3 (tag `v2.8.3`) plus three
> commits that give your own alts a personality. They talk in party chat, chat with you by whisper and remember past
> sessions, with lines written by an AI model through the Anthropic API. It is off by default, and nothing changes for
> random bots. Everything after this section is the CoA fork as it ships. The design and its status are in
> [PERSONALITY_SPEC.md](PERSONALITY_SPEC.md). The same work on CoA Bots 2.8.2 is on branch `feat/personality-phase1`.

## Bot personalities (this branch)

### What it does

- **Opt-in per character.** An alt has a personality when a file `personalities/<CharacterName>.md` exists next to the
  world server. Every heading is optional: who I am, how I talk, the others, likes and dislikes, never, voice. Start
  from [conf/personalities/_template.md](conf/personalities/_template.md). `# Voice` is `character` (a person of the
  world) or `player` (someone at the keyboard, chatting like players do).
- **Who it applies to.** Only player alt bots (your own characters played as bots), and only while exactly one real
  player is in the party.
- **Party chat.**
  - The alts answer what you say in party.
  - They react to wipes, bosses, deaths, good loot and levels.
  - They banter in quiet moments.
  - One API call writes a whole exchange, and each alt says its own lines a few seconds apart.
  - The model is told to improvise instead of reciting the file, and the words an alt keeps repeating are flagged to it
    as worn out.
- **Whisper chats.** Whisper one of your alts something that is not a command, and it answers in private, in
  character, and follows the conversation.
- **Memory.** At the end of a chapter (a dungeon left, the party split, a logout), a cheaper model writes a few
  first-person memories per alt to `personalities/<CharacterName>.memory.md`. The alts bring them up later. You can read,
  edit or prune that file. A whispered conversation becomes memories of that alt only.
- **The frame of the prompt is editable.** It lives in `personalities/_frame.md` (party chat) and
  `personalities/_whisper.md` (whispers). Changes need no rebuild: `.playerbots personality reload` reads them again.

### Build

- **No new dependency.** HTTPS and JSON go through Boost.Beast, Boost.JSON (header-only) and the OpenSSL the core
  already links. On Windows, OpenSSL is given the Windows trusted root certificates.
- **Core:** [Zyth45/azerothcore-wotlk-coa](https://github.com/Zyth45/azerothcore-wotlk-coa), branch `coa-bots-2.8.3`
  (`0e8d72a0`).
- **Modules:** put this branch in `modules/mod-playerbots`. To match the bot server of the SquidBots repack, also add
  [mod-dungeon-clear](https://github.com/Zyth45/mod-dungeon-clear) at `9a9d09c`. Build `worldserver` as usual.
- **On the SquidBots repack:** the bot server is `CoA-Bots\Core\worldserver.exe`.
  - Keep the official one, for example as `worldserver.exe.orig-2.8.3`, and only swap while the server is stopped.
  - Put the official one back before a launcher update.
  - Rebuild on the new release's commits before swapping again: never run a build of one release against the database
    of another.

### Turn it on

1. Put the API key in the environment of the world server's process, so that it never sits in a conf file:
   `ANTHROPIC_API_KEY` (Windows: a user environment variable, then restart the launcher). The key is never logged.
2. In `playerbots.conf` (every setting is read again on `.reload config`):

   ```
   AiPlayerbot.Personality.Enable = 1
   AiPlayerbot.Personality.Provider = "anthropic"
   AiPlayerbot.Personality.Model = "claude-sonnet-5-5"       # or claude-haiku-5-5, far cheaper, flatter lines
   AiPlayerbot.Personality.SummaryModel = "claude-haiku-5-5" # writes the memories
   ```

   The rest (budgets, chattiness, memory) is described in [conf/playerbots.conf.dist](conf/playerbots.conf.dist), under
   `AiPlayerbot.Personality`.
3. Copy `conf/personalities/_frame.md` and `_whisper.md` into the `personalities` folder, add a file per alt, and group
   up.

### Commands

| Command | Who | Does |
|---|---|---|
| `/w <alt> personality ?` | the owner | the file, its voice, how many memories |
| `/w <alt> personality reload` | the owner | reads its file and memories again |
| `/w <alt> personality memory` / `forget` | the owner | its last memories / sets the memory file aside (`.bak`) |
| `.playerbots personality scene [player]` | GM | the party's scene: who, what happened, what is waiting, the hourly budget |
| `.playerbots personality stats` | GM | calls, tokens (and what the cache saved), memories, last error |
| `.playerbots personality reload` | GM | every file and both frames again |

### Cost and privacy

- **Cost.** Measured with one alt on Claude Sonnet 5.5 at effort high, an exchange costs about 0.7 cent, or around $0.40
  per hour of steady chat.
  - Claude Haiku 5.5 costs about a twentieth of that.
  - Budgets bound it: `ExchangeCooldownSec`, `MaxExchangesPerHour` (what the alts start themselves; you talking is
    always answered), `DailyCallsPerAccount` and `MaxInFlight`.
  - At the default 400 calls per account and day, that is at most about $2.80 a day.
- **Privacy.** With the feature on, the party chat of these groups and the whispers to these alts are sent to the AI
  provider. Tell your players.

The code is in [src/Mgr/Personality](src/Mgr/Personality). [tests/personality_text.cpp](tests/personality_text.cpp) and
[tests/personality_provider.cpp](tests/personality_provider.cpp) build on their own, without the core.

---

> [!IMPORTANT]
> **This is the Conquest of Azeroth fork of mod-playerbots.** Build instructions, recommended settings and the current
> status of the CoA bots are in **[README_COA.md](README_COA.md)**.

> [!NOTE]
> **Development continues on the CoA Modding Community Discord: https://discord.gg/XTn28skRN** (news, new versions and
> support for the CoA bots).

<p align="center">
    <a href="https://github.com/mod-playerbots/mod-playerbots/blob/master/README.md">English</a>
    |
    <a href="https://github.com/mod-playerbots/mod-playerbots/blob/master/README_CN.md">中文</a>
    |
    <a href="https://github.com/mod-playerbots/mod-playerbots/blob/master/README_ES.md">Español</a>
</p>


<div align="center">
  <img src="banner.png" alt="Playerbots Banner" width="700px">
</div>

<div align="center">
    <img src="https://github.com/mod-playerbots/mod-playerbots/actions/workflows/macos_build.yml/badge.svg">
    <img src="https://github.com/mod-playerbots/mod-playerbots/actions/workflows/core_build.yml/badge.svg">
    <img src="https://github.com/mod-playerbots/mod-playerbots/actions/workflows/windows_build.yml/badge.svg">
</div>

# Playerbots Module
`mod-playerbots` is an [AzerothCore](https://www.azerothcore.org/) module that adds player-like bots to a server. The project is based off [IKE3's Playerbots](https://github.com/ike3/mangosbot).

Features include:

- The ability to log in alt characters as bots, allowing players to interact with their other characters, form parties, level up, and more
- Random bots that wander through the world, complete quests, and otherwise behave like players, simulating the MMO experience
- Bots capable of running most raids and battlegrounds
- Highly configurable settings to define how bots behave
- Excellent performance, even when running thousands of bots

We also have a **[Discord server](https://discord.gg/NQm5QShwf9)** where you can discuss the project, ask questions, and get involved in the community!

## Installation

Supported platforms are Ubuntu, Windows, and macOS. Other Linux distributions may work, but may not receive support.

> **Important:** All `mod-playerbots` installations require a custom fork of AzerothCore: [mod-playerbots/azerothcore-wotlk (Playerbot branch)](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot). The standard AzerothCore repository will **not** work.

### Quick Start

```bash
git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot
cd azerothcore-wotlk/modules
git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master
```

Then build the server following the platform-specific instructions in our **[Installation Guide](https://github.com/mod-playerbots/mod-playerbots/wiki/Installation-Guide)**.

> **Testing branch:** A `test-staging` branch is available with the latest features and fixes before they are merged into `master`. To use it, clone with `--branch=test-staging` instead. Note that this branch may contain unstable or breaking changes — use it at your own risk and only if you are comfortable troubleshooting issues.

### Detailed Guides

| Guide | Description |
|---|---|
| **[Installation Guide](https://github.com/mod-playerbots/mod-playerbots/wiki/Installation-Guide)** | Full step-by-step instructions for clean installs, migrating from existing AzerothCore, Docker setup, adding modules, and updating |
| **[Troubleshooting](https://github.com/mod-playerbots/mod-playerbots/wiki/Troubleshooting)** | Solutions to the most common build errors, database issues, configuration mistakes, crashes, and platform-specific problems |

For additional references, see the [AzerothCore Installation Guide](https://www.azerothcore.org/wiki/installation) and [Installing a Module](https://www.azerothcore.org/wiki/installing-a-module) pages.

## Documentation

The [Playerbots Wiki](https://github.com/mod-playerbots/mod-playerbots/wiki) contains an extensive overview of AddOns, commands, raids with programmed bot strategies, and recommended performance configurations. Please note that documentation may be incomplete or out-of-date in some sections, and contributions are welcome.

Bots are controlled via chat commands. For larger bot groups, this can be cumbersome. Because of this, community members have developed client AddOns to allow controlling bots through the in-game UI. We recommend you check out their projects listed in the [AddOns and Submodules](https://github.com/mod-playerbots/mod-playerbots/wiki/Playerbot-Addons-and-Sub%E2%80%90Modules) page.

## Contributing

This project is still under development. We encourage anyone to make contributions, anything from pull requests to reporting issues. If you encounter any errors or experience crashes, we encourage you [report them as GitHub issues](https://github.com/mod-playerbots/mod-playerbots/issues/new?template=bug_report.md). Your valuable feedback will help us improve this project collaboratively.

If you make coding contributions, `mod-playerbots` complies with the [C++ Code Standards](https://www.azerothcore.org/wiki/cpp-code-standards) established by AzerothCore. Each Pull Request must include all test scenarios the author performed, along with their results, to demonstrate that the changes were properly verified.

We recommend joining the [Discord server](https://discord.gg/NQm5QShwf9) to make your contributions to the project easier, as a lot of active support is carried out through this server.

Please click on the "⭐" button to stay up to date and help us gain more visibility on GitHub!

## Acknowledgements

`mod-playerbots` is based on [ZhengPeiRu21/mod-playerbots](https://github.com/ZhengPeiRu21/mod-playerbots) and [celguar/mangosbot-bots](https://github.com/celguar/mangosbot-bots). We extend our gratitude to [@ZhengPeiRu21](https://github.com/ZhengPeiRu21) and [@celguar](https://github.com/celguar) for their continued efforts in maintaining the module.

Also, a thank you to the many contributors who've helped build this project:

<a href="https://github.com/mod-playerbots/mod-playerbots/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=mod-playerbots/mod-playerbots" />
</a>
