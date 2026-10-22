// SteamTransport.cpp
// ISteamNetworkingMessages over the Steamworks SDK. Compiled and linked against the real SDK 1.65 in G6b; the connection of two accounts is still
// to be tried (docs/game/steam-setup.md), one account checks only that the transport starts (tests/rl/RlSteamSmoke.cpp).
//
// Wire format of this transport: the first byte of a message is its kind.
//   Hello  client -> host, repeated every 0.25 s until Accept      Accept host -> client      Bye  either side      Ping  either side, unreliable
//   Data   [kData][payload]  reliable messages on channel 0 (like the control ones: one ordered stream per peer), unreliable ones on channel 1

#include "SteamTransport.h"

#include <steam/steam_api.h>
#if defined(__has_include)
#if __has_include(<steam/isteamnetworkingmessages.h>)
#include <steam/isteamnetworkingmessages.h>
#endif
#endif

#include <algorithm>
#include <cstdlib>
#include <map>

namespace rl::net
{
    namespace
    {
        enum Kind : uint8_t
        {
            kHello = 1,
            kAccept = 2,
            kBye = 3,
            kPing = 4,
            kData = 5,
        };

        constexpr int kControlChannel = 0;   // control and reliable data: one ordered stream per peer
        constexpr int kUnreliableChannel = 1;
        constexpr double kHelloRetry = 0.25;
        constexpr double kConnectTimeout = 10.0;
        constexpr double kKeepAlive = 0.2;
        constexpr double kPeerTimeout = 5.0;
        constexpr double kRttPoll = 0.5;

        class SteamTransport final : public INetTransport
        {
        public:
            explicit SteamTransport(const std::vector<uint64_t>& allowed)
                : allowed_(allowed)
                , sessionRequest_(this, &SteamTransport::OnSessionRequest)
                , sessionFailed_(this, &SteamTransport::OnSessionFailed)
            {
            }
            ~SteamTransport() override { Close(); }

            bool Listen(uint16_t, std::string& error) override
            {
                if (SteamNetworkingMessages() == nullptr)
                {
                    error = "Steam is not running (SteamNetworkingMessages is not there)";
                    return false;
                }
                // the Valve network starts lazily: a first message would wait for it for some seconds
                if (SteamNetworkingUtils() != nullptr)
                {
                    SteamNetworkingUtils()->InitRelayNetworkAccess();
                }
                listening_ = true;
                return true;
            }

            PeerId Connect(const std::string& address, std::string& error) override
            {
                if (SteamNetworkingMessages() == nullptr)
                {
                    error = "Steam is not running (SteamNetworkingMessages is not there)";
                    return kInvalidPeer;
                }
                char* end = nullptr;
                const uint64_t id = std::strtoull(address.c_str(), &end, 10);
                if (id == 0 || end == address.c_str())
                {
                    error = "'" + address + "' is not a Steam id";
                    return kInvalidPeer;
                }
                if (SteamNetworkingUtils() != nullptr)
                {
                    SteamNetworkingUtils()->InitRelayNetworkAccess();
                }
                Peer& peer = AddPeer(id);
                peer.clientSide = true;
                peer.connectStart = -1.0;
                return peer.id;
            }

            bool Send(PeerId id, const uint8_t* data, size_t size, bool reliable) override
            {
                Peer* peer = Find(id);
                if (peer == nullptr || !peer->connected || size > kMaxMessageBytes)
                {
                    return false;
                }
                std::vector<uint8_t> message;
                message.reserve(size + 1);
                message.push_back(kData);
                message.insert(message.end(), data, data + size);
                return SendRaw(*peer, message, reliable ? kControlChannel : kUnreliableChannel, reliable);
            }

