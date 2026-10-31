/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYTEXT_H
#define PLAYERBOTS_PERSONALITYTEXT_H

// The text side of bot personalities, with no game object in sight: the name a personality file may
// carry, what such a file says, a party's scene log, and the chat text that goes into it. Kept apart
// so that tests/personality_text.cpp builds it on its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// Only ASCII letters fold: an accented capital stays as it is, as in the character names themselves.
inline std::string PersonalityFoldAscii(std::string_view text)
{
    std::string folded(text);
    for (char& c : folded)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return folded;
}

inline std::string PersonalityTrim(std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r'))
        ++begin;
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r'))
        --end;
    return std::string(text.substr(begin, end - begin));
}

// A character name becomes a file name: nothing that leaves the folder or that Windows refuses.
inline bool IsSafePersonalityName(std::string_view name)
{
    if (name.empty() || name.size() > 48)
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || std::string_view("/\\:*?\"<>|.").find(char(c)) != std::string_view::npos)
            return false;
    return true;
}

// The headings a personality file may use, in the order the prompt will show them. Text before the
// first heading belongs to the first one.
// "Voice" is a setting, not a description: its first word, "character" (the default) or "player", says how
// the companion speaks (PersonalityPrompt.h).
inline std::array<char const*, 6> const& PersonalityKnownHeadings()
{
    static std::array<char const*, 6> const headings = {"Who I am", "How I talk", "The others", "Likes and dislikes",
                                                       "Never", "Voice"};
    return headings;
}

struct PersonalitySection
{
    std::string heading;  // one of PersonalityKnownHeadings(), or the file's own heading
    std::string text;
};

struct PersonalityText
{
    std::vector<PersonalitySection> sections;
    size_t bytes = 0;        // what was kept, after the cut
    bool truncated = false;  // the file was longer than the limit

    std::string const* Find(std::string_view heading) const
    {
        for (PersonalitySection const& section : sections)
            if (PersonalityFoldAscii(section.heading) == PersonalityFoldAscii(heading))
                return &section.text;
        return nullptr;
    }

    bool Empty() const { return sections.empty(); }
};

// A text file as written by people: an optional UTF-8 byte order mark (Notepad), Windows line ends and
// <!-- comments --> are taken out.
inline std::string PersonalityStripComments(std::string raw)
{
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
        raw.erase(0, 3);

    std::string text;
    text.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw.compare(i, 4, "<!--") == 0)
        {
            size_t const close = raw.find("-->", i + 4);
            if (close == std::string::npos)
                break;  // an unclosed comment hides the rest, as in Markdown
            i = close + 2;
            continue;
        }
        if (raw[i] != '\r')
            text += raw[i];
    }
    return text;
}

// A personality file: PersonalityStripComments, then Markdown headings ("# How I talk", "## The others").
inline PersonalityText ParsePersonality(std::string raw, size_t maxBytes)
{
    PersonalityText parsed;
    std::string text = PersonalityStripComments(std::move(raw));

    // The limit counts what the model would read: comments and line ends are already gone.
    if (text.size() > maxBytes)
    {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;  // never in the middle of a character
        text.resize(cut);
        parsed.truncated = true;
    }

    std::string heading = PersonalityKnownHeadings()[0];
    std::string body;
    auto flush = [&]()
    {
        std::string trimmed = PersonalityTrim(body);
        body.clear();
        if (trimmed.empty())
            return;
        for (PersonalitySection& section : parsed.sections)
            if (section.heading == heading)
            {
                section.text += "\n" + trimmed;
                return;
            }
        parsed.sections.push_back({heading, trimmed});
    };

    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string_view const line(text.data() + start, end - start);

        // A heading as in Markdown: 1 to 6 '#' and a space. "#1 rule" stays text.
        size_t const skip = line.find_first_not_of('#');
        bool const isHeading = !line.empty() && line[0] == '#' && (skip == std::string_view::npos || skip <= 6) &&
                               (skip == std::string_view::npos || line[skip] == ' ' || line[skip] == '\t');
        if (isHeading)
        {
            flush();
            std::string const title = PersonalityTrim(skip == std::string_view::npos ? "" : line.substr(skip));
            heading = title.empty() ? PersonalityKnownHeadings()[0] : title;
            for (char const* known : PersonalityKnownHeadings())
                if (PersonalityFoldAscii(title) == PersonalityFoldAscii(known))
                    heading = known;
        }
        else
        {
            body.append(line);
            body += '\n';
        }
        start = end + 1;
    }
    flush();

    for (PersonalitySection const& section : parsed.sections)
        parsed.bytes += section.heading.size() + section.text.size();
    return parsed;
}

