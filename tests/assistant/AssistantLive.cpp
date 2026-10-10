// Manual integration check against the real Claude Code CLI (not part of CTest: it needs a login and spends money).
//
//   myengine_assistant_live [--dir <repository root>] [--session <id>] [--model <name>] (--prompt-file <utf-8 file> | <prompt>...)
//
// Runs one turn through ClaudeCliBackend exactly as the editor does and prints the events as they arrive.
// Exit code 0: the turn finished without an error.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <myengine/assistant/ClaudeCliBackend.h>

namespace
{
    namespace assistant = myengine::assistant;

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
    int stopAfterMs = 0;
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
        std::cerr << "usage: myengine_assistant_live [--dir <root>] [--session <id>] [--model <name>] (--prompt-file <file> | <prompt>)\n";
        return 2;
    }

    assistant::ClaudeCliConfig config;
    config.workingDirectory = directory;
    config.model = model;
    auto promptFile = directory / "assets" / "assistant" / "system_prompt.md";
    if (!std::filesystem::exists(promptFile))
    {
        promptFile = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets" / "assistant" / "system_prompt.md";
    }
    config.systemPromptFile = promptFile;

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

    int exitCode = 1;
    std::vector<assistant::AssistantEvent> events;
    const auto begin = std::chrono::steady_clock::now();
    double longestPollMs = 0.0;
    bool stopped = false;
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
    std::cout << "[wall " << totalMs << " ms, longest Poll " << longestPollMs << " ms]\n";
    return exitCode;
}
