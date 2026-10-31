// Builds on its own, with no core: g++ -std=c++20 -I src/Mgr/Personality tests/personality_text.cpp
// (or cl /std:c++20 /EHsc /I src\Mgr\Personality tests\personality_text.cpp)
#include "PersonalityPrompt.h"
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

    // Lines the model writes, made safe for party chat.
    assert(PersonalitySanitizeLine("Gidaeus: \"chill out, Pigeon\"", "Gidaeus") == "chill out, Pigeon");
    assert(PersonalitySanitizeLine("*sighs*", "Gidaeus") == "sighs");
    assert(PersonalitySanitizeLine("look |cffff0000|Hitem:19019|h[Thunderfury]|h|r\nnice", "X") ==
           "look cffff0000Hitem:19019h[Thunderfury]hr nice");
    assert(PersonalitySanitizeLine("\xE2\x80\x9Cquoted\xE2\x80\x9D", "X") == "quoted");
    assert(PersonalitySanitizeLine("  ", "X").empty());
    assert(PersonalitySanitizeLine("one two three four", "X", 12) == "one two");  // cut at a word
    assert(PersonalitySanitizeLine("it's fine", "X") == "it's fine");

    // Voice: a setting, default character.
    PersonalityText player = ParsePersonality("Chill.\n# Voice\nPlayer, at the keyboard\n", 4096);
    assert(PersonalityVoiceOf(player) == PersonalityVoice::Player);
    assert(PersonalityVoiceOf(ParsePersonality("Chill.", 4096)) == PersonalityVoice::Character);
    assert(PersonalityVoiceOf(ParsePersonality("# voice\nsomething else", 4096)) == PersonalityVoice::Character);

    // The system prompt depends on nothing but the characters, in guid order, and never shows "Voice".
    PromptInput a;
    a.playerName = "Pigeon";
    a.characters = {{7, "Gidaeus", player}, {3, "Brana", ParsePersonality("Healer.", 4096)}};
    PromptInput b = a;
    std::swap(b.characters[0], b.characters[1]);
    b.cards = {{"Pigeon", true, 30, "Human", "Chronomancer", "", "", "Westfall", 90, 50, true}};
    b.scene = {"[21:00] something"};
    b.trigger = "Pigeon said in party: \"hi\"";
    std::string const system = BuildPersonalitySystem(a);
    assert(system == BuildPersonalitySystem(b));
    assert(system.find("## Brana (voice: character)") < system.find("## Gidaeus (voice: player)"));
    assert(system.find("### Voice") == std::string::npos);
    assert(system.find("travel with Pigeon, and Pigeon wrote who they are") != std::string::npos);
    std::string const user = BuildPersonalityUser(b);
    assert(user.find("- Pigeon (the player): level 30 Human Chronomancer, in Westfall, 90% health, 50% mana") !=
           std::string::npos);
    assert(user.find("WHAT JUST HAPPENED\nPigeon said in party: \"hi\"") != std::string::npos);
    assert(user.find("LINES ALREADY SAID") == std::string::npos);  // nothing said yet: no empty section

    // The frame: the built-in one with its placeholders filled, or the file's text.
    assert(system.find("{player}") == std::string::npos && system.find("{max_lines}") == std::string::npos);
    assert(system.find("At most 3 lines") != std::string::npos);
    assert(system.find("Write in English.") != std::string::npos);
    assert(system.find("A description is a starting point, not a script.") != std::string::npos);
    PromptInput custom = a;
    custom.frame = PersonalityTrim(PersonalityStripComments("\xEF\xBB\xBF<!-- note -->\r\nBe {player}'s pirates. {max_lines} lines.\r\n"));
    std::string const customSystem = BuildPersonalitySystem(custom);
    assert(customSystem.rfind("Be Pigeon's pirates. 3 lines.\n\nCHARACTERS\n", 0) == 0);

    b.recentLines = {"Gidaeus: Muradin's beard!", "Brana: awww :)"};
    std::string const repeated = BuildPersonalityUser(b);
    assert(repeated.find("LINES ALREADY SAID (say something new: none of their catchphrases, jokes or openings again)\n"
                         "Gidaeus: Muradin's beard!\nBrana: awww :)\n") != std::string::npos);
    assert(repeated.find("LINES ALREADY SAID") < repeated.find("WHAT JUST HAPPENED"));

    // Worn phrases: what one companion keeps saying, found without a list of catchphrases.
    std::vector<std::string> const moradin = {"A sign, lad. By the mountain.", "Elves. I ate cheese by the mountain.",
                                              "The ledger grows, lad.", "Pass the cheese."};
    std::vector<std::string> const worn = WornPersonalityPhrases(moradin);
    assert(std::find(worn.begin(), worn.end(), "by the mountain") != worn.end());
    assert(std::find(worn.begin(), worn.end(), "cheese") != worn.end());
    assert(std::find(worn.begin(), worn.end(), "mountain") == worn.end());   // inside "by the mountain"
    assert(std::find(worn.begin(), worn.end(), "the mountain") == worn.end());
    assert(std::find(worn.begin(), worn.end(), "lad") == worn.end());        // short words are not phrases
    assert(std::find(worn.begin(), worn.end(), "the") == worn.end());
    assert(WornPersonalityPhrases({"Don't stand in fire.", "dont STAND there"}) == std::vector<std::string>{"dont stand"});
    assert(WornPersonalityPhrases({"one line only, mountain mountain"}).empty());  // counted once per line
    assert(WornPersonalityPhrases(moradin, 1).size() == 1);

    b.worn = {{"Moradin", worn}, {"Brana", {}}};
    std::string const withWorn = BuildPersonalityUser(b);
    assert(withWorn.find("WORN OUT") < withWorn.find("WHAT JUST HAPPENED"));
    assert(withWorn.find("- Moradin: \"by the mountain\"") != std::string::npos);
    assert(withWorn.find("- Brana") == std::string::npos);  // nothing worn, no line

    // Memories: in the cached part, after the character's own sections.
    PromptInput remembered = a;
    remembered.characters[0].memories = {"2026-10-31 Named the quest egg Gerald."};
    std::string const memorySystem = BuildPersonalitySystem(remembered);
    assert(memorySystem.find("## Gidaeus (voice: player)") < memorySystem.find("### Memories (oldest first)\n- 2026-10-31 Named"));
    assert(BuildPersonalitySystem(a).find("### Memories") == std::string::npos);
    assert(memorySystem.find("bring one up now and then") != std::string::npos);  // the frame knows about them

    MemoryInput chapter;
    chapter.playerName = "Pigeon";
    chapter.companions = {{"Sylanar", "Night Elf Primalist.", {"2026-10-30 Met Alythsera."}}};
    chapter.events = {"[21:00] Pigeon looted [Jar of Spiders] (rare)"};
    chapter.reason = "the party left The Deadmines";
    assert(BuildMemorySystem(chapter).find("Never record how a companion talks") != std::string::npos);
    std::string const chapterUser = BuildMemoryUser(chapter);
    assert(chapterUser.find("## Sylanar\nNight Elf Primalist.\nAlready remembers:\n- 2026-10-30 Met Alythsera.") != std::string::npos);
    assert(chapterUser.find("THE CHAPTER ENDS BECAUSE\nthe party left The Deadmines") != std::string::npos);
    assert(BuildMemoryFoldUser({"a", "b"}) == "OLD MEMORIES (oldest first)\n- a\n- b\n\nWrite the folded memories.");

    // Whisper chats: one character, the conversation so far, and the message to answer.
    WhisperInput whisper;
    whisper.playerName = "Pigeon";
    whisper.character = remembered.characters[0];  // Gidaeus, with a memory
    whisper.conversation = {"Pigeon: you there?", "Gidaeus: always."};
    whisper.worn = {"chill out"};
    whisper.message = "how was work";
    std::string const whisperSystem = BuildWhisperSystem(whisper);
    assert(whisperSystem.rfind("You write the private whispers of Gidaeus, one of Pigeon's companions", 0) == 0);
    assert(whisperSystem.find("{name}") == std::string::npos && whisperSystem.find("{player}") == std::string::npos);
    assert(whisperSystem.find("CHARACTER\n\n## Gidaeus (voice: player)") != std::string::npos);
    assert(whisperSystem.find("- 2026-10-31 Named the quest egg Gerald.") != std::string::npos);
    assert(whisperSystem.find("### Voice") == std::string::npos);
    std::string const whisperUser = BuildWhisperUser(whisper);
    assert(whisperUser.find("WHERE THEY ARE") == std::string::npos);  // no card, no section
    assert(whisperUser.find("CONVERSATION SO FAR (oldest first)\nPigeon: you there?\nGidaeus: always.\n") != std::string::npos);
    assert(whisperUser.find("WORN OUT (Gidaeus avoids these for now): \"chill out\"") != std::string::npos);
    assert(whisperUser.find("Pigeon JUST WHISPERED\n\"how was work\"\n\nWrite Gidaeus's reply.") != std::string::npos);
    whisper.conversation.clear();
    assert(BuildWhisperUser(whisper).find("(this is the first whisper)") != std::string::npos);
}
