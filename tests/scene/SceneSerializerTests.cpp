#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scene/SceneSerializer.h>

namespace
{
    using json = nlohmann::json;
    namespace ecs = myengine::ecs;
    namespace scene = myengine::scene;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct LoadEvents
    {
        ecs::World* expectedWorld = nullptr;
        std::size_t count = 0;
        bool completeWorld = false;
    };

    class CountingSystem final : public ecs::IUpdateSystem
    {
    public:
        explicit CountingSystem(unsigned& updates) : updates_(updates) {}

        void Update(ecs::World&, float) override
        {
            ++updates_;
        }

    private:
        unsigned& updates_;
    };

    void TestEntitySerialization()
    {
        ecs::World world;
        const auto entity = world.CreateEntity();
        const json entityJson = json::parse(R"({
            "id": 500,
            "Tag": {"name": "Coin"},
            "Transform": {"position": [1, 2, 3], "rotationDeg": [0, 90, 0], "scale": [2, 2, 2]},
            "MeshRenderer": {"meshPath": "assets/models/sphere.obj", "materialPath": "assets/materials/warm.material.json", "visible": false},
            "Motion": {"linearVelocity": [1, 0, 0], "angularVelocityDeg": [0, 2, 0]},
            "Rigidbody": {"mass": 2, "velocity": [3, 0, 0], "useGravity": false},
            "Collider": {"type": "sphere", "radius": 0.75, "isTrigger": true},
            "PlayerController": {"windowId": 1, "moveSpeed": 8},
            "WindowBinding": {"windowId": 1},
            "Camera": {"position": [0, 3, -10], "isPrimary": false},
            "CameraController": {"moveSpeed": 3},
            "Script": {"scripts": [
                {"module": "coin", "class": "Coin", "props": {"spin_speed": 120.5, "score_value": 2, "enabled": true, "label": "test", "nested": {"values": [1, null, false]}}},
                {"module": "other", "class": "Other"}
            ]}
        })");

        scene::DeserializeEntity(world, entity, entityJson);
        Check(world.GetEntities().size() == 1 && !world.IsAlive(500), "DeserializeEntity must ignore the saved id");
        const auto& script = world.Get<ecs::components::ScriptComponent>(entity);
        Check(script.scripts.size() == 2, "Multiple scripts were not loaded");
        Check(script.scripts[0].module == "coin" && script.scripts[0].className == "Coin", "Script identity was not loaded");
        Check(script.scripts[0].props == entityJson["Script"]["scripts"][0]["props"], "Script props changed");
        Check(script.scripts[1].props == json::object(), "Missing props must default to an empty object");

        const json saved = scene::SerializeEntity(world, entity);
        Check(saved["id"] == entity, "SerializeEntity must use the actual id");
        ecs::World restored;
        const auto restoredEntity = restored.CreateEntity();
        scene::DeserializeEntity(restored, restoredEntity, saved);
        Check(scene::SerializeEntity(restored, restoredEntity) == saved, "Component round-trip failed");

        // Replacing a component must replace the script list, not append to it.
        scene::DeserializeEntity(restored, restoredEntity, saved);
        Check(restored.Get<ecs::components::ScriptComponent>(restoredEntity).scripts.size() == 2, "Script list was duplicated");

