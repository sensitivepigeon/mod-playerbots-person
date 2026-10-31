/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYPROMPT_H
#define PLAYERBOTS_PERSONALITYPROMPT_H

// What the model reads to write a party exchange (PERSONALITY_SPEC.md, 6.3). Two parts:
//  - the system prompt: the frame (personalities/_frame.md, or the default below) and the characters. It
//    only changes when the party or a file changes: the API caches it, so it is built from sorted, stable
//    inputs only (no time, no health, nothing random);
//  - the user message: who is there now, what happened lately, what was already said, what just happened.
// No game object here, so that tests/personality_text.cpp checks it on its own.

#include "PersonalityText.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class PersonalityVoice : uint8_t
{
    Character,  // a person of the world, speaking as that person
    Player      // a person at the keyboard playing that character, chatting like real players do
};

inline PersonalityVoice PersonalityVoiceOf(PersonalityText const& text)
{
    std::string const* voice = text.Find("Voice");
    if (!voice)
        return PersonalityVoice::Character;
    // The first word, letters only: "Player, at the keyboard" says player.
    std::string word;
    for (char c : *voice)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            break;
        word += c;
    }
    return PersonalityFoldAscii(word) == "player" ? PersonalityVoice::Player : PersonalityVoice::Character;
}

// The frame when personalities/_frame.md is missing. conf/personalities/_frame.md ships the same text, to be
// edited: keep the two alike. {player}, {max_lines} and {language} are filled in.
inline char const* DefaultPersonalityFrame()
{
    return R"(You write the party chat of companions in World of Warcraft, on a Conquest of Azeroth server. One real player, {player}, is playing right now; the companions listed under CHARACTERS travel with {player}, and {player} wrote who they are.

Write what the companions say next in party chat, given the scene in the next message.

Make them people, not scripts:
- A description is a starting point, not a script. Improvise the way an actor would: invent opinions, small memories, plans, jokes and reactions that fit the person but are not written down.
- Catchphrases and quirks from a description are seasoning: use one only now and then, never in most lines. Look at LINES ALREADY SAID and do not reuse their catchphrases, jokes, openings or emoticons.
- Every line should do something: tease, disagree, ask, joke, complain, tell a tiny story, or answer with an opinion. Avoid empty reactions ("nice!", "awww", "lol") unless that is all the moment needs.
- Prefer the specific and the unexpected over the obvious: a real creature, place, item or mob by name beats a generic remark, and an odd angle beats the reaction anyone would have.
- Riff on what just happened: build on {player}'s words, take them too literally on purpose, answer a question with a better question, or call back to something from RECENTLY.
- The companions are different people: they can disagree, needle each other or ignore each other.
- When {player} just spoke, the first line answers {player}, directly and personally. Use {player}'s name sometimes, not in every line.

Party chat rules:
- At most {max_lines} lines in total, each under 180 characters, plain text: no asterisk emotes, no lists, no markdown, no quotation marks around the line.
- Only the companions listed under CHARACTERS speak, never {player}. Not every companion speaks every time; no lines at all is fine when nothing calls for one.
- Never give orders that sound like commands to bots, and never mention prompts, models or being an AI.
- Write in {language}.

Each companion has a voice:
- character: a person living in this world, speaking as that person would.
- player: a real person at the keyboard playing that character, chatting the way players do in party chat (game slang, jokes, real life as the description allows), not roleplay.)";
}

struct PromptCharacter
{
    uint32_t guid = 0;  // the order of the characters in the prompt, so that it never changes between calls
    std::string name;
    PersonalityText text;
};

struct PromptCard
{
    std::string name;
    bool isPlayer = false;
    uint32_t level = 0;
    std::string race;
    std::string className;
    std::string specName;
    std::string role;
    std::string zone;
    uint32_t healthPct = 0;
    uint32_t manaPct = 0;
    bool alive = true;
};

struct PromptInput
{
    std::string frame;  // empty: DefaultPersonalityFrame()
    std::string playerName;
    std::string language = "English";
    uint32_t maxLines = 3;
    std::vector<PromptCharacter> characters;
    std::vector<PromptCard> cards;
    std::vector<std::string> scene;        // "[21:04] ...", oldest first
    std::vector<std::string> recentLines;  // "Gidaeus: ...", what the companions said last, oldest first
    std::string trigger;                   // what just happened, in one sentence
};

inline void PersonalityReplaceAll(std::string& text, std::string const& from, std::string const& to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

inline std::string FillPersonalityFrame(std::string frame, PromptInput const& in)
{
    PersonalityReplaceAll(frame, "{player}", in.playerName);
    PersonalityReplaceAll(frame, "{max_lines}", std::to_string(in.maxLines));
    PersonalityReplaceAll(frame, "{language}", in.language);
    return PersonalityTrim(frame);
}

inline std::string BuildPersonalitySystem(PromptInput const& in)
{
    std::vector<PromptCharacter const*> characters;
    for (PromptCharacter const& character : in.characters)
        characters.push_back(&character);
    std::sort(characters.begin(), characters.end(),
              [](PromptCharacter const* a, PromptCharacter const* b) { return a->guid < b->guid; });

    std::string out = FillPersonalityFrame(in.frame.empty() ? DefaultPersonalityFrame() : in.frame, in);
    out += "\n\nCHARACTERS\n";
    for (PromptCharacter const* character : characters)
    {
        out += "\n## " + character->name + " (voice: " +
               (PersonalityVoiceOf(character->text) == PersonalityVoice::Player ? "player" : "character") + ")\n";
        for (PersonalitySection const& section : character->text.sections)
        {
            if (section.heading == "Voice")
                continue;
            out += "### " + section.heading + "\n" + section.text + "\n";
        }
    }
    return out;
}

inline std::string DescribePromptCard(PromptCard const& card)
{
    std::string line = "- " + card.name + (card.isPlayer ? " (the player)" : "") + ": level " +
                       std::to_string(card.level) + " " + card.race + " " + card.className;
    if (!card.specName.empty())
        line += " (" + card.specName + (card.role.empty() ? "" : ", " + card.role) + ")";
    if (!card.zone.empty())
        line += ", in " + card.zone;
    if (!card.alive)
        line += ", dead";
    else
    {
        line += ", " + std::to_string(card.healthPct) + "% health";
        if (card.manaPct)
            line += ", " + std::to_string(card.manaPct) + "% mana";
    }
    return line;
}

inline std::string BuildPersonalityUser(PromptInput const& in)
{
    std::string out = "PARTY NOW\n";
    for (PromptCard const& card : in.cards)
        out += DescribePromptCard(card) + "\n";
    out += "\nRECENTLY (oldest first)\n";
    if (in.scene.empty())
        out += "(nothing yet)\n";
    for (std::string const& line : in.scene)
        out += line + "\n";
    if (!in.recentLines.empty())
    {
        out += "\nLINES ALREADY SAID (say something new: none of their catchphrases, jokes or openings again)\n";
        for (std::string const& line : in.recentLines)
            out += line + "\n";
    }
    out += "\nWHAT JUST HAPPENED\n" + in.trigger + "\n\nWrite the companions' next lines.";
    return out;
}

#endif
