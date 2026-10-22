// RlNetProtocol.cpp

#include <rl/RlNetProtocol.h>

#include <rl/RlName.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rl
{
    namespace
    {
        class Writer
        {
        public:
            explicit Writer(std::vector<uint8_t>& out)
                : out_(out)
            {
                out_.clear();
            }
            void U8(uint8_t v) { out_.push_back(v); }
            void U16(uint16_t v)
            {
                U8(static_cast<uint8_t>(v));
                U8(static_cast<uint8_t>(v >> 8));
            }
            void U32(uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                {
                    U8(static_cast<uint8_t>(v >> (8 * i)));
                }
            }
            void U64(uint64_t v)
            {
                for (int i = 0; i < 8; ++i)
                {
                    U8(static_cast<uint8_t>(v >> (8 * i)));
                }
            }
            void F32(float v)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                U32(bits);
            }
            void Bytes(const uint8_t* data, size_t size) { out_.insert(out_.end(), data, data + size); }

        private:
            std::vector<uint8_t>& out_;
        };

        class Reader
        {
        public:
            Reader(const uint8_t* data, size_t size)
                : data_(data)
                , size_(size)
            {
            }
            bool Ok() const { return ok_; }
            uint8_t U8()
            {
                if (pos_ + 1 > size_)
                {
                    ok_ = false;
                    return 0;
                }
                return data_[pos_++];
            }
            uint16_t U16()
            {
                const uint16_t lo = U8();
                return static_cast<uint16_t>(lo | (static_cast<uint16_t>(U8()) << 8));
            }
            uint32_t U32()
            {
                uint32_t v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    v |= static_cast<uint32_t>(U8()) << (8 * i);
                }
                return v;
            }
            uint64_t U64()
            {
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                {
                    v |= static_cast<uint64_t>(U8()) << (8 * i);
                }
                return v;
            }
            float F32()
            {
                const uint32_t bits = U32();
                float v = 0.0f;
                std::memcpy(&v, &bits, sizeof(v));
                return v;
            }
            void Bytes(uint8_t* out, size_t size)
            {
                if (pos_ + size > size_)
                {
                    ok_ = false;
                    std::memset(out, 0, size);
                    return;
                }
                std::memcpy(out, data_ + pos_, size);
                pos_ += size;
            }

        private:
            const uint8_t* data_;
            size_t size_;
            size_t pos_ = 0;
            bool ok_ = true;
        };

        // ---- quantization -------------------------------------------------------------------------------------------------------

        // positions: 16 bits over [lo, lo + range): about 2 mm on x and z, 1 mm on y
        constexpr float kPosXZRange = 128.0f;
        constexpr float kPosXZLow = -64.0f;
        constexpr float kPosYRange = 64.0f;
        constexpr float kPosYLow = -8.0f;
        constexpr float kVelRange = 64.0f;     // +-64 m/s
        constexpr float kAngVelRange = 32.0f;  // +-32 rad/s

        uint16_t QuantizeRange(float v, float low, float range)
        {
            const float t = std::clamp((v - low) / range, 0.0f, 1.0f);
            return static_cast<uint16_t>(std::lround(t * 65535.0f));
        }
        float DequantizeRange(uint16_t q, float low, float range) { return low + static_cast<float>(q) * (range / 65535.0f); }

        uint16_t QuantizeSigned(float v, float limit)
        {
            const float t = std::clamp(v / limit, -1.0f, 1.0f);
            return static_cast<uint16_t>(static_cast<int16_t>(std::lround(t * 32767.0f)));
        }
        float DequantizeSigned(uint16_t q, float limit) { return static_cast<float>(static_cast<int16_t>(q)) * (limit / 32767.0f); }

        void PutPosition(Writer& w, const RlVec3& p)
        {
            w.U16(QuantizeRange(p.x, kPosXZLow, kPosXZRange));
            w.U16(QuantizeRange(p.y, kPosYLow, kPosYRange));
            w.U16(QuantizeRange(p.z, kPosXZLow, kPosXZRange));
        }
        RlVec3 GetPosition(Reader& r)
        {
            RlVec3 p;
            p.x = DequantizeRange(r.U16(), kPosXZLow, kPosXZRange);
            p.y = DequantizeRange(r.U16(), kPosYLow, kPosYRange);
            p.z = DequantizeRange(r.U16(), kPosXZLow, kPosXZRange);
            return p;
        }
        void PutVector(Writer& w, const RlVec3& v, float limit)
        {
            w.U16(QuantizeSigned(v.x, limit));
            w.U16(QuantizeSigned(v.y, limit));
            w.U16(QuantizeSigned(v.z, limit));
        }
        RlVec3 GetVector(Reader& r, float limit)
        {
            RlVec3 v;
            v.x = DequantizeSigned(r.U16(), limit);
            v.y = DequantizeSigned(r.U16(), limit);
            v.z = DequantizeSigned(r.U16(), limit);
            return v;
        }

        // "smallest three": the largest component is dropped (and rebuilt from the other three), 2 bits tell which one, 10 bits each
        uint32_t PackQuat(const RlQuat& q)
        {
            float c[4] = {q.x, q.y, q.z, q.w};
            const float length = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
            if (length > 1e-6f)
            {
                for (float& v : c)
                {
                    v /= length;
                }
            }
            else
            {
                c[0] = c[1] = c[2] = 0.0f;
                c[3] = 1.0f;
            }
            int largest = 0;
            for (int i = 1; i < 4; ++i)
            {
                if (std::fabs(c[i]) > std::fabs(c[largest]))
                {
                    largest = i;
                }
            }
            const float sign = c[largest] < 0.0f ? -1.0f : 1.0f;
            uint32_t packed = static_cast<uint32_t>(largest);
            int shift = 2;
            for (int i = 0; i < 4; ++i)
            {
                if (i == largest)
                {
                    continue;
                }
                const float v = std::clamp(c[i] * sign * 1.41421356f, -1.0f, 1.0f);  // |component| <= 1/sqrt(2)
                const uint32_t bits = static_cast<uint32_t>(std::lround((v * 0.5f + 0.5f) * 1023.0f));
                packed |= bits << shift;
                shift += 10;
            }
            return packed;
        }
        RlQuat UnpackQuat(uint32_t packed)
        {
            const int largest = static_cast<int>(packed & 3u);
            float c[4] = {0, 0, 0, 0};
            float sum = 0.0f;
            int shift = 2;
            for (int i = 0; i < 4; ++i)
            {
                if (i == largest)
                {
                    continue;
                }
                const uint32_t bits = (packed >> shift) & 1023u;
                shift += 10;
                c[i] = (static_cast<float>(bits) / 1023.0f * 2.0f - 1.0f) * 0.70710678f;
                sum += c[i] * c[i];
            }
            c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
            return RlQuat{c[0], c[1], c[2], c[3]};
        }

        enum CarFlags : uint8_t
        {
            kOnGround = 1,
            kDemolished = 2,
            kSupersonic = 4,
            kJumped = 8,
            kCanDodge = 16,
            kDodging = 32,
        };

        void PutBody(Writer& w, const RlVec3& position, const RlQuat& rotation, const RlVec3& velocity, const RlVec3& angular)
        {
            PutPosition(w, position);
            w.U32(PackQuat(rotation));
            PutVector(w, velocity, kVelRange);
            PutVector(w, angular, kAngVelRange);
        }
        void GetBody(Reader& r, RlVec3& position, RlQuat& rotation, RlVec3& velocity, RlVec3& angular)
        {
            position = GetPosition(r);
            rotation = UnpackQuat(r.U32());
            velocity = GetVector(r, kVelRange);
            angular = GetVector(r, kAngVelRange);
        }

        int8_t QuantizeAxis(float v) { return static_cast<int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }
        float DequantizeAxis(int8_t q) { return static_cast<float>(q) / 127.0f; }
    }

    const char* RlRejectText(RlRejectReason reason)
    {
        switch (reason)
        {
        case RlRejectReason::Version: return "The host has another version of the game: both players need the same one.";
        case RlRejectReason::Tuning: return "The host plays with other game settings: both players need the same game files.";
        case RlRejectReason::Full: return "The match has no free slot.";
        case RlRejectReason::Started: return "The match has already started.";
        case RlRejectReason::Cars: return "The host has other cars: both players need the same game files.";
        }
        return "the host refused the connection";
    }

    uint64_t RlTuningHash(const RlTuning& tuning)
    {
        const std::string text = tuning.ToJson();
        uint64_t hash = 1469598103934665603ull;
        for (const char c : text)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    RlCarInput RlQuantizeInput(const RlCarInput& input)
    {
        uint8_t bytes[kNetInputBytes];
        RlPackInput(input, bytes);
        return RlUnpackInput(bytes);
    }

    void RlPackInput(const RlCarInput& input, uint8_t out[kNetInputBytes])
    {
        out[0] = static_cast<uint8_t>(QuantizeAxis(input.throttle));
        out[1] = static_cast<uint8_t>(QuantizeAxis(input.steer));
        out[2] = static_cast<uint8_t>(QuantizeAxis(input.pitch));
        out[3] = static_cast<uint8_t>(QuantizeAxis(input.roll));
        out[4] = static_cast<uint8_t>((input.boost ? 1 : 0) | (input.jump ? 2 : 0) | (input.handbrake ? 4 : 0) | (input.airRoll ? 8 : 0));
    }

    RlCarInput RlUnpackInput(const uint8_t in[kNetInputBytes])
    {
        RlCarInput input;
        input.throttle = DequantizeAxis(static_cast<int8_t>(in[0]));
        input.steer = DequantizeAxis(static_cast<int8_t>(in[1]));
        input.pitch = DequantizeAxis(static_cast<int8_t>(in[2]));
        input.roll = DequantizeAxis(static_cast<int8_t>(in[3]));
        input.boost = (in[4] & 1) != 0;
        input.jump = (in[4] & 2) != 0;
        input.handbrake = (in[4] & 4) != 0;
        input.airRoll = (in[4] & 8) != 0;
        return input;
    }

    bool RlSameInput(const RlCarInput& a, const RlCarInput& b)
    {
        return a.throttle == b.throttle && a.steer == b.steer && a.pitch == b.pitch && a.roll == b.roll && a.boost == b.boost && a.jump == b.jump &&
               a.handbrake == b.handbrake && a.airRoll == b.airRoll;
    }

    // ---- encode ---------------------------------------------------------------------------------------------------------------------

    void RlEncode(const RlJoinMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Join));
        w.U16(msg.version);
        w.U64(msg.tuningHash);
        w.U64(msg.carsHash);
        w.U8(msg.car);
        const std::string name = SanitizePlayerName(msg.name);  // up to kPlayerNameBytes, a character is never cut in the middle
        w.U8(static_cast<uint8_t>(name.size()));
        w.Bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    }

    void RlEncode(const RlWelcomeMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Welcome));
        w.U16(msg.version);
        w.U8(msg.slot);
        w.U8(msg.mode);
        w.U8(msg.botLevel);
        w.U8(msg.started);
        w.U64(msg.seed);
        w.U32(msg.humanMask);
        w.U64(msg.tuningHash);
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            w.U8(msg.cars[slot]);
        }
    }

    void RlEncodeReject(RlRejectReason reason, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Reject));
        w.U8(static_cast<uint8_t>(reason));
    }

    void RlEncode(const RlInputMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Input));
        w.U32(msg.firstTick);
        const int count = std::clamp(msg.count, 0, kNetInputWindow);
        w.U8(static_cast<uint8_t>(count));
        for (int i = 0; i < count; ++i)
        {
            uint8_t bytes[kNetInputBytes];
            RlPackInput(msg.inputs[i], bytes);
            w.Bytes(bytes, kNetInputBytes);
        }
    }

    void RlEncode(const RlEventMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Event));
        w.U8(static_cast<uint8_t>(msg.kind));
        w.U32(msg.tick);
        w.U8(static_cast<uint8_t>(msg.slot));
        w.U8(msg.a);
        w.U32(msg.mask);
        w.U8(static_cast<uint8_t>(msg.other));
        w.U16(msg.value);
    }

    void RlEncode(const RlNamesMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::Names));
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            const size_t length = std::min(msg.names[slot].size(), static_cast<size_t>(kNameBytes - 1));
            w.U8(static_cast<uint8_t>(length));
            w.Bytes(reinterpret_cast<const uint8_t*>(msg.names[slot].data()), length);
        }
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            w.U8(msg.cars[slot]);
        }
    }

    void RlEncode(const RlCarMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::CarSelect));
        w.U8(msg.car);
    }

    void RlEncode(const RlSnapshotMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        const RlWorldState& world = msg.world;
        w.U8(static_cast<uint8_t>(RlMsg::Snapshot));
        w.U32(msg.tick);
        w.U32(msg.ackInputTick);
        w.U8(static_cast<uint8_t>(msg.margin));
        w.U32(msg.humanMask);
        w.U64(msg.stateHash);
        for (int i = 0; i < 5; ++i)
        {
            w.U8(static_cast<uint8_t>(msg.padMask >> (8 * i)));
        }
        w.U8(static_cast<uint8_t>(world.match.phase));
        w.U8(static_cast<uint8_t>(world.match.score[0]));
        w.U8(static_cast<uint8_t>(world.match.score[1]));
        w.U8(static_cast<uint8_t>(world.match.overtime != 0 ? 1 : 0));
        w.F32(world.match.clock);
        w.F32(world.match.phaseTimer);
        w.F32(world.match.lastGoalSpeed);
        w.U8(static_cast<uint8_t>(world.match.lastGoalAssist + 1));
        w.U8(static_cast<uint8_t>(world.lastBallToucher + 1));
        PutBody(w, world.ball.position, world.ball.rotation, world.ball.linearVelocity, world.ball.angularVelocity);

        const int carCount = std::clamp(world.carCount, 0, kMaxCars);
        w.U8(static_cast<uint8_t>(carCount));
        for (int i = 0; i < carCount; ++i)
        {
            const RlCarState& car = world.cars[i];
            PutBody(w, car.position, car.rotation, car.linearVelocity, car.angularVelocity);
            w.U8(static_cast<uint8_t>(std::lround(std::clamp(car.boost, 0.0f, 100.0f) * 2.55f)));
            uint8_t flags = 0;
            flags |= car.onGround ? kOnGround : 0;
            flags |= car.demolished ? kDemolished : 0;
            flags |= car.supersonic ? kSupersonic : 0;
            flags |= car.jumped ? kJumped : 0;
            flags |= car.canDodge ? kCanDodge : 0;
            flags |= car.dodging ? kDodging : 0;
            w.U8(flags);
            w.U16(static_cast<uint16_t>(std::clamp(world.match.points[i], 0, 65535)));
            w.U8(static_cast<uint8_t>(std::clamp(world.match.goals[i], 0, 255)));
            w.U8(static_cast<uint8_t>(std::clamp(world.match.assists[i], 0, 255)));
            w.U8(static_cast<uint8_t>(std::clamp(world.match.saves[i], 0, 255)));
            w.U8(static_cast<uint8_t>(std::clamp(world.match.shots[i], 0, 255)));
            w.U8(static_cast<uint8_t>(std::clamp(world.match.demos[i], 0, 255)));
        }

        const int window = std::clamp(msg.inputCount, 0, kNetInputWindow);
        w.U8(static_cast<uint8_t>(window));
        for (int slot = 0; slot < carCount; ++slot)
        {
            if ((msg.humanMask & (1u << slot)) == 0)
            {
                continue;  // bots have no input to send
            }
            for (int j = 0; j < window; ++j)
            {
                w.Bytes(msg.inputs[slot][j], kNetInputBytes);
            }
        }
    }

    // ---- decode ---------------------------------------------------------------------------------------------------------------------

    bool RlDecode(const uint8_t* data, size_t size, RlJoinMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Join))
        {
            return false;
        }
        msg.version = r.U16();
        msg.tuningHash = r.U64();
        msg.carsHash = r.U64();
        msg.car = r.U8();
        const size_t length = r.U8();
        char name[kNameBytes] = {};
        if (length > kPlayerNameBytes)
        {
            return false;
        }
        r.Bytes(reinterpret_cast<uint8_t*>(name), length);
        msg.name.assign(name, length);
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlWelcomeMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Welcome))
        {
            return false;
        }
        msg.version = r.U16();
        msg.slot = r.U8();
        msg.mode = r.U8();
        msg.botLevel = r.U8();
        msg.started = r.U8();
        msg.seed = r.U64();
        msg.humanMask = r.U32();
        msg.tuningHash = r.U64();
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            msg.cars[slot] = r.U8();
        }
        return r.Ok();
    }

    bool RlDecodeReject(const uint8_t* data, size_t size, RlRejectReason& reason)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Reject))
        {
            return false;
        }
        reason = static_cast<RlRejectReason>(r.U8());
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlInputMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Input))
        {
            return false;
        }
        msg.firstTick = r.U32();
        msg.count = r.U8();
        if (msg.count > kNetInputWindow)
        {
            return false;
        }
        for (int i = 0; i < msg.count; ++i)
        {
            uint8_t bytes[kNetInputBytes];
            r.Bytes(bytes, kNetInputBytes);
            msg.inputs[i] = RlUnpackInput(bytes);
        }
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlEventMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Event))
        {
            return false;
        }
        msg.kind = static_cast<RlEventKind>(r.U8());
        msg.tick = r.U32();
        msg.slot = static_cast<int8_t>(r.U8());
        msg.a = r.U8();
        msg.mask = r.U32();
        msg.other = static_cast<int8_t>(r.U8());
        msg.value = r.U16();
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlNamesMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Names))
        {
            return false;
        }
        msg = RlNamesMsg{};
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            const size_t length = r.U8();
            if (length > static_cast<size_t>(kNameBytes - 1))
            {
                return false;
            }
            char name[kNameBytes] = {};
            r.Bytes(reinterpret_cast<uint8_t*>(name), length);
            msg.names[slot].assign(name, length);
        }
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            msg.cars[slot] = r.U8();
        }
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlCarMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::CarSelect))
        {
            return false;
        }
        msg.car = r.U8();
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlSnapshotMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::Snapshot))
        {
            return false;
        }
        msg = RlSnapshotMsg{};
        RlWorldState& world = msg.world;
        msg.tick = r.U32();
        world.tick = msg.tick;
        msg.ackInputTick = r.U32();
        msg.margin = static_cast<int8_t>(r.U8());
        msg.humanMask = r.U32();
        msg.stateHash = r.U64();
        for (int i = 0; i < 5; ++i)
        {
            msg.padMask |= static_cast<uint64_t>(r.U8()) << (8 * i);
        }
        world.match.phase = static_cast<RlMatchPhase>(r.U8());
        world.match.score[0] = r.U8();
        world.match.score[1] = r.U8();
        world.match.overtime = r.U8();
        world.match.clock = r.F32();
        world.match.phaseTimer = r.F32();
        world.match.lastGoalSpeed = r.F32();
        world.match.lastGoalAssist = static_cast<int>(r.U8()) - 1;
        world.lastBallToucher = static_cast<int>(r.U8()) - 1;
        GetBody(r, world.ball.position, world.ball.rotation, world.ball.linearVelocity, world.ball.angularVelocity);

        const int carCount = r.U8();
        if (carCount > kMaxCars)
        {
            return false;
        }
        world.carCount = carCount;
        for (int i = 0; i < carCount; ++i)
        {
            RlCarState& car = world.cars[i];
            GetBody(r, car.position, car.rotation, car.linearVelocity, car.angularVelocity);
            car.boost = static_cast<float>(r.U8()) / 2.55f;
            const uint8_t flags = r.U8();
            car.onGround = (flags & kOnGround) != 0;
            car.demolished = (flags & kDemolished) != 0;
            car.supersonic = (flags & kSupersonic) != 0;
            car.jumped = (flags & kJumped) != 0;
            car.canDodge = (flags & kCanDodge) != 0;
            car.dodging = (flags & kDodging) != 0;
            world.match.points[i] = r.U16();
            world.match.goals[i] = r.U8();
            world.match.assists[i] = r.U8();
            world.match.saves[i] = r.U8();
            world.match.shots[i] = r.U8();
            world.match.demos[i] = r.U8();
        }
        for (int i = 0; i < kPadCount; ++i)
        {
            world.match.pads[i].respawnLeft = (msg.padMask & (1ull << i)) != 0 ? 0.0f : 1.0f;
        }

        msg.inputCount = r.U8();
        if (msg.inputCount > kNetInputWindow)
        {
            return false;
        }
        for (int slot = 0; slot < carCount; ++slot)
        {
            if ((msg.humanMask & (1u << slot)) == 0)
            {
                continue;
            }
            for (int j = 0; j < msg.inputCount; ++j)
            {
                r.Bytes(msg.inputs[slot][j], kNetInputBytes);
            }
        }
        return r.Ok();
    }

    // ---- the state of a running match for a player who joins it ----------------------------------------------------------------------

    void RlEncode(const RlStateChunkMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::StateChunk));
        w.U32(msg.tick);
        w.U32(msg.total);
        w.U32(msg.offset);
        w.U16(static_cast<uint16_t>(msg.bytes.size()));
        w.Bytes(msg.bytes.data(), msg.bytes.size());
    }

    void RlEncode(const RlInputRequestMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::InputRequest));
        w.U32(msg.first);
        w.U32(msg.count);
    }

    void RlEncode(const RlInputHistoryMsg& msg, std::vector<uint8_t>& out)
    {
        Writer w(out);
        w.U8(static_cast<uint8_t>(RlMsg::InputHistory));
        w.U32(msg.first);
        const int count = std::clamp(msg.count, 0, kHistoryTicksPerMessage);
        w.U8(static_cast<uint8_t>(count));
        for (int t = 0; t < count; ++t)
        {
            for (int slot = 0; slot < kMaxCars; ++slot)
            {
                w.Bytes(msg.inputs[t][slot], kNetInputBytes);
            }
        }
    }

    bool RlDecode(const uint8_t* data, size_t size, RlStateChunkMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::StateChunk))
        {
            return false;
        }
        msg.tick = r.U32();
        msg.total = r.U32();
        msg.offset = r.U32();
        const size_t length = r.U16();
        if (length > kStateChunkBytes || msg.total > (1u << 20) || msg.offset + length > msg.total)
        {
            return false;
        }
        msg.bytes.resize(length);
        r.Bytes(msg.bytes.data(), length);
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlInputRequestMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::InputRequest))
        {
            return false;
        }
        msg.first = r.U32();
        msg.count = r.U32();
        return r.Ok();
    }

    bool RlDecode(const uint8_t* data, size_t size, RlInputHistoryMsg& msg)
    {
        Reader r(data, size);
        if (r.U8() != static_cast<uint8_t>(RlMsg::InputHistory))
        {
            return false;
        }
        msg.first = r.U32();
        msg.count = r.U8();
        if (msg.count > kHistoryTicksPerMessage)
        {
            return false;
        }
        for (int t = 0; t < msg.count; ++t)
        {
            for (int slot = 0; slot < kMaxCars; ++slot)
            {
                r.Bytes(msg.inputs[t][slot], kNetInputBytes);
            }
        }
        return r.Ok();
    }
}
