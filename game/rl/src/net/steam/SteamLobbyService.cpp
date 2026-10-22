// SteamLobbyService.cpp
// The Steam lobby (docs/game/rl-online.md 7.4) written from the Steamworks documentation: ISteamMatchmaking (CreateLobby, JoinLobby, RequestLobbyList,
// lobby data and member data), ISteamFriends (the invite dialog of the overlay), the callbacks GameLobbyJoinRequested_t, LobbyChatUpdate_t,
// LobbyDataUpdate_t. Built and run against the real SDK 1.65 (G6b: tests/rl/RlSteamSmoke.cpp, one account); the play of two accounts is
// described in docs/game/steam-setup.md.
//
// The lobby is "friends only", and Steam does not put such a lobby into the result of RequestLobbyList (only a public one gets there). Friends find
// it through the presence: ISteamFriends::GetFriendGamePlayed gives the lobby a friend is in (FriendGameInfo_t::m_steamIDLobby); RefreshList reads
// the data of those lobbies (RequestLobbyData) and keeps the ones with rl_game=1.
//
// What is in the lobby data (set by the owner): rl_game=1 (our label, the list filters by it), rl_version, mode, max, host_name, host_id (the Steam id
// of the host of the match, fixed when the lobby is made: the owner of a Steam lobby can change when he leaves), state (open | match), start (a counter
// that appears when the host starts the match), game_port. In the data of a member: ready=0|1.

#include <rl/RlTypes.h>
#include <rl/net/LobbySteam.h>

#include "SteamTransport.h"

#include <steam/steam_api.h>

#include <algorithm>
#include <cstdlib>

namespace rl::net
{
    namespace
    {
        int g_steamUsers = 0;

        // SteamAPI_Init must succeed before any other call; it fails when no Steam client is running, when the AppID is not known
        // (steam_appid.txt next to the exe holds 480) or the OS user differs from that of the client
        bool AcquireSteam(std::string& why)
        {
            if (g_steamUsers == 0 && !SteamAPI_Init())
            {
                why = "Steam is not available: SteamAPI_Init failed. Start the Steam client, log in, and put steam_appid.txt (the line 480) next to the exe";
                return false;
            }
            ++g_steamUsers;
            return true;
        }

        void ReleaseSteam()
        {
            if (g_steamUsers > 0 && --g_steamUsers == 0)
            {
                SteamAPI_Shutdown();
            }
        }

        std::string IdToString(const CSteamID& id) { return std::to_string(id.ConvertToUint64()); }

        std::string DataOf(ISteamMatchmaking* matchmaking, const CSteamID& lobby, const char* key)
        {
            const char* value = matchmaking->GetLobbyData(lobby, key);
            return value != nullptr ? value : "";
        }

        int IntOf(const std::string& text, int fallback)
        {
            return text.empty() ? fallback : std::atoi(text.c_str());
        }

        class SteamLobbyService final : public ILobbyService
        {
        public:
            explicit SteamLobbyService(const std::string& name)
                : name_(name)
            {
                ready_ = AcquireSteam(initError_);
                if (ready_)
                {
                    callbacks_ = std::make_unique<Callbacks>(this);
                    if (name_.empty() && SteamFriends() != nullptr)
                    {
                        name_ = SteamFriends()->GetPersonaName();
                    }
                }
            }

            ~SteamLobbyService() override
            {
                Leave();
                callbacks_.reset();
                if (ready_)
                {
                    ReleaseSteam();
                }
            }

            const char* Name() const override { return "Steam"; }

            bool Available(std::string& whyNot) const override
            {
                whyNot = ready_ ? "" : initError_;
                return ready_;
            }

            void Update(double) override
            {
                if (ready_)
                {
                    SteamAPI_RunCallbacks();  // once a frame, on the main thread
                }
            }

            std::vector<LobbyEvent> TakeEvents() override
            {
                std::vector<LobbyEvent> result;
                result.swap(events_);
                return result;
            }

            // ---- hosting ------------------------------------------------------------------------------------------------------------

            bool Create(const LobbyCreateOptions& options, std::string& error) override
            {
                if (!ready_)
                {
                    error = initError_;
                    return false;
                }
                if (state_ != LobbyState::Idle && state_ != LobbyState::Failed)
                {
                    error = "already in a lobby: leave it first";
                    return false;
                }
                create_ = options;
                create_.mode = std::clamp(create_.mode, 1, 4);
                create_.maxPlayers = std::clamp(create_.maxPlayers, 2, kMaxCars);
                if (!options.hostName.empty())
                {
                    name_ = options.hostName;
                }
                // a lobby of friends only (never public: AppID 480 is shared with everybody who tests with Spacewar)
                const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(options.friendsOnly ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic, create_.maxPlayers);
                callbacks_->created.Set(call, this, &SteamLobbyService::OnLobbyCreated);
                state_ = LobbyState::Creating;
                lastError_.clear();
                return true;
            }

