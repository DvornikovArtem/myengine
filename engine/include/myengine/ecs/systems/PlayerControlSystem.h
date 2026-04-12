#pragma once

#include <myengine/ecs/System.h>

namespace myengine::input
{
    class InputManager;
}

namespace myengine::ecs::systems
{
    class PlayerControlSystem final : public IUpdateSystem
    {
    public:
        explicit PlayerControlSystem(input::InputManager& input);

        void Update(World& world, float deltaTime) override;

    private:
        input::InputManager& input_;
    };
}