// Manual integration check against the real Claude Code CLI (not part of CTest: it needs a login and spends money).
//
//   myengine_assistant_live [--dir <repository root>] [--session <id>] [--model <name>] [--stop-after <ms>]
//                           [--engine-tools [--approve all|none]] (--prompt-file <utf-8 file> | <prompt>...)
//
// Runs one turn through ClaudeCliBackend exactly as the editor does and prints the events as they arrive.
// --engine-tools also hosts a small headless scene (a Player, a few crates) with the engine tools and the MCP bridge;
// confirmations are answered by --approve (all: Apply, none: Reject). After the turn the scene is printed and the
// recorded undo steps are applied to show that every change can be taken back.
// Exit code 0: the turn finished without an error.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <myengine/assistant/AssistantBridge.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/assistant/ClaudeCliBackend.h>
#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>

namespace
{
    namespace assistant = myengine::assistant;
    namespace components = myengine::ecs::components;

    std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    void PrintScene(const myengine::ecs::World& world)
    {
        auto entities = world.GetEntities();
        std::sort(entities.begin(), entities.end());
        for (const auto entity : entities)
        {
            const auto* tag = world.TryGet<components::TagComponent>(entity);
            const auto* transform = world.TryGet<components::TransformComponent>(entity);
            std::cout << "  #" << entity << " " << (tag != nullptr ? tag->name : std::string("?"));
            if (transform != nullptr)
            {
                std::cout << " (" << transform->position.x << ", " << transform->position.y << ", " << transform->position.z << ")";
            }
            std::cout << '\n';
        }
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path directory = std::filesystem::current_path();
    std::string session;
    std::string model;
    std::string prompt;
    std::string approve = "all";
    int stopAfterMs = 0;
    bool engineTools = false;
    for (int index = 1; argv != nullptr && index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        const bool hasValue = index + 1 < argc;
        if (argument == L"--dir" && hasValue)
        {
            directory = std::filesystem::path(argv[++index]);
        }
        else if (argument == L"--session" && hasValue)
        {
            session = ToUtf8(argv[++index]);
        }
        else if (argument == L"--model" && hasValue)
        {
            model = ToUtf8(argv[++index]);
        }
        else if (argument == L"--stop-after" && hasValue)
        {
            stopAfterMs = std::stoi(ToUtf8(argv[++index]));
        }
        else if (argument == L"--engine-tools")
        {
            engineTools = true;
        }
        else if (argument == L"--approve" && hasValue)
        {
            approve = ToUtf8(argv[++index]);
        }
        else if (argument == L"--prompt-file" && hasValue)
        {
            std::ifstream file(std::filesystem::path(argv[++index]), std::ios::binary);
            prompt.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        else
        {
            prompt += (prompt.empty() ? "" : " ") + ToUtf8(argument);
        }
    }
    if (argv != nullptr)
    {
        LocalFree(argv);
    }
    if (prompt.empty())
    {
        std::cerr << "usage: myengine_assistant_live [--dir <root>] [--session <id>] [--model <name>] [--stop-after <ms>] "
                     "[--engine-tools [--approve all|none]] (--prompt-file <file> | <prompt>)\n";
        return 2;
    }

    myengine::jobs::Initialize(2);
    int exitCode = 1;
    {
        assistant::ClaudeCliConfig config;
        config.workingDirectory = directory;
        config.model = model;
        auto promptFile = directory / "assets" / "assistant" / "system_prompt.md";
        if (!std::filesystem::exists(promptFile))
        {
            promptFile = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets" / "assistant" / "system_prompt.md";
        }
        config.systemPromptFile = promptFile;

        // The optional headless scene with the engine tools and the bridge
        std::unique_ptr<myengine::core::Logger> logger;
        std::unique_ptr<myengine::scene::PrefabLibrary> prefabs;
        std::unique_ptr<myengine::ecs::World> world;
        std::unique_ptr<assistant::AssistantTools> tools;
        std::unique_ptr<assistant::AssistantBridge> bridge;
        std::vector<std::string> undoLabels;
        std::vector<std::string> undoSnapshots;
        if (engineTools)
        {
            logger = std::make_unique<myengine::core::Logger>();
            logger->Initialize(std::filesystem::temp_directory_path() / "myengine_assistant_live.log");
            prefabs = std::make_unique<myengine::scene::PrefabLibrary>();
            prefabs->Initialize(directory / "assets" / "prefabs", logger.get());
            world = std::make_unique<myengine::ecs::World>();
            const auto player = world->CreateEntity();
            world->Emplace<components::TagComponent>(player).name = "Player";
            world->Emplace<components::TransformComponent>(player).position = {4.0f, 0.5f, -2.0f};
            for (int index = 0; index < 2; ++index)
            {
                const auto crate = world->CreateEntity();
                world->Emplace<components::TagComponent>(crate).name = "Crate_" + std::to_string(index + 1);
                world->Emplace<components::TransformComponent>(crate).position = {static_cast<float>(index * 3), 0.0f, 5.0f};
            }

            assistant::AssistantToolContext context;
            context.world = world.get();
            context.prefabs = prefabs.get();
            context.scriptsDirectory = directory / "assets" / "scripts";
            context.captureSnapshot = [&]() { return myengine::scene::SerializeWorldToString(*world); };
            context.restoreSnapshot = [&](std::string_view text) { return myengine::scene::LoadWorldFromString(*world, text, nullptr); };
            context.recordUndo = [&](const std::string& label, const std::string& before)
            {
                undoLabels.push_back(label);
                undoSnapshots.push_back(before);
            };
            context.saveScene = []() { return true; };
            tools = std::make_unique<assistant::AssistantTools>(std::move(context));

            auto bridgeExecutable = std::filesystem::path();
            {
                wchar_t module[32768]{};
                GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
                const auto testDirectory = std::filesystem::path(module).parent_path();
                bridgeExecutable = testDirectory.parent_path().parent_path() / "app" / testDirectory.filename() / "myengine_mcp.exe";
            }
            assistant::AssistantBridgeConfig bridgeConfig;
            bridgeConfig.repositoryRoot = directory;
            bridge = std::make_unique<assistant::AssistantBridge>(*tools, bridgeConfig);
            std::string bridgeError;
            if (!std::filesystem::exists(bridgeExecutable) || !bridge->Start(bridgeError))
            {
                std::cerr << "The bridge is not available: " << (bridgeError.empty() ? bridgeExecutable.u8string() : bridgeError) << '\n';
                return 5;
            }
            config.bridge.executable = bridgeExecutable;
            config.bridge.pipeName = bridge->GetPipeName();
            config.bridge.token = bridge->GetToken();
            config.bridge.allowedTools = bridge->ReadOnlyToolNames();
            config.bridge.approveTool = assistant::AssistantBridge::ApproveToolFullName();
            bridge->onNotice = [](const std::string& text) { std::cout << "[card] " << text << '\n'; };
            std::cout << "scene before:\n";
            PrintScene(*world);
        }

        assistant::ClaudeCliBackend backend(config);
        const auto problem = backend.CheckAvailability();
        if (!problem.empty())
        {
            std::cerr << problem << '\n';
            return 3;
        }
        std::cout << "claude: " << backend.GetResolvedExecutable().u8string() << "\ncwd: " << directory.u8string() << "\n\n";

        std::string error;
        if (!backend.BeginTurn(prompt, session, error))
        {
            std::cerr << "BeginTurn failed: " << error << '\n';
            return 4;
        }

        std::vector<assistant::AssistantEvent> events;
        const auto begin = std::chrono::steady_clock::now();
        double longestPollMs = 0.0;
        bool stopped = false;
        std::size_t announcedCards = 0;
        while (backend.IsBusy())
        {
            if (stopAfterMs > 0 && !stopped && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() >= stopAfterMs)
            {
                const auto cancelBegin = std::chrono::steady_clock::now();
                backend.Cancel();
                stopped = true;
                std::cout << "\n[cancelled in " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelBegin).count()
                          << " ms, busy=" << backend.IsBusy() << "]\n";
                exitCode = 0;
                break;
            }
            if (bridge != nullptr)
            {
                bridge->Update();
                const auto pending = bridge->GetPending(); // a copy
                for (const auto& card : pending)
                {
                    std::cout << "\n[confirm] " << card.title << "  -> " << (approve == "all" ? "Apply" : "Reject") << '\n';
                    if (!card.detail.empty())
                    {
                        std::cout << card.detail << '\n';
                    }
                    bridge->Resolve(card.id, approve == "all");
                    ++announcedCards;
                }
            }
            events.clear();
            const auto pollBegin = std::chrono::steady_clock::now();
            backend.Poll(events);
            longestPollMs = std::max(longestPollMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pollBegin).count());
            for (const auto& event : events)
            {
                using Type = assistant::AssistantEventType;
                switch (event.type)
                {
                case Type::SessionStarted:
                    std::cout << "[session " << event.sessionId << ", model " << event.text << "]\n";
                    break;
                case Type::TextDelta:
                    std::cout << event.text << std::flush;
                    break;
                case Type::ToolUse:
                    std::cout << "\n[tool] " << event.toolName << "  " << event.text << '\n';
                    if (!event.detail.empty())
                    {
                        std::cout << event.detail;
                    }
                    break;
                case Type::ToolResult:
                    std::cout << "[result" << (event.isError ? " ERROR" : "") << "] " << event.text
                              << (event.filePath.empty() ? "" : "  -> changed " + event.filePath) << '\n';
                    break;
                case Type::Notice:
                    std::cout << "[notice] " << event.text << '\n';
                    break;
                case Type::Finished:
                    std::cout << "\n\n[finished" << (event.isError ? " WITH ERROR" : "") << "] " << event.durationMs << " ms, $" << event.costUsd << ", "
                              << event.inputTokens << " in / " << event.outputTokens << " out, session " << event.sessionId << '\n';
                    if (event.isError)
                    {
                        std::cout << event.text << '\n';
                    }
                    exitCode = event.isError ? 1 : 0;
                    break;
                case Type::Error:
                    std::cout << "\n[backend error] " << event.text << '\n';
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // one "frame"
        }
        const auto totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::cout << "[wall " << totalMs << " ms, longest Poll " << longestPollMs << " ms, cards " << announcedCards << "]\n";

        if (world != nullptr)
        {
            std::cout << "scene after:\n";
            PrintScene(*world);
            std::cout << "undo steps: " << undoLabels.size() << '\n';
            for (std::size_t index = undoLabels.size(); index > 0; --index)
            {
                myengine::scene::LoadWorldFromString(*world, undoSnapshots[index - 1], nullptr);
                std::cout << "  undo \"" << undoLabels[index - 1] << "\"\n";
            }
            if (!undoLabels.empty())
            {
                std::cout << "scene after undo:\n";
                PrintScene(*world);
            }
        }
        backend.Shutdown();
        if (bridge != nullptr)
        {
            bridge->Stop();
        }
        if (prefabs != nullptr)
        {
            prefabs->Shutdown();
        }
    }
    myengine::jobs::Shutdown();
    return exitCode;
}
