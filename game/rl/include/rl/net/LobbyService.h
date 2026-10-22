// LobbyService.h
// The lobby of the Rocket League game (docs/game/rl-online.md 7.4, docs/game/steam-setup.md): where players find each other, see who is in,
// get ready and start the match. The lobby does not play the match: when the host starts it, every member gets MatchStartInfo and the game
// goes on with RlNetHost (host) or RlNetClient (the others) over the transport that the service makes (CreateMatchTransport).
//
//   LanLobbyService    no internet: hosts announce their lobby by UDP broadcast once a second, the list shows them with their ping, a member
//                      connects to the host's lobby port; "ip:port" typed by hand works too
//   SteamLobbyService  ISteamMatchmaking lobbies and the overlay invitations, the match runs over SteamTransport (ISteamNetworkingMessages);
//                      built only with MYENGINE_STEAM (LobbySteam.h)
//
// Everything is driven by the game loop: call Update(now) once a frame (now is in seconds, any origin) and read TakeEvents() after it. No
// call blocks and none throws; what fails says why in the `error` string or in an Error event.
//
// The use of the screen "Private match":
//   host:    service->Create({"Artem", 2, 8, 47777}); show Members(); service->InviteText() (LAN: the address to give a friend) or
//            service->InviteFriends() (Steam overlay); when everybody is ready service->StartMatch(error)
//   guest:   service->RefreshList() every few seconds, show Lobbies(); service->Join(entry.id) (or Join("192.168.1.5:47778") by hand);
//            service->SetReady(true); wait for the MatchStarting event
//   both:    on MatchStarting take StartInfo(): host -> RlNetHost on service->CreateMatchTransport(), Listen(info.gamePort); guest ->
//            RlNetClient on its own transport, Connect(info.hostAddress)

#pragma once

#include <rl/net/NetTransport.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rl::net
{
    constexpr uint16_t kLanBeaconPort = 47779;    // hosts broadcast here, searching clients listen here
    constexpr uint16_t kDefaultLobbyPort = 47778; // members connect to the lobby of a LAN host here
    constexpr uint16_t kDefaultGamePort = 47777;  // the match itself (RlNetHost)
    constexpr int kLobbyProtocolVersion = 2;  // 2: names up to 32 bytes (G1n)

    // One line of the list of lobbies
    struct LobbyInfo
    {
        std::string id;           // what Join takes: LAN "ip:lobbyPort", Steam the lobby id in decimal
        std::string hostName;
        int mode = 2;             // players per team, 1..4
        int maxPlayers = 8;
        int players = 1;          // members now, the host included
        int pingMs = -1;          // round trip to the host, -1 until the first answer
        bool joinable = true;     // false: another version of the game or the lobby is full / in a match
        std::string note;         // why not joinable ("other version", "full", "match in progress")
    };

    struct LobbyMember
    {
        std::string id;           // Steam: the Steam id in decimal; LAN: "host" or "member-<n>" (only unique inside this lobby)
        std::string name;
        bool ready = false;
        bool isHost = false;
        bool isLocal = false;     // this is us
    };

    struct LobbyCreateOptions
    {
        std::string hostName = "Player";
        int mode = 2;             // players per team, 1..4
        int maxPlayers = 8;       // 2 .. 8
        uint16_t lobbyPort = kDefaultLobbyPort;  // LAN: members connect here
        uint16_t gamePort = kDefaultGamePort;    // LAN: the match listens here
        bool friendsOnly = true;  // Steam: the lobby is private and only reachable by an invitation (always true with AppID 480)
    };

    enum class LobbyState
    {
        Idle,       // not in a lobby
        Creating,   // Create was called, the lobby is not there yet (Steam answers later)
        Hosting,    // in our own lobby
        Joining,    // Join was called, the host has not answered
        Joined,     // in the lobby of somebody else
        Starting,   // the host started the match: StartInfo() is valid
        Failed,     // see LastError()
    };

    enum class LobbyEventKind
    {
        Created,        // our lobby is open
        Joined,         // we are in the lobby of somebody
        MemberJoined,   // `member` came
        MemberLeft,     // `member` left; `text` says why when it was not the player's own wish (timeout)
        MemberChanged,  // `member` changed its name or its readiness
        ListUpdated,    // Lobbies() has new or changed entries
        MatchStarting,  // the host started the match: go on with StartInfo()
        HostLeft,       // the host went: the lobby is gone for everybody
        InviteReceived, // a friend invited us (Steam): `text` is the lobby id to give to Join
        Error,          // `text` says what
    };

    struct LobbyEvent
    {
        LobbyEventKind kind = LobbyEventKind::Error;
        LobbyMember member;
        std::string text;
    };

    // What the game needs to start the match when the host has started it
    struct MatchStartInfo
    {
        bool isHost = false;
        std::string hostAddress;       // for RlNetClient::Connect: LAN "ip:gamePort", Steam the Steam id of the host in decimal
        uint16_t gamePort = kDefaultGamePort;  // for RlNetHost::Listen (Steam ignores it)
        int mode = 2;
        std::vector<LobbyMember> members;  // in the order of the lobby; the host first
    };

    class ILobbyService
    {
    public:
        virtual ~ILobbyService() = default;

        virtual const char* Name() const = 0;                          // "LAN" or "Steam"
        // false and why when the service cannot work here (no Steam client running, no SDK in this build, no network)
        virtual bool Available(std::string& whyNot) const = 0;

        // The frame of the service: callbacks, the beacon of the host, the pings of the list.
        virtual void Update(double now) = 0;
        virtual std::vector<LobbyEvent> TakeEvents() = 0;

        // Hosting
        virtual bool Create(const LobbyCreateOptions& options, std::string& error) = 0;
        // LAN: the address "ip:lobbyPort" that a friend types in by hand (the list shows the lobby by itself); Steam: "" (use InviteFriends)
        virtual std::string InviteText() const = 0;
        // Steam: opens the overlay with the list of friends; LAN: false
        virtual bool InviteFriends() = 0;
        virtual bool StartMatch(std::string& error) = 0;               // the host only; every member gets MatchStarting

        // Joining
        virtual void RefreshList() = 0;                                // look for lobbies again; the answers come with Update
        virtual const std::vector<LobbyInfo>& Lobbies() const = 0;
        virtual bool Join(const std::string& idOrAddress, std::string& error) = 0;
        virtual void SetReady(bool ready) = 0;
        virtual void SetLocalName(const std::string& name) = 0;

        virtual void Leave() = 0;                                      // out of the lobby (the host: closes it)
        virtual LobbyState State() const = 0;
        virtual const std::string& LastError() const = 0;
        virtual const std::vector<LobbyMember>& Members() const = 0;
        virtual const LobbyInfo& Current() const = 0;                  // the lobby we are in (valid in Hosting, Joined, Starting)
        virtual const MatchStartInfo& StartInfo() const = 0;           // valid in Starting

        // The transport of the match for this service: UDP on the LAN, SteamTransport with Steam. `sim` is the lag of --net-sim.
        virtual std::unique_ptr<INetTransport> CreateMatchTransport(const NetSimParams& sim) = 0;
    };

    // The LAN lobby over real UDP. `name` is shown to the others (the host name of the PC by default).
    std::unique_ptr<ILobbyService> MakeLanLobbyService(const std::string& name = "");
}