            std::string InviteText() const override { return ""; }

            bool InviteFriends() override
            {
                if (!ready_ || !lobby_.IsValid() || SteamFriends() == nullptr)
                {
                    return false;
                }
                SteamFriends()->ActivateGameOverlayInviteDialog(lobby_);  // the overlay must work in the window of the game: checked in G6 by hand
                return true;
            }

            bool StartMatch(std::string& error) override
            {
                if (state_ != LobbyState::Hosting)
                {
                    error = "only the host of a lobby can start the match";
                    return false;
                }
                for (const LobbyMember& member : members_)
                {
                    if (!member.isHost && !member.ready)
                    {
                        error = member.name + " is not ready";
                        return false;
                    }
                }
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                matchmaking->SetLobbyData(lobby_, "state", "match");
                matchmaking->SetLobbyData(lobby_, "game_port", std::to_string(create_.gamePort).c_str());
                matchmaking->SetLobbyData(lobby_, "start", "1");
                matchmaking->SetLobbyJoinable(lobby_, false);
                state_ = LobbyState::Starting;
                startInfo_ = MatchStartInfo{};
                startInfo_.isHost = true;
                startInfo_.gamePort = create_.gamePort;
                startInfo_.mode = create_.mode;
                startInfo_.members = members_;
                PushEvent(LobbyEventKind::MatchStarting, LobbyMember{}, "");
                return true;
            }

            // ---- joining ------------------------------------------------------------------------------------------------------------

            void RefreshList() override
            {
                if (!ready_)
                {
                    return;
                }
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                matchmaking->AddRequestLobbyListStringFilter("rl_game", "1", k_ELobbyComparisonEqual);
                matchmaking->AddRequestLobbyListResultCountFilter(50);
                const SteamAPICall_t call = matchmaking->RequestLobbyList();
                callbacks_->matchList.Set(call, this, &SteamLobbyService::OnLobbyList);
                ScanFriendLobbies();
            }

            const std::vector<LobbyInfo>& Lobbies() const override { return lobbies_; }

            bool Join(const std::string& idOrAddress, std::string& error) override
            {
                if (!ready_)
                {
                    error = initError_;
                    return false;
                }
                if (state_ != LobbyState::Idle && state_ != LobbyState::Failed)
                {
                    error = "already in a lobby: leave it first";
                    return false;
                }
                char* end = nullptr;
                const uint64_t id = std::strtoull(idOrAddress.c_str(), &end, 10);
                if (id == 0 || end == idOrAddress.c_str())
                {
                    error = "'" + idOrAddress + "' is not a lobby id of Steam (take one from the list or from an invitation)";
                    return false;
                }
                const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(id));
                callbacks_->entered.Set(call, this, &SteamLobbyService::OnLobbyEntered);
                state_ = LobbyState::Joining;
                lastError_.clear();
                return true;
            }

            void SetReady(bool ready) override
            {
                if (lobby_.IsValid())
                {
                    SteamMatchmaking()->SetLobbyMemberData(lobby_, "ready", ready ? "1" : "0");
                }
            }

            void SetLocalName(const std::string& name) override { name_ = name; }  // the Steam name is shown; this one only for our own messages

            void Leave() override
            {
                if (ready_ && lobby_.IsValid())
                {
                    SteamMatchmaking()->LeaveLobby(lobby_);
                }
                lobby_ = CSteamID();
                hostId_ = 0;
                members_.clear();
                lobbies_.clear();
                publicLobbies_.clear();
                friendLobbies_.clear();
                state_ = LobbyState::Idle;
            }

            LobbyState State() const override { return state_; }
            const std::string& LastError() const override { return lastError_; }
            const std::vector<LobbyMember>& Members() const override { return members_; }
            const LobbyInfo& Current() const override { return current_; }
            const MatchStartInfo& StartInfo() const override { return startInfo_; }

            // `sim` (the lag of --net-sim) is not applied to the packets of Steam
            std::unique_ptr<INetTransport> CreateMatchTransport(const NetSimParams&) override
            {
                std::vector<uint64_t> allowed;
                if (startInfo_.isHost)
                {
                    for (const LobbyMember& member : members_)
                    {
                        allowed.push_back(std::strtoull(member.id.c_str(), nullptr, 10));
                    }
                }
                return MakeSteamTransportFor(allowed);
            }

