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
- Memories are things that really happened with these people: bring one up now and then when it fits, the way old friends do, never as a list.
- WORN OUT lists what a companion has said too often lately: that companion avoids those words for now and finds new ones.
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
    std::vector<std::string> memories;  // "2026-10-31 Named the quest egg Gerald.", oldest first (<Name>.memory.md)
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
    // Per companion, the words it used too often lately (WornPersonalityPhrases): it avoids them for now.
    std::vector<std::pair<std::string, std::vector<std::string>>> worn;
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

// One character as the model reads it: its file without the Voice setting, then its memories.
inline void AppendPromptCharacter(std::string& out, PromptCharacter const& character)
{
    out += "\n## " + character.name + " (voice: " +
           (PersonalityVoiceOf(character.text) == PersonalityVoice::Player ? "player" : "character") + ")\n";
    for (PersonalitySection const& section : character.text.sections)
    {
        if (section.heading == "Voice")
            continue;
        out += "### " + section.heading + "\n" + section.text + "\n";
    }
    if (!character.memories.empty())
    {
        out += "### Memories (oldest first)\n";
        for (std::string const& memory : character.memories)
            out += "- " + memory + "\n";
    }
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
        AppendPromptCharacter(out, *character);
    return out;
}

// The words and phrases of one companion that came back in at least two of its last lines: a catchphrase
// the model leans on, found without anyone listing it. Phrases of two or three words that are not only
// filler, and single words of five letters or more; a phrase hides the words it contains. Lowercase.
inline std::vector<std::string> WornPersonalityPhrases(std::vector<std::string> const& lines, size_t max = 5)
{
    static char const* const fillerList[] = {
        "a", "an", "and", "are", "as", "at", "be", "but", "by", "can", "did", "do", "for", "from", "got", "had", "has",
        "have", "he", "her", "his", "i", "if", "im", "in", "is", "it", "its", "just", "me", "my", "no", "not", "now",
        "of", "on", "or", "our", "out", "so", "that", "the", "their", "them", "then", "they", "this", "to", "too",
        "up", "us", "was", "we", "were", "what", "when", "who", "will", "with", "you", "your", "yes", "dont", "thats",
        "about", "there", "would", "could", "should", "really", "still", "again", "going", "right", "think"};
    auto filler = [](std::string const& word)
    {
        for (char const* f : fillerList)
            if (word == f)
                return true;
        return false;
    };

    std::vector<std::pair<std::string, size_t>> counts;  // phrase, lines it is in
    for (std::string const& line : lines)
    {
        std::vector<std::string> words;
        std::string word;
        for (char c : line + " ")
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c & 0x80))
                word += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
            else if (c == '\'' && !word.empty())
                continue;  // "don't" counts as "dont"
            else if (!word.empty())
            {
                words.push_back(word);
                word.clear();
            }
        }

        std::vector<std::string> seen;  // once per line
        for (size_t n = 1; n <= 3; ++n)
            for (size_t i = 0; i + n <= words.size(); ++i)
            {
                bool allFiller = true;
                std::string phrase;
                for (size_t k = i; k < i + n; ++k)
                {
                    allFiller &= filler(words[k]);
                    phrase += (k > i ? " " : "") + words[k];
                }
                if (allFiller || (n == 1 && phrase.size() < 5) || (n > 1 && (filler(words[i + n - 1]))))
                    continue;
                if (std::find(seen.begin(), seen.end(), phrase) == seen.end())
                    seen.push_back(phrase);
            }
        for (std::string const& phrase : seen)
        {
            auto it = std::find_if(counts.begin(), counts.end(), [&](auto const& c) { return c.first == phrase; });
            if (it == counts.end())
                counts.emplace_back(phrase, 1);
            else
                ++it->second;
        }
    }

    std::vector<std::string> worn;
    for (auto const& [phrase, times] : counts)
        if (times >= 2)
            worn.push_back(phrase);
    // The longest first, and a word or phrase inside a worn one is not listed again.
    std::stable_sort(worn.begin(), worn.end(), [](std::string const& a, std::string const& b) { return a.size() > b.size(); });
    std::vector<std::string> kept;
    for (std::string const& phrase : worn)
    {
        bool inside = false;
        for (std::string const& longer : kept)
            if ((" " + longer + " ").find(" " + phrase + " ") != std::string::npos)
                inside = true;
        if (!inside && kept.size() < max)
            kept.push_back(phrase);
    }
    return kept;
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
    bool anyWorn = false;
    for (auto const& [name, phrases] : in.worn)
        if (!phrases.empty())
        {
            if (!anyWorn)
                out += "\nWORN OUT (used too much lately: this companion avoids them for now)\n";
            anyWorn = true;
            out += "- " + name + ":";
            for (size_t i = 0; i < phrases.size(); ++i)
                out += (i ? ", \"" : " \"") + phrases[i] + "\"";
            out += "\n";
        }
    out += "\nWHAT JUST HAPPENED\n" + in.trigger + "\n\nWrite the companions' next lines.";
    return out;
}

// Whisper chats: the player whispers one alt, and that alt answers in private. The frame when
// personalities/_whisper.md is missing; {name}, {player} and {language} are filled in.
inline char const* DefaultWhisperFrame()
{
    return R"(You write the private whispers of {name}, one of {player}'s companions in World of Warcraft, on a Conquest of Azeroth server. {player} is whispering {name} directly: nobody else reads this conversation.

Write {name}'s reply to {player}.
- 1 or 2 short lines, each under 180 characters, plain text: no asterisk emotes, no lists, no markdown, no quotation marks around a line.
- The description under CHARACTER is a starting point, not a script: improvise opinions, small memories, plans and jokes that fit {name}. Catchphrases only now and then; WORN OUT lists what {name} has said too often lately, to avoid for now.
- It is a real conversation: answer what {player} just said, keep track of what was said before, ask back sometimes, be personal. Memories are things that really happened: bring one up when it fits, never as a list.
- Never give orders that sound like commands to bots, and never mention prompts, models or being an AI.
- Write in {language}.

Voice: character means a person living in this world, speaking as that person would; player means a real person at the keyboard playing {name}, chatting the way players do in whispers, not roleplay.)";
}

