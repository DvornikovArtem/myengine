#pragma once

#include <myengine/events/EventBus.h>
#include <myengine/physics/PhysicsWorldState.h>

namespace myengine::core
{
    class ServiceLocator
    {
    public:
        static events::EventBus& GetEventBus()
        {
            static events::EventBus eventBus;
            return eventBus;
        }

        static physics::PhysicsWorldState& GetPhysicsWorldState()
        {
            static physics::PhysicsWorldState worldState;
            return worldState;
        }
    };
}