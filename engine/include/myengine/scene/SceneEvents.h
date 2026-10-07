#pragma once

namespace myengine::ecs
{
    class World;
}

namespace myengine::scene
{
    // Published after a complete scene load, including restoration of a Play snapshot.
    struct SceneLoadedEvent
    {
        ecs::World* world = nullptr;
    };
}