        private:
            // the callbacks are made after SteamAPI_Init succeeded (they register with the client)
            struct Callbacks
            {
                explicit Callbacks(SteamLobbyService* owner)
                    : chatUpdate(owner, &SteamLobbyService::OnChatUpdate)
                    , dataUpdate(owner, &SteamLobbyService::OnDataUpdate)
                    , joinRequested(owner, &SteamLobbyService::OnJoinRequested)
                {
                }
                CCallback<SteamLobbyService, LobbyChatUpdate_t> chatUpdate;
                CCallback<SteamLobbyService, LobbyDataUpdate_t> dataUpdate;
                CCallback<SteamLobbyService, GameLobbyJoinRequested_t> joinRequested;
                CCallResult<SteamLobbyService, LobbyCreated_t> created;
                CCallResult<SteamLobbyService, LobbyEnter_t> entered;
                CCallResult<SteamLobbyService, LobbyMatchList_t> matchList;
            };

            void PushEvent(LobbyEventKind kind, const LobbyMember& member, const std::string& text)
            {
                LobbyEvent event;
                event.kind = kind;
                event.member = member;
                event.text = text;
                events_.push_back(std::move(event));
            }

            void Fail(const std::string& text)
            {
                lastError_ = text;
                state_ = LobbyState::Failed;
                PushEvent(LobbyEventKind::Error, LobbyMember{}, text);
            }

            // ---- call results and callbacks -----------------------------------------------------------------------------------------

            void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure)
            {
                if (ioFailure || result->m_eResult != k_EResultOK)
                {
                    Fail("Steam could not create the lobby (is the Steam client online?)");
                    return;
                }
                lobby_ = CSteamID(result->m_ulSteamIDLobby);
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                hostId_ = SteamUser()->GetSteamID().ConvertToUint64();
                matchmaking->SetLobbyData(lobby_, "rl_game", "1");
                matchmaking->SetLobbyData(lobby_, "rl_version", std::to_string(kLobbyProtocolVersion).c_str());
                matchmaking->SetLobbyData(lobby_, "mode", std::to_string(create_.mode).c_str());
                matchmaking->SetLobbyData(lobby_, "max", std::to_string(create_.maxPlayers).c_str());
                matchmaking->SetLobbyData(lobby_, "host_name", name_.c_str());
                matchmaking->SetLobbyData(lobby_, "host_id", std::to_string(hostId_).c_str());
                matchmaking->SetLobbyData(lobby_, "state", "open");
                matchmaking->SetLobbyMemberData(lobby_, "ready", "0");
                ReadCurrent();
                members_.clear();
                RefreshMembers(false);
                state_ = LobbyState::Hosting;
                PushEvent(LobbyEventKind::Created, members_.empty() ? LobbyMember{} : members_[0], "");
            }

