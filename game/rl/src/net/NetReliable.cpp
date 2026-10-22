// NetReliable.cpp

#include <rl/net/NetReliable.h>

#include <rl/RlRng.h>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace rl::net
{
    namespace
    {
        constexpr uint8_t kMagic = 0xA7;
        constexpr uint16_t kTransportVersion = 1;
        constexpr size_t kHeaderBytes = 6;
        constexpr size_t kMaxPeers = 16;
        constexpr int kWindow = 128;               // reliable messages in flight
        constexpr uint16_t kReceiveWindow = 256;   // how far ahead of the next expected one a message is kept
        constexpr double kConnectRetry = 0.25;
        constexpr double kConnectTimeout = 10.0;
        constexpr double kKeepAlive = 0.2;
        constexpr double kPingInterval = 0.5;  // the round trip time is measured on its own: the game traffic is unreliable and gives no samples
        constexpr double kPeerTimeout = 5.0;
        constexpr double kMinRto = 0.06;
        constexpr double kMaxRto = 1.0;

        enum PacketType : uint8_t
        {
            kConnect = 1,
            kAccept = 2,
            kReject = 3,
            kUnreliable = 4,
            kReliable = 5,
            kAck = 6,
            kPing = 7,
            kPong = 8,
            kDisconnect = 9,
        };

        void Put16(std::vector<uint8_t>& out, uint16_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        void Put32(std::vector<uint8_t>& out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
            }
        }
        uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t Get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }

        // wrap-around order of 16-bit sequence numbers: > 0 when a is newer than b
        int Diff(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)); }

        uint32_t MixToken(uint32_t clientNonce, uint32_t serverNonce)
        {
            uint32_t v = clientNonce * 2654435761u ^ ((serverNonce << 13) | (serverNonce >> 19));
            return v == 0 ? 1u : v;
        }

        class ReliableTransport final : public INetTransport
        {
        public:
            ReliableTransport(std::unique_ptr<IDatagramLink> link, uint64_t seed)
                : link_(std::move(link))
                , rng_(seed)
            {
            }
            ~ReliableTransport() override { Close(); }

            bool Listen(uint16_t port, std::string& error) override
            {
                if (!link_->Bind(port, error))
                {
                    return false;
                }
                listening_ = true;
                return true;
            }

            PeerId Connect(const std::string& address, std::string& error) override
            {
                NetAddress target = 0;
                if (!link_->Resolve(address, target, error))
                {
                    return kInvalidPeer;
                }
                Peer& peer = AddPeer(target);
                peer.server = false;
                peer.clientNonce = rng_.NextU32() | 1u;
                peer.connectStart = -1.0;
                peer.lastConnectTry = -1.0;
                return peer.id;
            }

            bool Send(PeerId id, const uint8_t* data, size_t size, bool reliable) override
            {
                Peer* peer = Find(id);
                if (peer == nullptr || !peer->connected || size > kMaxMessageBytes)
                {
                    return false;
                }
                if (reliable)
                {
                    peer->pending.emplace_back(data, data + size);
                    PumpReliable(*peer, lastNow_);
                }
                else
                {
                    std::vector<uint8_t> packet = MakePacket(kUnreliable, peer->token);
                    packet.insert(packet.end(), data, data + size);
                    SendPacket(*peer, packet, lastNow_);
                }
                return true;
            }

            void Poll(double now, std::vector<NetEvent>& events) override
            {
                ZoneScopedN("Rl::Net::Poll");
                lastNow_ = now;
                events.insert(events.end(), std::make_move_iterator(deferred_.begin()), std::make_move_iterator(deferred_.end()));
                deferred_.clear();

                NetAddress from = 0;
                std::vector<uint8_t> packet;
                for (int guard = 0; guard < 8192 && link_->Receive(from, packet, now); ++guard)
                {
                    HandlePacket(from, packet, now, events);
                }

                std::vector<PeerId> gone;
                for (auto& entry : peers_)
                {
                    Peer& peer = entry.second;
                    if (!peer.connected)
                    {
                        if (!peer.server)
                        {
                            if (peer.connectStart < 0.0)
                            {
                                peer.connectStart = now;
                            }
                            if (now - peer.connectStart > kConnectTimeout)
                            {
                                events.push_back(DisconnectEvent(peer.id, DisconnectReason::Timeout));
                                gone.push_back(peer.id);
                            }
                            else if (peer.lastConnectTry < 0.0 || now - peer.lastConnectTry >= kConnectRetry)
                            {
                                peer.lastConnectTry = now;
                                std::vector<uint8_t> hello = MakePacket(kConnect, peer.clientNonce);
                                Put16(hello, kTransportVersion);
                                link_->Send(peer.addr, hello.data(), hello.size(), now);
                            }
                        }
                        continue;
                    }

                    if (now - peer.lastRecv > kPeerTimeout)
                    {
                        events.push_back(DisconnectEvent(peer.id, DisconnectReason::Timeout));
                        gone.push_back(peer.id);
                        continue;
                    }
                    Retransmit(peer, now);
                    if (now - peer.lastSend >= kKeepAlive || now - peer.lastPing >= kPingInterval)
                    {
                        peer.lastPing = now;
                        std::vector<uint8_t> ping = MakePacket(kPing, peer.token);
                        Put32(ping, static_cast<uint32_t>(now * 1000.0));
                        SendPacket(peer, ping, now);
                    }
                }
                for (PeerId id : gone)
                {
                    peers_.erase(id);
                }
            }

            void Disconnect(PeerId id) override
            {
                Peer* peer = Find(id);
                if (peer == nullptr)
                {
                    return;
                }
                if (peer->connected)
                {
                    std::vector<uint8_t> bye = MakePacket(kDisconnect, peer->token);
                    for (int i = 0; i < 3; ++i)
                    {
                        link_->Send(peer->addr, bye.data(), bye.size(), lastNow_);
                    }
                }
                peers_.erase(id);
            }

            void Close() override
            {
                std::vector<PeerId> ids;
                for (const auto& entry : peers_)
                {
                    ids.push_back(entry.first);
                }
                for (PeerId id : ids)
                {
                    Disconnect(id);
                }
                if (link_ != nullptr)
                {
                    link_->Close();
                }
                listening_ = false;
            }

            double RoundTripSeconds(PeerId id) const override
            {
                const auto it = peers_.find(id);
                return it == peers_.end() ? 0.0 : it->second.srtt;
            }
            uint16_t LocalPort() const override { return link_->LocalPort(); }
            bool IsConnected(PeerId id) const override
            {
                const auto it = peers_.find(id);
                return it != peers_.end() && it->second.connected;
            }

        private:
            struct Outgoing
            {
                uint16_t seq = 0;
                std::vector<uint8_t> data;
                double firstSent = 0.0;
                double lastSent = 0.0;
                int sends = 0;
            };

            struct Peer
            {
                PeerId id = kInvalidPeer;
                NetAddress addr = 0;
                bool server = false;      // true: we are the host and this is a client
                bool connected = false;
                uint32_t clientNonce = 0;
                uint32_t serverNonce = 0;
                uint32_t token = 0;
                double connectStart = 0.0;
                double lastConnectTry = 0.0;
                double lastRecv = 0.0;
                double lastSend = 0.0;
                double lastPing = -1e9;
                double srtt = 0.0;
                uint16_t nextSeq = 0;
                std::deque<Outgoing> unacked;
                std::deque<std::vector<uint8_t>> pending;
                uint16_t recvNext = 0;
                std::map<uint16_t, std::vector<uint8_t>> recvBuffer;
            };

            static std::vector<uint8_t> MakePacket(PacketType type, uint32_t token)
            {
                std::vector<uint8_t> packet;
                packet.reserve(64);
                packet.push_back(kMagic);
                packet.push_back(type);
                Put32(packet, token);
                return packet;
            }

            NetEvent DisconnectEvent(PeerId id, DisconnectReason reason)
            {
                NetEvent event;
                event.type = NetEventType::Disconnected;
                event.peer = id;
                event.reason = reason;
                return event;
            }

            Peer& AddPeer(NetAddress addr)
            {
                Peer peer;
                peer.id = nextPeer_++;
                peer.addr = addr;
                const PeerId id = peer.id;
                return peers_.emplace(id, std::move(peer)).first->second;
            }

            Peer* Find(PeerId id)
            {
                const auto it = peers_.find(id);
                return it == peers_.end() ? nullptr : &it->second;
            }

            Peer* FindByAddress(NetAddress addr)
            {
                for (auto& entry : peers_)
                {
                    if (entry.second.addr == addr)
                    {
                        return &entry.second;
                    }
                }
                return nullptr;
            }

            void SendPacket(Peer& peer, const std::vector<uint8_t>& packet, double now)
            {
                peer.lastSend = now;
                link_->Send(peer.addr, packet.data(), packet.size(), now);
            }

            void SendReliableNow(Peer& peer, Outgoing& message, double now)
            {
                std::vector<uint8_t> packet = MakePacket(kReliable, peer.token);
                Put16(packet, message.seq);
                packet.insert(packet.end(), message.data.begin(), message.data.end());
                if (message.sends == 0)
                {
                    message.firstSent = now;
                }
                message.lastSent = now;
                ++message.sends;
                SendPacket(peer, packet, now);
            }

            void PumpReliable(Peer& peer, double now)
            {
                while (!peer.pending.empty() && static_cast<int>(peer.unacked.size()) < kWindow)
                {
                    Outgoing message;
                    message.seq = peer.nextSeq++;
                    message.data = std::move(peer.pending.front());
                    peer.pending.pop_front();
                    peer.unacked.push_back(std::move(message));
                    SendReliableNow(peer, peer.unacked.back(), now);
                }
            }

            double Rto(const Peer& peer, int sends) const
            {
                double rto = peer.srtt > 0.0 ? peer.srtt * 1.5 + 0.02 : 0.2;
                rto = std::clamp(rto, kMinRto, kMaxRto);
                for (int i = 1; i < sends && rto < kMaxRto; ++i)
                {
                    rto *= 1.5;
                }
                return std::min(rto, kMaxRto);
            }

            void Retransmit(Peer& peer, double now)
            {
                for (Outgoing& message : peer.unacked)
                {
                    if (now - message.lastSent >= Rto(peer, message.sends))
                    {
                        SendReliableNow(peer, message, now);
                    }
                }
                PumpReliable(peer, now);
            }

            void SampleRtt(Peer& peer, double sample)
            {
                if (sample < 0.0 || sample > 10.0)
                {
                    return;
                }
                peer.srtt = peer.srtt <= 0.0 ? sample : peer.srtt * 0.875 + sample * 0.125;
            }

            void SendAck(Peer& peer, double now)
            {
                uint32_t mask = 0;
                for (int i = 0; i < 32; ++i)
                {
                    const uint16_t seq = static_cast<uint16_t>(peer.recvNext + 1 + i);
                    if (peer.recvBuffer.count(seq) != 0)
                    {
                        mask |= 1u << i;
                    }
                }
                std::vector<uint8_t> packet = MakePacket(kAck, peer.token);
                Put16(packet, peer.recvNext);
                Put32(packet, mask);
                SendPacket(peer, packet, now);
            }

            void HandleAck(Peer& peer, uint16_t next, uint32_t mask, double now)
            {
                for (auto it = peer.unacked.begin(); it != peer.unacked.end();)
                {
                    const int ahead = Diff(it->seq, next);  // < 0: acknowledged by the cumulative number
                    bool acked = ahead < 0;
                    if (!acked && ahead >= 1 && ahead <= 32)
                    {
                        acked = (mask & (1u << (ahead - 1))) != 0;
                    }
                    if (acked)
                    {
                        if (it->sends == 1)
                        {
                            SampleRtt(peer, now - it->firstSent);  // only messages that were not retransmitted give a clean sample
                        }
                        it = peer.unacked.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                PumpReliable(peer, now);
            }

            void HandlePacket(NetAddress from, const std::vector<uint8_t>& packet, double now, std::vector<NetEvent>& events)
            {
                if (packet.size() < kHeaderBytes || packet[0] != kMagic)
                {
                    return;
                }
                const uint8_t type = packet[1];
                const uint32_t token = Get32(packet.data() + 2);
                const uint8_t* body = packet.data() + kHeaderBytes;
                const size_t bodySize = packet.size() - kHeaderBytes;

                if (type == kConnect)
                {
                    HandleConnect(from, token, body, bodySize, now, events);
                    return;
                }

                Peer* peer = FindByAddress(from);
                if (peer == nullptr)
                {
                    return;
                }
                if (type == kAccept)
                {
                    if (!peer->server && !peer->connected && token == peer->clientNonce && bodySize >= 4)
                    {
                        peer->token = MixToken(peer->clientNonce, Get32(body));
                        peer->connected = true;
                        peer->lastRecv = now;
                        peer->lastSend = now;
                        NetEvent event;
                        event.type = NetEventType::Connected;
                        event.peer = peer->id;
                        events.push_back(std::move(event));
                    }
                    return;
                }
                if (type == kReject)
                {
                    if (!peer->server && !peer->connected && token == peer->clientNonce)
                    {
                        events.push_back(DisconnectEvent(peer->id, DisconnectReason::Rejected));
                        peers_.erase(peer->id);
                    }
                    return;
                }
                if (!peer->connected || token != peer->token)
                {
                    return;
                }
                peer->lastRecv = now;

                switch (type)
                {
                case kUnreliable:
                {
                    NetEvent event;
                    event.type = NetEventType::Data;
                    event.peer = peer->id;
                    event.reliable = false;
                    event.data.assign(body, body + bodySize);
                    events.push_back(std::move(event));
                    break;
                }
                case kReliable:
                {
                    if (bodySize < 2)
                    {
                        break;
                    }
                    const uint16_t seq = Get16(body);
                    const int ahead = Diff(seq, peer->recvNext);
                    if (ahead >= 0 && ahead < kReceiveWindow && peer->recvBuffer.count(seq) == 0)
                    {
                        peer->recvBuffer.emplace(seq, std::vector<uint8_t>(body + 2, body + bodySize));
                    }
                    while (true)
                    {
                        const auto it = peer->recvBuffer.find(peer->recvNext);
                        if (it == peer->recvBuffer.end())
                        {
                            break;
                        }
                        NetEvent event;
                        event.type = NetEventType::Data;
                        event.peer = peer->id;
                        event.reliable = true;
                        event.data = std::move(it->second);
                        events.push_back(std::move(event));
                        peer->recvBuffer.erase(it);
                        ++peer->recvNext;
                    }
                    SendAck(*peer, now);
                    break;
                }
                case kAck:
                    if (bodySize >= 6)
                    {
                        HandleAck(*peer, Get16(body), Get32(body + 2), now);
                    }
                    break;
                case kPing:
                    if (bodySize >= 4)
                    {
                        std::vector<uint8_t> pong = MakePacket(kPong, peer->token);
                        Put32(pong, Get32(body));
                        SendPacket(*peer, pong, now);
                    }
                    break;
                case kPong:
                    if (bodySize >= 4)
                    {
                        const uint32_t stamp = Get32(body);
                        const uint32_t nowMs = static_cast<uint32_t>(now * 1000.0);
                        SampleRtt(*peer, static_cast<double>(static_cast<uint32_t>(nowMs - stamp)) * 0.001);
                    }
                    break;
                case kDisconnect:
                    events.push_back(DisconnectEvent(peer->id, DisconnectReason::Closed));
                    peers_.erase(peer->id);
                    break;
                default:
                    break;
                }
            }

            void HandleConnect(NetAddress from, uint32_t clientNonce, const uint8_t* body, size_t bodySize, double now, std::vector<NetEvent>& events)
            {
                if (!listening_)
                {
                    return;
                }
                if (bodySize < 2 || Get16(body) != kTransportVersion)
                {
                    std::vector<uint8_t> reject = MakePacket(kReject, clientNonce);
                    link_->Send(from, reject.data(), reject.size(), now);
                    return;
                }
                Peer* existing = FindByAddress(from);
                if (existing != nullptr && existing->clientNonce == clientNonce)
                {
                    SendAccept(*existing, now);  // our Accept was lost: say it again
                    return;
                }
                if (existing != nullptr)
                {
                    // the same address came back with a new handshake: the old connection is gone
                    events.push_back(DisconnectEvent(existing->id, DisconnectReason::Closed));
                    peers_.erase(existing->id);
                }
                if (peers_.size() >= kMaxPeers)
                {
                    std::vector<uint8_t> reject = MakePacket(kReject, clientNonce);
                    link_->Send(from, reject.data(), reject.size(), now);
                    return;
                }
                Peer& peer = AddPeer(from);
                peer.server = true;
                peer.clientNonce = clientNonce;
                peer.serverNonce = rng_.NextU32() | 1u;
                peer.token = MixToken(clientNonce, peer.serverNonce);
                peer.connected = true;
                peer.lastRecv = now;
                peer.lastSend = now;
                SendAccept(peer, now);
                NetEvent event;
                event.type = NetEventType::Connected;
                event.peer = peer.id;
                events.push_back(std::move(event));
            }

            void SendAccept(Peer& peer, double now)
            {
                std::vector<uint8_t> accept = MakePacket(kAccept, peer.clientNonce);
                Put32(accept, peer.serverNonce);
                SendPacket(peer, accept, now);
            }

            std::unique_ptr<IDatagramLink> link_;
            rl::RlRng rng_;
            std::map<PeerId, Peer> peers_;
            std::vector<NetEvent> deferred_;
            PeerId nextPeer_ = 1;
            bool listening_ = false;
            double lastNow_ = 0.0;
        };
    }

    std::unique_ptr<INetTransport> MakeReliableTransport(std::unique_ptr<IDatagramLink> link, uint64_t seed)
    {
        return std::make_unique<ReliableTransport>(std::move(link), seed);
    }

    std::unique_ptr<INetTransport> MakeLoopbackTransport(LoopbackHub& hub, const NetSimParams& sim, uint64_t seed)
    {
        std::unique_ptr<IDatagramLink> link = MakeLoopbackLink(hub);
        if (sim.Active())
        {
            link = MakeSimLink(std::move(link), sim, seed * 7919u + 13u);
        }
        return MakeReliableTransport(std::move(link), seed);
    }

    std::unique_ptr<INetTransport> MakeUdpTransport(const NetSimParams& sim, uint64_t seed)
    {
        std::unique_ptr<IDatagramLink> link = MakeUdpLink();
        if (sim.Active())
        {
            link = MakeSimLink(std::move(link), sim, seed * 7919u + 13u);
        }
        return MakeReliableTransport(std::move(link), seed);
    }
}
