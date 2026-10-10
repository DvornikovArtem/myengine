// AssistantBridgeServer.cpp

#include <myengine/assistant/AssistantBridgeServer.h>

#include <algorithm>
#include <string_view>
#include <utility>

#include <windows.h>

namespace myengine::assistant
{
    namespace
    {
        constexpr DWORD kPipeBufferBytes = 1024 * 1024;
        constexpr std::size_t kMaxLineBytes = 8 * 1024 * 1024;
        constexpr DWORD kWriteTimeoutMs = 2000;
    }

    struct AssistantBridgeServer::Impl
    {
        enum class State
        {
            Stopped,
            Listening, // ConnectNamedPipe is pending
            Connected,
        };

        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE connectEvent = nullptr;
        HANDLE ioEvent = nullptr;
        OVERLAPPED connectOverlapped{};
        State state = State::Stopped;
        bool authenticated = false;
        std::string token;
        std::string buffer;
        std::uint64_t connection = 0;
        std::uint32_t disconnects = 0;

        void BeginListening()
        {
            state = State::Listening;
            authenticated = false;
            buffer.clear();
            ResetEvent(connectEvent);
            connectOverlapped = {};
            connectOverlapped.hEvent = connectEvent;
            if (ConnectNamedPipe(pipe, &connectOverlapped))
            {
                state = State::Connected; // not expected for overlapped mode, but harmless
                ++connection;
                return;
            }
            switch (GetLastError())
            {
            case ERROR_PIPE_CONNECTED:
                state = State::Connected; // the client was faster than this call
                ++connection;
                break;
            case ERROR_IO_PENDING:
                break;
            default:
                state = State::Stopped; // the pipe is broken; the editor keeps working without the bridge
                break;
            }
        }

        void Disconnect()
        {
            if (state == State::Connected && authenticated)
            {
                ++disconnects;
            }
            DisconnectNamedPipe(pipe);
            BeginListening();
        }

