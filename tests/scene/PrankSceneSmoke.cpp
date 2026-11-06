// Manual integration test of the mini-scenes of the easter egg generator (assets/prank/scenes, docs/editor/prank-tab.md): plays one of them in a DX12 window
// and checks that the game runs: no script fault, and the pieces of that game appear (coins, tentacles, fireballs, the ball that is reset after a goal).
//   myengine_prank_smoke octopus|fox|lava|hardcore|nakedcars

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Application.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/System.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>

namespace
{
    namespace ecs = myengine::ecs;
    namespace components = ecs::components;

    class PlayRound final : public ecs::IUpdateSystem
    {
    public:
        explicit PlayRound(std::string id) : id_(std::move(id)) {}

        void Update(ecs::World& world, float) override
        {
            ++frames_;
            ecs::EntityId ball = ecs::kInvalidEntity;
            world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& tag)
                {
                    sawCoins = sawCoins || tag.name.rfind("Coin_5_", 0) == 0;
                    sawChasers = sawChasers || tag.name.rfind("Chaser_6_", 0) == 0;
                    sawFireballs = sawFireballs || tag.name.rfind("Fireball_7_", 0) == 0;
                    if (tag.name == "\xD0\x9C\xD1\x8F\xD1\x87") // "Мяч"
                    {
                        ball = entity;
                    }
                });
            sawFault = sawFault || myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.faultedInstances != 0;
            if (id_ == "nakedcars" && ball != ecs::kInvalidEntity)
            {
                if (frames_ == 40)
                {
                    // a goal: the ball goes into the gates, and the script must take it back to the middle
                    world.Get<components::TransformComponent>(ball).position = {0.0f, 0.6f, 13.6f};
                    world.Get<components::RigidbodyComponent>(ball).velocity = {};
                }
                if (frames_ > 120 && world.Get<components::TransformComponent>(ball).position.z < 8.0f)
                {
                    ballWasReset = true;
                }
            }
            // The scripts count seconds, not frames (a frame of the empty window is a millisecond): 8 s of the game
            if (frames_ >= 300 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() >= 8.0)
            {
                PostQuitMessage(0);
            }
        }

        bool sawCoins = false;
        bool sawChasers = false;
        bool sawFireballs = false;
        bool sawFault = false;
        bool ballWasReset = false;

    private:
        std::string id_;
        unsigned frames_ = 0;
        std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    };
}

int main(const int argc, char** argv)
{
    const std::string id = argc > 1 ? argv[1] : "octopus";
    std::filesystem::path scenePath;
    try
    {
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        scenePath = std::filesystem::temp_directory_path() / ("myengine_prank_" + id + "_" + std::to_string(uniqueId) + ".json");
        std::filesystem::copy_file(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/prank/scenes" / (id + ".json"), scenePath);
        myengine::core::Application app;
        if (!app.Initialize(myengine::config::AppConfig::Default(), scenePath, true))
        {
            throw std::runtime_error("Application initialization failed; see logs/myengine.log");
        }
        auto round = std::make_unique<PlayRound>(id);
        const auto* result = round.get();
        app.GetWorld().AddUpdateSystem(std::move(round));
        if (app.Run() != 0)
        {
            throw std::runtime_error("Application loop failed");
        }
        bool passed = !result->sawFault;
        std::string expected = "no script fault";
        if (id == "octopus")
        {
            passed = passed && result->sawCoins && result->sawChasers;
            expected += ", coins, tentacles";
        }
        else if (id == "fox")
        {
            passed = passed && result->sawCoins && !result->sawChasers;
            expected += ", coins, no enemies";
        }
        else if (id == "lava")
        {
            passed = passed && result->sawCoins && result->sawFireballs;
            expected += ", coins, fireballs";
        }
        else if (id == "hardcore")
        {
            passed = passed && result->sawCoins && result->sawChasers;
            expected += ", coins, chasers";
        }
        else if (id == "nakedcars")
        {
            passed = passed && result->ballWasReset;
            expected += ", the ball is reset after a goal";
        }
        app.Shutdown();
        std::filesystem::remove(scenePath);
        if (!passed)
        {
            throw std::runtime_error("The mini-scene \"" + id + "\" did not show: " + expected);
        }
        std::cout << "OK: mini-scene " << id << ", 8 s: " << expected << "\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\nTemporary scene: " << scenePath.u8string() << '\n';
        return 1;
    }
}
