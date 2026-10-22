// LobbyLan.h
// The LAN lobby with its parts replaceable (the tests run it on a loopback "network" with a virtual clock). The game calls
// MakeLanLobbyService(name) of LobbyService.h.

#pragma once

#include <rl/net/LobbyService.h>
#include <rl/net/NetLink.h>

#include <functional>

namespace rl::net
{
    struct LanLobbyOptions
    {
        std::string name;  // shown to the others; the name of the PC when empty
        // the datagram links of the service: the beacon, the search and the lobby connection are each one link (default: UDP)
        std::function<std::unique_ptr<IDatagramLink>()> makeLink;
        // the transport of the match (default: UDP with the lag of --net-sim)
        std::function<std::unique_ptr<INetTransport>(const NetSimParams&)> makeMatchTransport;
        int protocolVersion = kLobbyProtocolVersion;  // the tests make a lobby of another version
        uint64_t seed = 1;
    };

    std::unique_ptr<ILobbyService> MakeLanLobbyService(const LanLobbyOptions& options);
}
