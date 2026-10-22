// LobbyLan.cpp

#include <rl/net/LobbyLan.h>

#include <rl/RlTypes.h>
#include <rl/net/NetReliable.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rl::net
{
    namespace
    {
        // ---- the beacon: datagrams on kLanBeaconPort --------------------------------------------------------------------------------
        //   Beacon  'R' 'L' 1  version u16, mode u8, maxPlayers u8, players u8, flags u8 (bit 0: a match is on), lobbyPort u16, name
        //   Ping    'R' 'L' 2  nonce u32            (to the source address of a beacon)
        //   Pong    'R' 'L' 3  nonce u32
        enum BeaconType : uint8_t
        {
            kBeacon = 1,
            kPing = 2,
            kPong = 3,
        };

        // ---- the lobby: messages on the reliable connection of a member to the lobby port of the host -------------------------------
        enum LobbyMsg : uint8_t
        {
            kHello = 1,    // member -> host: version u16, name
            kWelcome = 2,  // host -> member: yourIndex u8, mode u8, maxPlayers u8, host name
            kReject = 3,   // host -> member: reason u8 (1 version, 2 full, 3 match in progress)
            kMembers = 4,  // host -> members: count u8, then per member: name, ready u8, isHost u8
            kReady = 5,    // member -> host: ready u8
            kRename = 6,   // member -> host: name
            kStart = 7,    // host -> members: gamePort u16
        };

        constexpr double kBeaconInterval = 1.0;
        constexpr double kEntryLifetime = 4.0;
        constexpr double kPingInterval = 2.0;
        constexpr double kKickDelay = 0.6;
        constexpr size_t kMaxName = 24;

        class Writer
        {
        public:
            explicit Writer(std::vector<uint8_t>& out)
                : out_(out)
            {
                out_.clear();
            }
            void U8(uint8_t v) { out_.push_back(v); }
            void U16(uint16_t v)
            {
                U8(static_cast<uint8_t>(v));
                U8(static_cast<uint8_t>(v >> 8));
            }
            void U32(uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                {
                    U8(static_cast<uint8_t>(v >> (8 * i)));
                }
            }
            void Str(const std::string& text)
            {
                const size_t length = std::min(text.size(), kMaxName);
                U8(static_cast<uint8_t>(length));
                out_.insert(out_.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
            }

        private:
            std::vector<uint8_t>& out_;
        };

        class Reader
        {
        public:
            Reader(const uint8_t* data, size_t size)
                : data_(data)
                , size_(size)
            {
            }
            bool Ok() const { return ok_; }
            uint8_t U8()
            {
                if (pos_ + 1 > size_)
                {
                    ok_ = false;
                    return 0;
                }
                return data_[pos_++];
            }
            uint16_t U16()
            {
                const uint16_t lo = U8();
                return static_cast<uint16_t>(lo | (static_cast<uint16_t>(U8()) << 8));
            }
            uint32_t U32()
            {
                uint32_t v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    v |= static_cast<uint32_t>(U8()) << (8 * i);
                }
                return v;
            }
            std::string Str()
            {
                const size_t length = U8();
                if (length > kMaxName || pos_ + length > size_)
                {
                    ok_ = false;
                    return {};
                }
                std::string text(reinterpret_cast<const char*>(data_ + pos_), length);
                pos_ += length;
                return text;
            }

        private:
            const uint8_t* data_;
            size_t size_;
            size_t pos_ = 0;
            bool ok_ = true;
        };

        std::string ComputerName()
        {
            char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = static_cast<DWORD>(sizeof(buffer));
            if (GetComputerNameA(buffer, &size) != 0 && size > 0)
            {
                return std::string(buffer, size);
            }
            return "Player";
        }

        class LanLobbyService final : public ILobbyService
        {
        public:
            explicit LanLobbyService(const LanLobbyOptions& options)
                : options_(options)
            {
                if (!options_.makeLink)
                {
                    options_.makeLink = [] { return MakeUdpLink(); };
                }
                if (!options_.makeMatchTransport)
                {
                    options_.makeMatchTransport = [](const NetSimParams& sim) { return MakeUdpTransport(sim); };
                }
                name_ = options_.name.empty() ? ComputerName() : options_.name.substr(0, kMaxName);
            }

            ~LanLobbyService() override { Leave(); }

            const char* Name() const override { return "LAN"; }

            bool Available(std::string& whyNot) const override
            {
                whyNot.clear();
                return true;
            }

            // ---- the frame ----------------------------------------------------------------------------------------------------------

            void Update(double now) override
            {
                now_ = now;
                if (state_ == LobbyState::Hosting || state_ == LobbyState::Starting)
                {
                    if (hosting_)
                    {
                        UpdateHost(now);
                    }
                }
                if (guesting_)
                {
                    UpdateGuest(now);
                }
                if (scanLink_ != nullptr)
                {
                    UpdateScan(now);
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
                if (state_ != LobbyState::Idle && state_ != LobbyState::Failed)
                {
                    error = "already in a lobby: leave it first";
                    return Fail(error, false);
                }
                CloseScan();
                create_ = options;
                create_.mode = std::clamp(create_.mode, 1, 4);
                create_.maxPlayers = std::clamp(create_.maxPlayers, 2, kMaxCars);
                if (!create_.hostName.empty())
                {
                    name_ = create_.hostName.substr(0, kMaxName);
                }

                lobbyTransport_ = MakeReliableTransport(options_.makeLink(), options_.seed);
                if (!lobbyTransport_->Listen(create_.lobbyPort, error))
                {
                    lobbyTransport_.reset();
                    return Fail("cannot open the lobby port " + std::to_string(create_.lobbyPort) + ": " + error, true);
                }
                beaconLink_ = options_.makeLink();
                if (!beaconLink_->Bind(0, error))
                {
                    lobbyTransport_.reset();
                    beaconLink_.reset();
                    return Fail("cannot open the beacon: " + error, true);
                }

                hosting_ = true;
                guests_.clear();
                kicks_.clear();
                members_.clear();
                members_.push_back(LobbyMember{"host", name_, false, true, true});
                current_ = LobbyInfo{};
                current_.id = "";
                current_.hostName = name_;
                current_.mode = create_.mode;
                current_.maxPlayers = create_.maxPlayers;
                current_.players = 1;
                nextBeacon_ = 0.0;
                state_ = LobbyState::Hosting;
                lastError_.clear();
                PushEvent(LobbyEventKind::Created, members_[0], "");
                return true;
            }

            std::string InviteText() const override
            {
                if (!hosting_)
                {
                    return "";
                }
                return "LAN: friends on this network find the lobby in the list; by hand: this PC, port " + std::to_string(create_.lobbyPort);
            }

            bool InviteFriends() override { return false; }

            bool StartMatch(std::string& error) override
            {
                if (!hosting_ || state_ != LobbyState::Hosting)
                {
                    error = "only the host of a lobby can start the match";
                    return false;
                }
                for (const Guest& guest : guests_)
                {
                    if (guest.joined && !guest.ready)
                    {
                        error = guest.name + " is not ready";
                        return false;
                    }
                }
                state_ = LobbyState::Starting;
                startInfo_ = MatchStartInfo{};
                startInfo_.isHost = true;
                startInfo_.gamePort = create_.gamePort;
                startInfo_.mode = create_.mode;
                startInfo_.members = members_;
                writer_.clear();
                Writer w(writer_);
                w.U8(kStart);
                w.U16(create_.gamePort);
                for (const Guest& guest : guests_)
                {
                    if (guest.joined)
                    {
                        lobbyTransport_->Send(guest.peer, writer_.data(), writer_.size(), true);
                    }
                }
                SendBeacon(now_);  // the list shows "match in progress" at once
                PushEvent(LobbyEventKind::MatchStarting, LobbyMember{}, "");
                return true;
            }

            // ---- joining ------------------------------------------------------------------------------------------------------------

            void RefreshList() override
            {
                if (state_ != LobbyState::Idle && state_ != LobbyState::Failed)
                {
                    return;
                }
                if (scanLink_ == nullptr)
                {
                    scanLink_ = options_.makeLink();
                    std::string error;
                    if (!scanLink_->BindShared(kLanBeaconPort, error))
                    {
                        scanLink_.reset();
                        lastError_ = "cannot listen for lobbies: " + error;
                        PushEvent(LobbyEventKind::Error, LobbyMember{}, lastError_);
                        return;
                    }
                }
                // what was seen long ago goes, what is there stays and answers again with its next beacon
                lobbies_.clear();
                seen_.clear();
                PushEvent(LobbyEventKind::ListUpdated, LobbyMember{}, "");
            }

            const std::vector<LobbyInfo>& Lobbies() const override { return lobbies_; }

            bool Join(const std::string& idOrAddress, std::string& error) override
            {
                if (state_ != LobbyState::Idle && state_ != LobbyState::Failed)
                {
                    error = "already in a lobby: leave it first";
                    return Fail(error, false);
                }
                std::string text = idOrAddress;
                if (text.empty())
                {
                    error = "no lobby chosen";
                    return Fail(error, true);
                }
                if (text.find(':') == std::string::npos && text.find_first_not_of("0123456789") != std::string::npos)
                {
                    text += ":" + std::to_string(kDefaultLobbyPort);  // a bare address
                }
                std::unique_ptr<IDatagramLink> resolver = options_.makeLink();
                if (!resolver->Resolve(text, hostAddress_, error))
                {
                    return Fail(error, true);
                }
                CloseScan();
                lobbyTransport_ = MakeReliableTransport(options_.makeLink(), options_.seed + 17);
                hostPeer_ = lobbyTransport_->Connect(text, error);
                if (hostPeer_ == kInvalidPeer)
                {
                    lobbyTransport_.reset();
                    return Fail(error, true);
                }
                guesting_ = true;
                helloSent_ = false;
                myIndex_ = -1;
                members_.clear();
                current_ = LobbyInfo{};
                current_.id = text;
                state_ = LobbyState::Joining;
                lastError_.clear();
                return true;
            }

            void SetReady(bool ready) override
            {
                if (guesting_ && state_ == LobbyState::Joined)
                {
                    writer_.clear();
                    Writer w(writer_);
                    w.U8(kReady);
                    w.U8(ready ? 1 : 0);
                    lobbyTransport_->Send(hostPeer_, writer_.data(), writer_.size(), true);
                }
                else if (hosting_ && state_ == LobbyState::Hosting)
                {
                    members_[0].ready = ready;
                    BroadcastMembers();
                    PushEvent(LobbyEventKind::MemberChanged, members_[0], "");
                }
            }

            void SetLocalName(const std::string& name) override
            {
                name_ = name.substr(0, kMaxName);
                if (guesting_ && state_ == LobbyState::Joined)
                {
                    writer_.clear();
                    Writer w(writer_);
                    w.U8(kRename);
                    w.Str(name_);
                    lobbyTransport_->Send(hostPeer_, writer_.data(), writer_.size(), true);
                }
                else if (hosting_)
                {
                    members_[0].name = name_;
                    current_.hostName = name_;
                    BroadcastMembers();
                }
            }

            void Leave() override
            {
                if (lobbyTransport_ != nullptr)
                {
                    lobbyTransport_->Close();
                    lobbyTransport_.reset();
                }
                beaconLink_.reset();
                CloseScan();
                hosting_ = false;
                guesting_ = false;
                guests_.clear();
                kicks_.clear();
                members_.clear();
                state_ = LobbyState::Idle;
            }

            LobbyState State() const override { return state_; }
            const std::string& LastError() const override { return lastError_; }
            const std::vector<LobbyMember>& Members() const override { return members_; }
            const LobbyInfo& Current() const override { return current_; }
            const MatchStartInfo& StartInfo() const override { return startInfo_; }

            std::unique_ptr<INetTransport> CreateMatchTransport(const NetSimParams& sim) override { return options_.makeMatchTransport(sim); }

        private:
            struct Guest
            {
                PeerId peer = kInvalidPeer;
                std::string name;
                bool ready = false;
                bool joined = false;  // the Hello came
            };

            struct Seen
            {
                NetAddress from = 0;
                LobbyInfo info;
                double lastBeacon = 0.0;
                double lastPing = -1e9;
                uint32_t nonce = 0;
                double pingSent = 0.0;
            };

            bool Fail(const std::string& text, bool event)
            {
                lastError_ = text;
                if (state_ == LobbyState::Idle || state_ == LobbyState::Joining || state_ == LobbyState::Creating)
                {
                    state_ = LobbyState::Failed;
                }
                if (event)
                {
                    PushEvent(LobbyEventKind::Error, LobbyMember{}, text);
                }
                return false;
            }

            void PushEvent(LobbyEventKind kind, const LobbyMember& member, const std::string& text)
            {
                LobbyEvent event;
                event.kind = kind;
                event.member = member;
                event.text = text;
                events_.push_back(std::move(event));
            }

            void CloseScan()
            {
                scanLink_.reset();
                seen_.clear();
                lobbies_.clear();
            }

            // ---- the host ------------------------------------------------------------------------------------------------------------

            void SendBeacon(double now)
            {
                if (beaconLink_ == nullptr)
                {
                    return;
                }
                writer_.clear();
                Writer w(writer_);
                w.U8('R');
                w.U8('L');
                w.U8(kBeacon);
                w.U16(static_cast<uint16_t>(options_.protocolVersion));
                w.U8(static_cast<uint8_t>(create_.mode));
                w.U8(static_cast<uint8_t>(create_.maxPlayers));
                w.U8(static_cast<uint8_t>(members_.size()));
                w.U8(state_ == LobbyState::Starting ? 1 : 0);
                w.U16(create_.lobbyPort);
                w.Str(name_);
                beaconLink_->SendBroadcast(kLanBeaconPort, writer_.data(), writer_.size(), now);
            }

            void BroadcastMembers()
            {
                if (!hosting_)
                {
                    return;
                }
                current_.players = static_cast<int>(members_.size());
                for (const Guest& guest : guests_)
                {
                    if (guest.joined)
                    {
                        SendMembers(guest.peer);
                    }
                }
            }

            void SendMembers(PeerId peer)
            {
                std::vector<uint8_t> bytes;
                Writer w(bytes);
                w.U8(kMembers);
                w.U8(static_cast<uint8_t>(members_.size()));
                for (const LobbyMember& member : members_)
                {
                    w.Str(member.name);
                    w.U8(member.ready ? 1 : 0);
                    w.U8(member.isHost ? 1 : 0);
                }
                lobbyTransport_->Send(peer, bytes.data(), bytes.size(), true);
            }

            void RebuildMembers()
            {
                members_.resize(1);
                for (const Guest& guest : guests_)
                {
                    if (guest.joined)
                    {
                        members_.push_back(LobbyMember{"member-" + std::to_string(guest.peer), guest.name, guest.ready, false, false});
                    }
                }
            }

            Guest* FindGuest(PeerId peer)
            {
                for (Guest& guest : guests_)
                {
                    if (guest.peer == peer)
                    {
                        return &guest;
                    }
                }
                return nullptr;
            }

            void Reject(PeerId peer, uint8_t reason, double now)
            {
                std::vector<uint8_t> bytes;
                Writer w(bytes);
                w.U8(kReject);
                w.U8(reason);
                lobbyTransport_->Send(peer, bytes.data(), bytes.size(), true);
                kicks_.push_back({peer, now + kKickDelay});  // the connection stays a moment so that the answer gets through
            }

            void UpdateHost(double now)
            {
                std::vector<NetEvent> polled;
                lobbyTransport_->Poll(now, polled);
                for (NetEvent& event : polled)
                {
                    if (event.type == NetEventType::Connected)
                    {
                        Guest guest;
                        guest.peer = event.peer;
                        guests_.push_back(guest);
                    }
                    else if (event.type == NetEventType::Disconnected)
                    {
                        const Guest* guest = FindGuest(event.peer);
                        if (guest != nullptr)
                        {
                            const LobbyMember left{"member-" + std::to_string(guest->peer), guest->name, guest->ready, false, false};
                            const bool wasJoined = guest->joined;
                            guests_.erase(std::remove_if(guests_.begin(), guests_.end(), [&](const Guest& g) { return g.peer == event.peer; }), guests_.end());
                            if (wasJoined)
                            {
                                RebuildMembers();
                                BroadcastMembers();
                                PushEvent(LobbyEventKind::MemberLeft, left, event.reason == DisconnectReason::Timeout ? "timeout" : "");
                            }
                        }
                    }
                    else if (event.type == NetEventType::Data && !event.data.empty())
                    {
                        OnHostMessage(event.peer, event.data, now);
                    }
                }
                for (size_t i = 0; i < kicks_.size();)
                {
                    if (now >= kicks_[i].second)
                    {
                        lobbyTransport_->Disconnect(kicks_[i].first);
                        guests_.erase(std::remove_if(guests_.begin(), guests_.end(), [&](const Guest& g) { return g.peer == kicks_[i].first; }), guests_.end());
                        kicks_.erase(kicks_.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                    else
                    {
                        ++i;
                    }
                }

                // the beacon, and the answers to the pings of those who search
                if (beaconLink_ != nullptr)
                {
                    if (now >= nextBeacon_)
                    {
                        nextBeacon_ = now + kBeaconInterval;
                        SendBeacon(now);
                    }
                    NetAddress from = 0;
                    std::vector<uint8_t> data;
                    for (int guard = 0; guard < 64 && beaconLink_->Receive(from, data, now); ++guard)
                    {
                        if (data.size() >= 7 && data[0] == 'R' && data[1] == 'L' && data[2] == kPing)
                        {
                            data[2] = kPong;
                            beaconLink_->Send(from, data.data(), data.size(), now);
                        }
                    }
                }
            }

            void OnHostMessage(PeerId peer, const std::vector<uint8_t>& data, double now)
            {
                Guest* guest = FindGuest(peer);
                if (guest == nullptr)
                {
                    return;
                }
                Reader r(data.data(), data.size());
                const uint8_t type = r.U8();
                if (type == kHello && !guest->joined)
                {
                    const int version = r.U16();
                    const std::string name = r.Str();
                    if (!r.Ok())
                    {
                        return;
                    }
                    if (version != options_.protocolVersion)
                    {
                        Reject(peer, 1, now);
                        return;
                    }
                    if (state_ == LobbyState::Starting)
                    {
                        Reject(peer, 3, now);
                        return;
                    }
                    if (static_cast<int>(members_.size()) >= create_.maxPlayers)
                    {
                        Reject(peer, 2, now);
                        return;
                    }
                    guest->joined = true;
                    guest->name = name.empty() ? "Player" : name;
                    RebuildMembers();
                    const int index = static_cast<int>(members_.size()) - 1;
                    std::vector<uint8_t> bytes;
                    Writer w(bytes);
                    w.U8(kWelcome);
                    w.U8(static_cast<uint8_t>(index));
                    w.U8(static_cast<uint8_t>(create_.mode));
                    w.U8(static_cast<uint8_t>(create_.maxPlayers));
                    w.Str(name_);
                    lobbyTransport_->Send(peer, bytes.data(), bytes.size(), true);
                    BroadcastMembers();
                    PushEvent(LobbyEventKind::MemberJoined, members_.back(), "");
                }
                else if (type == kReady && guest->joined)
                {
                    guest->ready = r.U8() != 0;
                    RebuildMembers();
                    BroadcastMembers();
                    for (const LobbyMember& member : members_)
                    {
                        if (member.name == guest->name && !member.isHost)
                        {
                            PushEvent(LobbyEventKind::MemberChanged, member, "");
                        }
                    }
                }
                else if (type == kRename && guest->joined)
                {
                    const std::string name = r.Str();
                    if (r.Ok() && !name.empty())
                    {
                        guest->name = name;
                        RebuildMembers();
                        BroadcastMembers();
                    }
                }
            }

            // ---- the guest ------------------------------------------------------------------------------------------------------------

            void UpdateGuest(double now)
            {
                std::vector<NetEvent> polled;
                lobbyTransport_->Poll(now, polled);
                for (NetEvent& event : polled)
                {
                    if (event.type == NetEventType::Connected)
                    {
                        writer_.clear();
                        Writer w(writer_);
                        w.U8(kHello);
                        w.U16(static_cast<uint16_t>(options_.protocolVersion));
                        w.Str(name_);
                        lobbyTransport_->Send(hostPeer_, writer_.data(), writer_.size(), true);
                        helloSent_ = true;
                    }
                    else if (event.type == NetEventType::Disconnected)
                    {
                        if (state_ == LobbyState::Joining)
                        {
                            Fail(event.reason == DisconnectReason::Timeout ? "the host of the lobby does not answer" : "the host of the lobby refused the connection", true);
                        }
                        else if (state_ == LobbyState::Joined)
                        {
                            members_.clear();
                            PushEvent(LobbyEventKind::HostLeft, LobbyMember{}, event.reason == DisconnectReason::Timeout ? "timeout" : "");
                            state_ = LobbyState::Idle;
                        }
                        // in Starting the match has its own connection: the lobby may close
                        guesting_ = false;
                        lobbyTransport_.reset();
                        return;
                    }
                    else if (event.type == NetEventType::Data && !event.data.empty())
                    {
                        OnGuestMessage(event.data);
                    }
                }
            }

            void OnGuestMessage(const std::vector<uint8_t>& data)
            {
                Reader r(data.data(), data.size());
                const uint8_t type = r.U8();
                if (type == kWelcome)
                {
                    myIndex_ = r.U8();
                    current_.mode = r.U8();
                    current_.maxPlayers = r.U8();
                    current_.hostName = r.Str();
                    if (!r.Ok())
                    {
                        return;
                    }
                    state_ = LobbyState::Joined;
                    PushEvent(LobbyEventKind::Joined, LobbyMember{}, current_.hostName);
                }
                else if (type == kReject)
                {
                    const uint8_t reason = r.U8();
                    guesting_ = false;
                    lobbyTransport_->Disconnect(hostPeer_);
                    Fail(reason == 1 ? "the host runs another version of the game" : reason == 2 ? "the lobby is full" : "the match has started already", true);
                }
                else if (type == kMembers)
                {
                    const int count = r.U8();
                    std::vector<LobbyMember> now;
                    for (int i = 0; i < count && r.Ok(); ++i)
                    {
                        LobbyMember member;
                        member.name = r.Str();
                        member.ready = r.U8() != 0;
                        member.isHost = r.U8() != 0;
                        member.isLocal = i == myIndex_;
                        member.id = member.isHost ? "host" : "member-" + std::to_string(i);
                        now.push_back(std::move(member));
                    }
                    if (!r.Ok())
                    {
                        return;
                    }
                    DiffMembers(now);
                    members_ = now;
                    current_.players = count;
                }
                else if (type == kStart)
                {
                    const uint16_t gamePort = r.U16();
                    if (!r.Ok() || state_ != LobbyState::Joined)
                    {
                        return;
                    }
                    state_ = LobbyState::Starting;
                    startInfo_ = MatchStartInfo{};
                    startInfo_.isHost = false;
                    startInfo_.gamePort = gamePort;
                    startInfo_.hostAddress = scanFormat(WithPortOf(hostAddress_, gamePort));
                    startInfo_.mode = current_.mode;
                    startInfo_.members = members_;
                    PushEvent(LobbyEventKind::MatchStarting, LobbyMember{}, "");
                }
            }

            std::string scanFormat(NetAddress address)
            {
                std::unique_ptr<IDatagramLink> link = options_.makeLink();
                return link->FormatAddress(address);
            }

            NetAddress WithPortOf(NetAddress address, uint16_t port)
            {
                std::unique_ptr<IDatagramLink> link = options_.makeLink();
                return link->WithPort(address, port);
            }

            // who came and who went between the old list and the new one (by name and position)
            void DiffMembers(const std::vector<LobbyMember>& now)
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
                    const bool still = std::any_of(now.begin(), now.end(), [&](const LobbyMember& m) { return m.id == member.id; });
                    if (!still)
                    {
                        PushEvent(LobbyEventKind::MemberLeft, member, "");
                    }
                }
            }

            // ---- the search ----------------------------------------------------------------------------------------------------------

            void UpdateScan(double now)
            {
                NetAddress from = 0;
                std::vector<uint8_t> data;
                bool changed = false;
                for (int guard = 0; guard < 128 && scanLink_->Receive(from, data, now); ++guard)
                {
                    if (data.size() < 3 || data[0] != 'R' || data[1] != 'L')
                    {
                        continue;
                    }
                    Reader r(data.data(), data.size());
                    r.U8();
                    r.U8();
                    const uint8_t type = r.U8();
                    if (type == kBeacon)
                    {
                        const int version = r.U16();
                        LobbyInfo info;
                        info.mode = r.U8();
                        info.maxPlayers = r.U8();
                        info.players = r.U8();
                        const uint8_t flags = r.U8();
                        const uint16_t lobbyPort = r.U16();
                        info.hostName = r.Str();
                        if (!r.Ok())
                        {
                            continue;
                        }
                        info.id = scanFormat(WithPortOf(from, lobbyPort));
                        info.joinable = true;
                        if (version != options_.protocolVersion)
                        {
                            info.joinable = false;
                            info.note = "other version";
                        }
                        else if ((flags & 1) != 0)
                        {
                            info.joinable = false;
                            info.note = "match in progress";
                        }
                        else if (info.players >= info.maxPlayers)
                        {
                            info.joinable = false;
                            info.note = "full";
                        }
                        Seen* entry = nullptr;
                        for (Seen& s : seen_)
                        {
                            if (s.info.id == info.id)
                            {
                                entry = &s;
                            }
                        }
                        if (entry == nullptr)
                        {
                            seen_.emplace_back();
                            entry = &seen_.back();
                            entry->info = info;
                            entry->info.pingMs = -1;
                            changed = true;
                        }
                        else
                        {
                            const int ping = entry->info.pingMs;
                            changed = changed || entry->info.hostName != info.hostName || entry->info.players != info.players || entry->info.joinable != info.joinable ||
                                      entry->info.note != info.note || entry->info.mode != info.mode;
                            entry->info = info;
                            entry->info.pingMs = ping;
                        }
                        entry->from = from;
                        entry->lastBeacon = now;
                    }
                    else if (type == kPong)
                    {
                        const uint32_t nonce = r.U32();
                        for (Seen& s : seen_)
                        {
                            if (s.nonce == nonce && s.pingSent > 0.0 && s.from == from)
                            {
                                const int ping = std::max(0, static_cast<int>((now - s.pingSent) * 1000.0 + 0.5));
                                changed = changed || s.info.pingMs < 0 || std::abs(s.info.pingMs - ping) > 5;
                                s.info.pingMs = ping;
                                s.pingSent = 0.0;
                            }
                        }
                    }
                }

                for (size_t i = 0; i < seen_.size();)
                {
                    Seen& s = seen_[i];
                    if (now - s.lastBeacon > kEntryLifetime)
                    {
                        seen_.erase(seen_.begin() + static_cast<std::ptrdiff_t>(i));
                        changed = true;
                        continue;
                    }
                    if (now - s.lastPing >= kPingInterval)
                    {
                        s.lastPing = now;
                        s.nonce = ++pingCounter_;
                        s.pingSent = now > 0.0 ? now : 1e-6;
                        writer_.clear();
                        Writer w(writer_);
                        w.U8('R');
                        w.U8('L');
                        w.U8(kPing);
                        w.U32(s.nonce);
                        scanLink_->Send(s.from, writer_.data(), writer_.size(), now);
                    }
                    ++i;
                }

                if (changed)
                {
                    lobbies_.clear();
                    for (const Seen& s : seen_)
                    {
                        lobbies_.push_back(s.info);
                    }
                    std::stable_sort(lobbies_.begin(), lobbies_.end(), [](const LobbyInfo& a, const LobbyInfo& b) {
                        if (a.joinable != b.joinable)
                        {
                            return a.joinable;
                        }
                        const int pa = a.pingMs < 0 ? 100000 : a.pingMs;
                        const int pb = b.pingMs < 0 ? 100000 : b.pingMs;
                        return pa < pb;
                    });
                    PushEvent(LobbyEventKind::ListUpdated, LobbyMember{}, "");
                }
            }

            LanLobbyOptions options_;
            std::string name_;
            double now_ = 0.0;
            LobbyState state_ = LobbyState::Idle;
            std::string lastError_;
            std::vector<LobbyEvent> events_;
            std::vector<LobbyInfo> lobbies_;
            std::vector<LobbyMember> members_;
            LobbyInfo current_;
            MatchStartInfo startInfo_;
            std::vector<uint8_t> writer_;

            // host
            bool hosting_ = false;
            LobbyCreateOptions create_;
            std::unique_ptr<INetTransport> lobbyTransport_;
            std::unique_ptr<IDatagramLink> beaconLink_;
            double nextBeacon_ = 0.0;
            std::vector<Guest> guests_;
            std::vector<std::pair<PeerId, double>> kicks_;

            // guest
            bool guesting_ = false;
            bool helloSent_ = false;
            PeerId hostPeer_ = kInvalidPeer;
            NetAddress hostAddress_ = 0;
            int myIndex_ = -1;

            // search
            std::unique_ptr<IDatagramLink> scanLink_;
            std::vector<Seen> seen_;
            uint32_t pingCounter_ = 0;
        };
    }

    std::unique_ptr<ILobbyService> MakeLanLobbyService(const LanLobbyOptions& options) { return std::make_unique<LanLobbyService>(options); }

    std::unique_ptr<ILobbyService> MakeLanLobbyService(const std::string& name)
    {
        LanLobbyOptions options;
        options.name = name;
        return MakeLanLobbyService(options);
    }
}
