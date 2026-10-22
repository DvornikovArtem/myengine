// NetTransport.h
// The transport of the Rocket League netcode (docs/game/rl-online.md, 7.3): peers, messages, reliable and unreliable sends. The game code
// talks only to INetTransport, so the loopback of the tests, the UDP of two windows and a later Steam transport are interchangeable.
// Time is passed in by the caller (seconds, any origin): the tests run a virtual clock and never sleep.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rl::net
{
    using PeerId = uint32_t;
    constexpr PeerId kInvalidPeer = 0;

    // A message fits one packet (7.2): bigger ones are refused by Send.
    constexpr size_t kMaxMessageBytes = 1100;

    enum class NetEventType
    {
        Connected,     // a peer is there: the host sees a new client, the client sees that the host answered
        Disconnected,  // the peer left or timed out; `reason` says which
        Data,
    };

    enum class DisconnectReason
    {
        None,
        Closed,    // the peer said goodbye
        Timeout,   // nothing heard from it
        Rejected,  // the host refused the connection (full, other protocol)
        Local,     // we closed it
    };

    struct NetEvent
    {
        NetEventType type = NetEventType::Data;
        PeerId peer = kInvalidPeer;
        DisconnectReason reason = DisconnectReason::None;
        bool reliable = false;
        std::vector<uint8_t> data;
    };

    // Delay, jitter and losses added to what a transport sends (the `--net-sim latency=100,jitter=20,loss=5` switch and the tests).
    // Applied to every outgoing packet, so the round trip costs twice the latency.
    struct NetSimParams
    {
        float latencyMs = 0.0f;
        float jitterMs = 0.0f;      // each packet is moved by up to +-jitter, so packets may swap places
        float lossPercent = 0.0f;
        float duplicatePercent = 0.0f;

        bool Active() const { return latencyMs > 0.0f || jitterMs > 0.0f || lossPercent > 0.0f || duplicatePercent > 0.0f; }
        // "latency=100,jitter=20,loss=5,dup=1"; returns false (and the reason in `error`) for an unknown key or a bad number
        static bool Parse(const std::string& text, NetSimParams& out, std::string* error = nullptr);
    };

    class INetTransport
    {
    public:
        virtual ~INetTransport() = default;

        // Host: accepts connections on `port` (0 = any, see LocalPort).
        virtual bool Listen(uint16_t port, std::string& error) = 0;
        // Client: starts the handshake with "ip:port" ("localhost" and the loopback names of the tests work too); the returned peer is the
        // host. Connected or Disconnected(Timeout / Rejected) arrives from Poll later.
        virtual PeerId Connect(const std::string& address, std::string& error) = 0;
        // Sends one message of at most kMaxMessageBytes. Reliable messages arrive once and in order; unreliable ones may be lost,
        // duplicated or reordered.
        virtual bool Send(PeerId peer, const uint8_t* data, size_t size, bool reliable) = 0;
        // Receives, sends what is due (retransmits, keepalives, delayed packets) and appends the events.
        virtual void Poll(double now, std::vector<NetEvent>& events) = 0;
        virtual void Disconnect(PeerId peer) = 0;
        virtual void Close() = 0;

        // Smoothed round trip time to the peer in seconds, 0 until measured.
        virtual double RoundTripSeconds(PeerId peer) const = 0;
        virtual uint16_t LocalPort() const = 0;
        virtual bool IsConnected(PeerId peer) const = 0;
    };
}
