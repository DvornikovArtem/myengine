// RlNetTests.cpp
// Headless tests of the Rocket League netcode (game/rl, G5): the transport (reliable channel over loss, jitter and duplicates), the
// messages, and a host with clients that predict and roll back. Time is a virtual clock, so a minute of play takes seconds; one test
// goes over real UDP sockets on 127.0.0.1. `--verbose` prints more.

#include <rl/RlNet.h>
#include <rl/RlNetProtocol.h>
#include <rl/net/NetLink.h>
#include <rl/net/NetReliable.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    using namespace rl;

    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;

    void Check(bool ok, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL %s (%s:%d)\n", what, file, line);
        }
    }
#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    template <typename... Args>
    void Note(const char* format, Args... args)
    {
        std::printf("  ");
        std::printf(format, args...);
        std::printf("\n");
    }

    RlTuning g_tuning;

    // What a player does: a mix of driving, boost, jumps and flips, different for every seed
    RlCarInput Script(int seed, double t)
    {
        RlCarInput in;
        in.throttle = 1.0f;
        in.steer = static_cast<float>(std::sin(t * 0.7 + seed) * 0.8);
        in.boost = std::fmod(t + seed, 6.0) < 2.0;
        in.jump = std::fmod(t * 1.3 + seed * 0.37, 4.0) < 0.12;
        in.pitch = std::fmod(t * 0.9 + seed, 7.0) < 0.5 ? -1.0f : 0.0f;
        in.handbrake = std::fmod(t + seed * 2.0, 9.0) < 0.6;
        return in;
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // A host and clients on a loopback with a virtual clock
    // ------------------------------------------------------------------------------------------------------------------------------

    struct Rig
    {
        net::LoopbackHub hub;
        net::NetSimParams sim;
        std::unique_ptr<net::INetTransport> hostTransport;
        std::vector<std::unique_ptr<net::INetTransport>> clientTransports;
        std::unique_ptr<RlNetHost> host;
        std::vector<std::unique_ptr<RlNetClient>> clients;
        std::vector<bool> clientActive;
        std::map<uint32_t, uint64_t> hostHashes;  // tick -> hash of the host's state at that tick
        std::map<uint32_t, RlWorldState> hostWorlds;  // tick -> the host's world at that tick
        double now = 0.0;
        bool recordHashes = false;
        bool scripted = true;

        explicit Rig(const net::NetSimParams& simParams, int mode, const RlTuning& tuning = g_tuning)
            : sim(simParams)
        {
            hostTransport = net::MakeLoopbackTransport(hub, sim, 1);
            RlNetHostOptions options;
            options.match.mode = mode;
            options.match.seed = 77;
            host = std::make_unique<RlNetHost>(*hostTransport, tuning, options);
            std::string error;
            CHECK(host->Listen(5000, error));
            host->SetTickObserver(
                [this](uint32_t tick, const RlMatch& match)
                {
                    if (!recordHashes)
                    {
                        return;
                    }
                    std::vector<uint8_t> bytes;
                    match.SaveState(bytes);
                    hostHashes[tick] = RlSim::HashState(bytes);
                    hostWorlds[tick] = match.World();
                });
        }

        RlNetClient& AddClient(const RlTuning& tuning = g_tuning)
        {
            const int index = static_cast<int>(clients.size());
            clientTransports.push_back(net::MakeLoopbackTransport(hub, sim, 100 + static_cast<uint64_t>(index)));
            clients.push_back(std::make_unique<RlNetClient>(*clientTransports.back(), tuning));
            clientActive.push_back(true);
            std::string error;
            CHECK(clients.back()->Connect("5000", "player" + std::to_string(index), error));
            return *clients.back();
        }

        void Step(double dt)
        {
            now += dt;
            if (scripted)
            {
                host->SetLocalInput(Script(0, now));
                for (size_t i = 0; i < clients.size(); ++i)
                {
                    clients[i]->SetLocalInput(Script(static_cast<int>(i) + 1, now));
                }
            }
            host->Update(now);
            for (size_t i = 0; i < clients.size(); ++i)
            {
                if (clientActive[i])
                {
                    clients[i]->Update(now);
                }
            }
        }

        void Run(double seconds, double dt = 1.0 / 240.0)
        {
            const double end = now + seconds;
            while (now < end)
            {
                Step(dt);
            }
        }

        bool RunUntil(double maxSeconds, bool (*done)(Rig&))
        {
            const double end = now + maxSeconds;
            while (now < end)
            {
                Step(1.0 / 240.0);
                if (done(*this))
                {
                    return true;
                }
            }
            return false;
        }
    };

    bool AllInLobby(Rig& rig)
    {
        for (const auto& client : rig.clients)
        {
            if (client->State() != RlNetClientState::Lobby)
            {
                return false;
            }
        }
        return true;
    }

    // client states against the host's: how many of the last ticks the client has verified and found equal
    int CompareWithHost(Rig& rig, RlNetClient& client, int span, int* compared = nullptr)
    {
        int equal = 0;
        int total = 0;
        const uint32_t verified = client.Stats().verifiedTick;
        for (uint32_t t = verified > static_cast<uint32_t>(span) ? verified - static_cast<uint32_t>(span) : 1; t <= verified; ++t)
        {
            uint64_t clientHash = 0;
            const auto it = rig.hostHashes.find(t);
            if (it == rig.hostHashes.end() || !client.StateHashAt(t, clientHash))
            {
                continue;
            }
            ++total;
            equal += clientHash == it->second ? 1 : 0;
        }
        if (compared != nullptr)
        {
            *compared = total;
        }
        return equal;
    }

    // The same for a client that took poses from a snapshot: its states are close to the host's, not bit-equal
    int CompareWorldsWithHost(Rig& rig, RlNetClient& client, int span, int* compared)
    {
        int close = 0;
        int total = 0;
        const uint32_t verified = client.Stats().verifiedTick;
        for (uint32_t t = verified > static_cast<uint32_t>(span) ? verified - static_cast<uint32_t>(span) : 1; t <= verified; ++t)
        {
            RlWorldState mine;
            const auto it = rig.hostWorlds.find(t);
            if (it == rig.hostWorlds.end() || !client.WorldAt(t, mine))
            {
                continue;
            }
            ++total;
            const RlWorldState& theirs = it->second;
            const auto distance = [](const RlVec3& a, const RlVec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
            bool same = distance(mine.ball.position, theirs.ball.position) < 0.1f && mine.match.score[0] == theirs.match.score[0] &&
                        mine.match.score[1] == theirs.match.score[1];
            for (int i = 0; i < mine.carCount && same; ++i)
            {
                same = distance(mine.cars[i].position, theirs.cars[i].position) < 0.1f;
            }
            close += same ? 1 : 0;
        }
        *compared = total;
        return close;
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // Tests
    // ------------------------------------------------------------------------------------------------------------------------------

    void TestSimParams()
    {
        std::printf("net-sim parameters\n");
        net::NetSimParams params;
        std::string error;
        CHECK(net::NetSimParams::Parse("latency=100,jitter=20,loss=5", params, &error));
        CHECK(params.latencyMs == 100.0f && params.jitterMs == 20.0f && params.lossPercent == 5.0f && params.Active());
        CHECK(!net::NetSimParams::Parse("lag=3", params, &error));
        CHECK(error.find("lag") != std::string::npos);
        CHECK(!net::NetSimParams::Parse("latency=abc", params, &error));
        CHECK(!net::NetSimParams::Parse("latency", params, &error));
        CHECK(net::NetSimParams::Parse("", params, &error) && !params.Active());
    }

    void TestReliableChannel()
    {
        std::printf("reliable channel over loss, jitter, duplicates and reordering\n");
        net::LoopbackHub hub;
        net::NetSimParams sim;
        sim.latencyMs = 50.0f;
        sim.jitterMs = 30.0f;
        sim.lossPercent = 20.0f;
        sim.duplicatePercent = 5.0f;
        auto server = net::MakeLoopbackTransport(hub, sim, 3);
        auto client = net::MakeLoopbackTransport(hub, sim, 4);
        std::string error;
        CHECK(server->Listen(6000, error));
        const net::PeerId toServer = client->Connect("6000", error);
        CHECK(toServer != net::kInvalidPeer);

        double now = 0.0;
        net::PeerId toClient = net::kInvalidPeer;
        bool clientConnected = false;
        int reliableSent = 0;
        int unreliableSent = 0;
        int nextExpected = 0;
        int unreliableGot = 0;
        bool inOrder = true;
        bool sizesOk = true;
        std::vector<net::NetEvent> events;
        for (int step = 0; step < 24 * 120; ++step)
        {
            now += 1.0 / 120.0;
            events.clear();
            server->Poll(now, events);
            for (const net::NetEvent& event : events)
            {
                if (event.type == net::NetEventType::Connected)
                {
                    toClient = event.peer;
                }
                else if (event.type == net::NetEventType::Data && event.reliable)
                {
                    int value = 0;
                    std::memcpy(&value, event.data.data(), sizeof(value));
                    inOrder = inOrder && value == nextExpected;
                    sizesOk = sizesOk && event.data.size() == 4 + static_cast<size_t>(value % 700);
                    ++nextExpected;
                }
                else if (event.type == net::NetEventType::Data)
                {
                    ++unreliableGot;
                }
            }
            events.clear();
            client->Poll(now, events);
            for (const net::NetEvent& event : events)
            {
                clientConnected = clientConnected || event.type == net::NetEventType::Connected;
            }
            if (clientConnected && step < 12 * 120)
            {
                for (int k = 0; k < 2 && reliableSent < 400; ++k)
                {
                    std::vector<uint8_t> message(4 + static_cast<size_t>(reliableSent % 700), 0xAB);
                    std::memcpy(message.data(), &reliableSent, sizeof(int));
                    CHECK(client->Send(toServer, message.data(), message.size(), true));
                    ++reliableSent;
                }
                const uint8_t small[8] = {1, 2, 3, 4, 5, 6, 7, 8};
                client->Send(toServer, small, sizeof(small), false);
                ++unreliableSent;
            }
        }
        CHECK(clientConnected && toClient != net::kInvalidPeer);
        CHECK(reliableSent == 400);
        CHECK(nextExpected == reliableSent);  // every reliable message arrived, once
        CHECK(inOrder);
        CHECK(sizesOk);
        CHECK(unreliableGot > unreliableSent / 2 && unreliableGot < unreliableSent + unreliableSent / 5);
        Note("reliable %d/%d in order, unreliable %d of %d, rtt %.0f ms", nextExpected, reliableSent, unreliableGot, unreliableSent,
             client->RoundTripSeconds(toServer) * 1000.0);
        CHECK(client->RoundTripSeconds(toServer) > 0.05);

        std::vector<uint8_t> big(net::kMaxMessageBytes + 1, 0);
        CHECK(!client->Send(toServer, big.data(), big.size(), false));
        CHECK(!client->Send(toServer, big.data(), big.size(), true));
        big.resize(net::kMaxMessageBytes);
        CHECK(client->Send(toServer, big.data(), big.size(), true));
    }

    void TestConnectionLifecycle()
    {
        std::printf("connection: refused, closed, timed out\n");
        net::LoopbackHub hub;
        std::string error;

        // nobody listens: the handshake gives up
        {
            auto client = net::MakeLoopbackTransport(hub);
            const net::PeerId peer = client->Connect("6100", error);
            CHECK(peer != net::kInvalidPeer);
            bool timedOut = false;
            std::vector<net::NetEvent> events;
            for (double now = 0.0; now < 12.0 && !timedOut; now += 0.05)
            {
                events.clear();
                client->Poll(now, events);
                for (const net::NetEvent& event : events)
                {
                    timedOut = timedOut || (event.type == net::NetEventType::Disconnected && event.reason == net::DisconnectReason::Timeout);
                }
            }
            CHECK(timedOut);
        }

        // closed by the client, then timed out when a side goes silent
        {
            auto server = net::MakeLoopbackTransport(hub);
            auto client = net::MakeLoopbackTransport(hub);
            CHECK(server->Listen(6101, error));
            const net::PeerId toServer = client->Connect("6101", error);
            double now = 0.0;
            std::vector<net::NetEvent> events;
            bool serverSawConnect = false;
            bool serverSawClose = false;
            bool serverSawTimeout = false;
            for (int step = 0; step < 20 * 100; ++step)
            {
                now += 0.01;
                events.clear();
                server->Poll(now, events);
                for (const net::NetEvent& event : events)
                {
                    serverSawConnect = serverSawConnect || event.type == net::NetEventType::Connected;
                    if (event.type == net::NetEventType::Disconnected)
                    {
                        serverSawClose = serverSawClose || event.reason == net::DisconnectReason::Closed;
                        serverSawTimeout = serverSawTimeout || event.reason == net::DisconnectReason::Timeout;
                    }
                }
                if (step < 3 * 100)
                {
                    events.clear();
                    client->Poll(now, events);
                }
                if (step == 3 * 100)
                {
                    client->Disconnect(toServer);
                }
            }
            CHECK(serverSawConnect);
            CHECK(serverSawClose);
            CHECK(!serverSawTimeout);
        }
        {
            auto server = net::MakeLoopbackTransport(hub);
            auto client = net::MakeLoopbackTransport(hub);
            CHECK(server->Listen(6102, error));
            client->Connect("6102", error);
            double now = 0.0;
            std::vector<net::NetEvent> events;
            bool timeout = false;
            for (int step = 0; step < 15 * 100; ++step)
            {
                now += 0.01;
                events.clear();
                server->Poll(now, events);
                for (const net::NetEvent& event : events)
                {
                    timeout = timeout || (event.type == net::NetEventType::Disconnected && event.reason == net::DisconnectReason::Timeout);
                }
                if (step < 2 * 100)
                {
                    events.clear();
                    client->Poll(now, events);  // then the client is silent
                }
            }
            CHECK(timeout);
        }
    }

    void TestProtocol()
    {
        std::printf("messages: layout, size and quantization\n");
        std::vector<uint8_t> bytes;

        RlJoinMsg join;
        join.tuningHash = 0x1122334455667788ull;
        join.name = "alice";
        RlEncode(join, bytes);
        RlJoinMsg joinBack;
        CHECK(RlDecode(bytes.data(), bytes.size(), joinBack) && joinBack.name == "alice" && joinBack.tuningHash == join.tuningHash);
        CHECK(!RlDecode(bytes.data(), bytes.size() - 1, joinBack));  // cut short

        RlWelcomeMsg welcome;
        welcome.slot = 5;
        welcome.mode = 3;
        welcome.seed = 99;
        welcome.humanMask = 0x25;
        RlEncode(welcome, bytes);
        RlWelcomeMsg welcomeBack;
        CHECK(RlDecode(bytes.data(), bytes.size(), welcomeBack) && welcomeBack.slot == 5 && welcomeBack.humanMask == 0x25 && welcomeBack.seed == 99);

        RlInputMsg input;
        input.firstTick = 1234;
        input.count = kNetInputWindow;
        for (int i = 0; i < input.count; ++i)
        {
            input.inputs[i].throttle = 1.0f - 0.25f * static_cast<float>(i);
            input.inputs[i].jump = (i % 2) == 0;
            input.inputs[i] = RlQuantizeInput(input.inputs[i]);
        }
        RlEncode(input, bytes);
        const size_t inputSize = bytes.size();
        RlInputMsg inputBack;
        CHECK(RlDecode(bytes.data(), bytes.size(), inputBack) && inputBack.firstTick == 1234 && inputBack.count == kNetInputWindow);
        for (int i = 0; i < input.count; ++i)
        {
            CHECK(RlSameInput(input.inputs[i], inputBack.inputs[i]));
        }
        const RlCarInput odd = RlQuantizeInput(RlCarInput{0.333f, -0.777f, 0.1f, 0.0f, true, false, true, false});
        CHECK(RlSameInput(odd, RlQuantizeInput(odd)));  // quantizing twice changes nothing

        RlEventMsg event;
        event.kind = RlEventKind::Goal;
        event.tick = 4000;
        event.slot = -1;
        event.a = 1;
        RlEncode(event, bytes);
        RlEventMsg eventBack;
        CHECK(RlDecode(bytes.data(), bytes.size(), eventBack) && eventBack.kind == RlEventKind::Goal && eventBack.slot == -1 && eventBack.a == 1);

        // a full 4v4 snapshot
        RlSnapshotMsg snapshot;
        snapshot.tick = 5000;
        snapshot.humanMask = 0xFF;
        snapshot.stateHash = 0xDEADBEEFCAFEull;
        snapshot.inputCount = kNetInputWindow;
        snapshot.world.carCount = 8;
        snapshot.world.ball.position = {1.234f, 3.5f, -40.321f};
        snapshot.world.ball.linearVelocity = {12.5f, -3.0f, 30.0f};
        snapshot.world.ball.angularVelocity = {1.0f, -2.0f, 3.0f};
        snapshot.world.ball.rotation = {0.1f, 0.2f, 0.3f, 0.9273618f};
        for (int i = 0; i < 8; ++i)
        {
            RlCarState& car = snapshot.world.cars[i];
            car.position = {-30.0f + 7.5f * static_cast<float>(i), 0.17f + 0.2f * static_cast<float>(i), 20.0f - 5.0f * static_cast<float>(i)};
            car.linearVelocity = {-22.0f + 5.0f * static_cast<float>(i), 1.5f, 10.0f};
            car.angularVelocity = {0.5f, 2.0f - static_cast<float>(i), -5.0f};
            car.rotation = {-0.5f, 0.5f, 0.5f, 0.5f};
            car.boost = 12.0f * static_cast<float>(i);
            car.onGround = i % 2 == 0;
            car.canDodge = i % 3 == 0;
            for (int j = 0; j < kNetInputWindow; ++j)
            {
                RlPackInput(RlCarInput{1.0f, 0.5f, 0.0f, 0.0f, true, j == 3, false, false}, snapshot.inputs[i][j]);
            }
        }
        snapshot.world.match.score[0] = 2;
        snapshot.world.match.score[1] = 1;
        snapshot.world.match.clock = 123.5f;
        snapshot.world.match.phase = RlMatchPhase::Playing;
        RlEncode(snapshot, bytes);
        Note("sizes: Input %zu B (8 ticks), 4v4 Snapshot %zu B (limit %zu)", inputSize, bytes.size(), net::kMaxMessageBytes);
        CHECK(bytes.size() <= net::kMaxMessageBytes);
        CHECK(inputSize <= 64);
        RlSnapshotMsg snapshotBack;
        CHECK(RlDecode(bytes.data(), bytes.size(), snapshotBack));
        CHECK(snapshotBack.tick == 5000 && snapshotBack.stateHash == snapshot.stateHash && snapshotBack.world.carCount == 8);
        CHECK(snapshotBack.world.match.score[0] == 2 && snapshotBack.world.match.score[1] == 1);
        CHECK(std::fabs(snapshotBack.world.match.clock - 123.5f) < 1e-6f);
        float worstPosition = 0.0f;
        float worstVelocity = 0.0f;
        float worstRotation = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            const RlCarState& a = snapshot.world.cars[i];
            const RlCarState& b = snapshotBack.world.cars[i];
            worstPosition = std::max({worstPosition, std::fabs(a.position.x - b.position.x), std::fabs(a.position.y - b.position.y), std::fabs(a.position.z - b.position.z)});
            worstVelocity = std::max({worstVelocity, std::fabs(a.linearVelocity.x - b.linearVelocity.x), std::fabs(a.linearVelocity.y - b.linearVelocity.y)});
            const float dot = a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y + a.rotation.z * b.rotation.z + a.rotation.w * b.rotation.w;
            worstRotation = std::max(worstRotation, 2.0f * std::acos(std::min(1.0f, std::fabs(dot))));
            CHECK(a.onGround == b.onGround && a.canDodge == b.canDodge);
            CHECK(std::fabs(a.boost - b.boost) < 0.5f);
            for (int j = 0; j < kNetInputWindow; ++j)
            {
                CHECK(std::memcmp(snapshot.inputs[i][j], snapshotBack.inputs[i][j], kNetInputBytes) == 0);
            }
        }
        Note("quantization error: position %.4f m, velocity %.4f m/s, rotation %.4f rad", worstPosition, worstVelocity, worstRotation);
        CHECK(worstPosition < 0.003f);
        CHECK(worstVelocity < 0.004f);
        CHECK(worstRotation < 0.005f);

        // a snapshot that was cut, or garbage, never crashes the decoder
        for (size_t cut = 0; cut < bytes.size(); cut += 7)
        {
            RlSnapshotMsg junk;
            RlDecode(bytes.data(), cut, junk);
        }
        const uint8_t noise[3] = {5, 0xFF, 0xFF};
        RlSnapshotMsg junk;
        CHECK(!RlDecode(noise, sizeof(noise), junk));

        // the same input data hashes the same: the tuning hash is stable and sensitive
        RlTuning other = g_tuning;
        other.gravity += 0.1f;
        CHECK(RlTuningHash(g_tuning) == RlTuningHash(g_tuning));
        CHECK(RlTuningHash(g_tuning) != RlTuningHash(other));
    }

    // Prints what a run of host and client did
    void Report(const char* title, Rig& rig, RlNetClient& client)
    {
        const RlNetStats& c = client.Stats();
        const RlNetStats& h = rig.host->Stats();
        Note("%s: host tick %u, client tick %u (verified %u), lead %d ticks, margin %d", title, h.tick, c.tick, c.verifiedTick, c.lead, c.margin);
        Note("  rtt %.0f ms; client: %llu snapshots, %llu rollbacks (last %d ticks %.2f ms, max %.2f ms), %llu hash mismatches, %llu hard corrections",
             c.rttMs, static_cast<unsigned long long>(c.snapshots), static_cast<unsigned long long>(c.rollbacks), c.lastResimTicks, c.lastRollbackMs,
             c.maxRollbackMs, static_cast<unsigned long long>(c.hashMismatches), static_cast<unsigned long long>(c.hardCorrections));
        Note("  host: %llu snapshots, %llu late inputs, %llu repeated inputs, slowest tick %.2f ms; client slowest tick %.2f ms",
             static_cast<unsigned long long>(h.snapshots), static_cast<unsigned long long>(h.lateInputs),
             static_cast<unsigned long long>(h.repeatedInputs), h.stepMsMax, c.stepMsMax);
    }

    void TestHostAndClient(const char* title, const net::NetSimParams& sim, double seconds)
    {
        std::printf("host + client, %s\n", title);
        Rig rig(sim, 2);
        rig.recordHashes = true;
        RlNetClient& client = rig.AddClient();
        CHECK(rig.RunUntil(10.0, &AllInLobby));
        CHECK(client.Slot() == 2);  // the first guest takes the orange team
        CHECK(rig.host->StartMatch(rig.now));
        if (g_verbose)
        {
            uint64_t lastLate = 0;
            uint64_t lastRollbacks = 0;
            for (int chunk = 0; chunk < static_cast<int>(seconds / 2.0); ++chunk)
            {
                rig.Run(2.0);
                Note("  t=%2d s: late inputs +%llu, rollbacks +%llu, margin %d, lead %d, time scale %.3f", (chunk + 1) * 2,
                     static_cast<unsigned long long>(rig.host->Stats().lateInputs - lastLate),
                     static_cast<unsigned long long>(client.Stats().rollbacks - lastRollbacks), client.Stats().margin, client.Stats().lead, client.TimeScale());
                lastLate = rig.host->Stats().lateInputs;
                lastRollbacks = client.Stats().rollbacks;
            }
        }
        else
        {
            rig.Run(seconds);
        }

        CHECK(client.State() == RlNetClientState::Playing);
        Report(title, rig, client);
        const RlNetStats& c = client.Stats();
        const RlNetStats& h = rig.host->Stats();
        CHECK(h.tick > static_cast<uint32_t>(seconds * 100.0));
        CHECK(c.snapshots > 0);
        CHECK(c.verifiedTick + 90 > h.tick);  // snapshots keep verifying the client's state
        CHECK(c.hardCorrections == 0);        // the replay of the host's inputs reproduces the host exactly
        CHECK(c.hashMismatches == 0);
        int compared = 0;
        const int equal = CompareWithHost(rig, client, 60, &compared);
        CHECK(compared >= 30);
        CHECK(equal == compared);
        Note("states of the last %d verified ticks equal to the host's: %d", compared, equal);
        // the client lives ahead of the host by about half the round trip plus the margin
        const int lead = static_cast<int>(c.tick) - static_cast<int>(h.tick);
        CHECK(lead >= 0 && lead < 60);
        CHECK(c.maxRollbackMs < 12.0);
        // the world to draw is near the physics state
        const RlWorldState shown = client.RenderWorld();
        const RlWorldState& physics = client.Match()->World();
        CHECK(std::fabs(shown.ball.position.x - physics.ball.position.x) < 2.0f);
    }

    void TestGoalAndOutOfBandChange()
    {
        std::printf("a goal on the host is a goal on the client; a change only the host made is corrected\n");
        net::NetSimParams sim;
        sim.latencyMs = 50.0f;
        sim.jitterMs = 20.0f;
        sim.lossPercent = 5.0f;
        Rig rig(sim, 1);
        rig.recordHashes = true;
        RlNetClient& client = rig.AddClient();
        CHECK(rig.RunUntil(10.0, &AllInLobby));
        CHECK(rig.host->StartMatch(rig.now));
        rig.Run(5.0);  // the countdown is over
        CHECK(rig.host->Match()->World().match.phase == RlMatchPhase::Playing);

        // the host puts the ball into the blue goal (-Z): the clients are never told, they must see it in a snapshot
        rig.host->MutableMatch()->MutableSim().ResetBall({0.0f, 1.5f, -44.0f}, {0.0f, 0.0f, -45.0f});
        rig.Run(4.0);

        const RlWorldState& hostWorld = rig.host->Match()->World();
        const RlWorldState& clientWorld = client.Match()->World();
        Report("after the goal", rig, client);
        CHECK(hostWorld.match.score[1] == 1);                                       // orange scored
        CHECK(clientWorld.match.score[1] == hostWorld.match.score[1]);              // the same on the client
        CHECK(clientWorld.match.score[0] == hostWorld.match.score[0]);
        CHECK(client.Stats().hardCorrections > 0);                                  // the way the client learned about the push
        bool goalEvent = false;
        for (const RlEventMsg& event : client.TakeEvents())
        {
            goalEvent = goalEvent || (event.kind == RlEventKind::Goal && event.a == 1);
        }
        CHECK(goalEvent);

        // after the correction the two simulations run in step again
        rig.Run(8.0);
        int compared = 0;
        const int close = CompareWorldsWithHost(rig, client, 40, &compared);
        Note("after the goal: %d of %d recent worlds within 10 cm of the host's; score host %d:%d client %d:%d; %llu hard corrections in all", close, compared,
             rig.host->Match()->World().match.score[0], rig.host->Match()->World().match.score[1], client.Match()->World().match.score[0],
             client.Match()->World().match.score[1], static_cast<unsigned long long>(client.Stats().hardCorrections));
        CHECK(compared >= 20 && close == compared);
        CHECK(client.Stats().hardCorrections < 20);  // a few, not one per snapshot
    }

    void TestSeveralClientsAndLeaving()
    {
        std::printf("three players, one of them leaves in the middle of the match\n");
        net::NetSimParams sim;
        sim.latencyMs = 50.0f;
        sim.jitterMs = 20.0f;
        sim.lossPercent = 5.0f;
        Rig rig(sim, 2);
        rig.recordHashes = true;
        RlNetClient& first = rig.AddClient();
        RlNetClient& second = rig.AddClient();
        CHECK(rig.RunUntil(10.0, &AllInLobby));
        CHECK((first.Slot() == 2 && second.Slot() == 1) || (first.Slot() == 1 && second.Slot() == 2));  // orange first, then blue: who is first depends on the network
        CHECK(rig.host->StartMatch(rig.now));
        rig.Run(6.0);
        const int leaverSlot = second.Slot();
        const int stayerSlot = first.Slot();
        const uint32_t all = 1u | (1u << leaverSlot) | (1u << stayerSlot);
        CHECK(rig.host->HumanMask() == all);
        CHECK(!rig.host->Match()->IsBot(leaverSlot) && !rig.host->Match()->IsBot(stayerSlot));

        CHECK(first.Slot() != second.Slot());
        second.Close();
        rig.clientActive[1] = false;
        rig.Run(8.0);
        CHECK(rig.host->ClientCount() == 1);
        CHECK(rig.host->HumanMask() == (all & ~(1u << leaverSlot)));
        CHECK(rig.host->Match()->IsBot(leaverSlot));  // the slot plays on as a bot
        bool leftEvent = false;
        for (const RlEventMsg& event : first.TakeEvents())
        {
            leftEvent = leftEvent || (event.kind == RlEventKind::SlotMask && event.slot == leaverSlot && event.a == 0);
        }
        CHECK(leftEvent);
        CHECK(first.Match()->IsBot(leaverSlot));

        Report("the remaining client", rig, first);
        CHECK(first.Stats().hardCorrections == 0);
        int compared = 0;
        const int equal = CompareWithHost(rig, first, 40, &compared);
        CHECK(compared >= 20 && equal == compared);

        // and a client that just stops talking is dropped by the timeout
        rig.clientActive[0] = false;
        rig.Run(8.0);
        CHECK(rig.host->ClientCount() == 0);
        CHECK(rig.host->HumanMask() == 0x1u);
        CHECK(rig.host->Match()->IsBot(stayerSlot));
    }

    void TestJoinRefusals()
    {
        std::printf("a client with other numbers, a late client, a full match\n");
        net::NetSimParams sim;
        sim.latencyMs = 30.0f;
        sim.lossPercent = 10.0f;
        {
            Rig rig(sim, 1);
            RlTuning other = g_tuning;
            other.gravity += 0.5f;
            RlNetClient& client = rig.AddClient(other);
            rig.Run(6.0);
            CHECK(client.State() == RlNetClientState::Failed);
            Note("tuning mismatch: \"%s\"", client.Error().c_str());
            CHECK(client.Error().find("rl_tuning") != std::string::npos);
            CHECK(rig.host->ClientCount() == 0);
        }
        {
            Rig rig(sim, 1);  // 1v1: the host and one guest
            RlNetClient& guest = rig.AddClient();
            RlNetClient& extra = rig.AddClient();
            rig.Run(6.0);
            const bool oneFailed = (guest.State() == RlNetClientState::Failed) != (extra.State() == RlNetClientState::Failed);
            CHECK(oneFailed);
            RlNetClient& refused = guest.State() == RlNetClientState::Failed ? guest : extra;
            Note("full match: \"%s\"", refused.Error().c_str());
            CHECK(refused.Error().find("slot") != std::string::npos);
            CHECK(rig.host->StartMatch(rig.now));
            RlNetClient& late = rig.AddClient();
            rig.Run(6.0);
            CHECK(late.State() == RlNetClientState::Failed);
            Note("late: \"%s\"", late.Error().c_str());
            CHECK(late.Error().find("started") != std::string::npos);
        }
        {
            // a lossy start: Join, Welcome and MatchStart all have to be repeated
            net::NetSimParams bad;
            bad.latencyMs = 40.0f;
            bad.lossPercent = 45.0f;
            bad.duplicatePercent = 20.0f;
            Rig rig(bad, 1);
            RlNetClient& client = rig.AddClient();
            CHECK(rig.RunUntil(30.0, &AllInLobby));
            CHECK(rig.host->StartMatch(rig.now));
            rig.Run(12.0);
            CHECK(client.State() == RlNetClientState::Playing);
            Note("45%% loss: client state %d, tick %u", static_cast<int>(client.State()), client.Stats().tick);
            CHECK(client.Stats().tick > 100);
        }
    }

    void TestUdp()
    {
        std::printf("two transports over real UDP on 127.0.0.1\n");
        net::NetSimParams sim;
        sim.latencyMs = 15.0f;
        sim.jitterMs = 5.0f;
        auto hostTransport = net::MakeUdpTransport(sim, 5);
        auto clientTransport = net::MakeUdpTransport(sim, 6);
        RlNetHostOptions options;
        options.match.mode = 1;
        options.match.seed = 5;
        RlNetHost host(*hostTransport, g_tuning, options);
        std::string error;
        CHECK(host.Listen(0, error));
        const uint16_t port = hostTransport->LocalPort();
        CHECK(port != 0);
        RlNetClient client(*clientTransport, g_tuning);
        CHECK(client.Connect("127.0.0.1:" + std::to_string(port), "udp", error));

        const auto start = std::chrono::steady_clock::now();
        const auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
        bool started = false;
        double startedAt = 0.0;
        while (seconds() < 8.0)
        {
            const double now = seconds();
            host.SetLocalInput(Script(0, now));
            client.SetLocalInput(Script(1, now));
            host.Update(now);
            client.Update(now);
            if (!started && client.State() == RlNetClientState::Lobby)
            {
                started = host.StartMatch(now);
                startedAt = now;
            }
            if (started && now - startedAt > 4.0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(started);
        CHECK(client.State() == RlNetClientState::Playing);
        const RlNetStats& c = client.Stats();
        Note("udp: host tick %u, client tick %u (verified %u), rtt %.1f ms, %llu rollbacks, %llu hard corrections", host.Stats().tick, c.tick, c.verifiedTick,
             c.rttMs, static_cast<unsigned long long>(c.rollbacks), static_cast<unsigned long long>(c.hardCorrections));
        CHECK(host.Stats().tick > 100);
        CHECK(c.verifiedTick > 100);
        CHECK(c.hardCorrections == 0 && c.hashMismatches == 0);
    }

    // The budget of 9 (docs/game/rl-online.md): replaying 20 ticks of a 4v4 match, and the slowest single tick
    void TestRollbackBudget()
    {
        std::printf("budget: a rollback of 20 ticks in a 4v4 match, the slowest tick\n");
        RlMatchConfig config;
        config.mode = 4;
        config.humanMask = 0x1;
        config.seed = 3;
        RlMatch match(g_tuning, config);
        RlCarInput inputs[kMaxCars];
        std::vector<std::vector<uint8_t>> states(32);
        std::vector<double> stepMs;
        const auto now = [] { return std::chrono::steady_clock::now(); };
        // the countdown, then play
        for (int t = 0; t < 120 * 12; ++t)
        {
            inputs[0] = RlQuantizeInput(Script(0, t / 120.0));
            const auto begin = now();
            match.Step(inputs);
            stepMs.push_back(std::chrono::duration<double, std::milli>(now() - begin).count());
        }
        std::vector<double> sorted = stepMs;
        std::sort(sorted.begin(), sorted.end());
        const double p50 = sorted[sorted.size() / 2];
        const double p99 = sorted[sorted.size() * 99 / 100];
        const double worst = sorted.back();
        const size_t worstAt = static_cast<size_t>(std::max_element(stepMs.begin(), stepMs.end()) - stepMs.begin());
        Note("Step of 8 cars over %zu ticks: median %.3f ms, p99 %.3f ms, worst %.3f ms (tick %zu)", stepMs.size(), p50, p99, worst, worstAt);

        // Spikes: the same deterministic match twice; a tick that is slow both times is slow because of the simulation, one that is slow only once
        // is the machine (scheduler, cache). All eight cars are bots, so the match plays by itself for two minutes.
        {
            struct Tick
            {
                double ms = 0.0;
                int phase = 0;
                int ballHits = 0;
                int demolitions = 0;
            };
            const auto play = [&](std::vector<Tick>& out)
            {
                RlMatchConfig botsOnly;
                botsOnly.mode = 4;
                botsOnly.humanMask = 0;
                botsOnly.seed = 11;
                RlMatch bots(g_tuning, botsOnly);
                RlCarInput none[kMaxCars];
                for (int t = 0; t < 120 * 120; ++t)
                {
                    const auto begin = now();
                    bots.Step(none);
                    Tick tick;
                    tick.ms = std::chrono::duration<double, std::milli>(now() - begin).count();
                    tick.phase = static_cast<int>(bots.World().match.phase);
                    tick.ballHits = bots.LastEvents().ballHits;
                    tick.demolitions = bots.LastEvents().demolitions;
                    out.push_back(tick);
                }
            };
            std::vector<Tick> first;
            std::vector<Tick> second;
            play(first);
            play(second);
            std::vector<double> times;
            for (const Tick& tick : first)
            {
                times.push_back(tick.ms);
            }
            std::sort(times.begin(), times.end());
            Note("8 bots, 2 minutes: median %.3f ms, p99 %.3f ms, p99.9 %.3f ms, worst %.3f ms", times[times.size() / 2], times[times.size() * 99 / 100],
                 times[times.size() * 999 / 1000], times.back());
            int slowBoth = 0;
            int slowOnce = 0;
            int slowWithHit = 0;
            for (size_t i = 0; i < first.size(); ++i)
            {
                const bool a = first[i].ms > 0.3;
                const bool b = second[i].ms > 0.3;
                slowBoth += (a && b) ? 1 : 0;
                slowOnce += (a != b) ? 1 : 0;
                slowWithHit += (a && b && (first[i].ballHits > 0 || first[i].demolitions > 0)) ? 1 : 0;
            }
            Note("ticks slower than 0.3 ms: %d in both runs (%d of them with a ball hit or a demolition), %d in one run only", slowBoth, slowWithHit, slowOnce);
            size_t worstIndex = 0;
            for (size_t i = 0; i < first.size(); ++i)
            {
                if (std::min(first[i].ms, second[i].ms) > std::min(first[worstIndex].ms, second[worstIndex].ms))
                {
                    worstIndex = i;
                }
            }
            Note("slowest tick of both runs: #%zu, %.3f / %.3f ms, phase %d, ball hits %d, demolitions %d", worstIndex, first[worstIndex].ms, second[worstIndex].ms,
                 first[worstIndex].phase, first[worstIndex].ballHits, first[worstIndex].demolitions);
        }

        std::vector<uint8_t> saved;
        match.SaveState(saved);
        double worstRollback = 0.0;
        double totalRollback = 0.0;
        const int rounds = 30;
        for (int round = 0; round < rounds; ++round)
        {
            const auto begin = now();
            CHECK(match.RestoreState(saved));
            for (int t = 0; t < 20; ++t)
            {
                std::vector<uint8_t> scratch;
                match.SaveState(scratch);  // the client saves each replayed tick into its ring
                match.Step(inputs);
            }
            const double ms = std::chrono::duration<double, std::milli>(now() - begin).count();
            worstRollback = std::max(worstRollback, ms);
            totalRollback += ms;
            // go back to the same state for the next round
        }
        Note("restore + 20 ticks with saving each one: average %.2f ms, worst %.2f ms (budget 6 ms)", totalRollback / rounds, worstRollback);
        CHECK(worstRollback < 6.0);
        Note("size of a saved state: %zu bytes", saved.size());
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
    }
    std::string error;
    if (!g_tuning.LoadFromFile(RL_TUNING_JSON, &error))
    {
        std::printf("FAIL: cannot read %s: %s\n", RL_TUNING_JSON, error.c_str());
        return 1;
    }

    TestSimParams();
    TestReliableChannel();
    TestConnectionLifecycle();
    TestProtocol();

    net::NetSimParams good;
    good.latencyMs = 50.0f;
    good.jitterMs = 20.0f;
    good.lossPercent = 5.0f;
    TestHostAndClient("one-way 50 ms +-20 (RTT 100), 5% loss", good, 20.0);
    net::NetSimParams slow;
    slow.latencyMs = 100.0f;
    slow.jitterMs = 30.0f;
    slow.lossPercent = 10.0f;
    slow.duplicatePercent = 3.0f;
    TestHostAndClient("one-way 100 ms +-30 (RTT 200), 10% loss, 3% duplicates", slow, 20.0);
    TestGoalAndOutOfBandChange();
    TestSeveralClientsAndLeaving();
    TestJoinRefusals();
    TestUdp();
    TestRollbackBudget();

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "OK" : "FAILED", g_checks, g_failures);
    (void)g_verbose;
    return g_failures == 0 ? 0 : 1;
}