// Party chat as the client sends it, made plain for the scene: an item link "|cff...|Hitem:...|h[Buzzer
// Blade]|h|r" keeps only "[Buzzer Blade]", "||" is a "|", and newlines become spaces.
inline std::string PersonalityPlainChat(std::string_view raw, size_t maxBytes = 255)
{
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char const c = raw[i];
        if (c == '|' && i + 1 < raw.size())
        {
            char const code = raw[i + 1];
            if (code == '|')
            {
                out += '|';
                ++i;
            }
            else if (code == 'c')
                i += 9;  // |cAARRGGBB
            else if (code == 'H')
            {
                size_t const close = raw.find("|h", i + 2);
                i = close == std::string_view::npos ? raw.size() : close + 1;
            }
            else
                ++i;  // |r, |h and the rest
            continue;
        }
        if (c == '|')
            continue;
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }

    out = PersonalityTrim(out);
    if (out.size() > maxBytes)
    {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

// A line the model wrote, made safe for party chat: no '|' (an escape code could forge an item or quest
// link), one line, no "Name:" in front, no quotes or asterisks around it, at most maxBytes, cut at a word.
inline std::string PersonalitySanitizeLine(std::string_view raw, std::string_view speaker, size_t maxBytes = 255)
{
    std::string text;
    for (char c : raw)
    {
        if (c == '|')
            continue;
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && (text.empty() || text.back() == ' '))
            continue;
        text += c;
    }
    text = PersonalityTrim(text);

    std::string const prefix = PersonalityFoldAscii(speaker) + ":";
    if (!speaker.empty() && PersonalityFoldAscii(text.substr(0, prefix.size())) == prefix)
        text = PersonalityTrim(std::string_view(text).substr(prefix.size()));

    // Wrapping quotes and emote stars, also the typographic ones (UTF-8).
    static char const* const wrappers[][2] = {{"\"", "\""}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\xC2\xAB", "\xC2\xBB"},
                                              {"*", "*"}, {"'", "'"}};
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto const& pair : wrappers)
        {
            std::string_view const open = pair[0];
            std::string_view const close = pair[1];
            if (text.size() >= open.size() + close.size() + 1 && text.compare(0, open.size(), open) == 0 &&
                text.compare(text.size() - close.size(), close.size(), close) == 0)
            {
                text = PersonalityTrim(std::string_view(text).substr(open.size(), text.size() - open.size() - close.size()));
                changed = true;
            }
        }
    }

    if (text.size() > maxBytes)
    {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        size_t const space = text.rfind(' ', cut);
        if (space != std::string::npos && space > maxBytes / 2)
            cut = space;
        text = PersonalityTrim(std::string_view(text).substr(0, cut));
    }
    return text;
}

struct SceneEntry
{
    int64_t when = 0;   // seconds, as time() gives them
    std::string stamp;  // "21:04", written by the caller in server time
    std::string key;    // what makes two entries the same event; empty: never merged
    std::string text;
};

// What happened in a party, newest last. One event seen by five members comes in five times: the
// same key within dedupSeconds is kept once.
class SceneLog
{
public:
    explicit SceneLog(size_t capacity = 40) : capacity(capacity ? capacity : 1) {}

    bool Add(int64_t when, std::string stamp, std::string key, std::string text, int64_t dedupSeconds)
    {
        if (!key.empty())
            for (auto it = entries.rbegin(); it != entries.rend() && when - it->when <= dedupSeconds; ++it)
                if (it->key == key)
                    return false;

        entries.push_back({when, std::move(stamp), std::move(key), std::move(text)});
        while (entries.size() > capacity)
            entries.pop_front();
        return true;
    }

    // A later report of the same event that says more ("died to Sneed" after "died"): the entry
    // within the window takes the new text. False when there was none, and nothing was changed.
    bool Upgrade(int64_t when, std::string const& key, std::string text, int64_t dedupSeconds)
    {
        for (auto it = entries.rbegin(); it != entries.rend() && when - it->when <= dedupSeconds; ++it)
            if (it->key == key)
            {
                it->text = std::move(text);
                return true;
            }
        return false;
    }

    void SetCapacity(size_t value)
    {
        capacity = value ? value : 1;
        while (entries.size() > capacity)
            entries.pop_front();
    }

    std::vector<std::string> Lines() const
    {
        std::vector<std::string> lines;
        lines.reserve(entries.size());
        for (SceneEntry const& entry : entries)
            lines.push_back("[" + entry.stamp + "] " + entry.text);
        return lines;
    }

    std::deque<SceneEntry> const& Entries() const { return entries; }
    size_t Size() const { return entries.size(); }

private:
    size_t capacity;
    std::deque<SceneEntry> entries;
};

#endif
