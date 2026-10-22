// RlLobbyScreens.h (private)
// The lobby of the private match on the screens of the game (docs/game/rl-lobby.md, "Экраны"): the switch LAN / Steam, the list of lobbies, creating a
// lobby, the room with the members and their readiness, the invitation and the start. It drives rl::net::ILobbyService (LAN beacon or Steam); when the
// host starts, it hands RlGameFlow the RlGameMatchSetup of the match (with the transport of the service) and the flow goes on with the screens of G5.
//
//     lobby.Install(commandLine);                 // once: Steam in a build with it, +connect_lobby
//     lobby.Update(dt);                           // every frame
//     lobby.DrawPrivate(f, settings, name);       // the Private match tab of the Play screen: OpenRoom when a lobby is made or joined
//     lobby.DrawRoom(f, name);                    // the room: LeaveRoom / StartMatch

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/game/RlGameSettings.h>
#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::game
{
    class RlLobbyScreens
    {
    public:
        enum class Action
        {
            None,
            OpenRoom,    // go to the room (a lobby was made or joined, or an invitation came)
            LeaveRoom,   // back to the Play screen (the player left or the lobby failed: a panel with the reason is shown there)
            StartMatch,  // the host started: TakeStartedMatch gives the setup of the match
        };

        explicit RlLobbyScreens(std::function<void(const std::string&)> log);
        ~RlLobbyScreens();
        RlLobbyScreens(const RlLobbyScreens&) = delete;
        RlLobbyScreens& operator=(const RlLobbyScreens&) = delete;

        // Once, when the game starts. A build with Steam starts the Steam service now (the invitations of the friends come to a running game);
        // `commandLine` with "+connect_lobby <id>" joins that lobby (the room opens: TakeOpenRoom).
        void Install(const std::string& commandLine);
        // Every frame, whatever screen is shown: the callbacks of the services and their events
        void Update(float deltaSeconds);

        Action DrawPrivate(rlui::Frame& f, RlGameSettings& settings, const std::string& playerName);
        Action DrawRoom(rlui::Frame& f, const std::string& playerName);

        // An invitation, or +connect_lobby, wants the room open (once)
        bool TakeOpenRoom();
        // After StartMatch: the match to start and how many players the lobby had (the host starts when so many are connected)
        bool TakeStartedMatch(ecs::systems::RlGameMatchSetup& setup, int& humans);
        // The match was left or ended, the player is back in the menus: the lobby of that match is closed
        void ReleaseMatch();
        // The player left the Private match tab for the main menu: the search on the LAN stops
        void CloseIdle();
        // The panel with the reason is up (the Play screen must not take the keys)
        bool ErrorShown() const;
        // A failure that happened out of the Private match screen (+connect_lobby at the start): the main menu shows it in its own panel
        bool TakeError(std::string& title, std::string& text);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
