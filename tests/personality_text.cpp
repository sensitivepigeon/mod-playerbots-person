// Builds on its own, with no core: g++ -std=c++20 -I src/Mgr/Personality tests/personality_text.cpp
// (or cl /std:c++20 /EHsc /I src\Mgr\Personality tests\personality_text.cpp)
#include "PersonalityText.h"

#include <cassert>

int main()
{
    // Names: a character name, never a path.
    assert(IsSafePersonalityName("Thraxa"));
    assert(IsSafePersonalityName("Zéphyr"));
    assert(!IsSafePersonalityName(""));
    assert(!IsSafePersonalityName("..\\boot"));
    assert(!IsSafePersonalityName("a/b"));
    assert(!IsSafePersonalityName("Thraxa.memory"));
    assert(PersonalityFoldAscii("ThRaXa") == "thraxa");
    assert(PersonalityFoldAscii("Zéphyr") == "zéphyr");

    // A file written in Notepad: byte order mark, Windows line ends, a comment, text before any heading.
    PersonalityText file = ParsePersonality("\xEF\xBB\xBF" "Grumpy orc.\r\n<!-- skipped\r\nlines -->\r\n"
                                            "## how i TALK\r\nShort.\r\n#1 rule: no elves.\r\n"
                                            "# Never\r\nNo flirting.\r\n# Who I am\r\nRaised by dwarves.\r\n"
                                            "# Songs\r\n\r\n# Pets\r\nA boar.\r\n",
                                            4096);
    assert(!file.truncated);
    assert(file.sections.size() == 4);
    assert(*file.Find("Who I am") == "Grumpy orc.\nRaised by dwarves.");
    assert(*file.Find("How I talk") == "Short.\n#1 rule: no elves.");
    assert(file.sections[1].heading == "How I talk");  // the known spelling, not the file's
    assert(*file.Find("never") == "No flirting.");
    assert(!file.Find("Songs"));                       // an empty section is dropped
    assert(*file.Find("Pets") == "A boar.");           // an unknown heading is kept as written

    // An unclosed comment hides the rest; an empty file has no section.
    assert(ParsePersonality("Kept.\n<!-- open\n# Never\nlost", 4096).sections.size() == 1);
    assert(ParsePersonality("  \n\n", 4096).Empty());

    // The cut never splits a character: "é" is two bytes.
    PersonalityText cut = ParsePersonality("abcé", 4);
    assert(cut.truncated);
    assert(*cut.Find("Who I am") == "abc");

    // Party chat: links keep their name only.
    assert(PersonalityPlainChat("look |cff0070dd|Hitem:5191:0:0:0|h[Cruel Barb]|h|r !") == "look [Cruel Barb] !");
    assert(PersonalityPlainChat("a || b\nc") == "a | b c");
    assert(PersonalityPlainChat("  trailing |") == "trailing");
    assert(PersonalityPlainChat("ééé", 5) == "éé");

    // Scene: the same event within the window is kept once, the oldest go past the capacity.
    SceneLog scene(3);
    assert(scene.Add(100, "21:00", "death:1", "Thraxa died", 10));
    assert(!scene.Add(105, "21:00", "death:1", "Thraxa died", 10));
    assert(scene.Add(105, "21:00", "", "Sqazz: \"hi\"", 10));
    assert(scene.Add(105, "21:00", "", "Sqazz: \"hi\"", 10));  // no key: never merged
    assert(scene.Add(120, "21:00", "death:1", "Thraxa died", 10));
    assert(scene.Size() == 3);
    assert(scene.Lines().front() == "[21:00] Sqazz: \"hi\"");
    assert(scene.Upgrade(125, "death:1", "Thraxa died to Sneed", 10));
    assert(scene.Lines().back() == "[21:00] Thraxa died to Sneed");
    assert(!scene.Upgrade(200, "death:1", "too late", 10));
    // The limit counts text without comments.
    assert(!ParsePersonality("<!-- a long comment that is not counted -->abc", 3).truncated);
    scene.SetCapacity(1);
    assert(scene.Lines() == std::vector<std::string>{"[21:00] Thraxa died to Sneed"});
}
