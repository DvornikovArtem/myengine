// RlNetClient.cpp

#include <rl/RlNet.h>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>

namespace rl
{
    namespace
    {
        constexpr int kHistory = 256;        // ticks of inputs the client remembers
        constexpr int kRingSize = 128;       // ticks of saved states (7.1 / 5.3)
        constexpr uint32_t kNoTick = 0xFFFFFFFFu;
        constexpr int kTargetMargin = 4;     // ticks by which an input should reach the host before the host needs it
        constexpr int kJumpMargin = 12;      // a margin error bigger than this is corrected at once, not by the tick rate
        constexpr double kMaxScaleDelta = 0.06;
        constexpr double kSmoothSeconds = 0.06;  // time constant of the fading correction: ~0.15 s to disappear
        constexpr float kSnapDistance = 4.0f;    // a correction bigger than this (metres) is not smoothed
        constexpr int kMaxCatchUp = 40;

        struct Q
        {
            // minimal quaternion helpers (x y z w), the engine's RlQuat layout
            static RlQuat Mul(const RlQuat& a, const RlQuat& b)
            {
                return RlQuat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
            }
            static RlQuat Conj(const RlQuat& a) { return RlQuat{-a.x, -a.y, -a.z, a.w}; }
            static RlQuat Normalize(const RlQuat& a)
            {
                const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
                if (l < 1e-6f)
                {
                    return RlQuat{};
                }
                return RlQuat{a.x / l, a.y / l, a.z / l, a.w / l};
            }
            // normalized lerp on the shorter arc
            static RlQuat Nlerp(const RlQuat& a, const RlQuat& b, float t)
            {
                const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
                const float s = dot < 0.0f ? -1.0f : 1.0f;
                return Normalize(RlQuat{a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t, a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t});
            }
        };

