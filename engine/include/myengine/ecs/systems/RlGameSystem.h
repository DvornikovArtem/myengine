// RlGameSystem.h
// The Rocket League game in the editor: in Play mode it shows the menu, starts an rl::RlMatch (physics, rules, bots) and runs it at 120 Hz
// next to the frame loop, moving the scene's entities (RlArena, RlBall, RlBlue0.., RlOrange0.., RlPad0.., RlCamera) to the interpolated
// state. Keyboard and XInput drive the first blue car.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <myengine/ecs/System.h>

namespace myengine::input
{
    class InputManager;
}

namespace rl
{
    struct RlTuning;
}

namespace myengine::ecs::systems
{
    // The online game (G5): --rl-host [port] and --rl-join ip[:port]. With a role the game skips the menu: the host waits for its guests and starts
    // the match, a client joins the host and plays its slot with prediction and rollback (game/rl RlNet).
    struct RlOnlineOptions
    {
        enum class Role
        {
            None,
            Host,
            Client,
        };

        Role role = Role::None;
        std::string address;       // client: "ip" or "ip:port" of the host
        std::uint16_t port = 47777;  // host: the port to listen on
        int expectedClients = 1;   // host: the match starts when this many guests have joined (Enter starts it earlier)
        int playersPerTeam = 2;    // host: 1..4
        std::string netSim;        // "latency=100,jitter=20,loss=5": delay and losses added to everything this window sends
    };

    class RlGameSystem final : public IUpdateSystem
    {
    public:
        explicit RlGameSystem(input::InputManager& input);
        ~RlGameSystem() override;

        // Before the first Play: the role of this window in an online match
        void SetOnlineOptions(const RlOnlineOptions& options);

        void Update(World& world, float deltaTime) override;

        // The lines of the debug overlay (empty when the scene is not an RL sandbox or the editor is not in Play)
        std::vector<std::string> HudLines() const;

        // The numbers of the running game for the RL Tuning window (null when no match runs). After changing them call TuningEdited();
        // the game reads assets/data/rl_tuning.json again whenever it changes on disk.
        rl::RlTuning* EditableTuning();
        void TuningEdited();
        std::string SaveTuning();    // writes the json; the result is a message for the window
        std::string ReloadTuning();  // reads it again

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
