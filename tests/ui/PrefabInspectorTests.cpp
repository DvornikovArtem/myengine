#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <windows.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/scripting/ScriptSystem.h>
#include <myengine/ui/PrefabInspector.h>
#include <myengine/ui/ScriptInspector.h>

namespace
{
    namespace ui = myengine::ui;
    using json = nlohmann::json;
    using Field = myengine::scripting::ScriptFieldInfo;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        Check(static_cast<bool>(file), "Could not read the test template");
        return json::parse(file);
    }

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        Check(static_cast<bool>(file), "Could not write the test template");
        static unsigned revision = 0;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::milliseconds(++revision));
    }

    json& Props(json& prefab)
    {
        return prefab["entities"][0]["Script"]["scripts"][0]["props"];
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto source = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine T8 редактор " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique test directory");
            prefabsDirectory = directory / "prefabs";
            std::filesystem::create_directory(prefabsDirectory);
            for (const auto* name : {"coin", "chaser", "firework"})
            {
                const auto file = std::string(name) + ".prefab.json";
                std::filesystem::copy_file(source / "assets/prefabs" / file, prefabsDirectory / file);
                std::filesystem::copy_file(source / "assets/scripts" / (std::string(name) + ".py"), directory / (std::string(name) + ".py"));
            }
            std::filesystem::copy_file(source / "assets/scripts/game_manager.py", directory / "game_manager.py");
            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(directory / "test.log"), "Logger initialization failed");
            Check(library.Initialize(prefabsDirectory, logger.get()), "Prefab library initialization failed");
            wchar_t executable[32768]{};
            Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
            Check(runtime.Initialize({std::filesystem::path(executable).parent_path(), directory, logger.get()}), "Embedded Python initialization failed");
            system = std::make_unique<myengine::scripting::ScriptSystem>(runtime, input, library, *logger);

            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(1024.0f, 768.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            describe = [this](const std::string& module, const std::string& className)
            {
                return runtime.DescribeFields(module, className);
            };
        }

        ~Fixture()
        {
            ImGui::DestroyContext(context);
            system.reset();
            library.Shutdown();
            runtime.Shutdown();
            logger.reset();
            if (!keepArtifacts)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }

        void Frame(const std::function<void()>& draw)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f), ImGuiCond_Always);
            ImGui::Begin("T8 test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            draw();
            ImGui::End();
            ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "The prefab inspector did not draw any UI");
        }

        void Draw(ui::PrefabInspector& inspector)
        {
            Frame([&]() { inspector.Draw(library, describe); });
        }

        template <class Predicate>
        void WaitFor(Predicate&& ready)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do
            {
                library.Poll();
                if (ready())
                {
                    return;
                }
                Check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for the prefab watcher");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (true);
        }

        std::filesystem::path directory;
        std::filesystem::path prefabsDirectory;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::scene::PrefabLibrary library;
        myengine::scripting::ScriptRuntime runtime;
        myengine::input::InputManager input;
        std::unique_ptr<myengine::scripting::ScriptSystem> system;
        ImGuiContext* context = nullptr;
        ui::PrefabInspector::DescribeFields describe;
        bool keepArtifacts = false;
    };

    // The fields are drawn in a property grid (name | value | reset). Where the value and the reset arrow of the
    // single row of the grid that was just drawn are: found through the table, not through the last item.
    ImVec2 ValuePoint(const float offset)
    {
        ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##script_fields"));
        if (table == nullptr)
        {
            throw std::runtime_error("The script field grid was not drawn");
        }
        return ImVec2(table->Columns[1].WorkMinX + offset, (table->RowPosY1 + table->RowPosY2) * 0.5f);
    }

    ImVec2 ResetPoint()
    {
        ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##script_fields"));
        if (table == nullptr)
        {
            throw std::runtime_error("The script field grid was not drawn");
        }
        return ImVec2((table->Columns[2].WorkMinX + table->Columns[2].WorkMaxX) * 0.5f, (table->RowPosY1 + table->RowPosY2) * 0.5f);
    }

    void TestWidgets(Fixture& fixture)
    {
        // The panel reuses T7's real widgets, not a second implementation of the field types.
        const std::vector<Field> fields{
            {"speed", Field::Type::Float, 3.0}, {"damage", Field::Type::Int, 1},
            {"aggressive", Field::Type::Bool, true}, {"target", Field::Type::String, "Controlled_1"}
        };
        auto props = json::object();
        fixture.Frame([&]() { Check(!ui::DrawScriptFields(fields, props), "Viewing defaults changed props"); });
        Check(props.empty(), "Default fields were written without an edit");

        // Click the actual checkbox, then its Reset button; verify that the key is removed.
        const std::vector<Field> checkbox{fields[2]};
        ImVec2 position;
        ImRect reset;
        const auto drawCheckbox = [&]()
        {
            ui::DrawScriptFields(checkbox, props);
            position = ValuePoint(8.0f);
            reset = ImRect(ResetPoint(), ResetPoint());
        };
        fixture.Frame(drawCheckbox);
        fixture.Frame(drawCheckbox);
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        fixture.Frame(drawCheckbox);
        io.AddMouseButtonEvent(0, true);
        fixture.Frame(drawCheckbox);
        io.AddMouseButtonEvent(0, false);
        fixture.Frame(drawCheckbox);
        Check(props.contains("aggressive") && props["aggressive"] == false, "Checkbox did not store a bool override");
        fixture.Frame(drawCheckbox); // the newly stored value now has a Reset button

        io.AddMousePosEvent(reset.GetCenter().x, reset.GetCenter().y);
        fixture.Frame(drawCheckbox);
        io.AddMouseButtonEvent(0, true);
        fixture.Frame(drawCheckbox);
        io.AddMouseButtonEvent(0, false);
        fixture.Frame(drawCheckbox);
        Check(!props.contains("aggressive"), "Reset did not remove the override");

        for (const auto& field : std::vector<Field>{fields[0], fields[1]})
        {
            props = {{field.name, field.defaultValue}};
            const auto drawNumber = [&]()
            {
                ui::DrawScriptFields({field}, props);
                position = ValuePoint(30.0f);
            };
            for (int frame = 0; frame < 25; ++frame)
            {
                fixture.Frame(drawNumber); // not a double click on the preceding field's screen position
            }
            io.AddMousePosEvent(position.x, position.y);
            fixture.Frame(drawNumber);
            io.AddMouseButtonEvent(0, true);
            fixture.Frame(drawNumber);
            io.AddMousePosEvent(position.x + 20.0f, position.y);
            fixture.Frame(drawNumber); // crossing the drag threshold activates the numeric editor
            io.AddMousePosEvent(position.x + 120.0f, position.y);
            fixture.Frame(drawNumber);
            io.AddMouseButtonEvent(0, false);
            fixture.Frame(drawNumber);
            if (props[field.name].get<double>() <= field.defaultValue.get<double>())
            {
                throw std::runtime_error("Dragging " + field.name + " did not store a new value: " + props.dump());
            }
            Check(field.type != Field::Type::Int || props[field.name].is_number_integer(), "DragInt stored a non-integer value");
        }

        props = {{"target", "Controlled_1"}};
        const auto drawString = [&]()
        {
            ui::DrawScriptFields({fields[3]}, props);
            position = ValuePoint(30.0f);
        };
        fixture.Frame(drawString);
        io.AddMousePosEvent(position.x, position.y);
        fixture.Frame(drawString);
        io.AddMouseButtonEvent(0, true);
        fixture.Frame(drawString);
        io.AddMouseButtonEvent(0, false);
        fixture.Frame(drawString);
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        fixture.Frame(drawString);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        fixture.Frame(drawString);
        io.AddInputCharactersUTF8("Player");
        fixture.Frame(drawString);
        io.AddKeyEvent(ImGuiKey_Enter, true);
        fixture.Frame(drawString);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        fixture.Frame(drawString);
        Check(props["target"] == "Player", "InputText did not store a string override");

        props = {{"speed", 7.5}, {"damage", 4}, {"aggressive", false}, {"target", "Player"}};
        fixture.Frame([&]() { Check(!ui::DrawScriptFields(fields, props), "Viewing stored field types changed props"); });
        Check(props["speed"] == 7.5 && props["damage"] == 4 && props["target"] == "Player", "Widgets changed stored values");
    }

    void TestDraftAndSave(Fixture& fixture)
    {
        ui::PrefabInspector inspector;
        const auto names = fixture.library.ListPrefabs();
        Check(names == std::vector<std::string>({"chaser", "coin", "firework"}), "Prefab list is incomplete or unsorted");
        Check(inspector.Select(fixture.library, "chaser"), "Could not select Chaser");
        const auto original = inspector.GetDraft();
        fixture.Draw(inspector);
        Check(!inspector.IsDirty(), "Viewing a template marked it dirty");
        const auto fields = fixture.describe("chaser", "Chaser");
        for (const auto& expected : std::vector<Field>{{"speed", Field::Type::Float, 3.0}, {"damage", Field::Type::Int, 1},
                 {"aggressive", Field::Type::Bool, true}, {"target_name", Field::Type::String, "Controlled_1"}})
        {
            Check(std::any_of(fields.begin(), fields.end(), [&](const Field& field) { return field.name == expected.name && field.type == expected.type; }),
                "A Chaser field is missing or has the wrong widget type");
        }

        auto& editor = myengine::core::ServiceLocator::GetEditorRuntimeState();
        editor.mode = myengine::editor::RuntimeMode::Play;
        editor.sceneDirty = false;
        myengine::ecs::World world;
        const auto before = fixture.library.Instantiate(world, "chaser");
        fixture.system->Update(world, 0.0f);
        Props(inspector.GetDraft())["speed"] = 7.5;
        Props(inspector.GetDraft())["damage"] = 4;
        Props(inspector.GetDraft())["aggressive"] = false;
        Props(inspector.GetDraft())["target_name"] = "Player";
        Check(inspector.IsDirty(), "An edit did not mark the draft dirty");
        Check(!inspector.Select(fixture.library, "coin") && inspector.GetSelectedName() == "chaser", "Switching discarded an unsaved draft");
        Check(ReadJson(fixture.prefabsDirectory / "chaser.prefab.json") == original, "Typing changed the file before Save");
        const auto unsaved = fixture.library.Instantiate(world, "chaser");
        Check(world.Get<myengine::ecs::components::ScriptComponent>(unsaved).scripts[0].props["speed"] == 3.0, "An unsaved draft reached the spawn cache");
        fixture.Draw(inspector); // editable even in Play; this is not the scene's read-only live inspector
        Check(inspector.Save(fixture.library) && !inspector.IsDirty(), "Saving the draft failed");
        Check(ReadJson(fixture.prefabsDirectory / "chaser.prefab.json") == inspector.GetDraft(), "The file does not match the saved draft");
        const auto after = fixture.library.Instantiate(world, "chaser");
        fixture.system->Update(world, 0.0f);
        const auto liveBefore = fixture.system->GetLiveFields(before, 0);
        const auto liveAfter = fixture.system->GetLiveFields(after, 0);
        Check(liveBefore.at("speed") == 3.0 && liveBefore.at("aggressive") == true, "Save changed an existing script instance");
        Check(liveAfter.at("speed") == 7.5 && liveAfter.at("damage") == 4 && liveAfter.at("aggressive") == false && liveAfter.at("target_name") == "Player",
            "Saved float/int/bool/string props did not reach the next running script");
        Check(!editor.sceneDirty && fixture.system->GetRecentErrors().empty(), "Prefab editing changed scene dirty state or faulted a script");
        fixture.system->ResetInstances();

        Props(inspector.GetDraft()).erase("speed"); // Reset stores absence, not a copy of the Python default.
        Check(inspector.Save(fixture.library), "Saving Reset failed");
        const auto reset = fixture.library.Instantiate(world, "chaser");
        fixture.system->Update(world, 0.0f);
        Check(fixture.system->GetLiveFields(reset, 0).at("speed") == 3.0, "Reset did not restore the script default on the next spawn");
        fixture.system->ResetInstances();
    }

    void TestHierarchyAndDefaults(Fixture& fixture)
    {
        ui::PrefabInspector inspector;
        Check(inspector.Select(fixture.library, "coin"), "Could not select Coin");
        auto data = inspector.GetDraft();
        data["entities"][0]["Script"]["scripts"][0].erase("props");
        data["entities"][1]["Script"]["scripts"] = json::array({
            {{"module", "coin"}, {"class", "Coin"}, {"props", {{"score_value", 3}}}},
            {{"module", "firework"}, {"class", "Firework"}, {"props", {{"lifetime", 5.0}}}}
        });
        Check(fixture.library.SavePrefab("coin", data) && inspector.Reload(fixture.library), "Could not prepare nested script behaviours");
        fixture.Draw(inspector);
        Check(!inspector.IsDirty() && inspector.GetDraft() == data, "Drawing a child/default script changed the template");
        auto& childProps = inspector.GetDraft()["entities"][1]["Script"]["scripts"][1]["props"];
        childProps["lifetime"] = 8.0;
        Check(fixture.library.GetPrefabJson("COIN") != nullptr, "Could not prepare the Windows path alias");
        Check(inspector.Save(fixture.library), "Saving a child's script fields failed");
        Check(fixture.library.GetPrefabJson("COIN")->at("entities")[1]["Script"]["scripts"][1]["props"]["lifetime"] == 8.0, "Save left a stale cache alias");
        const auto saved = ReadJson(fixture.prefabsDirectory / "coin.prefab.json");
        Check(saved["entities"][1]["Hierarchy"] == data["entities"][1]["Hierarchy"] && saved["entities"][0]["MeshRenderer"] == data["entities"][0]["MeshRenderer"],
            "Saving props changed ids, hierarchy or render components");

        auto plain = data;
        for (auto& entity : plain["entities"])
        {
            entity.erase("Script");
        }
        Check(fixture.library.SavePrefab("plain", plain), "Could not create a script-free template");
        Check(inspector.Select(fixture.library, "plain"), "Could not select the script-free template");
        fixture.Draw(inspector);
        Check(!inspector.IsDirty(), "A script-free template became dirty");
        auto missing = data;
        missing["entities"][0]["Script"]["scripts"][0]["module"] = "missing_t8_script";
        Check(fixture.library.SavePrefab("missing", missing) && inspector.Select(fixture.library, "missing"), "Could not prepare a missing script");
        fixture.Draw(inspector);
        fixture.Draw(inspector); // DescribeFields caches the import failure; the editor continues drawing.
        Check(!inspector.IsDirty(), "An import failure changed the template data");
        auto incomplete = data;
        incomplete["entities"][0]["Script"]["scripts"][0].erase("module");
        incomplete["entities"][1]["Script"]["scripts"][0].erase("class");
        Check(fixture.library.SavePrefab("incomplete", incomplete) && inspector.Select(fixture.library, "incomplete"), "Could not prepare an incomplete script entry");
        fixture.Draw(inspector);
        Check(!inspector.IsDirty(), "A missing module/class changed the template data");
    }

    void TestFailuresAndExternalEdits(Fixture& fixture)
    {
        ui::PrefabInspector inspector;
        Check(inspector.Select(fixture.library, "chaser"), "Could not select the test template");
        const auto path = fixture.prefabsDirectory / "chaser.prefab.json";
        const auto original = ReadJson(path);
        Props(inspector.GetDraft())["damage"] = 9;
        Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "Could not protect the test file");
        const bool saved = inspector.Save(fixture.library);
        const bool writable = SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0;
        Check(writable && !saved && inspector.IsDirty() && !inspector.GetLastError().empty(), "A failed save lost its draft or error");
        Check(ReadJson(path) == original && *fixture.library.GetPrefabJson("chaser") == original, "A failed save changed the file/cache");
        for (const auto& entry : std::filesystem::directory_iterator(fixture.prefabsDirectory))
        {
            Check(entry.path().filename().u8string().find(".tmp.") == std::string::npos, "A failed save leaked a temporary file");
        }
        Check(inspector.Save(fixture.library), "Retrying Save after fixing permissions failed");
        Check(inspector.Reload(fixture.library), "Reload after Save failed");

        inspector.GetDraft()["entities"][0]["id"] = 0;
        const auto valid = ReadJson(path);
        Check(!inspector.Save(fixture.library) && inspector.IsDirty(), "An invalid draft was saved");
        Check(ReadJson(path) == valid && *fixture.library.GetPrefabJson("chaser") == valid, "An invalid draft corrupted the file/cache");
        Check(inspector.Reload(fixture.library), "Reload did not discard an invalid draft");

        Props(inspector.GetDraft())["damage"] = 30;
        auto external = ReadJson(path);
        Props(external)["damage"] = 40;
        WriteText(path, external.dump());
        // No Poll here: Save must catch a disk edit even before the asynchronous watcher sees it.
        Check(!inspector.Save(fixture.library) && inspector.HasConflict(), "Save overwrote an external edit");
        Check(ReadJson(path) == external && Props(inspector.GetDraft())["damage"] == 30, "Conflict handling lost disk data or the draft");
        fixture.Draw(inspector);
        Check(inspector.Reload(fixture.library) && !inspector.HasConflict() && !inspector.IsDirty(), "Reload did not resolve the conflict");
        Check(Props(inspector.GetDraft())["damage"] == 40, "Reload did not read the external edit");

        // A clean draft follows the watcher; a dirty draft is preserved and warns instead.
        Props(external)["damage"] = 41;
        WriteText(path, external.dump());
        fixture.WaitFor([&]() { inspector.Refresh(fixture.library); return Props(inspector.GetDraft())["damage"] == 41; });
        Props(inspector.GetDraft())["damage"] = 42;
        Props(external)["damage"] = 43;
        WriteText(path, external.dump());
        fixture.WaitFor([&]() { inspector.Refresh(fixture.library); return inspector.HasConflict(); });
        Check(Props(inspector.GetDraft())["damage"] == 42, "The watcher discarded an unsaved draft");
        Check(inspector.Reload(fixture.library), "Could not resolve the watcher conflict");

        WriteText(path, "{broken JSON");
        Check(!inspector.Reload(fixture.library) && inspector.HasConflict(), "A broken template did not report an error");
        fixture.Draw(inspector);
        WriteText(path, external.dump());
        Check(inspector.Reload(fixture.library), "A repaired template did not recover");
        std::filesystem::remove(path);
        Check(!inspector.Reload(fixture.library), "A deleted template was silently recreated");
        fixture.Draw(inspector);
        WriteText(path, external.dump());
        Check(inspector.Reload(fixture.library), "A recreated template did not recover");
        inspector.Clear();
        Check(inspector.GetSelectedName().empty() && !inspector.IsDirty(), "Clearing the panel left stale state");
        Check(!inspector.Select(fixture.library, "../outside"), "The panel accepted path traversal");
        fixture.Draw(inspector);
    }

    void TestEmptyLibrary(Fixture& fixture)
    {
        const auto directory = fixture.directory / "empty";
        std::filesystem::create_directory(directory);
        myengine::scene::PrefabLibrary empty;
        Check(empty.Initialize(directory, fixture.logger.get()), "Could not initialize the empty library");
        ui::PrefabInspector inspector;
        fixture.Frame([&]() { inspector.Draw(empty, fixture.describe); });
        Check(inspector.GetSelectedName().empty() && !inspector.IsDirty(), "An empty library left a bogus selection");
        Check(!inspector.Save(empty) && !inspector.Reload(empty), "An empty panel saved or reloaded a template");
    }
}

int main()
{
    myengine::jobs::Initialize(2);
    int result = 1;
    try
    {
        Fixture fixture;
        try
        {
            TestWidgets(fixture);
            TestDraftAndSave(fixture);
            TestHierarchyAndDefaults(fixture);
            TestFailuresAndExternalEdits(fixture);
            TestEmptyLibrary(fixture);
            std::cout << "OK: T8 prefab panel, shared widgets, drafts, Save/Reset/Reload, next-spawn live props, children, conflicts and errors\n";
            result = 0;
        }
        catch (const std::exception& error)
        {
            fixture.keepArtifacts = true;
            std::cerr << "FAILED: " << error.what() << "\nLog: " << (fixture.directory / "test.log").u8string() << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    myengine::jobs::Shutdown();
    return result;
}
