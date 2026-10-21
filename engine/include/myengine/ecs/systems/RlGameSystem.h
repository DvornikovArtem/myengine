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

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