            void Poll(double now, std::vector<NetEvent>& events) override
            {
                now_ = now;
                events.insert(events.end(), std::make_move_iterator(deferred_.begin()), std::make_move_iterator(deferred_.end()));
                deferred_.clear();

                ISteamNetworkingMessages* messages = SteamNetworkingMessages();
                if (messages == nullptr)
                {
                    return;
                }
                for (const int channel : {kControlChannel, kUnreliableChannel})
                {
                    SteamNetworkingMessage_t* received[32];
                    const int count = messages->ReceiveMessagesOnChannel(channel, received, 32);
                    for (int i = 0; i < count; ++i)
                    {
                        HandleMessage(received[i]->m_identityPeer.GetSteamID64(), static_cast<const uint8_t*>(received[i]->m_pData), static_cast<size_t>(received[i]->m_cbSize),
                                      events);
                        received[i]->Release();
                    }
                }

                std::vector<PeerId> gone;
                for (auto& entry : peers_)
                {
                    Peer& peer = entry.second;
                    if (!peer.connected)
                    {
                        if (peer.clientSide)
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
                            else if (now - peer.lastHello >= kHelloRetry)
                            {
                                peer.lastHello = now;
                                SendRaw(peer, {kHello}, kControlChannel, true);
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
                    if (now - peer.lastSend >= kKeepAlive)
                    {
                        SendRaw(peer, {kPing}, kUnreliableChannel, false);
                    }
                    if (now - peer.lastRtt >= kRttPoll)
                    {
                        peer.lastRtt = now;
                        SteamNetworkingIdentity identity = Identity(peer.steamId);
                        SteamNetConnectionRealTimeStatus_t status{};
                        if (messages->GetSessionConnectionInfo(identity, nullptr, &status) == k_ESteamNetworkingConnectionState_Connected && status.m_nPing >= 0)
                        {
                            peer.rttSeconds = static_cast<double>(status.m_nPing) * 0.001;
                        }
                    }
                }
                for (const PeerId id : gone)
                {
                    RemovePeer(id, false);
                }
            }

            void Disconnect(PeerId id) override
            {
                Peer* peer = Find(id);
                if (peer != nullptr)
                {
                    RemovePeer(id, true);
                }
            }

            void Close() override
            {
                std::vector<PeerId> ids;
                for (const auto& entry : peers_)
                {
                    ids.push_back(entry.first);
                }
                for (const PeerId id : ids)
                {
                    Disconnect(id);
                }
                listening_ = false;
            }

            double RoundTripSeconds(PeerId id) const override
            {
                const auto it = peers_.find(id);
                return it == peers_.end() ? 0.0 : it->second.rttSeconds;
            }
            uint16_t LocalPort() const override { return 0; }
            bool IsConnected(PeerId id) const override
            {
                const auto it = peers_.find(id);
                return it != peers_.end() && it->second.connected;
            }

        private:
            struct Peer
            {
                PeerId id = kInvalidPeer;
                uint64_t steamId = 0;
                bool clientSide = false;  // we started the handshake
                bool connected = false;
                double connectStart = 0.0;
                double lastHello = -1e9;
                double lastRecv = 0.0;
                double lastSend = 0.0;
                double lastRtt = -1e9;
                double rttSeconds = 0.0;
            };

            static SteamNetworkingIdentity Identity(uint64_t steamId)
            {
                SteamNetworkingIdentity identity;
                identity.Clear();
                identity.SetSteamID64(steamId);
                return identity;
            }

            NetEvent DisconnectEvent(PeerId id, DisconnectReason reason) const
            {
                NetEvent event;
                event.type = NetEventType::Disconnected;
                event.peer = id;
                event.reason = reason;
                return event;
            }

            bool IsAllowed(uint64_t steamId) const { return allowed_.empty() || std::find(allowed_.begin(), allowed_.end(), steamId) != allowed_.end(); }

            Peer& AddPeer(uint64_t steamId)
            {
                Peer peer;
                peer.id = nextPeer_++;
                peer.steamId = steamId;
                const PeerId id = peer.id;
                return peers_.emplace(id, std::move(peer)).first->second;
            }

            Peer* Find(PeerId id)
            {
                const auto it = peers_.find(id);
                return it == peers_.end() ? nullptr : &it->second;
            }

            Peer* FindBySteamId(uint64_t steamId)
            {
                for (auto& entry : peers_)
                {
                    if (entry.second.steamId == steamId)
                    {
                        return &entry.second;
                    }
                }
                return nullptr;
            }

            bool SendRaw(Peer& peer, const std::vector<uint8_t>& message, int channel, bool reliable)
            {
                ISteamNetworkingMessages* messages = SteamNetworkingMessages();
                if (messages == nullptr)
                {
                    return false;
                }
                peer.lastSend = now_;
                // AutoRestartBrokenSession: a send into a session that broke starts a new one instead of failing with NoConnection
                const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle) | k_nSteamNetworkingSend_AutoRestartBrokenSession;
                const EResult result = messages->SendMessageToUser(Identity(peer.steamId), message.data(), static_cast<uint32>(message.size()), flags, channel);
                return result == k_EResultOK;
            }

            void RemovePeer(PeerId id, bool tellPeer)
            {
                Peer* peer = Find(id);
                if (peer == nullptr)
                {
                    return;
                }
                if (tellPeer && peer->connected)
                {
                    SendRaw(*peer, {kBye}, kControlChannel, true);
                }
                if (SteamNetworkingMessages() != nullptr)
                {
                    // a reliable Bye may still be on its way: the session closes after the send queue of Steam is empty is not promised, so
                    // closing right away can lose it; the other side finds out by the timeout then
                    SteamNetworkingMessages()->CloseSessionWithUser(Identity(peer->steamId));
                }
                peers_.erase(id);
            }

            void HandleMessage(uint64_t steamId, const uint8_t* data, size_t size, std::vector<NetEvent>& events)
            {
                if (size == 0 || steamId == 0)
                {
                    return;
                }
                Peer* peer = FindBySteamId(steamId);
                const uint8_t kind = data[0];
                if (kind == kHello)
                {
                    if (!listening_ || !IsAllowed(steamId))
                    {
                        return;
                    }
                    if (peer == nullptr)
                    {
                        peer = &AddPeer(steamId);
                        peer->connected = true;
                        peer->lastRecv = now_;
                        NetEvent event;
                        event.type = NetEventType::Connected;
                        event.peer = peer->id;
                        events.push_back(std::move(event));
                    }
                    SendRaw(*peer, {kAccept}, kControlChannel, true);  // again if the first one was lost: the client repeats Hello
                    return;
                }
                if (peer == nullptr)
                {
                    return;
                }
                peer->lastRecv = now_;
                if (kind == kAccept)
                {
                    if (peer->clientSide && !peer->connected)
                    {
                        peer->connected = true;
                        NetEvent event;
                        event.type = NetEventType::Connected;
                        event.peer = peer->id;
                        events.push_back(std::move(event));
                    }
                }
                else if (kind == kBye)
                {
                    events.push_back(DisconnectEvent(peer->id, DisconnectReason::Closed));
                    RemovePeer(peer->id, false);
                }
                else if (kind == kData && peer->connected)
                {
                    NetEvent event;
                    event.type = NetEventType::Data;
                    event.peer = peer->id;
                    event.data.assign(data + 1, data + size);
                    events.push_back(std::move(event));
                }
            }

            // a player that has no session with us yet writes first: the messages are not delivered until we accept
            void OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* request)
            {
                const uint64_t steamId = request->m_identityRemote.GetSteamID64();
                if (listening_ && IsAllowed(steamId))
                {
                    SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
                }
            }

            // the handshake of a session failed (the other side is not there, no route): the peer we wait for will time out; the log says why
            void OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* failed)
            {
                Peer* peer = FindBySteamId(failed->m_info.m_identityRemote.GetSteamID64());
                if (peer != nullptr && peer->connected)
                {
                    deferred_.push_back(DisconnectEvent(peer->id, DisconnectReason::Timeout));
                    peers_.erase(peer->id);
                }
            }

            std::vector<uint64_t> allowed_;
            std::map<PeerId, Peer> peers_;
            std::vector<NetEvent> deferred_;
            PeerId nextPeer_ = 1;
            bool listening_ = false;
            double now_ = 0.0;
            CCallback<SteamTransport, SteamNetworkingMessagesSessionRequest_t> sessionRequest_;
            CCallback<SteamTransport, SteamNetworkingMessagesSessionFailed_t> sessionFailed_;
        };
    }

    std::unique_ptr<INetTransport> MakeSteamTransportFor(const std::vector<uint64_t>& allowed) { return std::make_unique<SteamTransport>(allowed); }
}
