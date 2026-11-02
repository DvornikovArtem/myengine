// RlNet.h
// The netcode of the Rocket League game (docs/game/rl-online.md, section 7): a listen server and clients that predict ahead and roll back.
//
//   RlNetHost    owns the authoritative RlMatch. It steps at 120 Hz, takes the input of every client for the tick it is simulating (an
//                input that did not arrive in time is the last one repeated), and sends 60 Hz snapshots that carry the inputs it really
//                used. Slots of players that left, or never came, are bots.
//   RlNetClient  owns its own RlMatch and lives `lead` ticks ahead of the host: its own input goes straight into the local simulation. A
//                snapshot says which inputs the host used and what the state hash was at its tick; if the inputs differ from what the
//                client simulated (or the hash does), the client restores its saved state of that tick and simulates again up to the
//                present, with the real inputs where it knows them and the last known ones for the other players where it does not. The
//                jump this makes on screen fades in 0.1-0.2 s (RlNetClient::RenderWorld).
//
// The simulation is deterministic (same binary), so replaying the host's inputs reproduces the host's state exactly. If it does not (a
// hash that stays wrong), the client falls back to setting the poses of the cars and the ball from the quantized snapshot and counts a
// "hard correction".
//
// Neither class sleeps or reads a clock: the caller passes `now` (seconds) to Update, so the tests run a virtual clock. Single threaded.

#pragma once

#include <rl/RlCars.h>
#include <rl/RlMatch.h>
#include <rl/RlNetProtocol.h>
#include <rl/net/NetTransport.h>

