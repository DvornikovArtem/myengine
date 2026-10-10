// Stand-in for `claude -p --output-format stream-json`: lets the assistant tests run without the real CLI.
// FAKE_CLAUDE_MODE: normal (default), hang, crash, split, error_result

#include <windows.h>
#include <process.h>

#include <chrono>
#include <filesystem>
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
        // argv arrives in the ANSI code page, so a path with Cyrillic is not valid UTF-8
        std::cout << line.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
    }

    void Pause(const int milliseconds)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
}

int main(int argc, char** argv)
{
    // The CLI reads the whole stdin message before it answers
    const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

    std::string mode = "normal";
    char value[64]{};
    if (GetEnvironmentVariableA("FAKE_CLAUDE_MODE", value, sizeof(value)) > 0)
    {
        mode = value;
    }
    std::string arguments;
    for (int index = 1; index < argc; ++index)
    {
        arguments += argv[index];
        arguments += ' ';
    }
    const auto directory = std::filesystem::current_path().generic_u8string(); // UTF-8: the path has Cyrillic
    const std::string pid = std::to_string(_getpid());

    const json init = {{"type", "system"}, {"subtype", "init"}, {"session_id", "fake-session-1"}, {"model", "fake-" + pid}};

    if (mode == "crash")
    {
        std::cerr << "boom: unknown option '--foo'\n" << std::flush;
        return 3;
    }
    if (mode == "split")
    {
        const auto text = init.dump() + "\n";
        std::cout << text.substr(0, 20) << std::flush;
        Pause(150);
        std::cout << text.substr(20) << std::flush;
    }
    else
    {
        Emit(init);
    }
    if (mode == "hang")
    {
        Pause(60000);
        return 0;
    }

    const std::string answer = "Hello world. cwd=" + directory + " args=" + arguments + " stdin=" + std::to_string(input.size());
    Emit({{"type", "stream_event"}, {"event", {{"type", "message_start"}, {"message", {{"id", "msg_1"}}}}}});
    Emit({{"type", "stream_event"}, {"event", {{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "text_delta"}, {"text", "Hello world. "}}}}}});
    Emit({{"type", "stream_event"}, {"event", {{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "text_delta"}, {"text", answer.substr(13)}}}}}});
    Emit({{"type", "assistant"}, {"message", {{"id", "msg_1"}, {"content", json::array({{{"type", "text"}, {"text", answer}}})}}}, {"parent_tool_use_id", nullptr}});
    Emit({{"type", "assistant"}, {"message", {{"id", "msg_2"}, {"content", json::array({{
        {"type", "tool_use"}, {"id", "tool_1"}, {"name", "Edit"},
        {"input", {{"file_path", directory + "/assets/prefabs/coin.prefab.json"}, {"old_string", "\"spin_speed\": 120.0"}, {"new_string", "\"spin_speed\": 300.0"}}}}})}}}, {"parent_tool_use_id", nullptr}});
    Emit({{"type", "user"}, {"message", {{"content", json::array({{{"type", "tool_result"}, {"tool_use_id", "tool_1"}, {"content", "The file was updated."}}})}}}});
    if (mode == "error_result")
    {
        Emit({{"type", "result"}, {"subtype", "error_max_turns"}, {"is_error", true}, {"result", ""}, {"session_id", "fake-session-1"},
            {"total_cost_usd", 0.5}, {"duration_ms", 10}, {"num_turns", 41}, {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}});
        return 1;
    }
    Emit({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", answer}, {"session_id", "fake-session-1"},
        {"total_cost_usd", 0.0123}, {"duration_ms", 1500}, {"num_turns", 2},
        {"usage", {{"input_tokens", 10}, {"cache_read_input_tokens", 90}, {"output_tokens", 5}}}});
    return 0;
}
