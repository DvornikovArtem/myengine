// myengine_mcp: MCP server on stdio that the Claude Code CLI starts (--mcp-config).
// It owns no tools: every request goes through the named pipe to the editor, which runs them on its main thread.
//
//   myengine_mcp --pipe \.\pipe\myengine-assistant-<pid>      (token in MYENGINE_BRIDGE_TOKEN)
//   myengine_mcp [--endpoint <file>]                          (an external client: pipe and token from the endpoint file
//                                                              that the editor writes: %APPDATA%\myengine\mcp_endpoint.json,
//                                                              or MYENGINE_MCP_ENDPOINT)

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <myengine/assistant/McpBridgeCore.h>

namespace
{
    using json = nlohmann::json;

    std::string Narrow(const std::wstring& text)
    {
        std::string result;
        for (const wchar_t c : text)
        {
            result += static_cast<char>(c); // pipe names and tokens are ASCII
        }
        return result;
    }

    std::wstring EnvironmentText(const wchar_t* name)
    {
        wchar_t buffer[2048]{};
        const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        return length > 0 && length < std::size(buffer) ? std::wstring(buffer, length) : std::wstring();
    }

    // The editor writes the pipe name and the token of its running bridge here when external clients are allowed
    std::wstring DefaultEndpointFile()
    {
        if (const auto fromEnvironment = EnvironmentText(L"MYENGINE_MCP_ENDPOINT"); !fromEnvironment.empty())
        {
            return fromEnvironment;
        }
        const auto appData = EnvironmentText(L"APPDATA");
        return appData.empty() ? std::wstring() : appData + L"\\myengine\\mcp_endpoint.json";
    }

    bool ReadEndpoint(const std::wstring& file, std::wstring& pipe, std::string& token, std::string& error)
    {
        std::ifstream stream(std::filesystem::path(file), std::ios::binary);
        if (file.empty() || !stream)
        {
            error = "The myengine editor does not accept external MCP clients: turn on \"Allow external MCP clients\" in its "
                    "Assistant tab and keep the editor open.";
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const json endpoint = json::parse(text, nullptr, false);
        if (!endpoint.is_object() || !endpoint.contains("pipe") || !endpoint["pipe"].is_string() || !endpoint.contains("token") ||
            !endpoint["token"].is_string())
        {
            error = "The endpoint file of the myengine editor is not valid.";
            return false;
        }
        const std::string name = endpoint["pipe"].get<std::string>();
        pipe.assign(name.begin(), name.end());
        token = endpoint["token"].get<std::string>();
        return true;
    }

    class EditorPipe
    {
    public:
        // `name` empty: the pipe and the token are read from `endpointFile` at every connection, so that a restart of the
        // editor (a new pipe, a new token) does not need a restart of the client
        EditorPipe(std::wstring name, std::wstring endpointFile) : name_(std::move(name)), endpointFile_(std::move(endpointFile)) {}

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
            std::wstring pipe = name_;
            std::string token;
            if (pipe.empty())
            {
                if (!ReadEndpoint(endpointFile_, pipe, token, error))
                {
                    return false;
                }
            }
            // The editor created the pipe before it started the CLI; a short retry covers slow starts
            for (int attempt = 0; attempt < 30; ++attempt)
            {
                handle_ = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle_ != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                if (GetLastError() == ERROR_PIPE_BUSY)
                {
                    WaitNamedPipeW(pipe.c_str(), 200);
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
            if (token.empty())
            {
                token = Narrow(EnvironmentText(L"MYENGINE_BRIDGE_TOKEN")); // hex digits only
            }
            std::string line;
            if (!WriteLine(json{{"op", "hello"}, {"token", token}}.dump()) || !ReadLine(line) || !json::parse(line, nullptr, false).is_object())
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
        std::wstring endpointFile_;
        HANDLE handle_ = INVALID_HANDLE_VALUE;
        std::string buffer_;
        std::uint64_t nextId_ = 0;
    };
}

int main()
{
    // The protocol is bytes: a text mode would turn LF into CRLF and cut the stream at Ctrl+Z
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        return 1;
    }

    std::wstring pipeName;
    std::wstring endpointFile;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int index = 1; argv != nullptr && index + 1 < argc; ++index)
    {
        if (std::wstring(argv[index]) == L"--pipe")
        {
            pipeName = argv[index + 1];
        }
        else if (std::wstring(argv[index]) == L"--endpoint")
        {
            endpointFile = argv[index + 1];
        }
    }
    if (argv != nullptr)
    {
        LocalFree(argv);
    }
    if (pipeName.empty() && endpointFile.empty())
    {
        endpointFile = DefaultEndpointFile();
    }

    EditorPipe editor(pipeName, endpointFile);
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