#include <myengine/net/NetLog.h>
#include <myengine/net/NetRpc.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rl
{
    struct RlNetStats
    {
        double rttMs = 0.0;
        uint32_t tick = 0;                // the next tick to simulate
        uint32_t verifiedTick = 0;        // client: newest tick of the host that the local state is known to match
        int margin = 0;                   // client: how many ticks ahead of the host its inputs arrive (the host sent it); host: of the worst client
        int lead = 0;                     // client: ticks ahead of the host
        uint64_t snapshots = 0;           // sent (host) / received (client)
        uint64_t rollbacks = 0;           // client
        uint64_t hardCorrections = 0;     // client: poses taken from a snapshot because the replay did not reproduce the host
        uint64_t hashMismatches = 0;      // client: snapshots whose state hash differed from ours although all the inputs of the host were known (a hard correction makes the states close, not bit-equal)
        uint64_t unconfirmedHashes = 0;   // client: snapshots whose hash differed while an input of the host was still missing (expected: the replay of the snapshot that brings it fixes the state)
        uint64_t lateInputs = 0;          // host: inputs of a tick that was simulated already
        uint64_t repeatedInputs = 0;      // host: ticks where a client's input was missing and the last one was repeated
        uint64_t lateJoins = 0;           // host: players that joined a running match
        int rollbacksLastSecond = 0;      // client: the "corrections per second" of the overlay
        int lastResimTicks = 0;
        double lastRollbackMs = 0.0;      // CPU time of the thread, like the next one
        double maxRollbackMs = 0.0;
        double stepMsMax = 0.0;           // slowest single tick (host or client); CPU time of the thread (RlClock.h), not wall time
        uint64_t snapshotsMissed = 0;     // client: snapshots that never came (gaps in their ticks): the losses of the unreliable stream
        uint64_t unknownMessages = 0;     // messages of a type nobody handles, thrown away (logged)
        uint64_t malformedMessages = 0;   // messages that did not decode, thrown away (logged)

        // client: the share of the snapshots that were lost, in percent
        double SnapshotLossPercent() const
        {
            const uint64_t expected = snapshots + snapshotsMissed;
            return expected == 0 ? 0.0 : 100.0 * static_cast<double>(snapshotsMissed) / static_cast<double>(expected);
        }
    };

    // A quick chat that the host accepted (RlRpcMethod::QuickChat): every machine shows it in the feed
    struct RlQuickChat
    {
        int slot = -1;
        int phrase = 0;
    };

    // ------------------------------------------------------------------------------------------------------------------------------

    // A guest of the host, for the lobby of a private match
    struct RlNetPeerInfo
    {
        int slot = -1;
        std::string name;
        double rttMs = 0.0;
        int car = 0;              // the car the guest wants (an index of the catalog)
    };

    struct RlNetHostOptions
    {
        RlMatchConfig match;      // mode, seed, botLevel; humanMask is made from the players, the cars from the wishes (below)
        bool hostPlays = true;    // slot 0 is the player of the host (false: a dedicated host, slot 0 is a bot)
        int maxClients = 7;
        const RlCarCatalog* cars = nullptr;  // the cars of the game (must outlive the host); null: the catalog with Pulse alone
        int hostCar = 0;                     // the car of the player of the host
        // The guests say their car with the Join and may change it in the lobby; the bots draw theirs from the seed of the match; the choice is
        // sent to everybody before the match starts (and in the Welcome of a player who joins a running one).
    };

    class RlNetHost
    {
    public:
        RlNetHost(net::INetTransport& transport, const RlTuning& tuning, const RlNetHostOptions& options);
        ~RlNetHost();
        RlNetHost(const RlNetHost&) = delete;
        RlNetHost& operator=(const RlNetHost&) = delete;

        bool Listen(uint16_t port, std::string& error);
        // Builds the match and starts the clock; players joined so far take their slots, the rest play as bots. No players after this.
        bool StartMatch(double now);
        bool Started() const;
        void Update(double now);

        void SetLocalInput(const RlCarInput& input);  // the player of the host (slot 0)
        int ClientCount() const;
        std::vector<RlNetPeerInfo> Peers() const;  // the guests that have a slot
        uint32_t HumanMask() const;
        // How far (0..1) the clock is into the tick that is being waited for: what the host draws interpolates by it.
        float TickFraction() const;
        const RlMatch* Match() const;
        RlMatch* MutableMatch();  // the tests set up odd situations through it
        std::vector<RlEventMsg> TakeEvents();
        const RlNetStats& Stats() const;
        // After each simulated tick: the tick that was just simulated and the match (tests)
        void SetTickObserver(std::function<void(uint32_t, const RlMatch&)> observer);

        // ---- RPC (myengine/net/NetRpc.h). The host serves the methods of RlRpcMethod to its guests; its own player calls the same methods
        // without the network, through the same checks. Every call gives one result in TakeRpcResults.
        uint32_t Call(uint16_t method, const std::vector<uint8_t>& args, const std::string& name = {});
        uint32_t QuickChat(int phrase);
        std::vector<net::RpcResult> TakeRpcResults();
        std::vector<RlQuickChat> TakeQuickChats();          // the accepted quick chats of everybody, the host's included
        const net::RpcCounters& RpcServerCounters() const;  // the calls of the guests and of the host's player
        const net::NetBadMessages& BadMessages() const;     // what the host received and threw away
        const std::string& LastRpcLine() const;             // the last call it served: "QuickChat #3 from peer 2: OK - ..."

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // ------------------------------------------------------------------------------------------------------------------------------

    enum class RlNetClientState
    {
        Idle,
        Connecting,  // handshake with the host
        Joining,     // Join sent, Welcome awaited
        Lobby,       // in; the host has not started the match
        Playing,
        Syncing,     // the match is on: the state of the host is coming in pieces, then the client catches up with it
        Failed,      // see Error()
        Disconnected,
    };

    class RlNetClient
    {
    public:
        // `cars`: the catalog of the game (must outlive the client); null: the catalog with Pulse alone. The host refuses a client with other cars.
        RlNetClient(net::INetTransport& transport, const RlTuning& tuning, const RlCarCatalog* cars = nullptr);
        ~RlNetClient();
        RlNetClient(const RlNetClient&) = delete;
        RlNetClient& operator=(const RlNetClient&) = delete;

        bool Connect(const std::string& address, const std::string& name, std::string& error);
        // The car this player wants (an index of the catalog): before Connect, or in the lobby (the host hears it until the match starts)
        void SetCar(int car);
        void Update(double now);
        void Close();

        RlNetClientState State() const;
        const std::string& Error() const;  // why the state is Failed or Disconnected
        int Slot() const;
        void SetLocalInput(const RlCarInput& input);
        const RlMatch* Match() const;
        // The world to draw: the last two ticks interpolated by the fraction of the tick that has passed, plus the fading correction.
        RlWorldState RenderWorld() const;
        std::vector<RlEventMsg> TakeEvents();
        const RlNetStats& Stats() const;
        // Hash of the saved state of tick `tick` (0 when the ring no longer holds it): the tests compare it with the host's
        bool StateHashAt(uint32_t tick, uint64_t& hash) const;
        // The plain world of a recent tick as the client simulated it (false when the ring no longer holds it)
        bool WorldAt(uint32_t tick, RlWorldState& world) const;
        // The time scale and margin debug (tests)
        double TimeScale() const;
        void SetTickObserver(std::function<void(uint32_t, const RlMatch&)> observer);

        // ---- RPC (myengine/net/NetRpc.h): calls of the methods of the host (RlRpcMethod). Every call gives one result in TakeRpcResults: the
        // reply of the host, a Timeout after 3 s without it, or Closed when the connection ends first.
        uint32_t Call(uint16_t method, const std::vector<uint8_t>& args, const std::string& name = {});
        uint32_t QuickChat(int phrase);
        std::vector<net::RpcResult> TakeRpcResults();
        std::vector<RlQuickChat> TakeQuickChats();       // the quick chats the host accepted (ours included)
        const net::RpcCounters& RpcClientCounters() const;
        const net::NetBadMessages& BadMessages() const;  // what this client received and threw away

        // ---- the checks of the defense (docs/network/lab4-demo.md)
        // Sends what the host cannot use: a message of an unknown type, a cut Input and a cut RpcCall. The host counts and logs them and goes on.
        void SendBrokenMessages();
        // The protocol version the Join says (0: kNetProtocolVersion), before Connect: a host of another version refuses the client
        void SetProtocolVersion(uint16_t version);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
