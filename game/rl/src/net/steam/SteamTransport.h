// SteamTransport.h (private)
// The transport of a match over ISteamNetworkingMessages (docs/game/rl-online.md 7.4): peers are Steam ids, the Steam network (direct or through the
// relays of Valve) carries the packets. Compiled only with MYENGINE_STEAM (or the mock check of CMake).

#pragma once

#include <rl/net/NetTransport.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace rl::net
{
    // `allowed`: the Steam ids that may open a session with the host (the members of the lobby); empty: anybody.
    std::unique_ptr<INetTransport> MakeSteamTransportFor(const std::vector<uint64_t>& allowed);
}