            void OnLobbyEntered(LobbyEnter_t* entered, bool ioFailure)
            {
                if (ioFailure || entered->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
                {
                    Fail("cannot enter the lobby (it may be full, closed or gone)");
                    return;
                }
                lobby_ = CSteamID(entered->m_ulSteamIDLobby);
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                const std::string host = DataOf(matchmaking, lobby_, "host_id");
                hostId_ = host.empty() ? matchmaking->GetLobbyOwner(lobby_).ConvertToUint64() : std::strtoull(host.c_str(), nullptr, 10);
                if (IntOf(DataOf(matchmaking, lobby_, "rl_version"), -1) != kLobbyProtocolVersion)
                {
                    matchmaking->LeaveLobby(lobby_);
                    lobby_ = CSteamID();
                    Fail("the host runs another version of the game");
                    return;
                }
                matchmaking->SetLobbyMemberData(lobby_, "ready", "0");
                ReadCurrent();
                members_.clear();
                RefreshMembers(false);
                state_ = LobbyState::Joined;
                PushEvent(LobbyEventKind::Joined, LobbyMember{}, current_.hostName);
            }

            // One line of the list from the data of a lobby; false if the data is not there yet or the lobby is not ours (rl_game=1)
            bool BuildInfo(const CSteamID& id, LobbyInfo& info) const
            {
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                if (DataOf(matchmaking, id, "rl_game") != "1")
                {
                    return false;
                }
                info = LobbyInfo{};
                info.id = IdToString(id);
                info.hostName = DataOf(matchmaking, id, "host_name");
                info.mode = IntOf(DataOf(matchmaking, id, "mode"), 2);
                info.maxPlayers = IntOf(DataOf(matchmaking, id, "max"), 8);
                info.players = matchmaking->GetNumLobbyMembers(id);
                info.pingMs = -1;  // Steam does not tell the ping to a lobby before one has entered it
                info.joinable = true;
                if (IntOf(DataOf(matchmaking, id, "rl_version"), -1) != kLobbyProtocolVersion)
                {
                    info.joinable = false;
                    info.note = "other version";
                }
                else if (DataOf(matchmaking, id, "state") == "match")
                {
                    info.joinable = false;
                    info.note = "match in progress";
                }
                else if (info.players >= info.maxPlayers)
                {
                    info.joinable = false;
                    info.note = "full";
                }
                return true;
            }

            void OnLobbyList(LobbyMatchList_t* list, bool ioFailure)
            {
                publicLobbies_.clear();
                if (!ioFailure)
                {
                    for (uint32 i = 0; i < list->m_nLobbiesMatching; ++i)
                    {
                        LobbyInfo info;
                        if (BuildInfo(SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i)), info))
                        {
                            publicLobbies_.push_back(std::move(info));
                        }
                    }
                }
                RebuildList();
                PushEvent(LobbyEventKind::ListUpdated, LobbyMember{}, "");
            }

            // The lobbies that we and our friends are in right now (the presence knows the lobby of a friend who plays this app); their data is asked for
            void ScanFriendLobbies()
            {
                ISteamFriends* friends = SteamFriends();
                if (friends == nullptr)
                {
                    return;
                }
                friendLobbies_.clear();
                const auto consider = [&](const CSteamID& who) {
                    FriendGameInfo_t game{};
                    if (friends->GetFriendGamePlayed(who, &game) && game.m_gameID.IsValid() && game.m_gameID.AppID() == static_cast<AppId_t>(kSteamAppId) &&
                        game.m_steamIDLobby.IsValid() && std::find(friendLobbies_.begin(), friendLobbies_.end(), game.m_steamIDLobby) == friendLobbies_.end())
                    {
                        friendLobbies_.push_back(game.m_steamIDLobby);
                    }
                };
                consider(SteamUser()->GetSteamID());
                const int count = friends->GetFriendCount(k_EFriendFlagImmediate);
                for (int i = 0; i < count; ++i)
                {
                    consider(friends->GetFriendByIndex(i, k_EFriendFlagImmediate));
                }
                for (const CSteamID& id : friendLobbies_)
                {
                    SteamMatchmaking()->RequestLobbyData(id);  // LobbyDataUpdate_t comes when it is there; what is cached is read right away
                }
                RebuildList();
                if (!lobbies_.empty())
                {
                    PushEvent(LobbyEventKind::ListUpdated, LobbyMember{}, "");
                }
            }

            // Lobbies() = the public ones of the search + the lobbies of the friends whose data is there
            void RebuildList()
            {
                lobbies_ = publicLobbies_;
                for (const CSteamID& id : friendLobbies_)
                {
                    LobbyInfo info;
                    if (BuildInfo(id, info) && std::none_of(lobbies_.begin(), lobbies_.end(), [&](const LobbyInfo& l) { return l.id == info.id; }))
                    {
                        lobbies_.push_back(std::move(info));
                    }
                }
            }

            // somebody came or went
            void OnChatUpdate(LobbyChatUpdate_t* update)
            {
                if (update->m_ulSteamIDLobby != lobby_.ConvertToUint64())
                {
                    return;
                }
                const bool gone = (update->m_rgfChatMemberStateChange & (k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected | k_EChatMemberStateChangeKicked |
                                                                         k_EChatMemberStateChangeBanned)) != 0;
                if (gone && update->m_ulSteamIDUserChanged == hostId_ && state_ == LobbyState::Joined)
                {
                    members_.clear();
                    state_ = LobbyState::Idle;
                    SteamMatchmaking()->LeaveLobby(lobby_);
                    lobby_ = CSteamID();
                    PushEvent(LobbyEventKind::HostLeft, LobbyMember{}, "");
                    return;
                }
                RefreshMembers(true);
            }

            // the data of the lobby (the host started the match) or of a member (ready) changed
            void OnDataUpdate(LobbyDataUpdate_t* update)
            {
                const CSteamID changed(update->m_ulSteamIDLobby);
                if (update->m_ulSteamIDLobby == update->m_ulSteamIDMember && std::find(friendLobbies_.begin(), friendLobbies_.end(), changed) != friendLobbies_.end())
                {
                    RebuildList();  // the data of a lobby of a friend arrived (RequestLobbyData)
                    PushEvent(LobbyEventKind::ListUpdated, LobbyMember{}, "");
                }
                if (update->m_ulSteamIDLobby != lobby_.ConvertToUint64())
                {
                    return;
                }
                RefreshMembers(true);
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                if (state_ == LobbyState::Joined && !DataOf(matchmaking, lobby_, "start").empty())
                {
                    state_ = LobbyState::Starting;
                    startInfo_ = MatchStartInfo{};
                    startInfo_.isHost = false;
                    startInfo_.hostAddress = std::to_string(hostId_);
                    startInfo_.gamePort = static_cast<uint16_t>(IntOf(DataOf(matchmaking, lobby_, "game_port"), kDefaultGamePort));
                    startInfo_.mode = current_.mode;
                    startInfo_.members = members_;
                    PushEvent(LobbyEventKind::MatchStarting, LobbyMember{}, "");
                }
            }

            // a friend accepted our invitation from the overlay or the friends list while the game runs; if it was not running, Steam starts it
            // with "+connect_lobby <id>" (FindConnectLobbyInCommandLine)
            void OnJoinRequested(GameLobbyJoinRequested_t* request) { PushEvent(LobbyEventKind::InviteReceived, LobbyMember{}, IdToString(request->m_steamIDLobby)); }

            void ReadCurrent()
            {
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                current_ = LobbyInfo{};
                current_.id = IdToString(lobby_);
                current_.hostName = DataOf(matchmaking, lobby_, "host_name");
                current_.mode = IntOf(DataOf(matchmaking, lobby_, "mode"), create_.mode);
                current_.maxPlayers = IntOf(DataOf(matchmaking, lobby_, "max"), create_.maxPlayers);
                current_.players = matchmaking->GetNumLobbyMembers(lobby_);
            }

            // the members from Steam: the host first, then the others in the order of Steam; the events say what changed
            void RefreshMembers(bool announce)
            {
                if (!lobby_.IsValid())
                {
                    return;
                }
                ISteamMatchmaking* matchmaking = SteamMatchmaking();
                const uint64_t self = SteamUser()->GetSteamID().ConvertToUint64();
                std::vector<LobbyMember> now;
                const int count = matchmaking->GetNumLobbyMembers(lobby_);
                for (int i = 0; i < count; ++i)
                {
                    const CSteamID id = matchmaking->GetLobbyMemberByIndex(lobby_, i);
                    LobbyMember member;
                    member.id = IdToString(id);
                    member.name = SteamFriends() != nullptr ? SteamFriends()->GetFriendPersonaName(id) : member.id;
                    const char* ready = matchmaking->GetLobbyMemberData(lobby_, id, "ready");
                    member.ready = ready != nullptr && ready[0] == '1';
                    member.isHost = id.ConvertToUint64() == hostId_;
                    member.isLocal = id.ConvertToUint64() == self;
                    if (member.isHost)
                    {
                        now.insert(now.begin(), member);
                    }
                    else
                    {
                        now.push_back(member);
                    }
                }
                if (announce)
                {
                    for (const LobbyMember& member : now)
                    {
                        const auto old = std::find_if(members_.begin(), members_.end(), [&](const LobbyMember& m) { return m.id == member.id; });
                        if (old == members_.end())
                        {
                            if (!member.isLocal)
                            {
                                PushEvent(LobbyEventKind::MemberJoined, member, "");
                            }
                        }
                        else if (old->name != member.name || old->ready != member.ready)
                        {
                            PushEvent(LobbyEventKind::MemberChanged, member, "");
                        }
                    }
                    for (const LobbyMember& member : members_)
                    {
                        if (std::none_of(now.begin(), now.end(), [&](const LobbyMember& m) { return m.id == member.id; }))
                        {
                            PushEvent(LobbyEventKind::MemberLeft, member, "");
                        }
                    }
                }
                members_ = std::move(now);
                current_.players = static_cast<int>(members_.size());
            }

            std::string name_;
            bool ready_ = false;
            std::string initError_;
            std::unique_ptr<Callbacks> callbacks_;
            LobbyState state_ = LobbyState::Idle;
            std::string lastError_;
            std::vector<LobbyEvent> events_;
            std::vector<LobbyInfo> lobbies_;
            std::vector<LobbyInfo> publicLobbies_;   // the last result of RequestLobbyList
            std::vector<CSteamID> friendLobbies_;    // the lobbies found through the presence of the friends
            std::vector<LobbyMember> members_;
            LobbyInfo current_;
            MatchStartInfo startInfo_;
            LobbyCreateOptions create_;
            CSteamID lobby_;
            uint64_t hostId_ = 0;
        };
    }

    std::unique_ptr<ILobbyService> MakeSteamLobbyService(const std::string& name) { return std::make_unique<SteamLobbyService>(name); }
    std::unique_ptr<INetTransport> MakeSteamTransport() { return MakeSteamTransportFor({}); }
}
