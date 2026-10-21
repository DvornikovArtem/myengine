// RlGameSystem.h
// The Rocket League game in the editor: in Play mode it shows the menu, starts an rl::RlMatch (physics, rules, bots) and runs it at 120 Hz
// next to the frame loop, moving the scene's entities (RlArena, RlBall, RlBlue0.., RlOrange0.., RlPad0.., RlCamera) to the interpolated
// state. Keyboard and XInput drive the first blue car.

#pragma once

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
    class RlGameSystem final : public IUpdateSystem
    {
    public:
        explicit RlGameSystem(input::InputManager& input);
        ~RlGameSystem() override;

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
