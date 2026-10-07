#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Application.h>
#include <myengine/ecs/System.h>
#include <myengine/ecs/components/ScriptComponent.h>

namespace
{
    // Uses the normal application loop and shutdown, without needing to click a test window.
    class QuitAfterFrames final : public myengine::ecs::IUpdateSystem
    {
    public:
        void Update(myengine::ecs::World&, float) override
        {
            if (++frames_ >= 120)
            {
                PostQuitMessage(0);
            }
        }

    private:
        unsigned frames_ = 0;
    };
}

int main()
{
    try
    {
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto scenePath = std::filesystem::temp_directory_path() /
            std::filesystem::u8path("myengine scripting сцена " + std::to_string(uniqueId) + ".json");
        std::filesystem::copy_file(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/scenes/scripting_demo.json", scenePath);

        myengine::core::Application app;
        if (!app.Initialize(myengine::config::AppConfig::Default(), scenePath))
        {
            throw std::runtime_error("Application initialization failed; see logs/myengine.log");
        }
        if (app.GetSceneSavePath() != scenePath)
        {
            throw std::runtime_error("Application is not saving into the selected scene");
        }
        auto& input = app.GetInputManager();
        input.OnKeyDown('E');
        if (!input.IsActionDown("camera_up"))
        {
            throw std::runtime_error("E is not bound to camera up");
        }
        input.OnKeyUp('E');
        input.OnKeyDown('Q');
        if (!input.IsActionDown("camera_down"))
        {
            throw std::runtime_error("Q is not bound to camera down");
        }
        input.OnKeyUp('Q');
        app.GetWorld().Get<myengine::ecs::components::ScriptComponent>(7).scripts[0].props["score_value"] = 99;
        app.GetWorld().AddUpdateSystem(std::make_unique<QuitAfterFrames>());
        if (app.Run() != 0)
        {
            throw std::runtime_error("Application loop failed");
        }
        app.Shutdown();

        std::ifstream file(scenePath);
        nlohmann::json saved;
        file >> saved;
        bool propsRestored = false;
        for (const auto& entityJson : saved["entities"])
        {
            if (entityJson["id"] == 7)
            {
                propsRestored = entityJson["Script"]["scripts"][0]["props"]["score_value"] == 1;
            }
        }
        if (!propsRestored)
        {
            throw std::runtime_error("Shutdown did not restore and save Script props from the Play snapshot");
        }
        file.close();
        std::filesystem::remove(scenePath);
        std::cout << "OK: application startup, selected scene, 120 frames, Play snapshot save and shutdown\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
}