        const auto emptyEntity = world.CreateEntity();
        world.Emplace<ecs::components::ScriptComponent>(emptyEntity);
        Check(scene::SerializeEntity(world, emptyEntity)["Script"]["scripts"] == json::array(), "Empty script list was lost");
    }

    void TestWorldSerialization(const std::shared_ptr<LoadEvents>& events)
    {
        // Children can precede parents in the file. Only parent is authoritative; children are rebuilt.
        const json root = json::parse(R"({"entities": [
            {"id": 13, "Tag": {"name": "Grandchild"}, "Hierarchy": {"parent": 11}},
            {"id": 11, "Tag": {"name": "ChildA"}, "Hierarchy": {"parent": 10}, "Script": {"scripts": [{"module": "coin", "class": "Coin", "props": {"score_value": 3}}]}},
            {"id": 12, "Tag": {"name": "ChildB"}, "Hierarchy": {"parent": 10}},
            {"id": 10, "Tag": {"name": "Parent"}, "Hierarchy": {"parent": 0, "children": [999]}}
        ]})");

        ecs::World world;
        events->expectedWorld = &world;
        unsigned updates = 0;
        world.AddUpdateSystem(std::make_unique<CountingSystem>(updates));
        const auto countBefore = events->count;
        Check(scene::LoadWorldFromString(world, root.dump()), "World load failed");
        Check(events->count == countBefore + 1 && events->completeWorld, "SceneLoadedEvent must see the complete world");
        const auto& parent = world.Get<ecs::components::HierarchyComponent>(10);
        Check(parent.children.size() == 2, "Sibling hierarchy links were lost");
        Check(world.Get<ecs::components::HierarchyComponent>(11).children == std::vector<ecs::EntityId>{13}, "Nested hierarchy links were lost");
        Check(world.Get<ecs::components::HierarchyComponent>(13).parent == 11, "Grandchild parent was lost");
        Check(world.SetParent(11, 10), "Setting the same parent should succeed");
        Check(world.Get<ecs::components::HierarchyComponent>(11).children.size() == 1, "Setting the same parent cleared descendants");
        Check(world.SetParent(12, 11), "Reparenting should succeed");
        Check(world.Get<ecs::components::HierarchyComponent>(10).children == std::vector<ecs::EntityId>{11}, "Reparenting left a stale child reference");
        Check(world.Get<ecs::components::HierarchyComponent>(11).children.size() == 2, "Reparenting cleared existing children");

        const std::string snapshot = scene::SerializeWorldToString(world);
        world.Get<ecs::components::ScriptComponent>(11).scripts[0].props["score_value"] = 99;
        world.CreateEntity();
        Check(scene::LoadWorldFromString(world, snapshot), "Snapshot restore failed");
        Check(world.GetEntities().size() == 4, "Snapshot did not remove spawned entities");
        Check(world.Get<ecs::components::ScriptComponent>(11).scripts[0].props["score_value"] == 3, "Snapshot did not restore props");
        Check(events->count == countBefore + 2, "Snapshot must publish SceneLoadedEvent");

        Check(!scene::LoadWorldFromString(world, "not json"), "Invalid JSON must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": 1, "Script": {"scripts": {}}}]})"), "Invalid script list must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": 1, "Script": {"scripts": [{"module": "coin", "class": "Coin", "props": []}]}}]})"), "Invalid props must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": 0}]})"), "Invalid entity id must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": -1}]})"), "Negative entity id must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": 1.5}]})"), "Fractional entity id must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"Tag": {"name": "MissingId"}}]})"), "Missing entity id must fail");
        Check(!scene::LoadWorldFromString(world, R"({"entities": [{"id": 1}, {"id": 1}]})"), "Duplicate entity id must fail");
        Check(world.GetEntities().size() == 4 && world.Get<ecs::components::ScriptComponent>(11).scripts[0].props["score_value"] == 3,
            "Failed loads must preserve the live world");
        Check(events->count == countBefore + 2, "A failed load must not publish SceneLoadedEvent");
        world.UpdateSystems(0.0f);
        Check(updates == 1, "Scene loads must preserve registered systems");
        Check(scene::LoadWorldFromString(world, R"({"entities": []})"), "Empty scene must load");
        Check(world.GetEntities().empty() && events->count == countBefore + 3, "Empty scene did not replace the world or publish an event");
        world.UpdateSystems(0.0f);
        Check(updates == 2, "Empty scene load discarded registered systems");
        events->expectedWorld = nullptr;
    }

    void TestSceneFiles(const std::shared_ptr<LoadEvents>& events)
    {
        ecs::World world;
        const auto sourceDir = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
        Check(scene::LoadWorldFromJson(world, sourceDir / "assets/scenes/benchmark.json"), "Existing benchmark scene must load");
        for (const auto entity : world.GetEntities())
        {
            Check(!world.Has<ecs::components::ScriptComponent>(entity), "Legacy scene gained a Script component");
        }

        Check(scene::LoadWorldFromJson(world, sourceDir / "assets/scenes/scripting_demo.json"), "Scripting demo must load");
        Check(world.Get<ecs::components::ScriptComponent>(7).scripts[0].props["spin_speed"] == 120.0, "Demo props were not loaded");

        // This unique directory is outside assets, so tests never save into a user's scene.
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto testDir = std::filesystem::temp_directory_path() / ("myengine-scene-tests-" + std::to_string(uniqueId));
        const auto scenePath = testDir / "scene.json";
        Check(scene::SaveWorldToJson(world, scenePath), "Saving a scene failed");
        ecs::World restored;
        const auto countBefore = events->count;
        Check(scene::LoadWorldFromJson(restored, scenePath), "Loading a saved scene failed");
        Check(events->count == countBefore + 1, "File load must publish SceneLoadedEvent");
        for (const auto entity : world.GetEntities())
        {
            Check(scene::SerializeEntity(restored, entity) == scene::SerializeEntity(world, entity), "File round-trip changed components");
        }
        Check(!scene::LoadWorldFromJson(restored, testDir / "missing.json"), "Missing scene must fail");
        Check(events->count == countBefore + 1, "Missing scene must not publish SceneLoadedEvent");
        std::filesystem::remove_all(testDir);
    }
}

int main()
{
    try
    {
        auto events = std::make_shared<LoadEvents>();
        myengine::core::ServiceLocator::GetEventBus().Subscribe<scene::SceneLoadedEvent>(
            [events](const scene::SceneLoadedEvent& event)
            {
                ++events->count;
                if (event.world == events->expectedWorld && event.world != nullptr)
                {
                    events->completeWorld = event.world->GetEntities().size() == 4 &&
                        event.world->Has<ecs::components::ScriptComponent>(11) &&
                        event.world->Get<ecs::components::HierarchyComponent>(13).parent == 11;
                }
            });

        TestEntitySerialization();
        TestWorldSerialization(events);
        TestSceneFiles(events);
        std::cout << "OK: entity and scene round-trips, Script props, hierarchy, snapshots and SceneLoadedEvent\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
}
