---
area: bots
type: added
audience: admins
title: Bot personalities, phase 2: your alts talk in party chat
---
With `AiPlayerbot.Personality.Provider = "anthropic"` and an API key (preferably in the `ANTHROPIC_API_KEY` environment variable of the world server), alts with a personality file answer you in party chat, react to wipes, bosses, deaths, good loot and levels, and banter with each other in quiet moments. One call writes a whole exchange of up to `MaxLines` lines; the system prompt is cached between calls. Budgets per party, per hour and per account keep the cost bounded, and `.playerbots personality stats` shows the calls and the tokens. A `# Voice` heading in the file chooses between an in-world `character` and a `player` at the keyboard. The party chat of these groups is sent to the AI provider.