        RlVec3 Add(const RlVec3& a, const RlVec3& b) { return RlVec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        RlVec3 Sub(const RlVec3& a, const RlVec3& b) { return RlVec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
        RlVec3 Scale(const RlVec3& a, float s) { return RlVec3{a.x * s, a.y * s, a.z * s}; }
        RlVec3 Lerp(const RlVec3& a, const RlVec3& b, float t) { return Add(a, Scale(Sub(b, a), t)); }
        float Length(const RlVec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

        // the fading offset of one body: rendered = physics + position, rotation * physics
        struct BodyOffset
        {
            RlVec3 position{};
            RlQuat rotation{};
        };
    }

    struct RlNetClient::Impl
    {
        struct StateSlot
        {
            uint32_t tick = kNoTick;
            std::vector<uint8_t> bytes;
        };
        struct WorldSlot
        {
            uint32_t tick = kNoTick;
            RlWorldState world;
        };
        struct Simulated
        {
            uint32_t tick = kNoTick;
            RlCarInput inputs[kMaxCars];
        };

        net::INetTransport& transport;
        RlTuning tuning;
        uint64_t tuningHash = 0;

        RlNetClientState state = RlNetClientState::Idle;
        std::string error;
        net::PeerId host = net::kInvalidPeer;
        std::string name;
        int slot = -1;
        int slotCount = 0;
        std::unique_ptr<RlMatch> match;

        // time
        double lastNow = -1.0;
        double accumulator = 0.0;
        double scale = 1.0;
        int holdTicks = 0;
        double ignoreMarginUntil = 0.0;
        uint32_t tick = 0;  // the next tick to simulate

        // inputs
        RlCarInput localInput;
        RlCarInput localHistory[kHistory];
        uint32_t localTick[kHistory];
        Simulated simulated[kHistory];
        RlCarInput canonical[kMaxCars][kHistory];
        uint32_t canonicalTick[kMaxCars][kHistory];
        RlCarInput lastCanonical[kMaxCars];
        uint32_t lastCanonicalTick[kMaxCars];
        std::vector<std::pair<uint32_t, uint32_t>> maskSchedule;  // (from tick, human mask)

        StateSlot ring[kRingSize];
        WorldSlot worldRing[kRingSize];  // the plain world of each saved tick, for comparing with a quantized snapshot

        // the fading correction
        BodyOffset carOffset[kMaxCars];
        BodyOffset ballOffset;

        RlNetStats stats;
        std::deque<double> rollbackTimes;
        std::vector<RlEventMsg> events;
        std::function<void(uint32_t, const RlMatch&)> observer;
        std::vector<uint8_t> packet;
        std::vector<uint8_t> scratch;

        Impl(net::INetTransport& t, const RlTuning& tn)
            : transport(t)
            , tuning(tn)
        {
            tuningHash = RlTuningHash(tuning);
            std::fill(std::begin(localTick), std::end(localTick), kNoTick);
            for (auto& row : canonicalTick)
            {
                std::fill(std::begin(row), std::end(row), kNoTick);
            }
            for (uint32_t& t2 : lastCanonicalTick)
            {
                t2 = kNoTick;
            }
        }

        // ---- the schedule of human slots --------------------------------------------------------------------------------------------

        uint32_t MaskAt(uint32_t t) const
        {
            uint32_t mask = 0;
            for (const auto& entry : maskSchedule)
            {
                if (entry.first <= t)
                {
                    mask = entry.second;
                }
            }
            return mask;
        }

        void AddMask(uint32_t from, uint32_t mask)
        {
            for (auto& entry : maskSchedule)
            {
                if (entry.first == from)
                {
                    entry.second = mask;
                    return;
                }
            }
            maskSchedule.emplace_back(from, mask);
            std::stable_sort(maskSchedule.begin(), maskSchedule.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }

        // ---- inputs -----------------------------------------------------------------------------------------------------------------

        // The input to simulate for `slotIndex` at tick `t`: the one the host used if we know it, otherwise our own input for our slot and
        // the last input we know of for the others.
        RlCarInput InputFor(int slotIndex, uint32_t t) const
        {
            if (canonicalTick[slotIndex][t % kHistory] == t)
            {
                return canonical[slotIndex][t % kHistory];
            }
            if (slotIndex == slot)
            {
                return localTick[t % kHistory] == t ? localHistory[t % kHistory] : RlCarInput{};
            }
            // a gap in the middle: the nearest known tick before t
            for (uint32_t back = 1; back <= 8 && back <= t; ++back)
            {
                const uint32_t before = t - back;
                if (canonicalTick[slotIndex][before % kHistory] == before)
                {
                    return canonical[slotIndex][before % kHistory];
                }
            }
            return lastCanonicalTick[slotIndex] == kNoTick ? RlCarInput{} : lastCanonical[slotIndex];
        }

        // ---- the ring of states ------------------------------------------------------------------------------------------------------

        void PushRing(uint32_t t)
        {
            StateSlot& s = ring[t % kRingSize];
            match->SaveState(s.bytes);
            s.tick = t;
        }

        const StateSlot* RingAt(uint32_t t) const
        {
            const StateSlot& s = ring[t % kRingSize];
            return s.tick == t ? &s : nullptr;
        }

        // ---- the simulation -----------------------------------------------------------------------------------------------------------

        // Simulates tick `t` from the present state (which is the state of tick `t`).
        void SimStep(uint32_t t)
        {
            const uint32_t mask = MaskAt(t);
            Simulated& rec = simulated[t % kHistory];
            rec.tick = t;
            RlCarInput inputs[kMaxCars];
            for (int s = 0; s < slotCount; ++s)
            {
                if ((mask & (1u << s)) != 0)
                {
                    inputs[s] = InputFor(s, t);
                }
                rec.inputs[s] = inputs[s];
            }
            PushRing(t);
            WorldSlot& worldSlot = worldRing[t % kRingSize];
            worldSlot.tick = t;
            worldSlot.world = match->World();
            match->SetHumanMask(mask);
            match->Step(inputs);
        }

        // One new tick at the present.
        void StepFresh()
        {
            ZoneScopedN("Rl::Client::Step");
            const auto begin = std::chrono::steady_clock::now();
            const RlCarInput quantized = RlQuantizeInput(localInput);
            localHistory[tick % kHistory] = quantized;
            localTick[tick % kHistory] = tick;
            SimStep(tick);
            const uint32_t stepped = tick;
            ++tick;
            stats.tick = tick;
            SendInputs(stepped);
            if (observer)
            {
                observer(tick, *match);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            stats.stepMsMax = std::max(stats.stepMsMax, ms);
        }

        void SendInputs(uint32_t newest)
        {
            RlInputMsg msg;
            const uint32_t count = std::min<uint32_t>(newest + 1, kNetInputWindow);
            msg.firstTick = newest + 1 - count;
            msg.count = static_cast<int>(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t t = msg.firstTick + i;
                msg.inputs[i] = localTick[t % kHistory] == t ? localHistory[t % kHistory] : RlCarInput{};
            }
            RlEncode(msg, packet);
            transport.Send(host, packet.data(), packet.size(), false);
        }

        // What the player sees now: the physics pose plus the fading correction
        struct Shown
        {
            int carCount = 0;
            RlVec3 car[kMaxCars];
            RlQuat carRotation[kMaxCars];
            RlVec3 ball;
            RlQuat ballRotation;
        };

        Shown CaptureShown() const
        {
            Shown shown;
            const RlWorldState& world = match->World();
            shown.carCount = world.carCount;
            for (int i = 0; i < world.carCount; ++i)
            {
                shown.car[i] = Add(world.cars[i].position, carOffset[i].position);
                shown.carRotation[i] = Q::Mul(carOffset[i].rotation, world.cars[i].rotation);
            }
            shown.ball = Add(world.ball.position, ballOffset.position);
            shown.ballRotation = Q::Mul(ballOffset.rotation, world.ball.rotation);
            return shown;
        }

        // Simulates from tick `from` up to the present. `restore`: first put back the saved state of `from` (a rollback); false: the match is
        // at tick `from` already (after a hard correction set the poses). `shown` is what the player saw before.
        void Resimulate(uint32_t from, bool restore, const Shown& shown)
        {
            ZoneScopedN("Rl::Rollback");
            const auto begin = std::chrono::steady_clock::now();
            if (restore)
            {
                const StateSlot* slotState = RingAt(from);
                if (slotState == nullptr || !match->RestoreState(slotState->bytes))
                {
                    return;
                }
            }
            for (uint32_t t = from; t < tick; ++t)
            {
                SimStep(t);
            }

            // the jump of the correction fades out instead of showing
            const RlWorldState& after = match->World();
            for (int i = 0; i < after.carCount && i < shown.carCount; ++i)
            {
                SetOffset(carOffset[i], shown.car[i], shown.carRotation[i], after.cars[i].position, after.cars[i].rotation);
            }
            SetOffset(ballOffset, shown.ball, shown.ballRotation, after.ball.position, after.ball.rotation);

            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            ++stats.rollbacks;
            stats.lastResimTicks = static_cast<int>(tick - from);
            stats.lastRollbackMs = ms;
            stats.maxRollbackMs = std::max(stats.maxRollbackMs, ms);
            rollbackTimes.push_back(lastNow);
        }

        void SetOffset(BodyOffset& offset, const RlVec3& shownPosition, const RlQuat& shownRotation, const RlVec3& position, const RlQuat& rotation)
        {
            const RlVec3 difference = Sub(shownPosition, position);
            if (Length(difference) > kSnapDistance)
            {
                offset = BodyOffset{};
                return;
            }
            offset.position = difference;
            offset.rotation = Q::Normalize(Q::Mul(shownRotation, Q::Conj(rotation)));
        }

        void DecayOffsets(double dt)
        {
            const float keep = static_cast<float>(std::exp(-dt / kSmoothSeconds));
            const auto decay = [&](BodyOffset& offset)
            {
                offset.position = Scale(offset.position, keep);
                offset.rotation = Q::Nlerp(RlQuat{}, offset.rotation, keep);
            };
            for (BodyOffset& offset : carOffset)
            {
                decay(offset);
            }
            decay(ballOffset);
        }

        // ---- messages -------------------------------------------------------------------------------------------------------------------

        void Fail(const std::string& text)
        {
            state = RlNetClientState::Failed;
            error = text;
        }

        void OnWelcome(const std::vector<uint8_t>& data)
        {
            RlWelcomeMsg welcome;
            if (!RlDecode(data.data(), data.size(), welcome) || state != RlNetClientState::Joining)
            {
                return;
            }
            if (welcome.version != kNetProtocolVersion)
            {
                Fail(RlRejectText(RlRejectReason::Version));
                return;
            }
            if (welcome.tuningHash != tuningHash)
            {
                Fail(RlRejectText(RlRejectReason::Tuning));
                return;
            }
            slot = welcome.slot;
            RlMatchConfig config;
            config.mode = welcome.mode;
            config.seed = welcome.seed;
            config.botLevel = welcome.botLevel;
            config.humanMask = welcome.humanMask;
            match = std::make_unique<RlMatch>(tuning, config);
            slotCount = match->SlotCount();
            if (slot >= slotCount)
            {
                Fail("the host sent a slot that does not exist");
                match.reset();
                return;
            }
            state = RlNetClientState::Lobby;
        }

        void OnEvent(const RlEventMsg& event, double now)
        {
            events.push_back(event);
            switch (event.kind)
            {
            case RlEventKind::MatchStart:
                if (state == RlNetClientState::Lobby)
                {
                    AddMask(0, event.mask);
                    BeginPlaying(now);
                }
                break;
            case RlEventKind::SlotMask:
                if (state == RlNetClientState::Playing)
                {
                    AddMask(event.tick, event.mask);
                    if (event.tick < tick && tick - event.tick < kRingSize - 1)
                    {
                        Resimulate(event.tick, true, CaptureShown());  // we simulated those ticks with the slot as it was
                    }
                }
                break;
            default:
                break;
            }
        }

        // The host started its clock when it sent MatchStart; we are half a round trip behind it, and want to be a margin ahead of it.
        void BeginPlaying(double now)
        {
            state = RlNetClientState::Playing;
            tick = 0;
            accumulator = 0.0;
            lastNow = now;
            double rtt = transport.RoundTripSeconds(host);
            if (rtt <= 0.0)
            {
                rtt = 0.1;  // not measured yet: a guess is better than no lead at all, the margin feedback corrects it
            }
            const int lead = static_cast<int>(std::ceil(rtt * 0.5 * kTickRate)) + kTargetMargin;
            for (int i = 0; i < lead; ++i)
            {
                StepFresh();
            }
        }

        void OnSnapshot(const std::vector<uint8_t>& data, double now)
        {
            if (state != RlNetClientState::Playing)
            {
                return;
            }
            RlSnapshotMsg msg;
            if (!RlDecode(data.data(), data.size(), msg))
            {
                return;
            }
            ZoneScopedN("Rl::Net::Snapshot");
            ++stats.snapshots;
            stats.margin = msg.margin;
            stats.lead = static_cast<int>(tick) - static_cast<int>(msg.tick);
            ControlRate(msg.margin, now);

            // 1. the inputs the host really used, against the ones we simulated
            uint32_t divergence = kNoTick;
            const uint32_t first = msg.tick - static_cast<uint32_t>(msg.inputCount);
            for (int j = 0; j < msg.inputCount; ++j)
            {
                const uint32_t t = first + static_cast<uint32_t>(j);
                for (int s = 0; s < slotCount; ++s)
                {
                    if ((msg.humanMask & (1u << s)) == 0)
                    {
                        continue;
                    }
                    const RlCarInput input = RlUnpackInput(msg.inputs[s][j]);
                    canonical[s][t % kHistory] = input;
                    canonicalTick[s][t % kHistory] = t;
                    if (lastCanonicalTick[s] == kNoTick || static_cast<int32_t>(t - lastCanonicalTick[s]) >= 0)
                    {
                        lastCanonical[s] = input;
                        lastCanonicalTick[s] = t;
                    }
                    const Simulated& rec = simulated[t % kHistory];
                    if (t < tick && rec.tick == t && !RlSameInput(rec.inputs[s], input) && (MaskAt(t) & (1u << s)) != 0)
                    {
                        divergence = std::min(divergence, t);
                    }
                }
            }

            // 2. replay from the first tick that went another way
            if (divergence != kNoTick && tick - divergence < kRingSize - 1 && RingAt(divergence) != nullptr)
            {
                Resimulate(divergence, true, CaptureShown());
            }

            // 3. the state of the host at its tick against ours
            if (msg.tick < tick && tick - msg.tick < kRingSize - 1)
            {
                const StateSlot* at = RingAt(msg.tick);
                if (at != nullptr)
                {
                    if (RlSim::HashState(at->bytes) == msg.stateHash)
                    {
                        stats.verifiedTick = std::max(stats.verifiedTick, msg.tick);
                    }
                    else
                    {
                        ++stats.hashMismatches;
                        const WorldSlot& local = worldRing[msg.tick % kRingSize];
                        if (local.tick == msg.tick && Similar(local.world, msg.world))
                        {
                            stats.verifiedTick = std::max(stats.verifiedTick, msg.tick);  // not bit-equal, but the same game
                        }
                        else
                        {
                            HardCorrect(msg);
                        }
                    }
                }
            }
            UpdateSecondCounter();
        }

        // Is our state of the same tick close enough to the quantized state of the host that no correction is needed? After a hard correction
        // the two can never be bit-equal again (the host's numbers were rounded), but they stay this close until something really differs.
        static bool Similar(const RlWorldState& local, const RlWorldState& host)
        {
            if (local.match.phase != host.match.phase || local.match.score[0] != host.match.score[0] || local.match.score[1] != host.match.score[1] ||
                local.match.overtime != host.match.overtime || local.carCount != host.carCount)
            {
                return false;
            }
            const auto close = [](const RlVec3& a, const RlVec3& b, float limit) { return Length(Sub(a, b)) <= limit; };
            if (!close(local.ball.position, host.ball.position, 0.05f) || !close(local.ball.linearVelocity, host.ball.linearVelocity, 0.5f))
            {
                return false;
            }
            for (int i = 0; i < local.carCount; ++i)
            {
                const RlCarState& a = local.cars[i];
                const RlCarState& b = host.cars[i];
                if (a.demolished != b.demolished || !close(a.position, b.position, 0.05f) || !close(a.linearVelocity, b.linearVelocity, 0.5f) ||
                    std::fabs(a.boost - b.boost) > 2.0f)
                {
                    return false;
                }
            }
            return true;
        }

        // The replay did not reproduce the host (a lost input beyond the window, a different build): take the poses from the snapshot.
        void HardCorrect(const RlSnapshotMsg& msg)
        {
            const StateSlot* at = RingAt(msg.tick);
            if (at == nullptr)
            {
                return;
            }
            const Shown shown = CaptureShown();
            if (!match->RestoreState(at->bytes))
            {
                return;
            }
            const RlWorldState& world = msg.world;
            RlSim& sim = match->MutableSim();
            for (int i = 0; i < world.carCount && i < slotCount; ++i)
            {
                if (world.cars[i].demolished)
                {
                    continue;
                }
                sim.SetCarPose(i, world.cars[i].position, world.cars[i].rotation, world.cars[i].linearVelocity, world.cars[i].angularVelocity);
                sim.SetCarBoost(i, world.cars[i].boost);
            }
            sim.ResetBall(world.ball.position, world.ball.linearVelocity, world.ball.angularVelocity);
            ++stats.hardCorrections;
            Resimulate(msg.tick, false, shown);
        }

        void UpdateSecondCounter()
        {
            while (!rollbackTimes.empty() && lastNow - rollbackTimes.front() > 1.0)
            {
                rollbackTimes.pop_front();
            }
            stats.rollbacksLastSecond = static_cast<int>(rollbackTimes.size());
        }

        // Keeps our inputs arriving kTargetMargin ticks ahead of the host's need: a bit faster or slower tick rate for small errors, a jump
        // for big ones (the first seconds).
        void ControlRate(int margin, double now)
        {
            if (now < ignoreMarginUntil)
            {
                return;
            }
            const int error2 = kTargetMargin - margin;  // > 0: our inputs are late, run further ahead
            if (std::abs(error2) > kJumpMargin)
            {
                if (error2 > 0)
                {
                    const int extra = std::min(error2, kMaxCatchUp);
                    for (int i = 0; i < extra; ++i)
                    {
                        StepFresh();
                    }
                }
                else
                {
                    holdTicks = std::min(-error2, 60);
                }
                ignoreMarginUntil = now + 0.7;  // the host's margin window still holds the old samples
                scale = 1.0;
                return;
            }
            if (std::abs(error2) <= 1)
            {
                scale = 1.0;
            }
            else
            {
                scale = 1.0 + std::clamp(static_cast<double>(error2) * 0.012, -kMaxScaleDelta, kMaxScaleDelta);
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
                    if (state == RlNetClientState::Connecting)
                    {
                        state = RlNetClientState::Joining;
                        RlJoinMsg join;
                        join.tuningHash = tuningHash;
                        join.name = name;
                        RlEncode(join, packet);
                        transport.Send(host, packet.data(), packet.size(), true);
                    }
                    break;
                case net::NetEventType::Disconnected:
                    if (state != RlNetClientState::Failed && state != RlNetClientState::Disconnected)
                    {
                        state = RlNetClientState::Disconnected;
                        error = event.reason == net::DisconnectReason::Rejected   ? "the host refused the connection"
                                : event.reason == net::DisconnectReason::Timeout ? "no answer from the host (timeout)"
                                                                                  : "the host closed the connection";
                    }
                    break;
                case net::NetEventType::Data:
                    switch (RlMessageType(event.data.data(), event.data.size()))
                    {
                    case RlMsg::Welcome: OnWelcome(event.data); break;
                    case RlMsg::Reject:
                    {
                        RlRejectReason reason{};
                        if (RlDecodeReject(event.data.data(), event.data.size(), reason))
                        {
                            Fail(RlRejectText(reason));
                        }
                        break;
                    }
                    case RlMsg::Snapshot: OnSnapshot(event.data, now); break;
                    case RlMsg::Event:
                    {
                        RlEventMsg msg;
                        if (RlDecode(event.data.data(), event.data.size(), msg))
                        {
                            OnEvent(msg, now);
                        }
                        break;
                    }
                    default: break;
                    }
                    break;
                }
            }
        }

        void Update(double now)
        {
            Pump(now);
            const double dt = lastNow < 0.0 ? 0.0 : std::max(0.0, now - lastNow);
            lastNow = now;
            stats.rttMs = host != net::kInvalidPeer ? transport.RoundTripSeconds(host) * 1000.0 : 0.0;
            if (state != RlNetClientState::Playing || match == nullptr)
            {
                return;
            }
            DecayOffsets(dt);
            accumulator += std::min(dt, 0.25) * scale;
            int steps = 0;
            while (accumulator >= kTickDt && steps < kMaxCatchUp)
            {
                accumulator -= kTickDt;
                if (holdTicks > 0)
                {
                    --holdTicks;  // waiting for the host to catch up with our lead
                    continue;
                }
                StepFresh();
                ++steps;
            }
            if (accumulator > 4.0 * kTickDt)
            {
                accumulator = 0.0;  // fell far behind (a stall): do not run a burst
            }
            UpdateSecondCounter();
        }

        // ---- what is drawn --------------------------------------------------------------------------------------------------------------

        RlWorldState Render() const
        {
            if (match == nullptr)
            {
                return RlWorldState{};
            }
            RlWorldState world = match->World();
            const RlWorldState& previous = match->PreviousWorld();
            const float alpha = std::clamp(static_cast<float>(accumulator / kTickDt), 0.0f, 1.0f);
            for (int i = 0; i < world.carCount; ++i)
            {
                RlCarState& car = world.cars[i];
                const RlCarState& prev = previous.cars[i];
                RlVec3 position = Lerp(prev.position, car.position, alpha);
                RlQuat rotation = Q::Nlerp(prev.rotation, car.rotation, alpha);
                car.position = Add(position, carOffset[i].position);
                car.rotation = Q::Mul(carOffset[i].rotation, rotation);
            }
            const RlVec3 ballPosition = Lerp(previous.ball.position, world.ball.position, alpha);
            const RlQuat ballRotation = Q::Nlerp(previous.ball.rotation, world.ball.rotation, alpha);
            world.ball.position = Add(ballPosition, ballOffset.position);
            world.ball.rotation = Q::Mul(ballOffset.rotation, ballRotation);
            return world;
        }
    };

    RlNetClient::RlNetClient(net::INetTransport& transport, const RlTuning& tuning)
        : impl_(std::make_unique<Impl>(transport, tuning))
    {
    }

    RlNetClient::~RlNetClient() = default;

    bool RlNetClient::Connect(const std::string& address, const std::string& name, std::string& error)
    {
        impl_->name = name;
        impl_->host = impl_->transport.Connect(address, error);
        if (impl_->host == net::kInvalidPeer)
        {
            impl_->Fail(error);
            return false;
        }
        impl_->state = RlNetClientState::Connecting;
        return true;
    }

    void RlNetClient::Update(double now) { impl_->Update(now); }

    void RlNetClient::Close()
    {
        if (impl_->host != net::kInvalidPeer)
        {
            impl_->transport.Disconnect(impl_->host);
        }
        impl_->state = RlNetClientState::Disconnected;
        impl_->error = "closed";
    }

    RlNetClientState RlNetClient::State() const { return impl_->state; }
    const std::string& RlNetClient::Error() const { return impl_->error; }
    int RlNetClient::Slot() const { return impl_->slot; }
    void RlNetClient::SetLocalInput(const RlCarInput& input) { impl_->localInput = input; }
    const RlMatch* RlNetClient::Match() const { return impl_->match.get(); }
    RlWorldState RlNetClient::RenderWorld() const { return impl_->Render(); }
    std::vector<RlEventMsg> RlNetClient::TakeEvents()
    {
        std::vector<RlEventMsg> result;
        result.swap(impl_->events);
        return result;
    }
    const RlNetStats& RlNetClient::Stats() const { return impl_->stats; }
    bool RlNetClient::StateHashAt(uint32_t tick, uint64_t& hash) const
    {
        const Impl::StateSlot* at = impl_->RingAt(tick);
        if (at == nullptr)
        {
            return false;
        }
        hash = RlSim::HashState(at->bytes);
        return true;
    }
    bool RlNetClient::WorldAt(uint32_t tick, RlWorldState& world) const
    {
        const Impl::WorldSlot& slot = impl_->worldRing[tick % 128];
        if (slot.tick != tick)
        {
            return false;
        }
        world = slot.world;
        return true;
    }
    double RlNetClient::TimeScale() const { return impl_->scale; }
    void RlNetClient::SetTickObserver(std::function<void(uint32_t, const RlMatch&)> observer) { impl_->observer = std::move(observer); }
}