        bool WriteLine(const std::string& line)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = ioEvent;
            ResetEvent(ioEvent);
            const std::string data = line + "\n";
            std::size_t written = 0;
            while (written < data.size())
            {
                DWORD count = 0;
                ResetEvent(ioEvent);
                if (!WriteFile(pipe, data.data() + written, static_cast<DWORD>(data.size() - written), &count, &overlapped))
                {
                    if (GetLastError() != ERROR_IO_PENDING)
                    {
                        return false;
                    }
                    if (WaitForSingleObject(ioEvent, kWriteTimeoutMs) != WAIT_OBJECT_0)
                    {
                        CancelIo(pipe);
                        return false;
                    }
                    if (!GetOverlappedResult(pipe, &overlapped, &count, FALSE))
                    {
                        return false;
                    }
                }
                if (count == 0)
                {
                    return false;
                }
                written += count;
            }
            return true;
        }
    };

    AssistantBridgeServer::AssistantBridgeServer() : impl_(std::make_unique<Impl>()) {}

    AssistantBridgeServer::~AssistantBridgeServer()
    {
        Stop();
    }

    std::wstring AssistantBridgeServer::DefaultPipeName()
    {
        return L"\\\\.\\pipe\\myengine-assistant-" + std::to_wstring(GetCurrentProcessId());
    }

    bool AssistantBridgeServer::Start(const std::wstring& pipeName, const std::string& token, std::string& error)
    {
        Stop();
        auto& impl = *impl_;
        impl.token = token;
        impl.pipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            kPipeBufferBytes,
            kPipeBufferBytes,
            0,
            nullptr);
        if (impl.pipe == INVALID_HANDLE_VALUE)
        {
            error = "Could not create the assistant pipe (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        impl.connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        impl.ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (impl.connectEvent == nullptr || impl.ioEvent == nullptr)
        {
            error = "Could not create the pipe events.";
            Stop();
            return false;
        }
        impl.BeginListening();
        if (impl.state == Impl::State::Stopped)
        {
            error = "The assistant pipe could not start listening.";
            Stop();
            return false;
        }
        return true;
    }

    void AssistantBridgeServer::Stop()
    {
        auto& impl = *impl_;
        if (impl.pipe != INVALID_HANDLE_VALUE)
        {
            CancelIo(impl.pipe);
            CloseHandle(impl.pipe);
            impl.pipe = INVALID_HANDLE_VALUE;
        }
        for (HANDLE* handle : {&impl.connectEvent, &impl.ioEvent})
        {
            if (*handle != nullptr)
            {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
        impl.state = Impl::State::Stopped;
        impl.authenticated = false;
        impl.buffer.clear();
    }

    bool AssistantBridgeServer::IsRunning() const
    {
        return impl_->state != Impl::State::Stopped;
    }

    bool AssistantBridgeServer::IsConnected() const
    {
        return impl_->state == Impl::State::Connected && impl_->authenticated;
    }

    std::uint32_t AssistantBridgeServer::TakeDisconnects()
    {
        const auto count = impl_->disconnects;
        impl_->disconnects = 0;
        return count;
    }

    void AssistantBridgeServer::Poll(std::vector<BridgeRequest>& requests)
    {
        auto& impl = *impl_;
        if (impl.state == Impl::State::Listening)
        {
            DWORD ignored = 0;
            if (GetOverlappedResult(impl.pipe, &impl.connectOverlapped, &ignored, FALSE))
            {
                impl.state = Impl::State::Connected;
                ++impl.connection;
                impl.authenticated = false;
                impl.buffer.clear();
            }
            else if (GetLastError() != ERROR_IO_INCOMPLETE)
            {
                impl.BeginListening();
            }
        }
        if (impl.state != Impl::State::Connected)
        {
            return;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(impl.pipe, nullptr, 0, nullptr, &available, nullptr))
        {
            impl.Disconnect(); // the client closed its end
            return;
        }
        if (available > 0)
        {
            char chunk[16384];
            OVERLAPPED overlapped{};
            overlapped.hEvent = impl.ioEvent;
            ResetEvent(impl.ioEvent);
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, sizeof(chunk));
            if (!ReadFile(impl.pipe, chunk, wanted, &read, &overlapped))
            {
                if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(impl.pipe, &overlapped, &read, TRUE))
                {
                    impl.Disconnect();
                    return;
                }
            }
            impl.buffer.append(chunk, read);
        }

        std::size_t start = 0;
        while (true)
        {
            const auto end = impl.buffer.find('\n', start);
            if (end == std::string::npos)
            {
                break;
            }
            const std::string_view line(impl.buffer.data() + start, end - start);
            start = end + 1;
            if (line.empty())
            {
                continue;
            }
            const auto message = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
            if (message.is_discarded() || !message.is_object())
            {
                continue;
            }
            const auto op = message.contains("op") && message["op"].is_string() ? message["op"].get<std::string>() : std::string();
            if (!impl.authenticated)
            {
                if (op == "hello" && message.contains("token") && message["token"].is_string() && message["token"].get<std::string>() == impl.token)
                {
                    impl.authenticated = true;
                    impl.WriteLine(R"({"ok":true})");
                    continue;
                }
                impl.Disconnect(); // wrong or missing token
                return;
            }
            BridgeRequest request;
            request.id = message.contains("id") && message["id"].is_number_unsigned() ? message["id"].get<std::uint64_t>() : 0;
            request.connection = impl.connection;
            request.op = op;
            request.tool = message.contains("tool") && message["tool"].is_string() ? message["tool"].get<std::string>() : std::string();
            if (message.contains("arguments") && message["arguments"].is_object())
            {
                request.arguments = message["arguments"];
            }
            requests.push_back(std::move(request));
        }
        impl.buffer.erase(0, start);
        if (impl.buffer.size() > kMaxLineBytes)
        {
            impl.Disconnect();
        }
    }

    bool AssistantBridgeServer::Reply(const BridgeRequest& request, const nlohmann::json& response)
    {
        auto& impl = *impl_;
        if (impl.state != Impl::State::Connected || !impl.authenticated || request.connection != impl.connection)
        {
            return false;
        }
        nlohmann::json envelope = {{"id", request.id}, {"result", response}};
        if (!impl.WriteLine(envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)))
        {
            impl.Disconnect();
            return false;
        }
        return true;
    }
}
