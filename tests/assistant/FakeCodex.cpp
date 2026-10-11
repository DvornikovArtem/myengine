// Stand-in for `codex exec --json`: lets the assistant tests run without the real CLI.
// FAKE_CODEX_MODE: normal (default), hang, crash, fail, plain

#include <windows.h>

#include <chrono>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace
{
    using json = nlohmann::json;

    void Emit(const json& line)
    {
        std::cout << line.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
    }
}

int main(int argc, char** argv)
{
    // `codex exec ... -` reads the whole prompt from stdin
    const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

    std::string mode = "normal";
    char value[64]{};
    if (GetEnvironmentVariableA("FAKE_CODEX_MODE", value, sizeof(value)) > 0)
    {
        mode = value;
    }

    std::string arguments;
    std::string resumeId;
    for (int index = 1; index < argc; ++index)
    {
        arguments += argv[index];
        arguments += ' ';
        if (std::string(argv[index]) == "resume" && index + 1 < argc)
        {
            resumeId = argv[index + 1];
        }
    }
    const std::string thread = resumeId.empty() ? "fake-thread-1" : resumeId;

    if (mode == "crash")
    {
        std::cerr << "Error: not logged in. Please run `codex login`.\n" << std::flush;
        return 3;
    }

    // Banners and warnings between the JSON lines must be skipped by the parser
    std::cout << "Reading prompt from stdin...\n" << std::flush;
    Emit({{"type", "thread.started"}, {"thread_id", thread}});
    Emit({{"type", "turn.started"}});

    if (mode == "hang")
    {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "fail")
    {
        Emit({{"type", "error"}, {"message", "Reconnecting... 1/5"}});
        Emit({{"type", "turn.failed"}, {"error", {{"message", "The model is overloaded"}}}});
        return 1;
    }

    Emit({{"type", "item.completed"}, {"item", {{"id", "item_0"}, {"type", "reasoning"}, {"text", "Thinking about the request"}}}});
    Emit({{"type", "item.started"}, {"item", {{"id", "item_1"}, {"type", "command_execution"}, {"command", "bash -lc ls"},
                                              {"aggregated_output", ""}, {"status", "in_progress"}}}});
    Emit({{"type", "item.completed"}, {"item", {{"id", "item_1"}, {"type", "command_execution"}, {"command", "bash -lc ls"},
                                                {"aggregated_output", "assets\nengine\n"}, {"exit_code", 0}, {"status", "completed"}}}});
    const json editArguments = {{"path", "assets/scripts/coin.py"}, {"old_text", "90.0"}, {"new_text", "300.0"}};
    Emit({{"type", "item.started"}, {"item", {{"id", "item_2"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "edit_file"},
                                              {"arguments", editArguments}, {"status", "in_progress"}}}});
    Emit({{"type", "item.completed"}, {"item", {{"id", "item_2"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "edit_file"},
                                                {"arguments", editArguments}, {"status", "completed"},
                                                {"result", {{"content", json::array({{{"type", "text"}, {"text", "Done: assets/scripts/coin.py was written."}}})}}}}}});
    Emit({{"type", "item.started"}, {"item", {{"id", "item_3"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "get_mode"},
                                              {"arguments", json::object()}, {"status", "in_progress"}}}});
    Emit({{"type", "item.completed"}, {"item", {{"id", "item_3"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "get_mode"},
                                                {"arguments", json::object()}, {"status", "failed"},
                                                {"error", {{"message", "The user rejected this action."}}}}}});
    Emit({{"type", "item.completed"}, {"item", {{"id", "item_4"}, {"type", "future_item_kind"}, {"payload", 1}}}});

    // The answer shows what the backend passed: the arguments and the head of the prompt
    const std::string head = input.substr(0, 40);
    Emit({{"type", "item.completed"}, {"item", {{"id", "item_5"}, {"type", "agent_message"},
                                                {"text", "answer: " + arguments + "| stdin: " + head}}}});
    Emit({{"type", "turn.completed"}, {"usage", {{"input_tokens", 1200}, {"cached_input_tokens", 800}, {"output_tokens", 34}, {"reasoning_output_tokens", 6}}}});
    return 0;
}
