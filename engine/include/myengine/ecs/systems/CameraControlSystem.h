// CameraControlSystem.h

#pragma once

#include <myengine/core/Types.h>
#include <myengine/ecs/System.h>

namespace myengine::input
{
    class InputManager;
}

namespace myengine::ecs::systems
{
    class CameraControlSystem final : public IUpdateSystem
    {
    public:
        CameraControlSystem(input::InputManager& input, const core::WindowId& activeWindowId);

        void Update(World& world, float deltaTime) override;

    private:
        input::InputManager& input_;
        const core::WindowId& activeWindowId_;
    };
}