// The easter egg tab of the editor (docs/editor/prank-tab.md), the parts that need no window: the choice of a mini-scene by key words (the fallback of the
// generator), the switch (the "prankTab" setting and --no-prank), the tables of the texts and the colours, and the mini-scenes themselves.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "../../engine/src/ui/editor/PrankMatcher.h"
#include "../../engine/src/ui/editor/PrankSettings.h"
#include "../../engine/src/ui/editor/PrankStrings.h"

namespace
{
    namespace fs = std::filesystem;
    namespace prank = myengine::ui::prank;
    using prank::MiniScene;

    int g_checks = 0;

    void Check(const bool condition, const std::string& message)
    {
        ++g_checks;
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    fs::path SourceRoot()
    {
#ifdef MYENGINE_SOURCE_DIR
        return fs::u8path(MYENGINE_SOURCE_DIR);
#else
        return fs::current_path();
#endif
    }

    std::string ReadAll(const fs::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    void Write(const fs::path& file, const std::string& text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    // WCAG contrast of two 0xRRGGBB colours
    double Luminance(const unsigned rgb)
    {
        auto channel = [](const unsigned v)
        {
            const double c = static_cast<double>(v) / 255.0;
            return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel((rgb >> 16) & 0xFF) + 0.7152 * channel((rgb >> 8) & 0xFF) + 0.0722 * channel(rgb & 0xFF);
    }

    double Contrast(const unsigned a, const unsigned b)
    {
        const double la = Luminance(a);
        const double lb = Luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }

    // ---- the choice of a scene ----
    void TestMatcher()
    {
        // The table of the card: the request and the game
        struct Case
        {
            const char* request;
            MiniScene scene;
        };
        const Case cases[] = {
            {"хентай с щупальцами", MiniScene::Octopus},
            {"щупальца", MiniScene::Octopus},
            {"осьминог", MiniScene::Octopus},
            {"фури", MiniScene::Fox},
            {"Фури, продолжение (14)", MiniScene::Fox},
            {"лисичка", MiniScene::Fox},
            {"что-то горячее", MiniScene::Lava},
            {"лава", MiniScene::Lava},
            {"огонь", MiniScene::Lava},
            {"жёсткое порно", MiniScene::Hardcore},
            {"ЖЁСТКОЕ", MiniScene::Hardcore},
            {"жестко", MiniScene::Hardcore},
            {"хардкор", MiniScene::Hardcore},
            {"голые", MiniScene::NakedCars},
            {"ГОЛЫЕ машинки", MiniScene::NakedCars},
            // the chips of the empty chat
            {"Горячее", MiniScene::Lava},
            {"Щупальца", MiniScene::Octopus},
            {"Фури", MiniScene::Fox},
            {"Хардкор", MiniScene::Hardcore},
        };
        for (const Case& c : cases)
        {
            const prank::Match match = prank::MatchRequest(c.request, 1);
            Check(match.keyword && !match.refused, std::string("a key word is found in: ") + c.request);
            Check(match.scene == c.scene, std::string("the scene of: ") + c.request + " is " + prank::SceneId(match.scene));
        }
        // No key word: any scene, a different one for a different seed; the answer is the shrug
        std::set<int> seen;
        for (std::uint32_t seed = 0; seed < 64; ++seed)
        {
            const prank::Match match = prank::MatchRequest("привет, как дела", seed);
            Check(!match.keyword && !match.refused, "no key word in a greeting");
            Check(static_cast<int>(match.scene) >= 0 && static_cast<int>(match.scene) < prank::kMiniSceneCount, "a random scene is one of the five");
            seen.insert(static_cast<int>(match.scene));
            Check(prank::AnswerText(match, seed).find("Не совсем понял") != std::string::npos, "the shrug answers an unknown request");
        }
        Check(seen.size() >= 3, "the random scene changes with the seed");
        Check(prank::MatchRequest("привет", 5).scene == prank::MatchRequest("привет", 5).scene, "the same seed gives the same scene");
        // The refusals: children, real people, violence against the living
        for (const char* request : {"порно с детьми", "школьницы", "Путин и лиса", "изнасилование", "Трамп"})
        {
            const prank::Match match = prank::MatchRequest(request, 3);
            Check(match.refused, std::string("a refusal for: ") + request);
            Check(prank::AnswerText(match, 3).find("не генерирую") != std::string::npos, std::string("the refusal says it: ") + request);
        }
        // The answers: one per game, with the closing line
        const MiniScene all[] = {MiniScene::Octopus, MiniScene::Fox, MiniScene::Lava, MiniScene::Hardcore, MiniScene::NakedCars};
        const char* const starts[] = {"Готово", "Фури доставлено", "Горячее некуда", "Жёстко, как просил", "Совсем голые"};
        for (int i = 0; i < prank::kMiniSceneCount; ++i)
        {
            prank::Match match;
            match.scene = all[i];
            match.keyword = true;
            const std::string answer = prank::AnswerText(match, 11);
            Check(answer.find(starts[i]) == 0, std::string("the answer of ") + prank::SceneId(all[i]) + " starts with its phrase");
            Check(answer.find("Жми Play") != std::string::npos, "the answer ends with a line about Play");
            Check(prank::StepCount(all[i]) >= 3 && prank::StepCount(all[i]) <= 5, "3 to 5 steps");
            Check(std::string(prank::StepText(all[i], 0)).size() > 3, "a step has a text");
        }
        Check(prank::FoldCase("ЖЁсткое ФУРИ Hello") == "жесткое фури hello", "case and the letter yo are folded");
        std::cout << "  matcher: " << (sizeof(cases) / sizeof(cases[0])) << " requests, 5 refusals, " << seen.size() << " random scenes\n";
    }

    // ---- the switch ----
    void TestSettings()
    {
        const fs::path directory = fs::temp_directory_path() / "myengine_prank_test";
        fs::remove_all(directory);
        const fs::path file = directory / "editor_settings.json";

        // No file, a broken file, a silent file, a key of a wrong type: the tab is on
        Check(prank::LoadEnabled(file), "no file: on");
        Write(file, "{ not json");
        Check(prank::LoadEnabled(file), "a broken file: on");
        Write(file, R"({"assistant": {"backend": "claude"}})");
        Check(prank::LoadEnabled(file), "no key: on");
        Write(file, R"({"prankTab": "no"})");
        Check(prank::LoadEnabled(file), "a key of a wrong type: on");
        Write(file, R"({"prankTab": true})");
        Check(prank::LoadEnabled(file), "true: on");
        Write(file, R"({"prankTab": false})");
        Check(!prank::LoadEnabled(file), "false: off");

        // Saving keeps the other keys of the file
        Write(file, R"({"assistant": {"backend": "claude", "claudePath": "x"}, "other": 7})");
        std::string error;
        Check(prank::SaveEnabled(file, false, error), "save false: " + error);
        const auto saved = nlohmann::json::parse(ReadAll(file));
        Check(saved.at("prankTab") == false, "prankTab is false in the file");
        Check(saved.at("assistant").at("backend") == "claude" && saved.at("assistant").at("claudePath") == "x" && saved.at("other") == 7, "the other keys stayed");
        Check(!prank::LoadEnabled(file), "the saved file reads as off");
        Check(prank::SaveEnabled(file, true, error) && prank::LoadEnabled(file), "save true: on again");

        // The state of the process: the file and --no-prank (MYENGINE_NO_PRANK=1 by the time the editor starts)
        SetEnvironmentVariableW(L"MYENGINE_ASSISTANT_SETTINGS", file.wstring().c_str());
        Check(prank::SettingsFile() == file, "MYENGINE_ASSISTANT_SETTINGS names the file");
        prank::Initialize(false);
        Check(prank::Enabled() && prank::SettingOn() && !prank::Hidden() && prank::FontsWanted(), "on by default: tab, menu button and fonts");
        prank::Initialize(true);
        Check(!prank::Enabled() && prank::SettingOn() && prank::Hidden() && !prank::FontsWanted(), "--no-prank: nothing, not even the View item, no fonts");
        Write(file, R"({"prankTab": false})");
        prank::Initialize(false);
        Check(!prank::Enabled() && !prank::SettingOn() && !prank::Hidden() && !prank::FontsWanted(), "the setting off: no tab, the View item stays to switch it on");
        prank::SetEnabled(true);
        Check(prank::Enabled() && prank::LoadEnabled(file), "the check mark turns it on at once and is saved");
        prank::SetEnabled(false);
        Check(!prank::Enabled() && !prank::LoadEnabled(file), "the check mark turns it off at once and is saved");
        SetEnvironmentVariableW(L"MYENGINE_ASSISTANT_SETTINGS", nullptr);
        fs::remove_all(directory);
        std::cout << "  settings: file, keys kept, --no-prank, the check mark\n";
    }

    // ---- the tables ----
    void TestTables()
    {
        namespace str = prank::str;
        // The ten folders are one inside another: every level has the folder of the next one, the last one holds the generator
        for (int i = 0; i < str::kLevelCount; ++i)
        {
            const str::LevelDef& level = str::kLevels[i];
            Check(level.folderName != nullptr && level.itemCount >= 1 && level.itemCount <= 3, "a level has 1 to 3 items");
            int folders = 0;
            for (int k = 0; k < level.itemCount; ++k)
            {
                Check(level.items[k].name != nullptr, "an item has a name");
                if (level.items[k].folder)
                {
                    ++folders;
                    Check(i + 1 < str::kLevelCount && std::string(level.items[k].name) == str::kLevels[i + 1].folderName, std::string("the folder leads to the next level: ") + level.items[k].name);
                }
                else if (level.items[k].viewer == str::Viewer::Notepad)
                {
                    Check(level.items[k].text != nullptr && level.items[k].text[0] != '\0', std::string("a file has its text: ") + level.items[k].name);
                }
            }
            Check(folders == (i + 1 < str::kLevelCount ? 1 : 0), "one folder per level, none in the last one");
            // The banner grows louder with the depth
            if (i > 0)
            {
                Check(str::kLevels[i].shake >= str::kLevels[i - 1].shake, "the shaking does not fall");
                Check(level.banner != str::BannerKind::None && level.bannerSize >= str::kLevels[i - 1].bannerSize, "the banner does not shrink");
            }
        }
        Check(std::string(str::kLevels[str::kLevelCount - 1].items[0].name) == "AI_генератор_18+.exe" && str::kLevels[str::kLevelCount - 1].items[0].viewer == str::Viewer::Generator,
              "the last folder holds the generator");
        Check(std::abs(str::kLevels[7].tiltDegrees) <= 1.2f && std::abs(str::kLevels[9].tiltDegrees) <= 1.2f, "the tilt is at most 1.2 degrees");
        // The colours: the text is readable (4.5:1) on every level
        double worst = 100.0;
        for (int i = 0; i < str::kLevelCount; ++i)
        {
            const double contrast = Contrast(str::kPalettes[i].text, str::kPalettes[i].bg);
            worst = std::min(worst, contrast);
            Check(contrast >= 4.5, "the text on the background of level " + std::to_string(i + 1) + " has contrast " + std::to_string(contrast));
            Check(Contrast(str::kPalettes[i].text, str::kPalettes[i].side) >= 4.5, "the text in the left column of level " + std::to_string(i + 1));
            Check(Contrast(str::kPalettes[i].text, str::kPalettes[i].chrome) >= 4.5, "the text on the bars of level " + std::to_string(i + 1));
            Check(Contrast(str::kPalettes[i].text, str::kPalettes[i].sel) >= 4.5, "the text on the selection of level " + std::to_string(i + 1));
            Check(Contrast(str::kPalettes[i].text, str::kPalettes[i].hover) >= 4.5, "the text on the hover of level " + std::to_string(i + 1));
        }
        std::cout << "  tables: 10 levels, the chain of folders, the worst contrast of the text " << worst << "\n";
    }

    // ---- the mini-scenes ----
    void TestScenes()
    {
        const fs::path root = SourceRoot();
        const char* const banned[] = {"порно", "хентай", "секс", "голы", "18+", "обнаж", "эрот"};
        // The materials with the photograph of a human head (the texture african_head_diffuse): not in a mini-scene, not in a prefab it spawns, not in a material it uses
        const char* const faces[] = {"/cool.material", "/warm.material", "africanhead", "african_head"};   // the slash: prank_cool.material is not cool.material
        auto noFaces = [&](const std::string& text, const std::string& what)
        {
            for (const char* face : faces)
            {
                Check(text.find(face) == std::string::npos, what + " has no \"" + face + "\"");
            }
        };
        std::set<std::string> modules;
        for (int i = 0; i < prank::kMiniSceneCount; ++i)
        {
            const auto scene = static_cast<MiniScene>(i);
            const fs::path file = root / prank::SceneFile(scene);
            Check(fs::is_regular_file(file), std::string("the scene exists: ") + file.string());
            const std::string text = ReadAll(file);
            const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
            Check(!json.is_discarded() && json.contains("entities") && json.at("entities").is_array(), std::string("the scene is a JSON with entities: ") + prank::SceneId(scene));
            bool camera = false;
            bool controlled = false;
            bool manager = false;
            std::set<int> ids;
            for (const auto& entity : json.at("entities"))
            {
                Check(entity.contains("id") && ids.insert(entity.at("id").get<int>()).second, "the ids of the entities are unique");
                const std::string name = entity.contains("Tag") ? entity.at("Tag").at("name").get<std::string>() : std::string();
                camera = camera || entity.contains("Camera");
                controlled = controlled || name == "Controlled_1";
                if (entity.contains("Script"))
                {
                    for (const auto& script : entity.at("Script").at("scripts"))
                    {
                        const std::string module = script.at("module").get<std::string>();
                        modules.insert(module);
                        manager = manager || script.at("class") == "GameManager";
                    }
                }
            }
            Check(camera && controlled && manager, std::string("the scene has a camera, the controlled player and the game manager: ") + prank::SceneId(scene));
            noFaces(text, std::string("the scene ") + prank::SceneId(scene));
            // the prefabs that its spawners ask for, and the materials of those prefabs and of the scene
            std::set<std::string> prefabs;
            for (const auto& entity : json.at("entities"))
            {
                if (!entity.contains("Script"))
                {
                    continue;
                }
                for (const auto& script : entity.at("Script").at("scripts"))
                {
                    if (script.contains("props") && script.at("props").contains("prefab_name"))
                    {
                        prefabs.insert(script.at("props").at("prefab_name").get<std::string>());
                    }
                }
            }
            for (const std::string& prefab : prefabs)
            {
                const fs::path prefabFile = root / "assets" / "prefabs" / (prefab + ".prefab.json");
                Check(fs::is_regular_file(prefabFile), "the prefab of a spawner exists: " + prefab);
                const std::string prefabText = ReadAll(prefabFile);
                noFaces(prefabText, "the prefab " + prefab);
                for (std::size_t at = prefabText.find("assets/materials/"); at != std::string::npos; at = prefabText.find("assets/materials/", at + 1))
                {
                    const std::size_t end = prefabText.find('"', at);
                    const fs::path material = root / prefabText.substr(at, end - at);
                    Check(fs::is_regular_file(material), "the material exists: " + material.string());
                    Check(ReadAll(material).find("african_head") == std::string::npos, "the material has no face texture: " + material.string());
                }
            }
            for (std::size_t at = text.find("assets/materials/"); at != std::string::npos; at = text.find("assets/materials/", at + 1))
            {
                const std::size_t end = text.find('"', at);
                const fs::path material = root / text.substr(at, end - at);
                Check(fs::is_regular_file(material), "the material exists: " + material.string());
                Check(ReadAll(material).find("african_head") == std::string::npos, "the material has no face texture: " + material.string());
            }
            // No adult content: not in a name, not in a text
            const std::string folded = prank::FoldCase(text);
            for (const char* word : banned)
            {
                Check(folded.find(word) == std::string::npos, std::string("the scene ") + prank::SceneId(scene) + " has no \"" + word + "\"");
            }
        }
        // The scripts they use are there, and have no adult words either
        for (const std::string& module : modules)
        {
            const fs::path script = root / "assets" / "scripts" / (module + ".py");
            Check(fs::is_regular_file(script), "the script exists: " + module);
            const std::string folded = prank::FoldCase(ReadAll(script));
            for (const char* word : banned)
            {
                Check(folded.find(word) == std::string::npos, "the script " + module + " has no \"" + word + "\"");
            }
        }
        // The prompt keeps the rule
        const std::string prompt = ReadAll(root / "assets" / "assistant" / "prank_prompt.md");
        Check(!prompt.empty() && prompt.find("Never write, draw, name or describe anything sexual") != std::string::npos, "the prompt of the generator has the rule");
        // The prompt names the forbidden materials (that is its job) and sends to the neutral ones
        Check(prompt.find("cool.material.json") != std::string::npos && prompt.find("african_head") != std::string::npos && prompt.find("prank_coin") != std::string::npos &&
                  prompt.find("prank_cool.material.json") != std::string::npos, "the prompt forbids the face materials and names the neutral ones");
        std::cout << "  scenes: 5 files, " << modules.size() << " scripts, no adult words\n";
    }
}

int main()
{
    try
    {
        TestMatcher();
        TestSettings();
        TestTables();
        TestScenes();
        std::cout << "prank OK: " << g_checks << " checks\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "prank FAILED: " << error.what() << '\n';
        return 1;
    }
}
