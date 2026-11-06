// PrankMatcher.h
//
// The fallback of the easter egg generator (no neural network): a request is matched to one of the five ready mini-scenes by key words, and the
// answer is one of the prepared phrases. No ImGui here: the test (tests/ui/PrankTests.cpp) includes this file.

#pragma once

#include <cstdint>
#include <string>

namespace myengine::ui::prank
{
    enum class MiniScene : std::uint8_t
    {
        Octopus,   // tentacles: the boss octopus, "dodge the tentacles", score
        Fox,       // the fox collects coins
        Lava,      // lava floor, fireballs fall
        Hardcore,  // one touch and it is over
        NakedCars, // wheels without bodies play football
        Count,
    };

    inline constexpr int kMiniSceneCount = static_cast<int>(MiniScene::Count);

    // Where the scene lives, relative to the repository root ("assets/prank/scenes/octopus.json")
    const char* SceneFile(MiniScene scene);
    // Short id for logs and tests: "octopus", "fox", "lava", "hardcore", "nakedcars"
    const char* SceneId(MiniScene scene);

    struct Match
    {
        MiniScene scene = MiniScene::Octopus;
        bool keyword = false;   // a key word was found (otherwise the scene is a random one)
        bool refused = false;   // the request is about children, real people or violence against the living: a joke instead, and a harmless scene
    };

    // Lower case and the letter yo -> ye, for Cyrillic and Latin letters, in UTF-8. Anything else is kept as it is
    std::string FoldCase(const std::string& utf8);

    // `seed` picks the random scene when no key word matches (any number: the tests pass one, the editor passes the time)
    Match MatchRequest(const std::string& request, std::uint32_t seed);

    // The prepared answer of the generator: the line about the scene (or the refusal, or the shrug) and then one of the closing lines
    std::string AnswerText(const Match& match, std::uint32_t seed);

    // The tool steps shown under the progress bar while the ready scene "builds" (3 to 5 lines)
    int StepCount(MiniScene scene);
    const char* StepText(MiniScene scene, int index);
}
