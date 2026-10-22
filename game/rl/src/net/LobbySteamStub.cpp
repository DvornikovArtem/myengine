// LobbySteamStub.cpp
// What the game gets from LobbySteam.h in a build without the Steamworks SDK (MYENGINE_STEAM OFF), and the one function of that header that
// needs no SDK.

#include <rl/net/LobbySteam.h>

#include <algorithm>
#include <cctype>

namespace rl::net
{
    std::string FindConnectLobbyInCommandLine(const std::string& commandLine)
    {
        const std::string key = "+connect_lobby";
        size_t at = commandLine.find(key);
        if (at == std::string::npos)
        {
            return "";
        }
        at += key.size();
        while (at < commandLine.size() && (commandLine[at] == ' ' || commandLine[at] == '=' || commandLine[at] == '\t'))
        {
            ++at;
        }
        size_t end = at;
        while (end < commandLine.size() && std::isdigit(static_cast<unsigned char>(commandLine[end])) != 0)
        {
            ++end;
        }
        return commandLine.substr(at, end - at);
    }

#ifndef MYENGINE_STEAM
    namespace
    {
        class UnavailableSteamLobbyService final : public ILobbyService
        {
        public:
            const char* Name() const override { return "Steam"; }
            bool Available(std::string& whyNot) const override
            {
                whyNot = "this build has no Steam support (CMake option MYENGINE_STEAM is OFF); see docs/game/steam-setup.md";
                return false;
            }
            void Update(double) override {}
            std::vector<LobbyEvent> TakeEvents() override { return {}; }
            bool Create(const LobbyCreateOptions&, std::string& error) override { return Refuse(error); }
            std::string InviteText() const override { return ""; }
            bool InviteFriends() override { return false; }
            bool StartMatch(std::string& error) override { return Refuse(error); }
            void RefreshList() override {}
            const std::vector<LobbyInfo>& Lobbies() const override { return lobbies_; }
            bool Join(const std::string&, std::string& error) override { return Refuse(error); }
            void SetReady(bool) override {}
            void SetLocalName(const std::string&) override {}
            void Leave() override {}
            LobbyState State() const override { return LobbyState::Idle; }
            const std::string& LastError() const override { return error_; }
            const std::vector<LobbyMember>& Members() const override { return members_; }
            const LobbyInfo& Current() const override { return current_; }
            const MatchStartInfo& StartInfo() const override { return start_; }
            std::unique_ptr<INetTransport> CreateMatchTransport(const NetSimParams&) override { return nullptr; }

        private:
            bool Refuse(std::string& error)
            {
                Available(error);
                error_ = error;
                return false;
            }

            std::vector<LobbyInfo> lobbies_;
            std::vector<LobbyMember> members_;
            LobbyInfo current_;
            MatchStartInfo start_;
            std::string error_;
        };
    }

    std::unique_ptr<ILobbyService> MakeSteamLobbyService(const std::string&) { return std::make_unique<UnavailableSteamLobbyService>(); }
    std::unique_ptr<INetTransport> MakeSteamTransport() { return nullptr; }
#endif
}
