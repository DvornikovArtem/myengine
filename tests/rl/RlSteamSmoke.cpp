// RlSteamSmoke.cpp
// The manual check of the Steam code against the real Steamworks SDK (docs/game/steam-setup.md). NOT part of ctest: it needs the Steam client
// running and logged in, and it is built only with -DMYENGINE_STEAM=ON.
//
// It does only what stays on this PC and in the account of the person who runs it: SteamAPI_Init with AppID 480, the network of Valve, one lobby
// "friends only" of our own that is made, read, listed, started, left. No invitation is sent, no message, nothing of the profile is changed.
//
// Exit code: 0 everything passed, 1 something failed, 77 skipped (no Steam client running; the check never starts the client itself).

#include <rl/net/LobbySteam.h>

#include <steam/steam_api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace rl::net;
    using Clock = std::chrono::steady_clock;

    int g_failed = 0;
    const Clock::time_point g_start = Clock::now();

    double Now() { return std::chrono::duration<double>(Clock::now() - g_start).count(); }

    void Check(bool ok, const std::string& what)
    {
        std::printf("[%6.2f] %s  %s\n", Now(), ok ? " ok " : "FAIL", what.c_str());
        if (!ok)
        {
            ++g_failed;
        }
    }

    void Info(const std::string& what) { std::printf("[%6.2f] info  %s\n", Now(), what.c_str()); }

    const char* StateName(LobbyState state)
    {
        switch (state)
        {
        case LobbyState::Idle: return "Idle";
        case LobbyState::Creating: return "Creating";
        case LobbyState::Hosting: return "Hosting";
        case LobbyState::Joining: return "Joining";
        case LobbyState::Joined: return "Joined";
        case LobbyState::Starting: return "Starting";
        case LobbyState::Failed: return "Failed";
        }
        return "?";
    }

    // The frame of the game: Update, then the events; true when `done` became true before the timeout
    bool Pump(ILobbyService& service, std::vector<LobbyEvent>& log, double seconds, const std::function<bool()>& done)
    {
        const double end = Now() + seconds;
        while (Now() < end)
        {
            service.Update(Now());
            for (LobbyEvent& event : service.TakeEvents())
            {
                log.push_back(std::move(event));
            }
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return false;
    }

    bool Has(const std::vector<LobbyEvent>& log, LobbyEventKind kind)
    {
        for (const LobbyEvent& event : log)
        {
            if (event.kind == kind)
            {
                return true;
            }
        }
        return false;
    }

    std::string Data(const CSteamID& lobby, const char* key)
    {
        const char* value = SteamMatchmaking()->GetLobbyData(lobby, key);
        return value != nullptr ? value : "";
    }

    const char* AvailabilityName(ESteamNetworkingAvailability a)
    {
        switch (a)
        {
        case k_ESteamNetworkingAvailability_CannotTry: return "CannotTry";
        case k_ESteamNetworkingAvailability_Failed: return "Failed";
        case k_ESteamNetworkingAvailability_Previously: return "Previously";
        case k_ESteamNetworkingAvailability_Retrying: return "Retrying";
        case k_ESteamNetworkingAvailability_NeverTried: return "NeverTried";
        case k_ESteamNetworkingAvailability_Waiting: return "Waiting";
        case k_ESteamNetworkingAvailability_Attempting: return "Attempting";
        case k_ESteamNetworkingAvailability_Current: return "Current";
        default: return "?";
        }
    }

    int Run()
    {
        // 0. the Steam client must be there; we never start it ourselves
        if (!SteamAPI_IsSteamRunning())
        {
            std::printf("SKIPPED: the Steam client is not running (start it, log in, run this again)\n");
            return 77;
        }
        Info("the Steam client is running");

        // 1. SteamAPI_Init with AppID 480 (steam_appid.txt next to the exe), done by the first service
        std::vector<LobbyEvent> log;
        std::unique_ptr<ILobbyService> service = MakeSteamLobbyService("smoke");
        std::string why;
        const bool available = service->Available(why);
        Check(available, "SteamAPI_Init (AppID 480 from steam_appid.txt)" + (available ? std::string() : ": " + why));
        if (!available)
        {
            return 1;
        }
        Check(std::string(service->Name()) == "Steam", "the service says its name is Steam");
        Check(SteamUtils() != nullptr && SteamUtils()->GetAppID() == static_cast<AppId_t>(kSteamAppId), "the app id of the process is 480");
        Check(SteamUser() != nullptr && SteamUser()->BLoggedOn(), "the user is logged in to Steam");
        Check(SteamUser() != nullptr && SteamUser()->GetSteamID().IsValid(), "a valid Steam id");
        if (SteamFriends() != nullptr)
        {
            Info(std::string("persona name: ") + SteamFriends()->GetPersonaName());
        }

        // 2. the network of Valve (ISteamNetworkingMessages: the lazy start of the relay network)
        Check(SteamNetworkingMessages() != nullptr, "SteamNetworkingMessages() is there");
        Check(SteamNetworkingUtils() != nullptr, "SteamNetworkingUtils() is there");
        if (SteamNetworkingUtils() != nullptr)
        {
            SteamNetworkingUtils()->InitRelayNetworkAccess();
            SteamRelayNetworkStatus_t status{};
            Pump(*service, log, 20.0, [&] {
                SteamNetworkingUtils()->GetRelayNetworkStatus(&status);
                return status.m_eAvail == k_ESteamNetworkingAvailability_Current;
            });
            SteamNetworkingUtils()->GetRelayNetworkStatus(&status);
            Check(status.m_eAvail == k_ESteamNetworkingAvailability_Current, std::string("the relay network of Valve is Current (it is ") + AvailabilityName(status.m_eAvail) + ")");
            Info(std::string("relay network: any relay ") + AvailabilityName(status.m_eAvailAnyRelay) + ", network config " + AvailabilityName(status.m_eAvailNetworkConfig) +
                 ", ping measurement " + (status.m_bPingMeasurementInProgress ? "in progress" : "done") + ", " + status.m_debugMsg);
        }

        // 3. our own lobby for friends only
        LobbyCreateOptions options;
        options.hostName = "smoke-host";
        options.mode = 2;
        options.maxPlayers = 8;
        options.friendsOnly = true;
        std::string error;
        // a bad id is refused while we are in no lobby
        {
            const bool refused = !service->Join("not-a-number", error);
            Check(refused, "Join(\"not-a-number\") is refused: " + error);
        }
        error.clear();
        const bool created = service->Create(options, error);
        Check(created, "Create(friends only) accepted" + (error.empty() ? std::string() : ": " + error));
        Check(service->State() == LobbyState::Creating, std::string("state Creating right after Create (it is ") + StateName(service->State()) + ")");
        const bool hosting = Pump(*service, log, 20.0, [&] { return service->State() == LobbyState::Hosting || service->State() == LobbyState::Failed; });
        Check(hosting && service->State() == LobbyState::Hosting, std::string("the lobby is created (state ") + StateName(service->State()) + ")" + (service->LastError().empty() ? "" : ": " + service->LastError()));
        if (service->State() != LobbyState::Hosting)
        {
            return 1;
        }
        Check(Has(log, LobbyEventKind::Created), "event Created");
        const std::string lobbyIdText = service->Current().id;
        const CSteamID lobby(std::strtoull(lobbyIdText.c_str(), nullptr, 10));
        Check(lobby.IsValid() && lobby.IsLobby(), "the lobby id is a valid lobby id");
        Check(service->Members().size() == 1 && service->Members()[0].isHost && service->Members()[0].isLocal, "one member: us, the host");
        Check(service->Current().maxPlayers == 8 && service->Current().mode == 2 && service->Current().hostName == "smoke-host", "Current(): max 8, mode 2, host_name");

        // 4. the data of the lobby as Steam keeps it
        Check(Data(lobby, "rl_game") == "1", "lobby data rl_game=1");
        Check(Data(lobby, "rl_version") == std::to_string(kLobbyProtocolVersion), "lobby data rl_version");
        Check(Data(lobby, "mode") == "2" && Data(lobby, "max") == "8", "lobby data mode and max");
        Check(Data(lobby, "host_name") == "smoke-host", "lobby data host_name");
        Check(Data(lobby, "host_id") == std::to_string(SteamUser()->GetSteamID().ConvertToUint64()), "lobby data host_id is our Steam id");
        Check(Data(lobby, "state") == "open", "lobby data state=open");
        Check(SteamMatchmaking()->GetNumLobbyMembers(lobby) == 1, "Steam counts one member");
        Check(SteamMatchmaking()->GetLobbyOwner(lobby) == SteamUser()->GetSteamID(), "the owner of the lobby is us");

        // 5. the data of a member (ready) and the callback about it
        service->SetReady(true);
        const bool readySeen = Pump(*service, log, 5.0, [&] { return service->Members().size() == 1 && service->Members()[0].ready; });
        Check(readySeen, "SetReady(true) shows in Members() (LobbyDataUpdate_t about our own change)");
        {
            const char* ready = SteamMatchmaking()->GetLobbyMemberData(lobby, SteamUser()->GetSteamID(), "ready");
            Check(ready != nullptr && ready[0] == '1', "member data ready=1 as Steam keeps it");
        }

        // 6. the list with our filter: our own lobby has to be in it
        bool found = false;
        int attempts = 0;
        for (; attempts < 6 && !found; ++attempts)
        {
            log.clear();
            service->RefreshList();
            Pump(*service, log, 6.0, [&] { return Has(log, LobbyEventKind::ListUpdated); });
            for (const LobbyInfo& info : service->Lobbies())
            {
                if (info.id == lobbyIdText)
                {
                    found = true;
                    Info("list entry: host '" + info.hostName + "' mode " + std::to_string(info.mode) + " players " + std::to_string(info.players) + "/" + std::to_string(info.maxPlayers) +
                         " joinable " + (info.joinable ? "yes" : "no") + " " + info.note);
                }
            }
            if (!found)
            {
                Info("the list has " + std::to_string(service->Lobbies().size()) + " lobby(ies), ours is not among them (attempt " + std::to_string(attempts + 1) + ")");
                Pump(*service, log, 2.0, [] { return false; });
            }
        }
        Check(found, "our own friends-only lobby is in Lobbies() (found through the presence: RequestLobbyList does not return friends-only lobbies)");

        // 7. a bad id
        {
            const bool refused = !service->Join(lobbyIdText, error);
            Check(refused, "Join while in a lobby is refused: " + error);
        }

        // 8. the transport object over ISteamNetworkingMessages (a second account is needed for a real connection: docs/game/steam-setup.md)
        {
            std::unique_ptr<INetTransport> transport = MakeSteamTransport();
            Check(transport != nullptr, "MakeSteamTransport() gives a transport");
            std::string listenError;
            Check(transport != nullptr && transport->Listen(0, listenError), "SteamTransport::Listen" + (listenError.empty() ? std::string() : ": " + listenError));
            std::string badError;
            Check(transport != nullptr && transport->Connect("abc", badError) == kInvalidPeer, "Connect(\"abc\") is refused");
            std::vector<NetEvent> events;
            if (transport != nullptr)
            {
                transport->Poll(Now(), events);
                Check(events.empty(), "Poll on a transport with no peers gives no events");
                transport->Close();
            }
        }

        // 9. the host starts the match: lobby closed, "start" set
        error.clear();
        const bool started = service->StartMatch(error);
        Check(started, "StartMatch (alone in the lobby)" + (error.empty() ? std::string() : ": " + error));
        Check(service->State() == LobbyState::Starting, std::string("state Starting (it is ") + StateName(service->State()) + ")");
        Pump(*service, log, 1.0, [] { return false; });
        Check(Data(lobby, "state") == "match" && Data(lobby, "start") == "1", "lobby data state=match and start=1");
        Check(service->StartInfo().isHost && service->StartInfo().members.size() == 1, "StartInfo(): host, one member");
        std::unique_ptr<INetTransport> matchTransport = service->CreateMatchTransport(NetSimParams{});
        Check(matchTransport != nullptr, "CreateMatchTransport() gives the Steam transport");
        matchTransport.reset();

        // 10. out of the lobby
        service->Leave();
        Check(service->State() == LobbyState::Idle && service->Members().empty(), "Leave(): state Idle, no members");
        Pump(*service, log, 3.0, [] { return false; });
        Info("Steam counts " + std::to_string(SteamMatchmaking()->GetNumLobbyMembers(lobby)) + " member(s) in the lobby we left (the cache of the client may be stale)");
        found = false;
        log.clear();
        service->RefreshList();
        Pump(*service, log, 6.0, [&] { return Has(log, LobbyEventKind::ListUpdated); });
        for (const LobbyInfo& info : service->Lobbies())
        {
            found = found || info.id == lobbyIdText;
        }
        Check(!found, "the left lobby is no longer in the list");

        // 11. the last service shuts Steam down; a new one starts it again
        service.reset();
        service = MakeSteamLobbyService("smoke-2");
        Check(service->Available(why), "after the last service was gone SteamAPI_Init works again" + (why.empty() ? std::string() : ": " + why));
        service.reset();

        Check(FindConnectLobbyInCommandLine("game.exe +connect_lobby 109775241234567890 -x") == "109775241234567890", "+connect_lobby is parsed");
        return g_failed == 0 ? 0 : 1;
    }
}

int main()
{
    const int code = Run();
    if (code == 77)
    {
        return code;
    }
    std::printf("%s (%d failed)\n", code == 0 ? "PASSED" : "FAILED", g_failed);
    return code;
}
