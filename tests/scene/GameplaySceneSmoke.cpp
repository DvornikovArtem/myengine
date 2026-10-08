#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

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

    // Goes through the real application loop: rendering, physics, scripts and Play snapshot shutdown.
    class PlayRound final : public ecs::IUpdateSystem
    {
    public:
        void Update(ecs::World& world, float) override
        {
            ++frames_;
            ecs::EntityId firstCoin = ecs::kInvalidEntity;
            world.ForEach<components::TagComponent>([&](const ecs::EntityId entity, const components::TagComponent& tag)
                {
                    if (tag.name.rfind("Coin_5_", 0) == 0)
                    {
                        sawCoins = true;
                        firstCoin = entity;
                    }
                    sawEnemies = sawEnemies || tag.name.rfind("Chaser_6_", 0) == 0;
                    sawFireworks = sawFireworks || tag.name.rfind("Firework_4_", 0) == 0;
                });
            sawFault = sawFault || myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.faultedInstances != 0;
            if (frames_ == 20 && firstCoin != ecs::kInvalidEntity)
            {
                // The next physics step must publish a real pickup event, then Python starts the fireworks.
                world.Get<components::TransformComponent>(2).position = world.Get<components::TransformComponent>(firstCoin).position;
                world.Get<components::RigidbodyComponent>(2).velocity = {};
            }
            if (frames_ >= 120)
            {
                PostQuitMessage(0);
            }
        }

        bool sawCoins = false;
        bool sawEnemies = false;
        bool sawFireworks = false;
        bool sawFault = false;

    private:
        unsigned frames_ = 0;
    };
}

int main()
{
    std::filesystem::path scenePath;
    try
    {
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        scenePath = std::filesystem::temp_directory_path() /
            std::filesystem::u8path("myengine T6 сцена " + std::to_string(uniqueId) + ".json");
        std::filesystem::copy_file(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/scenes/coin_guard_demo.json", scenePath);
        myengine::core::Application app;
        if (!app.Initialize(myengine::config::AppConfig::Default(), scenePath))
        {
            throw std::runtime_error("Application initialization failed; see logs/myengine.log");
        }
        // Temporary Play-only settings: Shutdown must save the original scene, not this winning round.
        app.GetWorld().Get<components::ScriptComponent>(4).scripts[0].props["target_score"] = 1;
        app.GetWorld().Get<components::ScriptComponent>(6).scripts[0].props["initial_delay"] = 0.0;
        auto round = std::make_unique<PlayRound>();
        const auto* result = round.get();
        app.GetWorld().AddUpdateSystem(std::move(round));
        if (app.Run() != 0)
        {
            throw std::runtime_error("Application loop failed");
        }
        const bool passed = result->sawCoins && result->sawEnemies && result->sawFireworks && !result->sawFault;
        app.Shutdown();
        if (!passed)
        {
            throw std::runtime_error("The rendered round did not spawn coins, enemies and victory particles without script faults");
        }
        std::ifstream file(scenePath);
        nlohmann::json saved;
        file >> saved;
        if (saved.at("entities").size() != 6)
        {
            throw std::runtime_error("Shutdown saved runtime-spawned entities into the scene");
        }
        for (const auto& entity : saved.at("entities"))
        {
            if (entity.at("id") == 4 && entity.at("Script").at("scripts").at(0).at("props").at("target_score") != 10)
            {
                throw std::runtime_error("Shutdown did not restore the original round settings");
            }
        }
        file.close();
        std::filesystem::remove(scenePath);
        std::cout << "OK: T6 application, 120 frames, coins, enemies, physics pickup, victory fireworks and Play snapshot shutdown\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\nTemporary scene: " << scenePath.u8string() << '\n';
        return 1;
    }
}
