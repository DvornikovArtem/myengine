// RlLobbyScreens.cpp
// See RlLobbyScreens.h. The state of the lobby lives in the services (rl::net::ILobbyService); this file holds the choices of the player (LAN or Steam,
// what is selected), turns the events of the services into what the screens do, and draws.

#include "RlLobbyScreens.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rl/net/LobbySteam.h>

#include <myengine/ui/rl/RlUiHost.h>
#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::game
{
    using namespace rlui;
    using namespace rlui::paint;
    namespace sys = ecs::systems;
    namespace lobbynet = ::rl::net;

    namespace
    {
        std::string Format(const char* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        bool CopyToClipboard(const std::string& text)
        {
            if (!OpenClipboard(nullptr))
            {
                return false;
            }
            EmptyClipboard();
            const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            bool ok = false;
            if (memory != nullptr)
            {
                void* data = GlobalLock(memory);
                if (data != nullptr)
                {
                    std::memcpy(data, text.c_str(), text.size() + 1);
                    GlobalUnlock(memory);
                    ok = SetClipboardData(CF_TEXT, memory) != nullptr;
                }
                if (!ok)
                {
                    GlobalFree(memory);
                }
            }
            CloseClipboard();
            return ok;
        }

        // The words of `text` in lines of at most `width` design pixels
        std::vector<std::string> WrapLines(Frame& f, const std::string& text, const float size, const float width)
        {
            std::vector<std::string> lines;
            std::string line;
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find(' ', at);
                if (end == std::string::npos)
                {
                    end = text.size();
                }
                const std::string word = text.substr(at, end - at);
                const std::string joined = line.empty() ? word : line + " " + word;
                if (!line.empty() && TextWidth(f, FontStyle::SemiBold, size, joined) > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                {
                    line = joined;
                }
                at = end + 1;
            }
            if (!line.empty())
            {
                lines.push_back(line);
            }
            return lines;
        }

        constexpr int kNetLan = 0;
        constexpr int kNetSteam = 1;
        constexpr float kSteamRefreshSeconds = 6.0f;
    }

    struct RlLobbyScreens::Impl
    {
        std::function<void(const std::string&)> log;
        std::shared_ptr<lobbynet::ILobbyService> lan;
        std::shared_ptr<lobbynet::ILobbyService> steam;
        std::string lanName;
        int kind = kNetLan;

        float refreshTimer = 0.0f;
        double clock = 0.0;
        bool searchStarted = false;
        bool openRoom = false;
        bool leaveRoom = false;
        bool matchReady = false;
        sys::RlGameMatchSetup pending;
        int pendingHumans = 0;
        bool dialogShown = false;
        bool readyFocused = false;  // the room: the focus was put on the button of the guest
        bool startFocused = false;  // the room: the host could start the last time

        std::string errorTitle;
        std::string errorText;
        std::string startError;  // why the host cannot start yet (shown in the room)

        // the choices of the host, for the match that the lobby starts
        int hostBotLevel = 1;
        bool hostFillBots = true;

        explicit Impl(std::function<void(const std::string&)> logFunction) : log(std::move(logFunction)) {}

        void Log(const std::string& text)
        {
            if (log)
            {
                log("lobby: " + text);
            }
        }

        std::shared_ptr<lobbynet::ILobbyService> EnsureSteam()
        {
            if (!steam)
            {
                steam = lobbynet::MakeSteamLobbyService("");
                std::string why;
                const bool available = steam->Available(why);
                Log(available ? "Steam is available" : "Steam is not available: " + why);
            }
            return steam;
        }

        std::shared_ptr<lobbynet::ILobbyService> EnsureLan(const std::string& name)
        {
            if (!lan)
            {
                lan = lobbynet::MakeLanLobbyService(name);
                lanName = name;
                searchStarted = false;
            }
            else if (lanName != name)
            {
                lan->SetLocalName(name);
                lanName = name;
            }
            return lan;
        }

        std::shared_ptr<lobbynet::ILobbyService> Active(const std::string& name)
        {
            return kind == kNetSteam ? EnsureSteam() : EnsureLan(name);
        }

        std::shared_ptr<lobbynet::ILobbyService> ActiveIfAny() const { return kind == kNetSteam ? steam : lan; }

        static bool InLobby(const lobbynet::ILobbyService& service)
        {
            const lobbynet::LobbyState state = service.State();
            return state != lobbynet::LobbyState::Idle && state != lobbynet::LobbyState::Failed;
        }

        // ---- the events of the services

        void Handle(const std::shared_ptr<lobbynet::ILobbyService>& service, const lobbynet::LobbyEvent& event)
        {
            using Kind = lobbynet::LobbyEventKind;
            switch (event.kind)
            {
            case Kind::Error:
                Log(std::string(service->Name()) + " error: " + event.text);
                if (service->State() == lobbynet::LobbyState::Failed)
                {
                    errorTitle = "LOBBY ERROR";
                    errorText = event.text;
                    leaveRoom = true;
                }
                break;
            case Kind::HostLeft:
                Log("the host left the lobby");
                errorTitle = "LOBBY CLOSED";
                errorText = "The host left the lobby.";
                service->Leave();
                leaveRoom = true;
                break;
            case Kind::InviteReceived:
                if (!InLobby(*service) && !matchReady)
                {
                    Log("invitation to the lobby " + event.text);
                    kind = service == steam ? kNetSteam : kNetLan;
                    std::string error;
                    if (service->Join(event.text, error))
                    {
                        openRoom = true;
                    }
                    else
                    {
                        errorTitle = "CANNOT JOIN";
                        errorText = error;
                    }
                }
                break;
            case Kind::MatchStarting:
                BuildMatch(service);
                break;
            default: break;
            }
        }

        void BuildMatch(const std::shared_ptr<lobbynet::ILobbyService>& service)
        {
            const lobbynet::MatchStartInfo& info = service->StartInfo();
            sys::RlGameMatchSetup setup;
            setup.kind = info.isHost ? sys::RlGameMatchSetup::Kind::Host : sys::RlGameMatchSetup::Kind::Join;
            setup.mode = std::clamp(info.mode, 1, 4);
            setup.botLevel = hostBotLevel;
            setup.fillWithBots = hostFillBots;
            setup.port = info.gamePort;
            setup.address = info.hostAddress;
            setup.transportFactory = [service](const lobbynet::NetSimParams& sim) { return service->CreateMatchTransport(sim); };
            pending = setup;
            pendingHumans = static_cast<int>(info.members.size());
            matchReady = true;
            Log(Format("the match starts (%s, %d players in the lobby)", info.isHost ? "host" : "guest", pendingHumans));
        }

        void Pump(const std::shared_ptr<lobbynet::ILobbyService>& service)
        {
            if (!service)
            {
                return;
            }
            service->Update(clock);
            for (const lobbynet::LobbyEvent& event : service->TakeEvents())
            {
                Handle(service, event);
            }
        }

        void ShowError(Frame& f)
        {
            if (errorText.empty())
            {
                return;
            }
            dialogShown = true;
            const DialogResult result = ConfirmDialog(f, HashId("lobby/error"), {errorTitle, errorText, "OK", "Close"});
            if (result != DialogResult::None)
            {
                errorTitle.clear();
                errorText.clear();
            }
        }

        // The state of the network row: what works and why not
        void NetworkStatus(Frame& f, const std::shared_ptr<lobbynet::ILobbyService>& service, const Vec2 pos)
        {
            std::string why;
            const bool ok = service->Available(why);
            if (ok)
            {
                Text(f, FontStyle::SemiBold, 22.0f, pos, style::kGood,
                     kind == kNetSteam ? "Steam is ready. Lobbies are for friends only." : "The lobbies of this network show up in the list.");
                return;
            }
            float y = pos.y - 8.0f;
            for (const std::string& line : WrapLines(f, why, 20.0f, 760.0f))
            {
                Text(f, FontStyle::SemiBold, 20.0f, {pos.x, y}, style::kBad, line);
                y += 24.0f;
            }
        }
    };

    RlLobbyScreens::RlLobbyScreens(std::function<void(const std::string&)> log) : impl_(std::make_unique<Impl>(std::move(log))) {}
    RlLobbyScreens::~RlLobbyScreens() = default;

    void RlLobbyScreens::Install(const std::string& commandLine)
    {
        Impl& d = *impl_;
#ifdef MYENGINE_STEAM
        // the invitations of the friends come to a running game: Steam is started with it (SteamAPI_Init)
        d.EnsureSteam();
#endif
        const std::string lobby = lobbynet::FindConnectLobbyInCommandLine(commandLine);
        if (!lobby.empty())
        {
            std::string error;
            d.kind = kNetSteam;
            const std::shared_ptr<lobbynet::ILobbyService> steam = d.EnsureSteam();
            if (steam->Join(lobby, error))
            {
                d.Log("+connect_lobby " + lobby);
                d.openRoom = true;
            }
            else
            {
                d.errorTitle = "CANNOT JOIN";
                d.errorText = error;
            }
        }
    }

    void RlLobbyScreens::Update(const float deltaSeconds)
    {
        Impl& d = *impl_;
        d.clock += static_cast<double>(deltaSeconds);
        d.dialogShown = false;
        d.Pump(d.steam);
        d.Pump(d.lan);
        d.refreshTimer -= deltaSeconds;
    }

    bool RlLobbyScreens::TakeOpenRoom()
    {
        const bool open = impl_->openRoom;
        impl_->openRoom = false;
        if (open)
        {
            impl_->readyFocused = false;
            impl_->startFocused = false;
        }
        return open;
    }

    bool RlLobbyScreens::TakeStartedMatch(sys::RlGameMatchSetup& setup, int& humans)
    {
        Impl& d = *impl_;
        if (!d.matchReady)
        {
            return false;
        }
        d.matchReady = false;
        setup = d.pending;
        humans = d.pendingHumans;
        d.pending = sys::RlGameMatchSetup{};
        return true;
    }

    void RlLobbyScreens::ReleaseMatch()
    {
        Impl& d = *impl_;
        for (const std::shared_ptr<lobbynet::ILobbyService>& service : {d.steam, d.lan})
        {
            if (service && service->State() == lobbynet::LobbyState::Starting)
            {
                service->Leave();
                d.Log(std::string(service->Name()) + ": the lobby of the match is closed");
            }
        }
        d.matchReady = false;
        d.pending = sys::RlGameMatchSetup{};
    }

    void RlLobbyScreens::CloseIdle()
    {
        Impl& d = *impl_;
        if (d.lan && !Impl::InLobby(*d.lan))
        {
            d.lan.reset();
            d.searchStarted = false;
        }
    }

    bool RlLobbyScreens::ErrorShown() const { return impl_->dialogShown; }

    bool RlLobbyScreens::TakeError(std::string& title, std::string& text)
    {
        Impl& d = *impl_;
        if (d.errorText.empty())
        {
            return false;
        }
        title = d.errorTitle;
        text = d.errorText;
        d.errorTitle.clear();
        d.errorText.clear();
        return true;
    }

    // ---- the Private match tab ------------------------------------------------------------------------------------------------------------

    RlLobbyScreens::Action RlLobbyScreens::DrawPrivate(Frame& f, RlGameSettings& settings, const std::string& playerName)
    {
        Impl& d = *impl_;
        Action action = Action::None;
        d.hostBotLevel = settings.play.botLevel;
        d.hostFillBots = settings.play.hostFillBots;
        if (d.openRoom)
        {
            return Action::OpenRoom;
        }

        const std::shared_ptr<lobbynet::ILobbyService> service = d.Active(playerName);
        std::string why;
        const bool available = service->Available(why);
        const bool free = !Impl::InLobby(*service);
        const bool dialog = !d.errorText.empty();

        // the network
        const std::string_view networks[] = {"LAN", "Steam"};
        const int newKind = OptionRow(f, HashId("private/net"), {142.0f, 290.0f, 618.0f, 52.0f}, "Network", Span<std::string_view>(networks), d.kind, !dialog);
        if (newKind != d.kind)
        {
            d.kind = newKind;
            d.refreshTimer = 0.0f;
            d.searchStarted = false;
            d.Log(newKind == kNetSteam ? "network: Steam" : "network: LAN");
            return Action::None;
        }
        d.NetworkStatus(f, service, {800.0f, 302.0f});

        // the search: the LAN one listens by itself after the first call, Steam asks again every few seconds
        const bool lan = d.kind == kNetLan;
        if (available && free && (!d.searchStarted || (!lan && d.refreshTimer <= 0.0f)))
        {
            service->RefreshList();
            d.searchStarted = true;
            d.refreshTimer = kSteamRefreshSeconds;
        }

        // ---- host
        Panel(f, {45.0f, 350.0f, 810.0f, 470.0f}, true);
        IconGlyph(f, Icon::Server, 40.0f, {160.0f, 416.0f}, style::kText);
        Text(f, FontStyle::BlackItalic, 52.0f, {196.0f, 382.0f}, style::kText, "HOST");
        Text(f, FontStyle::SemiBold, 24.0f, {142.0f, 448.0f}, style::kTextDim, lan ? "Friends on your network find the lobby." : "Friends on Steam get the lobby in their list.");
        const std::string_view sizes[] = {"1v1", "2v2", "3v3", "4v4"};
        f.SetDefaultFocus(HashId("host/size"));
        settings.play.hostTeamSize =
            OptionRow(f, HashId("host/size"), {142.0f, 502.0f, 618.0f, 58.0f}, "Team size", Span<std::string_view>(sizes), settings.play.hostTeamSize - 1, !dialog) + 1;
        settings.play.hostFillBots = ToggleRow(f, HashId("host/fill"), {142.0f, 566.0f, 618.0f, 58.0f}, "Fill with bots", settings.play.hostFillBots, !dialog, "Off", "On");
        if (lan)
        {
            std::string portText = std::to_string(settings.play.hostPort);
            if (TextRow(f, HashId("host/port"), {142.0f, 630.0f, 618.0f, 58.0f}, "Match port", portText, 5, "0123456789"))
            {
                const int port = portText.empty() ? 0 : std::atoi(portText.c_str());
                if (port >= 1 && port <= 65535)
                {
                    settings.play.hostPort = port;
                }
            }
        }
        else
        {
            ValueRow(f, HashId("host/privacy"), {142.0f, 630.0f, 618.0f, 58.0f}, "Visible to", "Friends only", !dialog, false);
        }
        if (ActionButton(f, HashId("host/go"), {142.0f, 718.0f, 440.0f, 76.0f}, "CREATE LOBBY", Icon::Play, available && free && !dialog, true))
        {
            lobbynet::LobbyCreateOptions options;
            options.hostName = playerName;
            options.mode = settings.play.hostTeamSize;
            options.maxPlayers = std::min(settings.play.hostTeamSize * 2, 8);
            options.gamePort = static_cast<std::uint16_t>(settings.play.hostPort);
            options.friendsOnly = true;
            std::string error;
            if (service->Create(options, error))
            {
                d.Log(Format("lobby created (%s, %dv%d)", service->Name(), options.mode, options.mode));
                d.startError.clear();
                action = Action::OpenRoom;
            }
            else
            {
                d.errorTitle = "CANNOT CREATE THE LOBBY";
                d.errorText = error;
            }
        }

        // ---- join
        Panel(f, {782.0f, 350.0f, 810.0f, 470.0f}, false);
        IconGlyph(f, Icon::Join, 40.0f, {904.0f, 416.0f}, style::kText);
        Text(f, FontStyle::BlackItalic, 52.0f, {940.0f, 382.0f}, style::kText, "JOIN");
        Text(f, FontStyle::SemiBold, 24.0f, {886.0f, 448.0f}, style::kTextDim, lan ? "Lobbies on this network" : "Lobbies of your friends");

        const std::vector<lobbynet::LobbyInfo>& lobbies = service->Lobbies();
        const Rect view{886.0f, 480.0f, 618.0f, 168.0f};
        if (lobbies.empty())
        {
            Text(f, FontStyle::SemiBold, 22.0f, {view.x + 4.0f, view.y + 14.0f}, style::kTextFaint,
                 !available ? "Not available." : (lan ? "No lobby found yet. Type the address of the host below." : "None of your friends has a lobby open."));
        }
        {
            ScrollView scroll(f, HashId("join/list"), view, static_cast<float>(lobbies.size()) * 56.0f);
            for (size_t i = 0; i < lobbies.size(); ++i)
            {
                const lobbynet::LobbyInfo& info = lobbies[i];
                std::string detail = Format("%dv%d \xC2\xB7 %d/%d", info.mode, info.mode, info.players, info.maxPlayers);
                if (info.pingMs >= 0)
                {
                    detail += Format(" \xC2\xB7 %d ms", info.pingMs);
                }
                if (!info.note.empty())
                {
                    detail += " \xC2\xB7 " + info.note;
                }
                const Rect row{view.x, view.y + static_cast<float>(i) * 56.0f - scroll.Offset(), view.w, 52.0f};
                const ItemState state = ValueRow(f, HashId("join/lobby/" + info.id), row, info.hostName.empty() ? "Lobby" : info.hostName, detail,
                                                 info.joinable && free && !dialog, false);
                if (state.activated && info.joinable)
                {
                    const std::string id = info.id;  // Join changes the list that `info` points into
                    std::string error;
                    if (service->Join(id, error))
                    {
                        d.Log("joining " + id);
                        action = Action::OpenRoom;
                    }
                    else
                    {
                        d.errorTitle = "CANNOT JOIN";
                        d.errorText = error;
                    }
                    break;
                }
            }
        }

        if (lan)
        {
            TextRow(f, HashId("join/address"), {886.0f, 662.0f, 618.0f, 52.0f}, "Address", settings.play.joinAddress, 21, "0123456789.:");
        }
        else
        {
            ValueRow(f, HashId("join/friends"), {886.0f, 662.0f, 618.0f, 52.0f}, "Invitations", "From the Steam overlay", !dialog, false);
        }
        const bool addressOk = lan && !settings.play.joinAddress.empty();
        if (ActionButton(f, HashId("join/go"), {886.0f, 734.0f, 300.0f, 62.0f}, "JOIN ADDRESS", Icon::Join, addressOk && available && free && !dialog, true))
        {
            std::string error;
            if (service->Join(settings.play.joinAddress, error))
            {
                d.Log("joining " + settings.play.joinAddress);
                action = Action::OpenRoom;
            }
            else
            {
                d.errorTitle = "CANNOT JOIN";
                d.errorText = error;
            }
        }
        if (ActionButton(f, HashId("join/refresh"), {1204.0f, 734.0f, 300.0f, 62.0f}, "REFRESH", Icon::Refresh, available && free && !dialog))
        {
            service->RefreshList();
            d.refreshTimer = kSteamRefreshSeconds;
        }

        d.ShowError(f);
        if (action == Action::OpenRoom)
        {
            d.readyFocused = false;
            d.startFocused = false;
        }
        return action;
    }

    // ---- the room -------------------------------------------------------------------------------------------------------------------------

    RlLobbyScreens::Action RlLobbyScreens::DrawRoom(Frame& f, const std::string& playerName)
    {
        Impl& d = *impl_;
        if (d.matchReady)
        {
            return Action::StartMatch;
        }
        const std::shared_ptr<lobbynet::ILobbyService> service = d.ActiveIfAny() ? d.ActiveIfAny() : d.Active(playerName);
        if (d.leaveRoom || !Impl::InLobby(*service))
        {
            d.leaveRoom = false;
            return Action::LeaveRoom;
        }
        const lobbynet::LobbyState state = service->State();
        const bool hosting = state == lobbynet::LobbyState::Hosting || (state == lobbynet::LobbyState::Creating) || (state == lobbynet::LobbyState::Starting && service->StartInfo().isHost);
        const bool connecting = state == lobbynet::LobbyState::Creating || state == lobbynet::LobbyState::Joining;
        const bool starting = state == lobbynet::LobbyState::Starting;
        const lobbynet::LobbyInfo& info = service->Current();
        const std::vector<lobbynet::LobbyMember>& members = service->Members();
        const bool lan = d.kind == kNetLan;

        f.SetSpace(Space::Screen);
        Dim(f, Color::Hex(0x050b1c, 0.93f));
        f.SetSpace(Space::Menu);
        ScreenTitle(f, lan ? "PRIVATE MATCH \xC2\xB7 LAN" : "PRIVATE MATCH \xC2\xB7 STEAM", hosting ? "YOUR LOBBY" : "LOBBY");
        std::string subtitle;
        if (connecting)
        {
            subtitle = hosting ? "Creating the lobby ..." : "Joining the lobby ...";
        }
        else if (starting)
        {
            subtitle = "Starting the match ...";
        }
        else
        {
            subtitle = Format("%dv%d \xC2\xB7 %d of %d players \xC2\xB7 host %s", info.mode, info.mode, static_cast<int>(members.size()), info.maxPlayers, info.hostName.c_str());
        }
        Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 238.0f}, style::kTextDim, subtitle);

        // the members
        const float rows = static_cast<float>(std::max<size_t>(members.size(), 1));
        Panel(f, {80.0f, 300.0f, 1080.0f, 76.0f + 62.0f * rows + 14.0f}, false, true);
        ColumnCaption(f, {120.0f, 322.0f}, "PLAYER");
        ColumnCaption(f, {1110.0f, 322.0f}, "STATUS", Align::Right);
        float y = 360.0f;
        bool allReady = true;
        std::string waitingFor;
        bool localReady = false;
        for (const lobbynet::LobbyMember& member : members)
        {
            const Rect row{100.0f, y, 1040.0f, 54.0f};
            Parallelogram(f, row, member.isLocal ? style::kNavy3 : style::kNavy2);
            IconGlyph(f, Icon::Person, 24.0f, {row.x + 34.0f, row.y + 27.0f}, style::kText);
            TextIn(f, FontStyle::Bold, 24.0f, {row.x + 70.0f, row.y, 560.0f, row.h}, style::kText, member.name + (member.isLocal ? "  (you)" : ""));
            if (member.isHost)
            {
                TextIn(f, FontStyle::Bold, 16.0f, {row.x + row.w - 330.0f, row.y, 120.0f, row.h}, style::kAccent, "HOST", Align::Right);
            }
            const bool ready = member.isHost || member.ready;
            TextIn(f, FontStyle::Bold, 20.0f, {row.x + row.w - 190.0f, row.y, 170.0f, row.h}, ready ? style::kGood : style::kTextFaint, member.isHost ? "HOST" : (ready ? "READY" : "NOT READY"),
                   Align::Right);
            if (!member.isHost && !member.ready)
            {
                allReady = false;
                if (waitingFor.empty())
                {
                    waitingFor = member.name;
                }
            }
            if (member.isLocal)
            {
                localReady = member.ready;
            }
            y += 62.0f;
        }
        if (members.empty())
        {
            Text(f, FontStyle::SemiBold, 22.0f, {120.0f, y + 14.0f}, style::kTextFaint, "...");
        }

        // the invitation (the host) or what the guest waits for
        Panel(f, {1210.0f, 300.0f, 630.0f, 320.0f}, true);
        IconGlyph(f, Icon::Info, 28.0f, {1260.0f, 346.0f}, style::kText);
        Text(f, FontStyle::BlackItalic, 36.0f, {1290.0f, 326.0f}, style::kText, hosting ? "INVITE" : "WAITING");
        if (!hosting)
        {
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 394.0f}, style::kTextDim, "Press I AM READY when you are.");
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 422.0f}, style::kTextDim, "The host starts the match when everybody is ready.");
        }
        else if (lan)
        {
            const std::string address = service->InviteText();
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 390.0f}, style::kTextDim, "Friends on this network see the lobby in the list.");
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 418.0f}, style::kTextDim, "Or they type this address:");
            Text(f, FontStyle::BlackItalic, 44.0f, {1250.0f, 450.0f}, style::kAccent, address.empty() ? std::string("...") : address);
            if (ActionButton(f, HashId("room/invite"), {1250.0f, 506.0f, 360.0f, 62.0f}, "COPY ADDRESS", Icon::Join, hosting && !address.empty()))
            {
                f.ShowToast(CopyToClipboard(address) ? "Address copied" : "Cannot copy", style::kMvp);
            }
        }
        else
        {
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 394.0f}, style::kTextDim, "Friends who play the game see this lobby");
            Text(f, FontStyle::SemiBold, 22.0f, {1250.0f, 422.0f}, style::kTextDim, "in their list. Or invite them from Steam.");
            if (ActionButton(f, HashId("room/invite"), {1250.0f, 506.0f, 360.0f, 62.0f}, "INVITE FRIENDS", Icon::Join, hosting && !connecting))
            {
                if (!service->InviteFriends())
                {
                    f.ShowToast("The Steam overlay is not available", style::kBad);
                }
            }
        }

        // the buttons
        f.SetDefaultFocus(HashId("room/leave"));
        const bool dialog = !d.errorText.empty();
        if (hosting)
        {
            const bool canStart = !connecting && !starting && allReady;
            if (canStart != d.startFocused)
            {
                d.startFocused = canStart;
                if (canStart && members.size() > 1)
                {
                    f.SetFocus(HashId("room/start"));  // the last guest got ready: the focus comes to Start
                }
            }
            f.SetDefaultFocus(HashId("room/leave"));
            if (!canStart && !connecting && !starting && !waitingFor.empty())
            {
                Text(f, FontStyle::SemiBold, 22.0f, {1560.0f, 868.0f}, style::kTextDim, waitingFor + " is not ready", Align::Right);
            }
            if (ActionButton(f, HashId("room/start"), {1200.0f, 900.0f, 360.0f, 76.0f}, "START MATCH", Icon::Play, canStart && !dialog, true))
            {
                std::string error;
                if (!service->StartMatch(error))
                {
                    d.errorTitle = "CANNOT START";
                    d.errorText = error;
                }
            }
        }
        else if (!connecting && !starting)
        {
            if (!d.readyFocused)
            {
                d.readyFocused = true;
                f.SetFocus(HashId("room/ready"));  // Enter must not leave the lobby by accident: the focus goes to the button of the guest
            }
            if (ActionButton(f, HashId("room/ready"), {1100.0f, 900.0f, 460.0f, 76.0f}, localReady ? "NOT READY" : "I AM READY", Icon::Play, !dialog, !localReady))
            {
                service->SetReady(!localReady);
            }
        }
        else
        {
            Text(f, FontStyle::SemiBold, 24.0f, {1560.0f, 925.0f}, style::kTextDim, starting ? "Starting ..." : "Connecting ...", Align::Right);
        }
        Action action = Action::None;
        if (ActionButton(f, HashId("room/leave"), {1580.0f, 900.0f, 260.0f, 76.0f}, "LEAVE", Icon::Exit, !dialog) || (f.input().Pressed(Nav::Back) && !dialog && !d.dialogShown))
        {
            service->Leave();
            d.Log("left the lobby");
            action = Action::LeaveRoom;
        }
        std::vector<Hint> hints = {{Glyph::AllArrows, Glyph::PadA, "Select"}, {Glyph::Esc, Glyph::PadB, "Leave"}};
        HintBar(f, Span<Hint>(hints));
        d.ShowError(f);
        return action;
    }
}
