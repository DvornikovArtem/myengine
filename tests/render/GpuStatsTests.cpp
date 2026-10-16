// GPU diagnostics without a GPU: the statistics text, the capture tool lookup.

#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <myengine/render/GpuCapture.h>
#include <myengine/render/GpuStats.h>

namespace
{
    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    myengine::render::GpuFrameStats MakeFrame()
    {
        using namespace myengine::render;
        GpuFrameStats stats;
        stats.frame = 120;
        stats.gpuMs = 1.25;
        stats.draws = 43;
        stats.triangles = 15996;
        stats.psoSwitches = 3;
        stats.textureSwitches = 14;
        stats.barriers = 2;
        stats.renderTargetPasses = 1;

        GpuPassStats frame;
        frame.name = "Frame";
        frame.gpuMs = 1.25;
        GpuPassStats scene;
        scene.name = "Scene";
        scene.depth = 1;
        scene.draws = 14;
        scene.triangles = 13308;
        scene.gpuMs = 0.5;
        GpuPassStats thumbnail;
        thumbnail.name = "Thumbnail: coin.prefab.json";
        thumbnail.depth = 1;
        thumbnail.renderTarget = true;
        thumbnail.width = 128;
        thumbnail.height = 128;
        thumbnail.draws = 2;
        stats.passes = {frame, scene, thumbnail};
        return stats;
    }

    void TestJson()
    {
        const auto stats = MakeFrame();
        const auto json = nlohmann::json::parse(myengine::render::GpuFrameStatsToJson(stats, "coin_guard_demo", "Test GPU"));
        Check(json["map"] == "coin_guard_demo" && json["adapter"] == "Test GPU" && json["frame"] == 120, "header");
        Check(json["totals"]["draws"] == 43 && json["totals"]["triangles"] == 15996, "totals");
        Check(json["passes"].size() == 3, "passes");
        Check(json["passes"][1]["name"] == "Scene" && json["passes"][1]["draws"] == 14, "scene pass");
        Check(!json["passes"][1].contains("renderTarget"), "a pass of the frame has no target");
        Check(json["passes"][2]["renderTarget"]["width"] == 128, "render target size");
    }

    void TestMarkdown()
    {
        const auto stats = MakeFrame();
        const std::string text = myengine::render::GpuFrameStatsToMarkdown(stats, "coin_guard_demo", "Test GPU");
        Check(text.find("# GPU frame 120 of `coin_guard_demo`") != std::string::npos, "title");
        Check(text.find("| Scene | 14 | 13308 |") != std::string::npos || text.find("  Scene | 14 | 13308 |") != std::string::npos,
            "the scene row");
        Check(text.find("128x128") != std::string::npos, "target size");
        Check(text.find("1.250") != std::string::npos, "GPU time");

        myengine::render::GpuFrameStats unmeasured;
        unmeasured.passes.push_back({});
        Check(myengine::render::GpuFrameStatsToMarkdown(unmeasured, "x", "y").find("| - |") != std::string::npos,
            "a pass without timing shows a dash");
    }

    // Whether RenderDoc is installed on the machine or not, the lookup answers and says what to do
    void TestCaptureLookup()
    {
        myengine::render::GpuCapture capture;
        std::string message;
        const bool found = capture.LoadRenderDoc(message);
        Check(!message.empty(), "LoadRenderDoc must say what it found or what to install");
        Check(found == capture.IsRenderDocAvailable(), "availability follows the result");
        if (!found)
        {
            Check(message.find("winget install") != std::string::npos, "the hint names the installer");
            std::string trigger;
            Check(!capture.TriggerNextFrame(trigger) && !trigger.empty(), "a capture without RenderDoc fails with a message");
            Check(capture.CaptureCount() == 0 && !capture.IsCapturing() && capture.LastCapturePath().empty(), "nothing captured");
        }
    }
}

int main()
{
    try
    {
        TestJson();
        TestMarkdown();
        TestCaptureLookup();
        std::cout << "OK: gpu stats: json, markdown, capture tool lookup\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