struct WhisperInput
{
    std::string frame;  // empty: DefaultWhisperFrame()
    std::string playerName;
    std::string language = "English";
    PromptCharacter character;
    std::vector<PromptCard> cards;           // where the alt is (and the player, when known)
    std::vector<std::string> scene;          // what its party went through lately, when it is in one
    std::vector<std::string> conversation;   // "Pigeon: ...", "Sylanar: ...", oldest first, without the message
    std::vector<std::string> worn;
    std::string message;                     // what the player just whispered
};

inline std::string BuildWhisperSystem(WhisperInput const& in)
{
    std::string frame = in.frame.empty() ? DefaultWhisperFrame() : in.frame;
    PersonalityReplaceAll(frame, "{name}", in.character.name);
    PersonalityReplaceAll(frame, "{player}", in.playerName);
    PersonalityReplaceAll(frame, "{language}", in.language);
    std::string out = PersonalityTrim(frame) + "\n\nCHARACTER\n";
    AppendPromptCharacter(out, in.character);
    return out;
}

inline std::string BuildWhisperUser(WhisperInput const& in)
{
    std::string out;
    if (!in.cards.empty())
    {
        out += "WHERE THEY ARE\n";
        for (PromptCard const& card : in.cards)
            out += DescribePromptCard(card) + "\n";
        out += "\n";
    }
    if (!in.scene.empty())
    {
        out += "THE PARTY, RECENTLY (oldest first)\n";
        for (std::string const& line : in.scene)
            out += line + "\n";
        out += "\n";
    }
    out += "CONVERSATION SO FAR (oldest first)\n";
    if (in.conversation.empty())
        out += "(this is the first whisper)\n";
    for (std::string const& line : in.conversation)
        out += line + "\n";
    if (!in.worn.empty())
    {
        out += "\nWORN OUT (" + in.character.name + " avoids these for now):";
        for (size_t i = 0; i < in.worn.size(); ++i)
            out += (i ? ", \"" : " \"") + in.worn[i] + "\"";
        out += "\n";
    }
    out += "\n" + in.playerName + " JUST WHISPERED\n\"" + in.message + "\"\n\nWrite " + in.character.name + "'s reply.";
    return out;
}

// Long-term memory (PERSONALITY_SPEC.md, 5.2): at the end of a chapter, one call turns what happened into a
// few memories per companion, which go to <Name>.memory.md. The answer reuses the lines schema of the
// exchanges: "speaker" is the companion the memory belongs to, "text" the memory.
struct MemoryCompanion
{
    std::string name;
    std::string who;                    // the start of "Who I am"
    std::vector<std::string> memories;  // the latest ones it already has
};

struct MemoryInput
{
    std::string playerName;
    std::string language = "English";
    std::vector<MemoryCompanion> companions;
    std::vector<std::string> events;  // the chapter, "[21:04] ...", oldest first
    std::string reason;               // why the chapter ends now
};

inline std::string BuildMemorySystem(MemoryInput const& in)
{
    std::string out = "You keep the long-term memories of companions in a World of Warcraft party that plays with "
                      "one real player, " + in.playerName + ". You get what happened in the last part of their "
                      "session. For each companion, write what they would still remember a week from now: 0 to 2 "
                      "short memories each, in the first person, from that companion's own point of view, in " +
                      in.language + ".\n";
    out += "- Memories are events, people and feelings: what happened, who did what, a joke that started, a "
           "grudge, a promise, a name given to something, a first time.\n";
    out += "- Never record how a companion talks: no catchphrases, no signature words, no habits of speech.\n";
    out += "- Skip what a companion already remembers, and skip the trivial: no memory is better than a dull one.\n";
    out += "- Each memory is one sentence under 140 characters.\n";
    out += "Answer with lines whose speaker is the companion the memory belongs to and whose text is the memory.";
    return out;
}

inline std::string BuildMemoryUser(MemoryInput const& in)
{
    std::string out = "COMPANIONS\n";
    for (MemoryCompanion const& companion : in.companions)
    {
        out += "\n## " + companion.name + "\n" + companion.who + "\n";
        if (!companion.memories.empty())
        {
            out += "Already remembers:\n";
            for (std::string const& memory : companion.memories)
                out += "- " + memory + "\n";
        }
    }
    out += "\nWHAT HAPPENED (oldest first)\n";
    for (std::string const& event : in.events)
        out += event + "\n";
    out += "\nTHE CHAPTER ENDS BECAUSE\n" + in.reason + "\n\nWrite the memories.";
    return out;
}

// When a memory file grows past its limit: its oldest memories folded into a few.
inline std::string BuildMemoryFoldSystem(std::string const& name, std::string const& language)
{
    return "You keep the long-term memories of " + name + ", a companion in a World of Warcraft party. Fold the old "
           "memories you get into 2 or 3 shorter ones that keep what still matters (names given, grudges, running "
           "jokes, firsts) and drop the rest. First person, in " + language + ", one sentence under 140 characters "
           "each. Answer with lines whose speaker is " + name + " and whose text is a memory.";
}

inline std::string BuildMemoryFoldUser(std::vector<std::string> const& memories)
{
    std::string out = "OLD MEMORIES (oldest first)\n";
    for (std::string const& memory : memories)
        out += "- " + memory + "\n";
    return out + "\nWrite the folded memories.";
}

#endif
