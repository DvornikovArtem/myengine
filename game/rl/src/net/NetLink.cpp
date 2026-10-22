// NetLink.cpp

#include <rl/net/NetLink.h>

#include <rl/RlRng.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace rl::net
{
    bool NetSimParams::Parse(const std::string& text, NetSimParams& out, std::string* error)
    {
        NetSimParams result;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(',', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            const std::string item = text.substr(start, end - start);
            start = end + 1;
            if (item.empty())
            {
                continue;
            }
            const size_t eq = item.find('=');
            if (eq == std::string::npos)
            {
                if (error != nullptr)
                {
                    *error = "net-sim: '" + item + "' is not key=value";
                }
                return false;
            }
            const std::string key = item.substr(0, eq);
            const std::string valueText = item.substr(eq + 1);
            char* parsedEnd = nullptr;
            const float value = std::strtof(valueText.c_str(), &parsedEnd);
            if (valueText.empty() || parsedEnd == nullptr || *parsedEnd != '\0' || value < 0.0f)
            {
                if (error != nullptr)
                {
                    *error = "net-sim: bad number in '" + item + "'";
                }
                return false;
            }
            if (key == "latency")
            {
                result.latencyMs = value;
            }
            else if (key == "jitter")
            {
                result.jitterMs = value;
            }
            else if (key == "loss")
            {
                result.lossPercent = std::min(value, 100.0f);
            }
            else if (key == "dup")
            {
                result.duplicatePercent = std::min(value, 100.0f);
            }
            else
            {
                if (error != nullptr)
                {
                    *error = "net-sim: unknown key '" + key + "' (latency, jitter, loss, dup)";
                }
                return false;
            }
        }
        out = result;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Loopback
    // ---------------------------------------------------------------------------------------------------------------------------------

    bool LoopbackHub::Register(uint16_t port, uint16_t& assigned)
    {
        if (port == 0)
        {
            while (inboxes_.count(nextFree_) != 0)
            {
                ++nextFree_;
            }
            port = nextFree_++;
        }
        if (inboxes_.count(port) != 0)
        {
            return false;
        }
        inboxes_[port];
        assigned = port;
        return true;
    }

    void LoopbackHub::Unregister(uint16_t port) { inboxes_.erase(port); }

    void LoopbackHub::Deliver(uint16_t to, NetAddress from, const uint8_t* data, size_t size)
    {
        const auto it = inboxes_.find(to);
        if (it == inboxes_.end())
        {
            return;  // nobody listens: the packet is lost, as on a network
        }
        Packet packet;
        packet.from = from;
        packet.data.assign(data, data + size);
        it->second.push_back(std::move(packet));
    }

    bool LoopbackHub::Take(uint16_t port, Packet& out)
    {
        const auto it = inboxes_.find(port);
        if (it == inboxes_.end() || it->second.empty())
        {
            return false;
        }
        out = std::move(it->second.front());
        it->second.pop_front();
        return true;
    }

    namespace
    {
        class LoopbackLink final : public IDatagramLink
        {
        public:
            explicit LoopbackLink(LoopbackHub& hub)
                : hub_(hub)
            {
            }
            ~LoopbackLink() override { Close(); }

            bool Bind(uint16_t port, std::string& error) override
            {
                Close();
                if (!hub_.Register(port, port_))
                {
                    error = "loopback port " + std::to_string(port) + " is taken";
                    return false;
                }
                bound_ = true;
                return true;
            }
            uint16_t LocalPort() const override { return port_; }
            bool Resolve(const std::string& text, NetAddress& out, std::string& error) override
            {
                // "name:port", "port" or "ip:port": only the port matters inside one process
                const size_t colon = text.rfind(':');
                const std::string portText = colon == std::string::npos ? text : text.substr(colon + 1);
                const long value = std::strtol(portText.c_str(), nullptr, 10);
                if (value <= 0 || value > 65535)
                {
                    error = "bad loopback address '" + text + "'";
                    return false;
                }
                out = static_cast<NetAddress>(value);
                return true;
            }
            void Send(NetAddress to, const uint8_t* data, size_t size, double) override
            {
                if (!bound_)
                {
                    // a client that never bound still needs a return address
                    std::string error;
                    Bind(0, error);
                }
                hub_.Deliver(static_cast<uint16_t>(to), port_, data, size);
            }
            bool Receive(NetAddress& from, std::vector<uint8_t>& data, double) override
            {
                LoopbackHub::Packet packet;
                if (!bound_ || !hub_.Take(port_, packet))
                {
                    return false;
                }
                from = packet.from;
                data = std::move(packet.data);
                return true;
            }
            void Close() override
            {
                if (bound_)
                {
                    hub_.Unregister(port_);
                    bound_ = false;
                }
            }

        private:
            LoopbackHub& hub_;
            uint16_t port_ = 0;
            bool bound_ = false;
        };

        // -----------------------------------------------------------------------------------------------------------------------------
        // Delay, loss and duplicates on the way out
        // -----------------------------------------------------------------------------------------------------------------------------

        class SimLink final : public IDatagramLink
        {
        public:
            SimLink(std::unique_ptr<IDatagramLink> inner, const NetSimParams& params, uint64_t seed)
                : inner_(std::move(inner))
                , params_(params)
                , rng_(seed)
            {
            }

            bool Bind(uint16_t port, std::string& error) override { return inner_->Bind(port, error); }
            uint16_t LocalPort() const override { return inner_->LocalPort(); }
            bool Resolve(const std::string& text, NetAddress& out, std::string& error) override { return inner_->Resolve(text, out, error); }

            void Send(NetAddress to, const uint8_t* data, size_t size, double now) override
            {
                Flush(now);
                if (rng_.Float01() * 100.0f < params_.lossPercent)
                {
                    return;
                }
                const int copies = rng_.Float01() * 100.0f < params_.duplicatePercent ? 2 : 1;
                for (int i = 0; i < copies; ++i)
                {
                    const double delayMs = std::max(0.0, static_cast<double>(params_.latencyMs) + static_cast<double>(params_.jitterMs) * static_cast<double>(rng_.Signed()));
                    Held held;
                    held.due = now + delayMs * 0.001;
                    held.order = counter_++;
                    held.to = to;
                    held.data.assign(data, data + size);
                    queue_.push_back(std::move(held));
                }
                Flush(now);
            }

            bool Receive(NetAddress& from, std::vector<uint8_t>& data, double now) override
            {
                Flush(now);
                return inner_->Receive(from, data, now);
            }

            void Close() override
            {
                queue_.clear();
                inner_->Close();
            }

        private:
            struct Held
            {
                double due = 0.0;
                uint64_t order = 0;
                NetAddress to = 0;
                std::vector<uint8_t> data;
            };

            void Flush(double now)
            {
                if (queue_.empty())
                {
                    return;
                }
                std::stable_sort(queue_.begin(), queue_.end(), [](const Held& a, const Held& b) { return a.due != b.due ? a.due < b.due : a.order < b.order; });
                size_t sent = 0;
                while (sent < queue_.size() && queue_[sent].due <= now)
                {
                    inner_->Send(queue_[sent].to, queue_[sent].data.data(), queue_[sent].data.size(), now);
                    ++sent;
                }
                queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(sent));
            }

            std::unique_ptr<IDatagramLink> inner_;
            NetSimParams params_;
            rl::RlRng rng_;
            std::vector<Held> queue_;
            uint64_t counter_ = 0;
        };

        // -----------------------------------------------------------------------------------------------------------------------------
        // UDP
        // -----------------------------------------------------------------------------------------------------------------------------

        bool StartWinsock()
        {
            static const bool started = []
            {
                WSADATA data{};
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
        }

        // SIO_UDP_CONNRESET of mstcpip.h
        constexpr DWORD kUdpConnReset = _WSAIOW(IOC_VENDOR, 12);

        class UdpLink final : public IDatagramLink
        {
        public:
            ~UdpLink() override { Close(); }

            bool Bind(uint16_t port, std::string& error) override
            {
                Close();
                if (!StartWinsock())
                {
                    error = "WSAStartup failed";
                    return false;
                }
                socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (socket_ == INVALID_SOCKET)
                {
                    error = "socket() failed: " + std::to_string(WSAGetLastError());
                    return false;
                }
                u_long nonBlocking = 1;
                ioctlsocket(socket_, FIONBIO, &nonBlocking);
                // a closed peer must not turn into an error of the next recvfrom (WSAECONNRESET on UDP)
                BOOL noReset = FALSE;
                DWORD bytes = 0;
                WSAIoctl(socket_, kUdpConnReset, &noReset, sizeof(noReset), nullptr, 0, &bytes, nullptr, nullptr);
                int buffer = 1 << 20;
                setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_ANY);
                address.sin_port = htons(port);
                if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                {
                    error = "bind(" + std::to_string(port) + ") failed: " + std::to_string(WSAGetLastError());
                    Close();
                    return false;
                }
                int length = sizeof(address);
                getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
                port_ = ntohs(address.sin_port);
                return true;
            }

            uint16_t LocalPort() const override { return port_; }

            bool Resolve(const std::string& text, NetAddress& out, std::string& error) override
            {
                std::string host = text;
                std::string portText;
                const size_t colon = text.rfind(':');
                if (colon != std::string::npos)
                {
                    host = text.substr(0, colon);
                    portText = text.substr(colon + 1);
                }
                const long port = portText.empty() ? 0 : std::strtol(portText.c_str(), nullptr, 10);
                if (port <= 0 || port > 65535)
                {
                    error = "address '" + text + "' has no valid port (use ip:port)";
                    return false;
                }
                if (host.empty() || host == "localhost")
                {
                    host = "127.0.0.1";
                }
                in_addr ip{};
                if (inet_pton(AF_INET, host.c_str(), &ip) != 1)
                {
                    if (!StartWinsock())
                    {
                        error = "WSAStartup failed";
                        return false;
                    }
                    addrinfo hints{};
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_DGRAM;
                    addrinfo* result = nullptr;
                    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
                    {
                        error = "cannot resolve '" + host + "'";
                        return false;
                    }
                    ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
                    freeaddrinfo(result);
                }
                out = (static_cast<NetAddress>(ntohl(ip.s_addr)) << 16) | static_cast<NetAddress>(port);
                return true;
            }

            void Send(NetAddress to, const uint8_t* data, size_t size, double) override
            {
                if (socket_ == INVALID_SOCKET)
                {
                    // a client that never bound still needs a return address
                    std::string error;
                    if (!Bind(0, error))
                    {
                        return;
                    }
                }
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(static_cast<uint32_t>(to >> 16));
                address.sin_port = htons(static_cast<uint16_t>(to & 0xFFFF));
                sendto(socket_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            }

            bool Receive(NetAddress& from, std::vector<uint8_t>& data, double) override
            {
                if (socket_ == INVALID_SOCKET)
                {
                    return false;
                }
                uint8_t buffer[2048];
                sockaddr_in address{};
                int length = sizeof(address);
                const int received = recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&address), &length);
                if (received <= 0)
                {
                    return false;
                }
                data.assign(buffer, buffer + received);
                from = (static_cast<NetAddress>(ntohl(address.sin_addr.s_addr)) << 16) | static_cast<NetAddress>(ntohs(address.sin_port));
                return true;
            }

            void Close() override
            {
                if (socket_ != INVALID_SOCKET)
                {
                    closesocket(socket_);
                    socket_ = INVALID_SOCKET;
                }
                port_ = 0;
            }

        private:
            SOCKET socket_ = INVALID_SOCKET;
            uint16_t port_ = 0;
        };
    }

    std::unique_ptr<IDatagramLink> MakeLoopbackLink(LoopbackHub& hub) { return std::make_unique<LoopbackLink>(hub); }
    std::unique_ptr<IDatagramLink> MakeUdpLink() { return std::make_unique<UdpLink>(); }
    std::unique_ptr<IDatagramLink> MakeSimLink(std::unique_ptr<IDatagramLink> inner, const NetSimParams& params, uint64_t seed)
    {
        return std::make_unique<SimLink>(std::move(inner), params, seed);
    }
}
