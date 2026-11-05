// NetRng.h
// The random numbers of the network: the handshake nonces and the losses and delays of the network simulation. splitmix64, the same numbers as
// rl::RlRng, so a seeded test sees the same losses it saw before the transport moved into the engine.

#pragma once

#include <cstdint>

namespace myengine::net
{
    struct NetRng
    {
        uint64_t state = 0x9E3779B97F4A7C15ull;

        explicit NetRng(uint64_t seed = 1) { state = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull; }

        uint64_t NextU64()
        {
            state += 0x9E3779B97F4A7C15ull;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        uint32_t NextU32() { return static_cast<uint32_t>(NextU64() >> 32); }
        // 0..1 (24 bits, exactly representable)
        float Float01() { return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f); }
        // -1..1
        float Signed() { return Float01() * 2.0f - 1.0f; }
    };
}
