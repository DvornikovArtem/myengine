// RlNetHost.cpp

#include <rl/RlNet.h>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rl
{
    namespace
    {
        constexpr int kHistory = 256;          // ticks of inputs the host keeps per slot
        constexpr int kMarginSamples = 64;     // about half a second of input packets
        constexpr uint32_t kNoTick = 0xFFFFFFFFu;
        constexpr double kKickDelay = 0.6;     // a refused client keeps its connection this long so that the Reject gets through
        constexpr int kMaxCatchUp = 30;        // ticks a single Update may simulate
        constexpr double kResyncAfterTicks = 120.0;
    }

    struct RlNetHost::Impl
    {
        struct Client
        {
            net::PeerId peer = net::kInvalidPeer;
            int slot = -1;
            std::string name;
            RlCarInput inputs[kHistory];
            uint32_t inputTick[kHistory];
            RlCarInput last;
            uint32_t newestTick = 0;
            bool haveNewest = false;
            int margins[kMarginSamples] = {};
            int marginCount = 0;
            int marginPos = 0;

            Client() { std::fill(std::begin(inputTick), std::end(inputTick), kNoTick); }

            void AddMargin(int value)
            {
                margins[marginPos] = value;
                marginPos = (marginPos + 1) % kMarginSamples;
                marginCount = std::min(marginCount + 1, kMarginSamples);
            }
            int MinMargin() const
            {
                if (marginCount == 0)
                {
                    return 0;
                }
                int result = 127;
                for (int i = 0; i < marginCount; ++i)
                {
                    result = std::min(result, margins[i]);
                }
                return result;
            }
        };

        struct Kick
        {
            net::PeerId peer;
            double when;
        };

        net::INetTransport& transport;
        RlTuning tuning;
        RlNetHostOptions options;
        uint64_t tuningHash = 0;
        int slotCount = 0;

        std::unique_ptr<RlMatch> match;
        bool started = false;
        double startTime = 0.0;
        double lastNow = 0.0;
        uint32_t tick = 0;  // the next tick to simulate
        uint32_t humanMask = 0;

        std::map<net::PeerId, Client> clients;
        std::vector<Kick> kicks;
        RlCarInput localInput;
        RlCarInput used[kMaxCars][kHistory];

        std::vector<RlEventMsg> events;
        RlNetStats stats;
        std::function<void(uint32_t, const RlMatch&)> observer;
        std::vector<uint8_t> scratch;
        std::vector<uint8_t> packet;

        Impl(net::INetTransport& t, const RlTuning& tn, const RlNetHostOptions& o)
            : transport(t)
            , tuning(tn)
            , options(o)
        {
            options.match.mode = std::clamp(options.match.mode, 0, 4);
            slotCount = options.match.mode == 0 ? 1 : 2 * options.match.mode;
            tuningHash = RlTuningHash(tuning);
            if (options.hostPlays)
            {
                humanMask = 1u;
            }
        }

        bool FreeSlot(int& out) const
        {
            if (options.match.mode == 0)
            {
                return false;  // practice: nobody else
            }
            const int mode = options.match.mode;
            // orange first, then blue, alternating, so that two players start on different teams: 2, 1, 3, 4 ... in 2v2
            std::vector<int> blue;
            std::vector<int> orange;
            for (int i = 0; i < mode; ++i)
            {
                if (i != 0 || !options.hostPlays)
                {
                    blue.push_back(i);
                }
                orange.push_back(mode + i);
            }
            std::vector<int> order;
            for (size_t i = 0; i < std::max(blue.size(), orange.size()); ++i)
            {
                if (i < orange.size())
                {
                    order.push_back(orange[i]);
                }
                if (i < blue.size())
                {
                    order.push_back(blue[i]);
                }
            }
            for (const int candidate : order)
            {
                if (candidate < slotCount && (humanMask & (1u << candidate)) == 0)
                {
                    out = candidate;
                    return true;
                }
            }
            return false;
        }

        void Send(net::PeerId peer, const std::vector<uint8_t>& bytes, bool reliable) { transport.Send(peer, bytes.data(), bytes.size(), reliable); }

        void Reject(net::PeerId peer, RlRejectReason reason, double now)
        {
            std::vector<uint8_t> bytes;
            RlEncodeReject(reason, bytes);
            Send(peer, bytes, true);
            kicks.push_back({peer, now + kKickDelay});
        }

        void BroadcastEvent(const RlEventMsg& event)
        {
            events.push_back(event);
            std::vector<uint8_t> bytes;
            RlEncode(event, bytes);
            for (auto& entry : clients)
            {
                if (entry.second.slot >= 0)
                {
                    Send(entry.first, bytes, true);
                }
            }
        }

        void OnJoin(net::PeerId peer, const std::vector<uint8_t>& data, double now)
        {
            RlJoinMsg join;
            if (!RlDecode(data.data(), data.size(), join))
            {
                return;
            }
            if (join.version != kNetProtocolVersion)
            {
                Reject(peer, RlRejectReason::Version, now);
                return;
            }
            if (join.tuningHash != tuningHash)
            {
                Reject(peer, RlRejectReason::Tuning, now);
                return;
            }
            if (started)
            {
                Reject(peer, RlRejectReason::Started, now);
                return;
            }
            Client& client = clients[peer];
            if (client.slot >= 0)
            {
                return;  // a repeated Join
            }
            int slot = -1;
            if (static_cast<int>(clients.size()) > options.maxClients || !FreeSlot(slot))
            {
                Reject(peer, RlRejectReason::Full, now);
                clients.erase(peer);
                return;
            }
            client.peer = peer;
            client.slot = slot;
            client.name = join.name;
            humanMask |= 1u << slot;

            RlWelcomeMsg welcome;
            welcome.slot = static_cast<uint8_t>(slot);
            welcome.mode = static_cast<uint8_t>(options.match.mode);
            welcome.botLevel = static_cast<uint8_t>(options.match.botLevel);
            welcome.started = 0;
            welcome.seed = options.match.seed;
            welcome.humanMask = humanMask;
            welcome.tuningHash = tuningHash;
            std::vector<uint8_t> bytes;
            RlEncode(welcome, bytes);
            Send(peer, bytes, true);
        }

        void OnInput(net::PeerId peer, const std::vector<uint8_t>& data)
        {
            const auto it = clients.find(peer);
            if (it == clients.end() || it->second.slot < 0 || !started)
            {
                return;
            }
            RlInputMsg msg;
            if (!RlDecode(data.data(), data.size(), msg) || msg.count == 0)
            {
                return;
            }
            Client& client = it->second;
            for (int i = 0; i < msg.count; ++i)
            {
                const uint32_t t = msg.firstTick + static_cast<uint32_t>(i);
                if (t < tick)
                {
                    if (client.inputTick[t % kHistory] != t)
                    {
                        ++stats.lateInputs;  // the host simulated this tick without it; the copies in the next packets are the same input
                        client.inputTick[t % kHistory] = t;
                    }
                    continue;
                }
                if (t >= tick + kHistory / 2)
                {
                    continue;  // too far ahead to be real
                }
                client.inputs[t % kHistory] = msg.inputs[i];
                client.inputTick[t % kHistory] = t;
            }
            const uint32_t newest = msg.firstTick + static_cast<uint32_t>(msg.count - 1);
            if (!client.haveNewest || static_cast<int32_t>(newest - client.newestTick) > 0)
            {
                client.newestTick = newest;
                client.haveNewest = true;
                client.AddMargin(std::clamp(static_cast<int>(static_cast<int32_t>(newest - tick)), -100, 100));
            }
        }

        void OnDisconnected(net::PeerId peer)
        {
            const auto it = clients.find(peer);
            if (it == clients.end())
            {
                return;
            }
            const int slot = it->second.slot;
            clients.erase(it);
            if (slot < 0)
            {
                return;
            }
            humanMask &= ~(1u << slot);
            if (started)
            {
                // from the next tick on the slot is a bot; the clients learn it with the tick
                RlEventMsg event;
                event.kind = RlEventKind::SlotMask;
                event.tick = tick;
                event.slot = static_cast<int8_t>(slot);
                event.a = 0;
                event.mask = humanMask;
                BroadcastEvent(event);
            }
        }

        void Pump(double now)
        {
            std::vector<net::NetEvent> polled;
            transport.Poll(now, polled);
            for (net::NetEvent& event : polled)
            {
                switch (event.type)
                {
                case net::NetEventType::Connected:
                    break;
                case net::NetEventType::Disconnected:
                    OnDisconnected(event.peer);
                    break;
                case net::NetEventType::Data:
                    switch (RlMessageType(event.data.data(), event.data.size()))
                    {
                    case RlMsg::Join: OnJoin(event.peer, event.data, now); break;
                    case RlMsg::Input: OnInput(event.peer, event.data); break;
                    default: break;
                    }
                    break;
                }
            }
            for (size_t i = 0; i < kicks.size();)
            {
                if (now >= kicks[i].when)
                {
                    transport.Disconnect(kicks[i].peer);
                    clients.erase(kicks[i].peer);
                    kicks.erase(kicks.begin() + static_cast<std::ptrdiff_t>(i));
                }
                else
                {
                    ++i;
                }
            }
        }

        bool Start(double now)
        {
            if (started)
            {
                return false;
            }
            RlMatchConfig config = options.match;
            config.humanMask = humanMask;
            for (const auto& entry : clients)
            {
                if (entry.second.slot >= 0)
                {
                    config.names[entry.second.slot] = entry.second.name;
                }
            }
            match = std::make_unique<RlMatch>(tuning, config);
            started = true;
            startTime = now;
            tick = 0;
            std::memset(used, 0, sizeof(used));

            // the names are part of the state: every side takes the host's resolved ones before its tick 0
            RlNamesMsg names;
            for (int slot = 0; slot < slotCount; ++slot)
            {
                names.names[slot] = match->World().match.names[slot];
            }
            std::vector<uint8_t> nameBytes;
            RlEncode(names, nameBytes);
            for (auto& entry : clients)
            {
                if (entry.second.slot >= 0)
                {
                    Send(entry.first, nameBytes, true);
                }
            }

            RlEventMsg event;
            event.kind = RlEventKind::MatchStart;
            event.tick = 0;
            event.mask = humanMask;
            BroadcastEvent(event);
            return true;
        }

        void StepOne()
        {
            ZoneScopedN("Rl::Host::Step");
            const auto begin = std::chrono::steady_clock::now();
            RlCarInput inputs[kMaxCars];
            for (int slot = 0; slot < slotCount; ++slot)
            {
                if ((humanMask & (1u << slot)) == 0)
                {
                    continue;
                }
                if (slot == 0 && options.hostPlays)
                {
                    inputs[slot] = RlQuantizeInput(localInput);
                    continue;
                }
                for (auto& entry : clients)
                {
                    Client& client = entry.second;
                    if (client.slot != slot)
                    {
                        continue;
                    }
                    if (client.inputTick[tick % kHistory] == tick)
                    {
                        client.last = client.inputs[tick % kHistory];
                    }
                    else
                    {
                        ++stats.repeatedInputs;  // nothing arrived for this tick: the last input goes on
                    }
                    inputs[slot] = client.last;
                    break;
                }
            }
            for (int slot = 0; slot < kMaxCars; ++slot)
            {
                used[slot][tick % kHistory] = inputs[slot];
            }

            match->SetHumanMask(humanMask);
            match->Step(inputs);
            ++tick;
            stats.tick = tick;

            DetectEvents();
            if (observer)
            {
                observer(tick, *match);
            }
            if (tick % kSnapshotInterval == 0)
            {
                SendSnapshots();
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            stats.stepMsMax = std::max(stats.stepMsMax, ms);
        }

        void DetectEvents()
        {
            const RlWorldState& now = match->World();
            const RlWorldState& before = match->PreviousWorld();
            for (int team = 0; team < 2; ++team)
            {
                if (now.match.score[team] > before.match.score[team])
                {
                    RlEventMsg event;
                    event.kind = RlEventKind::Goal;
                    event.tick = tick;
                    event.slot = static_cast<int8_t>(now.match.lastGoalScorer);
                    event.a = static_cast<uint8_t>(team);
                    event.other = static_cast<int8_t>(now.match.lastGoalAssist);
                    event.value = static_cast<uint16_t>(std::clamp(std::lround(now.match.lastGoalSpeed * 10.0f), 0l, 65535l));
                    BroadcastEvent(event);
                }
            }
            // the events of the ring that are new since the last tick: assists, saves, shots and who demolished whom
            for (int n = std::max(before.match.eventHead, now.match.eventHead - kEventRing); n < now.match.eventHead; ++n)
            {
                const RlMatchEvent& entry = now.match.events[n % kEventRing];
                RlEventMsg event;
                event.tick = tick;
                event.slot = static_cast<int8_t>(entry.slot);
                event.other = static_cast<int8_t>(entry.other);
                switch (static_cast<RlMatchEventType>(entry.type))
                {
                case RlMatchEventType::Assist: event.kind = RlEventKind::Assist; break;
                case RlMatchEventType::Save: event.kind = RlEventKind::Save; break;
                case RlMatchEventType::Shot: event.kind = RlEventKind::Shot; break;
                case RlMatchEventType::Demo:
                    event.kind = RlEventKind::Demolition;
                    event.slot = static_cast<int8_t>(entry.other);  // the Demolition event names the victim
                    event.other = static_cast<int8_t>(entry.slot);
                    break;
                default: continue;
                }
                BroadcastEvent(event);
            }
            if (now.match.phase == RlMatchPhase::Finished && before.match.phase != RlMatchPhase::Finished)
            {
                RlEventMsg event;
                event.kind = RlEventKind::MatchEnd;
                event.tick = tick;
                event.a = static_cast<uint8_t>(std::max(now.match.winner, 0));
                BroadcastEvent(event);
            }
        }

        void SendSnapshots()
        {
            if (clients.empty())
            {
                return;
            }
            ZoneScopedN("Rl::Net::Snapshot");
            RlSnapshotMsg msg;
            msg.tick = tick;
            msg.humanMask = humanMask;
            msg.world = match->World();
            msg.world.tick = tick;
            match->SaveState(scratch);
            msg.stateHash = RlSim::HashState(scratch);
            for (int i = 0; i < kPadCount; ++i)
            {
                if (msg.world.match.pads[i].respawnLeft <= 0.0f)
                {
                    msg.padMask |= 1ull << i;
                }
            }
            msg.inputCount = static_cast<int>(std::min<uint32_t>(tick, kNetInputWindow));
            for (int slot = 0; slot < slotCount; ++slot)
            {
                for (int j = 0; j < msg.inputCount; ++j)
                {
                    const uint32_t t = tick - static_cast<uint32_t>(msg.inputCount) + static_cast<uint32_t>(j);
                    RlPackInput(used[slot][t % kHistory], msg.inputs[slot][j]);
                }
            }
            int worstMargin = 127;
            for (auto& entry : clients)
            {
                Client& client = entry.second;
                if (client.slot < 0)
                {
                    continue;
                }
                msg.ackInputTick = client.newestTick;
                msg.margin = static_cast<int8_t>(client.MinMargin());
                worstMargin = std::min(worstMargin, static_cast<int>(msg.margin));
                RlEncode(msg, packet);
                transport.Send(entry.first, packet.data(), packet.size(), false);
                ++stats.snapshots;
            }
            stats.margin = worstMargin == 127 ? 0 : worstMargin;
        }

        void Update(double now)
        {
            lastNow = now;
            Pump(now);
            if (started)
            {
                const double due = (now - startTime) * static_cast<double>(kTickRate);
                if (due - static_cast<double>(tick) > kResyncAfterTicks)
                {
                    startTime = now - static_cast<double>(tick) / static_cast<double>(kTickRate);  // a long stall: do not run a burst of seconds
                }
                int steps = 0;
                while (static_cast<double>(tick) + 1.0 <= (now - startTime) * static_cast<double>(kTickRate) && steps < kMaxCatchUp)
                {
                    StepOne();
                    ++steps;
                }
                double rtt = 0.0;
                for (const auto& entry : clients)
                {
                    rtt = std::max(rtt, transport.RoundTripSeconds(entry.first));
                }
                stats.rttMs = rtt * 1000.0;
            }
        }
    };

    RlNetHost::RlNetHost(net::INetTransport& transport, const RlTuning& tuning, const RlNetHostOptions& options)
        : impl_(std::make_unique<Impl>(transport, tuning, options))
    {
    }

    RlNetHost::~RlNetHost() = default;

    bool RlNetHost::Listen(uint16_t port, std::string& error) { return impl_->transport.Listen(port, error); }
    bool RlNetHost::StartMatch(double now) { return impl_->Start(now); }
    bool RlNetHost::Started() const { return impl_->started; }
    void RlNetHost::Update(double now) { impl_->Update(now); }
    void RlNetHost::SetLocalInput(const RlCarInput& input) { impl_->localInput = input; }
    int RlNetHost::ClientCount() const
    {
        int count = 0;
        for (const auto& entry : impl_->clients)
        {
            count += entry.second.slot >= 0 ? 1 : 0;
        }
        return count;
    }
    std::vector<RlNetPeerInfo> RlNetHost::Peers() const
    {
        std::vector<RlNetPeerInfo> peers;
        for (const auto& entry : impl_->clients)
        {
            if (entry.second.slot >= 0)
            {
                peers.push_back({entry.second.slot, entry.second.name, impl_->transport.RoundTripSeconds(entry.first) * 1000.0});
            }
        }
        std::sort(peers.begin(), peers.end(), [](const RlNetPeerInfo& a, const RlNetPeerInfo& b) { return a.slot < b.slot; });
        return peers;
    }
    uint32_t RlNetHost::HumanMask() const { return impl_->humanMask; }
    float RlNetHost::TickFraction() const
    {
        if (!impl_->started)
        {
            return 0.0f;
        }
        const double fraction = (impl_->lastNow - impl_->startTime) * static_cast<double>(kTickRate) - static_cast<double>(impl_->tick);
        return static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    }
    const RlMatch* RlNetHost::Match() const { return impl_->match.get(); }
    RlMatch* RlNetHost::MutableMatch() { return impl_->match.get(); }
    std::vector<RlEventMsg> RlNetHost::TakeEvents()
    {
        std::vector<RlEventMsg> result;
        result.swap(impl_->events);
        return result;
    }
    const RlNetStats& RlNetHost::Stats() const { return impl_->stats; }
    void RlNetHost::SetTickObserver(std::function<void(uint32_t, const RlMatch&)> observer) { impl_->observer = std::move(observer); }
}
