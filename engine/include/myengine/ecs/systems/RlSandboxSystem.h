// RlSandboxSystem.h
// The editor sandbox of the Rocket League physics (game/rl): in Play mode it runs an rl::RlSim at 120 Hz next to the frame loop and
// moves the scene's entities (RlArena, RlBall, RlCar0.., RlCamera) to the interpolated state. Keyboard and XInput drive RlCar0.

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
    class RlSandboxSystem final : public IUpdateSystem
    {
    public:
        explicit RlSandboxSystem(input::InputManager& input);
        ~RlSandboxSystem() override;

        void Update(World& world, float deltaTime) override;

        // The lines of the debug overlay (empty when the scene is not an RL sandbox or the editor is not in Play)
        std::vector<std::string> HudLines() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
