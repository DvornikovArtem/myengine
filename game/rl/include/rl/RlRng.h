// RlRng.h
// The random numbers of a match: splitmix64 seeded by the match. A plain 8-byte state, so a snapshot carries it and a replay draws the
// same numbers. Nothing in the simulation may use rand() or std::random_device.

#pragma once

#include <cstdint>

namespace rl
{
    struct RlRng
    {
        uint64_t state = 0x9E3779B97F4A7C15ull;

        explicit RlRng(uint64_t seed = 1) { state = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull; }

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
        // 0..count-1
        int Range(int count) { return count <= 1 ? 0 : static_cast<int>(NextU32() % static_cast<uint32_t>(count)); }
    };
}
