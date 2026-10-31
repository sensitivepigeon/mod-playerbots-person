---
area: bots
type: added
audience: admins
title: Bot personalities: long-term memory, whisper chats, and every alt of the party can speak
---
Alts with a personality now remember: at the end of a chapter (a dungeon left, the party split, a logout, or every `ChapterEvents` scene entries) one call with `SummaryModel` (Claude Haiku 5.5 by default) writes 0 to 2 first-person memories per alt to `personalities/<CharacterName>.memory.md`, which the alts bring up later and the player can read and edit; past `MemoryMaxBullets` the oldest are folded into a few. Whisper an alt something that is not a command and it answers in private, in character, remembering the conversation (`personalities/_whisper.md` sets the tone); the conversation becomes memories of that alt only. Phrases an alt keeps repeating are found and listed to it as worn out. An exchange now has room for every alt of the party, the player talking is always answered past `MaxExchangesPerHour`, and `.playerbots personality scene` shows the hourly budget. New whispers: `personality memory`, `personality forget`.
