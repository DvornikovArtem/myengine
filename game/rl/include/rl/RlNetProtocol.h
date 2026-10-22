// RlNetProtocol.h
// The messages of the Rocket League netcode (docs/game/rl-online.md, 7.2) and their byte layout. Everything is little endian and fits one
// packet (kMaxMessageBytes = 1100). The first byte of a message is its type.
//
//   Join      client -> host   reliable    protocol version, hash of the tuning, name
//   Welcome   host -> client   reliable    slot, mode, bot level, seed, mask of human slots, hash of the tuning (or Reject)
//   Input     client -> host   unreliable  the inputs of the last <= 8 ticks, one per tick, quantized
//   Snapshot  host -> clients  unreliable  60 Hz: tick, match, ball, cars, pads, the inputs the host really used, a hash of the full state
//   Names     host -> clients  reliable    the names of the table (before MatchStart: they are part of the state)
//   Event     host -> clients  reliable    match start, a slot that took a player / went to a bot, goal, assist, save, shot, demolition, match end
//
// Inputs are quantized to bytes before the simulation takes them, on both sides, so that the numbers the host and the client simulate are
// the same ones.

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rl
{
    constexpr uint16_t kNetProtocolVersion = 3;  // 2: statistics of the table (G1.0): Names, Event extras, stats in the Snapshot; 3: names up to 32 bytes (G1n, kNameBytes 33)
    constexpr int kNetInputBytes = 5;
    constexpr int kNetInputWindow = 8;   // ticks of input carried by one Input and one Snapshot message
    constexpr int kSnapshotInterval = 2; // ticks between snapshots: 60 Hz

    enum class RlMsg : uint8_t
    {
        Join = 1,
        Welcome = 2,
        Reject = 3,
        Input = 4,
        Snapshot = 5,
        Event = 6,
        StateChunk = 7,     // host -> client, reliable: a piece of the full state of the match (a player who joins a running match)
        InputRequest = 8,   // client -> host, reliable: the inputs the host used for these ticks
        InputHistory = 9,   // host -> client, reliable: the answer
        Names = 10,         // host -> clients, reliable: the names of the table, before MatchStart (they are part of the state)
    };

    enum class RlRejectReason : uint8_t
    {
        Version = 1,   // other protocol version
        Tuning = 2,    // other rl_tuning.json
        Full = 3,      // no free slot
        Started = 4,   // the match is on already (no longer sent: a running match takes new players into the slots of bots)
    };

    const char* RlRejectText(RlRejectReason reason);

    // Hash of the numbers that decide the simulation (FNV-1a over RlTuning::ToJson()); hosts and clients must agree on it.
    uint64_t RlTuningHash(const RlTuning& tuning);

    // The input as the simulation takes it: every axis rounded to 1/127, the buttons as bits.
    RlCarInput RlQuantizeInput(const RlCarInput& input);
    void RlPackInput(const RlCarInput& input, uint8_t out[kNetInputBytes]);
    RlCarInput RlUnpackInput(const uint8_t in[kNetInputBytes]);
    bool RlSameInput(const RlCarInput& a, const RlCarInput& b);

    struct RlJoinMsg
    {
        uint16_t version = kNetProtocolVersion;
        uint64_t tuningHash = 0;
        std::string name;  // up to kPlayerNameBytes bytes (RlName.h)
    };

    struct RlWelcomeMsg
    {
        uint16_t version = kNetProtocolVersion;
        uint8_t slot = 0;
        uint8_t mode = 1;
        uint8_t botLevel = 1;
        uint8_t started = 0;      // the match runs already (not used for now: a started match refuses new players)
        uint64_t seed = 1;
        uint32_t humanMask = 0;
        uint64_t tuningHash = 0;
    };

    struct RlInputMsg
    {
        uint32_t firstTick = 0;
        int count = 0;
        RlCarInput inputs[kNetInputWindow];  // ticks firstTick .. firstTick + count - 1
    };

    enum class RlEventKind : uint8_t
    {
        MatchStart = 1,  // tick 0: the match begins; `mask` is the human slots
        SlotMask = 2,    // from `tick` on the human slots are `mask`; `slot` joined (a = 1) or went to a bot (a = 0)
        Goal = 3,        // `slot` scored for team a (slot -1 = nobody touched the ball); other: the assisting slot or -1; value: the speed of the ball, dm/s
                         // (an own goal: the team of `slot` is not a)
        Demolition = 4,  // slot `slot` was demolished by `other` (-1 unknown)
        MatchEnd = 5,    // winner a (0 blue, 1 orange)
        Assist = 6,      // `slot` assisted the goal of `other`
        Save = 7,        // `slot` saved a shot
        Shot = 8,        // `slot` shot at the goal
    };

    struct RlEventMsg
    {
        RlEventKind kind = RlEventKind::MatchStart;
        uint32_t tick = 0;
        int8_t slot = -1;
        uint8_t a = 0;
        uint32_t mask = 0;
        int8_t other = -1;
        uint16_t value = 0;
    };

    // The names of the table: one per slot, up to kNameBytes - 1 bytes (UTF-8), the empty ones are filled with the defaults by RlMatch.
    struct RlNamesMsg
    {
        std::string names[kMaxCars];
    };

    // What the host knows at tick `tick` (the state before the step of that tick). The world is the dequantized one: positions,
    // rotations and velocities of the cars and the ball, the boost and a few flags, the score and the clock, the statistics of the table (points, goals,
    // assists, saves, shots, demolitions per slot) and the last goal's speed and assist; the rest is zero.
    struct RlSnapshotMsg
    {
        uint32_t tick = 0;
        uint32_t ackInputTick = 0;    // the newest tick of input the host has from this client
        int8_t margin = 0;            // ticks that the newest input of this client arrived ahead of the host (negative: late)
        uint32_t humanMask = 0;
        uint64_t stateHash = 0;       // hash of RlMatch::SaveState at `tick`
        uint64_t padMask = 0;         // bit i: pad i is ready
        RlWorldState world;
        int inputCount = 0;           // ticks of used input: tick - inputCount .. tick - 1
        uint8_t inputs[kMaxCars][kNetInputWindow][kNetInputBytes] = {};
    };

    // A player who joins a running match gets the saved state of the match at tick `tick` in pieces of at most kStateChunkBytes
    constexpr size_t kStateChunkBytes = 1000;
    struct RlStateChunkMsg
    {
        uint32_t tick = 0;
        uint32_t total = 0;    // bytes of the whole state
        uint32_t offset = 0;   // where this piece goes
        std::vector<uint8_t> bytes;
    };

    constexpr int kHistoryTicksPerMessage = 20;
    struct RlInputRequestMsg
    {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    struct RlInputHistoryMsg
    {
        uint32_t first = 0;
        int count = 0;  // <= kHistoryTicksPerMessage; every slot, so that the client knows what the others did
        uint8_t inputs[kHistoryTicksPerMessage][kMaxCars][kNetInputBytes] = {};
    };

    void RlEncode(const RlStateChunkMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlInputRequestMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlInputHistoryMsg& msg, std::vector<uint8_t>& out);
    bool RlDecode(const uint8_t* data, size_t size, RlStateChunkMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlInputRequestMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlInputHistoryMsg& msg);

    void RlEncode(const RlJoinMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlWelcomeMsg& msg, std::vector<uint8_t>& out);
    void RlEncodeReject(RlRejectReason reason, std::vector<uint8_t>& out);
    void RlEncode(const RlInputMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlEventMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlSnapshotMsg& msg, std::vector<uint8_t>& out);
    void RlEncode(const RlNamesMsg& msg, std::vector<uint8_t>& out);

    bool RlDecode(const uint8_t* data, size_t size, RlJoinMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlWelcomeMsg& msg);
    bool RlDecodeReject(const uint8_t* data, size_t size, RlRejectReason& reason);
    bool RlDecode(const uint8_t* data, size_t size, RlInputMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlEventMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlSnapshotMsg& msg);
    bool RlDecode(const uint8_t* data, size_t size, RlNamesMsg& msg);

    inline RlMsg RlMessageType(const uint8_t* data, size_t size) { return size == 0 ? static_cast<RlMsg>(0) : static_cast<RlMsg>(data[0]); }
}
