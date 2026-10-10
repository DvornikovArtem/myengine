// myengine_mcp: MCP server on stdio that the Claude Code CLI starts (--mcp-config).
// It owns no tools: every request goes through the named pipe to the editor, which runs them on its main thread.
//
//   myengine_mcp --pipe \.\pipe\myengine-assistant-<pid>      (token in MYENGINE_BRIDGE_TOKEN)

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <myengine/assistant/McpBridgeCore.h>

namespace
{
    using json = nlohmann::json;

    class EditorPipe
    {
    public:
        explicit EditorPipe(std::wstring name) : name_(std::move(name)) {}

        ~EditorPipe()
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle_);
            }
        }

        json Request(json request)
        {
            std::string error;
            if (!EnsureConnected(error))
            {
                return {{"error", error}};
            }
            request["id"] = ++nextId_;
            if (!WriteLine(request.dump(-1, ' ', false, json::error_handler_t::replace)))
            {
                Close();
                return {{"error", "The editor closed the connection."}};
            }
            std::string line;
            while (ReadLine(line))
            {
                const json response = json::parse(line, nullptr, false);
                if (response.is_object() && response.contains("id") && response["id"] == nextId_ && response.contains("result"))
                {
                    return response["result"];
                }
            }
            Close();
            return {{"error", "The editor closed the connection."}};
        }

    private:
        bool EnsureConnected(std::string& error)
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                return true;
            }
            // The editor created the pipe before it started the CLI; a short retry covers slow starts
            for (int attempt = 0; attempt < 30; ++attempt)
            {
                handle_ = CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle_ != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                if (GetLastError() == ERROR_PIPE_BUSY)
                {
                    WaitNamedPipeW(name_.c_str(), 200);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if (handle_ == INVALID_HANDLE_VALUE)
            {
                error = "The myengine editor is not reachable (the assistant pipe is closed). Is the editor still running?";
                return false;
            }
            wchar_t token[256]{};
            const DWORD length = GetEnvironmentVariableW(L"MYENGINE_BRIDGE_TOKEN", token, 256);
            std::string tokenUtf8;
            for (DWORD index = 0; index < length && index < 256; ++index)
            {
                tokenUtf8 += static_cast<char>(token[index]); // hex digits only
            }
            std::string line;
            if (!WriteLine(json{{"op", "hello"}, {"token", tokenUtf8}}.dump()) || !ReadLine(line) || !json::parse(line, nullptr, false).is_object())
            {
                Close();
                error = "The editor refused the connection.";
                return false;
            }
            return true;
        }

        void Close()
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
            buffer_.clear();
        }

        bool WriteLine(const std::string& text)
        {
            const std::string data = text + "\n";
            std::size_t written = 0;
            while (written < data.size())
            {
                DWORD count = 0;
                if (!WriteFile(handle_, data.data() + written, static_cast<DWORD>(data.size() - written), &count, nullptr) || count == 0)
                {
                    return false;
                }
                written += count;
            }
            return true;
        }

        bool ReadLine(std::string& line)
        {
            while (true)
            {
                const auto end = buffer_.find('\n');
                if (end != std::string::npos)
                {
                    line = buffer_.substr(0, end);
                    buffer_.erase(0, end + 1);
                    return true;
                }
                char chunk[16384];
                DWORD count = 0;
                if (!ReadFile(handle_, chunk, sizeof(chunk), &count, nullptr) || count == 0)
                {
                    return false;
                }
                buffer_.append(chunk, count);
            }
        }

        std::wstring name_;
        HANDLE handle_ = INVALID_HANDLE_VALUE;
        std::string buffer_;
        std::uint64_t nextId_ = 0;
    };
}

int main()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::wstring pipeName;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int index = 1; argv != nullptr && index + 1 < argc; ++index)
    {
        if (std::wstring(argv[index]) == L"--pipe")
        {
            pipeName = argv[index + 1];
        }
    }
    if (argv != nullptr)
    {
        LocalFree(argv);
    }
    if (pipeName.empty())
    {
        std::fputs("usage: myengine_mcp --pipe <pipe name>\n", stderr);
        return 2;
    }

    EditorPipe editor(pipeName);
    const myengine::assistant::McpBridgeCore core([&editor](const json& request) { return editor.Request(request); });

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        const json message = json::parse(line, nullptr, false);
        const auto response = core.Handle(message);
        if (response.has_value())
        {
            const std::string text = response->dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
        }
    }
    return 0;
}
