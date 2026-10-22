// LobbySteam.h
// The Steam side of the lobby (docs/game/rl-online.md 7.4, docs/game/steam-setup.md): ISteamMatchmaking lobbies and the overlay invitations
// (SteamLobbyService), and the transport of the match over ISteamNetworkingMessages (SteamTransport). The code that talks to the Steamworks SDK is
// built only with the CMake option MYENGINE_STEAM (OFF by default, the SDK is not in the repository: STEAMWORKS_SDK_DIR). Without it the
// functions here still exist: the service says "no Steam in this build" from Available(), so the game can offer it unconditionally.
//
// The ids that cross this API are decimal strings (a Steam lobby id and a Steam id are 64-bit numbers).

#pragma once

#include <rl/net/LobbyService.h>

#include <memory>
#include <string>

namespace rl::net
{
    // The Steam lobby service; with MYENGINE_STEAM OFF a service that is never Available. SteamAPI_Init is done by the first service and
    // SteamAPI_Shutdown by the last one.
    std::unique_ptr<ILobbyService> MakeSteamLobbyService(const std::string& name = "");

    // The transport of a match over Steam: Listen() on the host (it accepts sessions only from the members of the lobby), Connect("<steam id of the
    // host>") on a guest. nullptr with MYENGINE_STEAM OFF.
    std::unique_ptr<INetTransport> MakeSteamTransport();

    // Steam starts the game of a friend who accepts an invitation with "+connect_lobby <lobby id>" on the command line: the id from such a line, or
    // an empty string. Works without the SDK (the game calls it before Steam is up).
    std::string FindConnectLobbyInCommandLine(const std::string& commandLine);

    constexpr int kSteamAppId = 480;  // Spacewar, the test app of Valve: steam_appid.txt next to the exe holds this number
}
