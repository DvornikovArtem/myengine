// AI2: engine tools, MCP bridge (the real myengine_mcp.exe over a real named pipe) and confirmations.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>
#include <imgui/imgui.h>

#include <nlohmann/json.hpp>

#include <myengine/assistant/AssistantBridge.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/editor/EditorState.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/physics/PhysicsWorldState.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/ui/AssistantPanel.h>

namespace
{
    namespace assistant = myengine::assistant;
    namespace ecs = myengine::ecs;
    namespace components = myengine::ecs::components;
    namespace editor = myengine::editor;
    using json = nlohmann::json;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::filesystem::path ExecutableDirectory()
    {
        wchar_t executable[32768]{};
        Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
        return std::filesystem::path(executable).parent_path();
    }

    // The bridge is built next to the editor: build/app/<Config>/myengine_mcp.exe; the tests are in build/tests/<Config>
    std::filesystem::path BridgeExecutable()
    {
        const auto directory = ExecutableDirectory();
        return directory.parent_path().parent_path() / "app" / directory.filename() / "myengine_mcp.exe";
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine AI2 мост " + std::to_string(stamp));
            Check(std::filesystem::create_directories(root / "assets" / "scripts"), "Could not create the test directory");
            std::filesystem::create_directories(root / "assets" / "prefabs");
            std::filesystem::create_directories(root / "assets" / "scenes");
            std::ofstream(root / "assets" / "scripts" / "coin.py", std::ios::binary) << "import myengine as me\n\n\nclass Coin(me.Behaviour):\n    spin_speed: float = 90.0\n";
            std::ofstream(root / "assets" / "scripts" / "helper.py", std::ios::binary) << "x = 1\n";
            std::ofstream(root / "assets" / "prefabs" / "coin.prefab.json", std::ios::binary)
                << R"({"name":"Coin","entities":[{"id":1,"Tag":{"name":"Coin"},"Transform":{"position":[0,0.5,0],"rotationDeg":[0,0,0],"scale":[1,1,1]}}]})";

            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(root / "test.log"), "Logger initialization failed");
            Check(prefabs.Initialize(root / "assets" / "prefabs", logger.get()), "Prefab library initialization failed");

            auto& tag = world.Emplace<components::TagComponent>(player = world.CreateEntity());
            tag.name = "Player";
            world.Emplace<components::TransformComponent>(player).position = {1.0f, 0.0f, 2.0f};

            coinEntity = world.CreateEntity();
            world.Emplace<components::TagComponent>(coinEntity).name = "Coin_A";
            world.Emplace<components::TransformComponent>(coinEntity);
            auto& script = world.Emplace<components::ScriptComponent>(coinEntity);
            script.scripts.push_back({"coin", "Coin", json::object({{"spin_speed", 120.0}})});

            assistant::AssistantToolContext context;
            context.world = &world;
            context.prefabs = &prefabs;
            context.scriptsDirectory = root / "assets" / "scripts";
            context.captureSnapshot = [this]() { return myengine::scene::SerializeWorldToString(world); };
            context.restoreSnapshot = [this](std::string_view text) { return myengine::scene::LoadWorldFromString(world, text, nullptr); };
            context.recordUndo = [this](const std::string& label, const std::string& before)
            {
                undoLabels.push_back(label);
                undoSnapshots.push_back(before);
            };
            context.saveScene = [this]() { ++saves; return true; };
            context.describeScriptFields = [](const std::string& module, const std::string& className)
            {
                std::vector<myengine::scripting::ScriptFieldInfo> fields;
                if (module == "coin" && className == "Coin")
                {
                    fields.push_back({"spin_speed", myengine::scripting::ScriptFieldInfo::Type::Float, 90.0});
                    fields.push_back({"score_value", myengine::scripting::ScriptFieldInfo::Type::Int, 1});
                }
                return fields;
            };
            tools = std::make_unique<assistant::AssistantTools>(std::move(context));

