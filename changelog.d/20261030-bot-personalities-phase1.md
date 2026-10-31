---
area: bots
type: added
audience: admins
title: Bot personalities, phase 1: personality files and party scenes for your alts
---
Off by default (`AiPlayerbot.Personality.Enable = 0`). An alt (your own character played as a bot) gets a personality when a file `personalities/<CharacterName>.md` exists next to the world server; `conf/personalities/_template.md` shows the layout. While you group with such alts, the server keeps a scene of your party: who is there, deaths, wipes, bosses and rares killed, good loot, levels, quests, places entered and left, and what you say in party chat. `.playerbots personality scene [player]` shows it, `.playerbots personality reload` reads the files again, and `personality ?` or `personality reload` whispered to your alt checks its own file. The alts do not talk yet: that is phase 2.