            auto& state = myengine::core::ServiceLocator::GetEditorRuntimeState();
            state.mode = editor::RuntimeMode::Edit;
            state.selectedEntity = ecs::kInvalidEntity;
            state.playModeSnapshot.clear();
        }

        ~Fixture()
        {
            tools.reset();
            prefabs.Shutdown();
            logger.reset();
            auto& state = myengine::core::ServiceLocator::GetEditorRuntimeState();
            state.mode = editor::RuntimeMode::Edit;
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::size_t CountNamed(const std::string& name) const
        {
            std::size_t count = 0;
            for (const auto entity : world.GetEntities())
            {
                const auto* tag = world.TryGet<components::TagComponent>(entity);
                count += tag != nullptr && tag->name == name ? 1 : 0;
            }
            return count;
        }

        std::filesystem::path root;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::scene::PrefabLibrary prefabs;
        ecs::World world;
        ecs::EntityId player = ecs::kInvalidEntity;
        ecs::EntityId coinEntity = ecs::kInvalidEntity;
        std::unique_ptr<assistant::AssistantTools> tools;
        std::vector<std::string> undoLabels;
        std::vector<std::string> undoSnapshots;
        int saves = 0;
    };

    void TestToolsDirectly(Fixture& fixture)
    {
        auto& tools = *fixture.tools;

        const auto mode = tools.Call("get_mode", json::object());
        Check(mode.ok && mode.value["mode"] == "Edit" && mode.value["entityCount"] == 2, "get_mode is wrong");

        const auto listed = tools.Call("list_entities", {{"filter", "coin"}});
        Check(listed.ok && listed.value["matched"] == 1 && listed.value["entities"][0]["name"] == "Coin_A", "list_entities filter is wrong");
        Check(listed.value["entities"][0]["components"].size() == 2, "list_entities lost the component list");

        const auto entity = tools.Call("get_entity", {{"name", "Player"}});
        Check(entity.ok && entity.value["Transform"]["position"][2] == 2.0, "get_entity by name is wrong");
        Check(!tools.Call("get_entity", {{"id", 999}}).ok && !tools.Call("get_entity", json::object()).ok, "A missing entity was not reported");

        const auto prefabs = tools.Call("list_prefabs", json::object());
        Check(prefabs.ok && prefabs.value["prefabs"].size() == 1 && prefabs.value["prefabs"][0] == "coin", "list_prefabs is wrong");
        const auto prefab = tools.Call("get_prefab", {{"name", "coin"}});
        Check(prefab.ok && prefab.value["prefab"]["entities"].size() == 1, "get_prefab is wrong");
        Check(!tools.Call("get_prefab", {{"name", "nothing"}}).ok, "A missing prefab was not reported");

        const auto scripts = tools.Call("list_scripts", json::object());
        Check(scripts.ok && scripts.value["scripts"].size() == 2 && scripts.value["scripts"][0]["module"] == "coin" &&
            scripts.value["scripts"][0]["classes"][0] == "Coin", "list_scripts is wrong");
        const auto fields = tools.Call("describe_script_fields", {{"module", "coin"}, {"class", "Coin"}});
        Check(fields.ok && fields.value["fields"].size() == 2 && fields.value["fields"][0]["type"] == "float", "describe_script_fields is wrong");
        Check(!tools.Call("describe_script_fields", {{"module", "coin"}, {"class", "Nope"}}).ok, "Unknown script class was not reported");
        Check(tools.Call("get_recent_errors", json::object()).ok, "get_recent_errors failed");
        Check(!tools.Call("no_such_tool", json::object()).ok, "An unknown tool was accepted");

        // Changes: undo snapshots, validation, all-or-nothing
        const auto before = fixture.world.GetEntities().size();
        const auto spawn = tools.Call("spawn_prefab", {{"name", "coin"}, {"positions", json::array({json::array({1, 0, 1}), json::array({2, 0, 2}), json::array({3, 0, 3})})}});
        Check(spawn.ok && spawn.value["spawned"].size() == 3 && fixture.world.GetEntities().size() == before + 3, "spawn_prefab did not create 3 entities");
        Check(fixture.undoLabels.size() == 1 && fixture.undoLabels[0] == "Assistant: Spawn coin", "spawn_prefab left no undo step");
        Check(fixture.CountNamed("Coin") == 3, "Spawned entities have the wrong name");
        {
            // Undo: the editor restores the snapshot that was taken before the call
            Check(myengine::scene::LoadWorldFromString(fixture.world, fixture.undoSnapshots[0], nullptr), "Restore failed");
            Check(fixture.world.GetEntities().size() == before, "Undo did not remove the spawned entities");
        }
        fixture.undoLabels.clear();
        fixture.undoSnapshots.clear();

        const auto failedSpawn = tools.Call("spawn_prefab", {{"name", "missing_prefab"}, {"position", json::array({0, 0, 0})}});
        Check(!failedSpawn.ok && fixture.world.GetEntities().size() == before && fixture.undoLabels.empty(), "A failed spawn changed the scene");
        Check(!tools.Call("spawn_prefab", {{"name", "coin"}, {"position", json::array({0, 0})}}).ok, "A bad position was accepted");

        const auto created = tools.Call("create_entity", {{"name", "Marker"}, {"position", json::array({5, 6, 7})}, {"components", {{"Rigidbody", {{"mass", 2.0}}}}}});
        Check(created.ok && fixture.CountNamed("Marker") == 1, "create_entity failed");
        Check(!tools.Call("create_entity", {{"components", {{"Hierarchy", {{"parent", 1}}}}}}).ok, "Hierarchy must not be accepted by create_entity");

        const auto props = tools.Call("set_script_props", {{"name", "Coin_A"}, {"props", {{"spin_speed", 300.0}}}});
        Check(props.ok && props.value["props"]["spin_speed"] == 300.0, "set_script_props failed");
        Check(fixture.world.TryGet<components::ScriptComponent>(fixture.coinEntity)->scripts[0].props["spin_speed"] == 300.0, "Props were not stored");
        const auto typo = tools.Call("set_script_props", {{"name", "Coin_A"}, {"props", {{"spin_spedd", 1.0}}}});
        Check(!typo.ok && typo.message.find("spin_speed") != std::string::npos, "A typo in a prop name was accepted");
        Check(!tools.Call("set_script_props", {{"name", "Coin_A"}, {"props", {{"spin_speed", "fast"}}}}).ok, "A wrong prop type was accepted");

        const auto component = tools.Call("set_component", {{"id", fixture.player}, {"component", "Transform"}, {"value", {{"position", json::array({9, 9, 9})}}}});
        Check(component.ok && fixture.world.TryGet<components::TransformComponent>(fixture.player)->position.x == 9.0f, "set_component failed");
        Check(component.value["value"]["scale"][0] == 1.0, "set_component lost the fields it did not change");
        Check(!tools.Call("set_component", {{"id", fixture.player}, {"component", "Hierarchy"}, {"value", {{"parent", 1}}}}).ok, "Hierarchy must not be changed by set_component");

        const auto deleted = tools.Call("delete_entity", {{"name", "Marker"}});
        Check(deleted.ok && fixture.CountNamed("Marker") == 0, "delete_entity failed");

        // Play: changes are refused, stop restores the scene
        Check(tools.Call("play", json::object()).ok, "play failed");
        Check(myengine::core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play, "play did not switch the mode");
        const auto refused = tools.Call("spawn_prefab", {{"name", "coin"}});
        Check(!refused.ok && refused.message.find("Edit mode") != std::string::npos, "A change was accepted in Play");
        Check(tools.Call("get_mode", json::object()).value["mode"] == "Play", "get_mode does not show Play");
        Check(tools.Call("stop", json::object()).ok && myengine::core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Edit, "stop failed");
        Check(!tools.Call("stop", json::object()).ok, "stop worked twice");
        Check(tools.Call("save_scene", json::object()).ok && fixture.saves == 1, "save_scene failed");

        Check(tools.Describe("spawn_prefab", {{"name", "coin"}, {"positions", json::array({json::array({1, 0, 1}), json::array({2, 0, 2})})}}).find("x2") != std::string::npos,
            "Describe does not mention the count");
    }

    // A real child process: myengine_mcp.exe with its stdin / stdout on pipes
    class McpClient
    {
    public:
        McpClient(const std::filesystem::path& executable, const std::wstring& pipeName, const std::string& token)
        {
            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;
            HANDLE stdinRead = nullptr;
            HANDLE stdoutWrite = nullptr;
            Check(CreatePipe(&stdinRead, &stdinWrite_, &inheritable, 0) && CreatePipe(&stdoutRead_, &stdoutWrite, &inheritable, 0), "CreatePipe failed");
            SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
            SetEnvironmentVariableW(L"MYENGINE_BRIDGE_TOKEN", std::wstring(token.begin(), token.end()).c_str());

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = stdinRead;
            startup.hStdOutput = stdoutWrite;
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            std::wstring commandLine = L"\"" + executable.wstring() + L"\" --pipe " + pipeName;
            PROCESS_INFORMATION information{};
            Check(CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information) != FALSE,
                "Could not start myengine_mcp.exe");
            process_ = information.hProcess;
            CloseHandle(information.hThread);
            CloseHandle(stdinRead);
            CloseHandle(stdoutWrite);
        }

        ~McpClient()
        {
            Terminate();
            CloseHandle(stdinWrite_);
            CloseHandle(stdoutRead_);
            CloseHandle(process_);
        }

        void Send(const json& message)
        {
            const std::string line = message.dump() + "\n";
            DWORD written = 0;
            Check(WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE, "Could not write to the bridge");
        }

        bool TryRead(json& message)
        {
            DWORD available = 0;
            if (PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr) && available > 0)
            {
                char chunk[8192];
                DWORD read = 0;
                if (ReadFile(stdoutRead_, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr))
                {
                    buffer_.append(chunk, read);
                }
            }
            const auto end = buffer_.find('\n');
            if (end == std::string::npos)
            {
                return false;
            }
            message = json::parse(buffer_.substr(0, end));
            buffer_.erase(0, end + 1);
            return true;
        }

        void Terminate()
        {
            if (process_ != nullptr)
            {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 2000);
            }
        }

    private:
        HANDLE process_ = nullptr;
        HANDLE stdinWrite_ = nullptr;
        HANDLE stdoutRead_ = nullptr;
        std::string buffer_;
    };

    // The editor side polls from "the main thread" (this thread) while the bridge waits for the answer
    json Await(McpClient& client, assistant::AssistantBridge& bridge, const int timeoutMs = 15000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        json message;
        while (std::chrono::steady_clock::now() < deadline)
        {
            bridge.Update();
            if (client.TryRead(message))
            {
                return message;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("Timed out waiting for the bridge");
    }

    json Request(McpClient& client, assistant::AssistantBridge& bridge, const int id, const std::string& method, const json& params)
    {
        client.Send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        const auto response = Await(client, bridge);
        Check(response.contains("id") && response["id"] == id, "The response id does not match the request");
        return response;
    }

    json CallTool(McpClient& client, assistant::AssistantBridge& bridge, const int id, const std::string& tool, const json& arguments)
    {
        return Request(client, bridge, id, "tools/call", {{"name", tool}, {"arguments", arguments}});
    }

    std::string ResultText(const json& response)
    {
        return response.at("result").at("content").at(0).at("text").get<std::string>();
    }

    bool IsError(const json& response)
    {
        return response.at("result").value("isError", false);
    }

    void PumpUntilPending(assistant::AssistantBridge& bridge, const std::size_t count)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (bridge.GetPending().size() < count)
        {
            Check(std::chrono::steady_clock::now() < deadline, "The confirmation never reached the editor");
            bridge.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void TestBridge(Fixture& fixture)
    {
        const auto executable = BridgeExecutable();
        Check(std::filesystem::exists(executable), "myengine_mcp.exe was not built (expected next to the editor build)");

        assistant::AssistantBridgeConfig config;
        config.repositoryRoot = fixture.root;
        config.pipeName = L"\\\\.\\pipe\\myengine-assistant-test-" + std::to_wstring(GetCurrentProcessId());
        assistant::AssistantBridge bridge(*fixture.tools, config);
        std::vector<std::string> notices;
        bridge.onNotice = [&](const std::string& text) { notices.push_back(text); };
        std::string error;
        Check(bridge.Start(error), error.c_str());
        {
            assistant::AssistantBridge second(*fixture.tools, config);
            Check(!second.Start(error), "A second server took the same pipe name");
        }

        const auto readOnly = bridge.ReadOnlyToolNames();
        Check(std::find(readOnly.begin(), readOnly.end(), "mcp__myengine__list_entities") != readOnly.end() &&
            std::find(readOnly.begin(), readOnly.end(), "mcp__myengine__spawn_prefab") == readOnly.end(), "The read-only tool list is wrong");
        Check(assistant::AssistantBridge::ApproveToolFullName() == "mcp__myengine__approve", "The approve tool name is wrong");

        // A client with a wrong token is dropped and gets nothing
        {
            McpClient intruder(executable, config.pipeName, "not-the-token");
            intruder.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "get_mode"}, {"arguments", json::object()}}}});
            const auto response = Await(intruder, bridge);
            Check(IsError(response), "A client with a wrong token was served");
        }

        McpClient client(executable, config.pipeName, bridge.GetToken());
        int id = 100;

        const auto initialize = Request(client, bridge, ++id, "initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", json::object()}});
        Check(initialize["result"]["serverInfo"]["name"] == "myengine" && initialize["result"]["protocolVersion"] == "2025-03-26" &&
            initialize["result"]["capabilities"].contains("tools"), "initialize is wrong");
        client.Send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

        const auto list = Request(client, bridge, ++id, "tools/list", json::object());
        std::vector<std::string> names;
        for (const auto& tool : list["result"]["tools"])
        {
            names.push_back(tool["name"].get<std::string>());
            Check(tool["inputSchema"]["type"] == "object", "A tool has no input schema");
        }
        Check(names.size() == 17 && std::find(names.begin(), names.end(), "approve") != names.end() &&
            std::find(names.begin(), names.end(), "spawn_prefab") != names.end(), "tools/list is wrong");
        Check(Request(client, bridge, ++id, "no/such/method", json::object()).contains("error"), "An unknown method was not an error");

        // A read tool needs no confirmation
        const auto mode = CallTool(client, bridge, ++id, "get_mode", json::object());
        Check(!IsError(mode) && json::parse(ResultText(mode))["mode"] == "Edit" && bridge.GetPending().empty(), "get_mode through the bridge failed");
        const auto entities = CallTool(client, bridge, ++id, "list_entities", json::object());
        Check(json::parse(ResultText(entities))["total"] == 2, "list_entities through the bridge failed");

        // A change without a confirmation is refused
        const auto unconfirmed = CallTool(client, bridge, ++id, "spawn_prefab", {{"name", "coin"}, {"position", json::array({0, 0, 0})}});
        Check(IsError(unconfirmed) && ResultText(unconfirmed).find("not confirmed") != std::string::npos && fixture.CountNamed("Coin") == 0,
            "A change ran without a confirmation");

        // Confirm, then the call goes through and leaves an undo step
        const json spawnArguments = {{"name", "coin"}, {"positions", json::array({json::array({1, 0, 0}), json::array({2, 0, 0}), json::array({3, 0, 0})})}};
        client.Send({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
            {"params", {{"name", "approve"}, {"arguments", {{"tool_name", "mcp__myengine__spawn_prefab"}, {"input", spawnArguments}, {"tool_use_id", "toolu_1"}}}}}});
        PumpUntilPending(bridge, 1);
        Check(bridge.GetPending()[0].title.find("Spawn prefab 'coin' x3") != std::string::npos &&
            bridge.GetPending()[0].detail.find("\"positions\"") != std::string::npos, "The confirmation card is wrong");
        json premature;
        Check(!client.TryRead(premature), "The permission answer arrived before the user decided");
        bridge.Resolve(bridge.GetPending()[0].id, true);
        const auto allow = Await(client, bridge);
        const auto verdict = json::parse(ResultText(allow));
        Check(verdict["behavior"] == "allow" && verdict["updatedInput"]["name"] == "coin", "The allow verdict is wrong");
        const auto spawned = CallTool(client, bridge, ++id, "spawn_prefab", spawnArguments);
        Check(!IsError(spawned) && fixture.CountNamed("Coin") == 3 && fixture.undoLabels.size() == 1, "The confirmed spawn did not happen");
        Check(IsError(CallTool(client, bridge, ++id, "spawn_prefab", spawnArguments)) && fixture.CountNamed("Coin") == 3, "One confirmation covered two calls");
        Check(notices.size() == 1 && notices[0].rfind("Applied: ", 0) == 0, "No 'Applied' note was produced");

        // Reject: the model gets a refusal with a reason, nothing changes
        client.Send({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
            {"params", {{"name", "approve"}, {"arguments", {{"tool_name", "mcp__myengine__delete_entity"}, {"input", {{"name", "Player"}}}}}}}});
        PumpUntilPending(bridge, 1);
        Check(bridge.GetPending()[0].title == "Delete entity 'Player' with its children", "The delete card is wrong");
        bridge.Resolve(bridge.GetPending()[0].id, false);
        const auto deny = json::parse(ResultText(Await(client, bridge)));
        Check(deny["behavior"] == "deny" && deny["message"].get<std::string>().find("rejected") != std::string::npos, "The deny verdict is wrong");
        Check(IsError(CallTool(client, bridge, ++id, "delete_entity", {{"name", "Player"}})) && fixture.CountNamed("Player") == 1, "A rejected delete happened");

        // Files: scripts and prefabs get a card with a diff, everything else is refused at once
        const std::string scriptPath = (fixture.root / "assets" / "scripts" / "coin.py").u8string();
        client.Send({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
            {"params", {{"name", "approve"}, {"arguments", {{"tool_name", "Edit"},
                {"input", {{"file_path", scriptPath}, {"old_string", "90.0"}, {"new_string", "300.0"}}}}}}}});
        PumpUntilPending(bridge, 1);
        Check(bridge.GetPending()[0].title == "Edit assets/scripts/coin.py" && bridge.GetPending()[0].detail == "- 90.0\n+ 300.0\n", "The edit card is wrong");
        bridge.Resolve(bridge.GetPending()[0].id, true);
        Check(json::parse(ResultText(Await(client, bridge)))["behavior"] == "allow", "An edit of a script was not allowed");

        const auto refuse = [&](const json& input, const std::string& tool, const char* why)
        {
            const auto response = CallTool(client, bridge, ++id, "approve", {{"tool_name", tool}, {"input", input}});
            const auto parsed = json::parse(ResultText(response));
            Check(parsed["behavior"] == "deny" && bridge.GetPending().empty(), why);
        };
        refuse({{"file_path", (fixture.root / "assets" / "scenes" / "level.json").u8string()}}, "Write", "A scene file was accepted");
        refuse({{"file_path", "assets/scripts/../scenes/level.json"}}, "Edit", "A path with .. was accepted");
        refuse({{"file_path", (fixture.root / "assets" / "scripts" / "notes.txt").u8string()}}, "Write", "A non-.py file in scripts was accepted");
        refuse({{"file_path", (fixture.root / "engine" / "core.cpp").u8string()}}, "Edit", "C++ was accepted");
        refuse({{"file_path", "C:\\Windows\\win.ini"}}, "Read", "A read outside the repository was accepted");
        refuse({{"command", "dir"}}, "Bash", "Bash was accepted");
        refuse({}, "mcp__myengine__no_such_tool", "An unknown engine tool was accepted");
        const auto readInside = json::parse(ResultText(CallTool(client, bridge, ++id, "approve",
            {{"tool_name", "Read"}, {"input", {{"file_path", scriptPath}}}})));
        Check(readInside["behavior"] == "allow", "A read inside the repository was refused");
        const auto readTool = json::parse(ResultText(CallTool(client, bridge, ++id, "approve",
            {{"tool_name", "mcp__myengine__list_entities"}, {"input", json::object()}})));
        Check(readTool["behavior"] == "allow" && bridge.GetPending().empty(), "An engine read tool needed a confirmation");

        // The CLI is stopped while a card is open: the card disappears and its confirmation is void
        client.Send({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
            {"params", {{"name", "approve"}, {"arguments", {{"tool_name", "mcp__myengine__save_scene"}, {"input", json::object()}}}}}});
        PumpUntilPending(bridge, 1);
        client.Terminate();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!bridge.GetPending().empty() || bridge.IsRunning() == false)
        {
            Check(std::chrono::steady_clock::now() < deadline, "The card stayed after the bridge process ended");
            bridge.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // A new bridge connects to the same pipe
        McpClient again(executable, config.pipeName, bridge.GetToken());
        const auto second = CallTool(again, bridge, 1, "get_mode", json::object());
        Check(!IsError(second), "The pipe did not accept a new bridge");
        bridge.RejectAll("done");
    }

    class PanelFixture
    {
    public:
        PanelFixture()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(900.0f, 700.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~PanelFixture()
        {
            ImGui::DestroyContext(context);
        }

        void Frame(myengine::ui::AssistantPanel& panel)
        {
            panel.Update();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            ImGui::Begin("Assistant test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            panel.Draw();
            ImGui::End();
            ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "The panel did not draw anything");
        }

        ImGuiContext* context = nullptr;
    };

    // The card of a pending confirmation is drawn, and it goes away (as a refusal) when the turn is stopped
    void TestPanelCards(Fixture& fixture)
    {
        const auto executable = BridgeExecutable();
        const auto fake = ExecutableDirectory() / "myengine_fake_claude.exe";
        Check(std::filesystem::exists(fake), "myengine_fake_claude.exe was not built next to the test");
        SetEnvironmentVariableW(L"MYENGINE_CLAUDE_PATH", fake.wstring().c_str());
        SetEnvironmentVariableW(L"FAKE_CLAUDE_MODE", L"hang");

        PanelFixture imgui;
        myengine::ui::AssistantPanelConfig config;
        config.repositoryRoot = fixture.root;
        config.tools = fixture.tools.get();
        config.bridgeExecutable = executable;
        myengine::ui::AssistantPanel panel(config);
        auto* bridge = panel.GetBridge();
        Check(bridge != nullptr && bridge->IsRunning(), "The panel did not start the bridge");

        Check(panel.GetService().Send("edit the coin script"), "Send failed");
        McpClient client(executable, bridge->GetPipeName(), bridge->GetToken());
        const std::string scriptPath = (fixture.root / "assets" / "scripts" / "coin.py").u8string();
        client.Send({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
            {"params", {{"name", "approve"}, {"arguments", {{"tool_name", "Edit"},
                {"input", {{"file_path", scriptPath}, {"old_string", "90.0"}, {"new_string", "300.0"}}}}}}}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (bridge->GetPending().empty())
        {
            Check(std::chrono::steady_clock::now() < deadline, "The card never appeared");
            imgui.Frame(panel);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        imgui.Frame(panel);
        imgui.Frame(panel);
        Check(bridge->GetPending().size() == 1 && bridge->GetPending()[0].title == "Edit assets/scripts/coin.py", "The card is wrong");

        panel.GetService().Cancel(); // Stop
        imgui.Frame(panel);
        Check(bridge->GetPending().empty(), "The card stayed after Stop");
        const auto response = Await(client, *bridge);
        const auto verdict = json::parse(ResultText(response));
        Check(verdict["behavior"] == "deny", "A card closed by Stop was not refused");
        bool noted = false;
        for (const auto& message : panel.GetService().GetMessages())
        {
            noted = noted || message.text.rfind("Rejected: ", 0) == 0;
        }
        Check(noted, "The transcript has no note about the closed card");

        SetEnvironmentVariableW(L"MYENGINE_CLAUDE_PATH", nullptr);
        SetEnvironmentVariableW(L"FAKE_CLAUDE_MODE", nullptr);
    }
}

int main()
{
    myengine::jobs::Initialize(2);
    int result = 1;
    try
    {
        {
            Fixture fixture;
            const auto run = [&](const char* name, const auto& test) { test(); std::cout << "  ok: " << name << '\n'; };
            run("tools", [&] { TestToolsDirectly(fixture); });
        }
        {
            Fixture fixture;
            const auto run = [&](const char* name, const auto& test) { test(); std::cout << "  ok: " << name << '\n'; };
            run("bridge, confirmations, permissions", [&] { TestBridge(fixture); });
        }
        {
            Fixture fixture;
            const auto run = [&](const char* name, const auto& test) { test(); std::cout << "  ok: " << name << '\n'; };
            run("panel cards", [&] { TestPanelCards(fixture); });
        }
        std::cout << "OK: AI2 engine tools, undo steps, MCP bridge over a named pipe, confirmations, file and tool permissions\n";
        result = 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    myengine::jobs::Shutdown();
    return result;
}
