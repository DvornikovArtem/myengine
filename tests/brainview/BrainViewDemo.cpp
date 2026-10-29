// BrainViewDemo.cpp
// V0 (FlyDoom): the 3D fly brain on a real connectome without the editor (manual, not part of CTest): it loads a .fbrain, runs the brain on its thread, draws the cloud into
// a render target every frame and either measures (--bench: frame time, FPS, GPU time of the cloud pass, the simulation next to it) or saves frames of the picture as
// TGA (--out <dir> --every K) for the evidence of the task. The HUD is ImGui and is not in these frames: the editor window shows it.
//
//   myengine_brainview_demo --fbrain <file> [--preset vision|rest|pain|taste|everything|silence] [--size 960x540] [--seconds 12] [--warm 3]
//                           [--out <dir> [--every 15]] [--yaw 0.6] [--distance 3.2] [--vsync] [--no-brain]

#include <brainview/BrainRunner.h>
#include <brainview/BrainView.h>

#include <flybrain/Connectome.h>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace brainview;
    using Clock = std::chrono::steady_clock;

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void PumpMessages()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // RGBA8 rows from the top to a 32 bit TGA (bottom-up, BGRA)
    bool WriteTga(const fs::path& path, const std::vector<std::uint8_t>& rgba, const std::uint32_t width, const std::uint32_t height)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        std::uint8_t header[18] = {};
        header[2] = 2;
        header[12] = static_cast<std::uint8_t>(width & 0xFF);
        header[13] = static_cast<std::uint8_t>(width >> 8);
        header[14] = static_cast<std::uint8_t>(height & 0xFF);
        header[15] = static_cast<std::uint8_t>(height >> 8);
        header[16] = 32;
        header[17] = 0x28;   // top-left origin, 8 bits of alpha
        stream.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                row[4ull * x + 0] = p[2];
                row[4ull * x + 1] = p[1];
                row[4ull * x + 2] = p[0];
                row[4ull * x + 3] = 255;
            }
            stream.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        return static_cast<bool>(stream);
    }

    StimulusPreset ParsePreset(const std::string& text)
    {
        if (text == "silence") return StimulusPreset::Silence;
        if (text == "rest") return StimulusPreset::Rest;
        if (text == "pain") return StimulusPreset::Pain;
        if (text == "taste") return StimulusPreset::Taste;
        if (text == "everything") return StimulusPreset::Everything;
        return StimulusPreset::Vision;
    }
}

int main(int argc, char** argv)
{
    std::string fbrain;
    std::string out;
    std::string preset = "vision";
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    double seconds = 12.0;
    double warm = 3.0;
    int every = 15;
    float yaw = 0.6f;
    float distance = 3.2f;
    bool vsync = false;
    bool noBrain = false;
    bool rotate = false;
    double interval = 0.0;
    ViewStyle style;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--fbrain") fbrain = next();
        else if (arg == "--out") out = next();
        else if (arg == "--preset") preset = next();
        else if (arg == "--seconds") seconds = std::atof(next().c_str());
        else if (arg == "--warm") warm = std::atof(next().c_str());
        else if (arg == "--every") every = std::max(1, std::atoi(next().c_str()));
        else if (arg == "--yaw") yaw = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--distance") distance = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--vsync") vsync = true;
        else if (arg == "--no-brain") noBrain = true;
        else if (arg == "--rotate") rotate = true;
        else if (arg == "--interval") interval = std::atof(next().c_str());
        else if (arg == "--flash") style.flashBrightness = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--rest") style.baseBrightness = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--fade") style.decaySeconds = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--size") { const std::string v = next(); if (v.find('x') != std::string::npos) { sscanf_s(v.c_str(), "%ux%u", &width, &height); } else { style.baseSizePixels = static_cast<float>(std::atof(v.c_str())); } }
        else if (arg == "--flashsize") style.flashSizePixels = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--glow") style.bloomIntensity = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--threshold") style.bloomThreshold = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--exposure") style.exposure = static_cast<float>(std::atof(next().c_str()));
    }
    if (fbrain.empty())
    {
        std::cerr << "usage: myengine_brainview_demo --fbrain <file> [--preset vision|rest|pain|taste|everything|silence] [--size 960x540] [--seconds 12] [--warm 3] [--out <dir> [--every 15]]\n";
        return 2;
    }

    myengine::jobs::Initialize();
    int result = 0;
    {
        myengine::core::Logger logger;
        logger.Initialize(fs::temp_directory_path() / "myengine_brainview_demo.log");

        const auto loadStart = Clock::now();
        std::string error;
        const auto connectome = flybrain::Connectome::Load(fbrain, &error);
        if (connectome == nullptr)
        {
            std::cerr << "cannot load " << fbrain << ": " << error << "\n";
            myengine::jobs::Shutdown();
            return 1;
        }
        std::cout << "connectome: " << connectome->NeuronCount() << " neurons, " << connectome->EdgeCount() << " synapses, loaded in "
                  << std::chrono::duration<double>(Clock::now() - loadStart).count() << " s\n";

        myengine::render::dx12::Dx12RenderAdapter adapter(logger);
        if (!adapter.Initialize())
        {
            std::cerr << "no DX12 device\n";
            myengine::jobs::Shutdown();
            return 1;
        }
        std::cout << "adapter: " << adapter.GetAdapterName() << "\n";
        adapter.SetVSync(vsync);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MyEngineBrainViewDemo";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"brainview demo", WS_OVERLAPPEDWINDOW, 50, 50, static_cast<int>(width), static_cast<int>(height), nullptr, nullptr,
                                    windowClass.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        const auto surface = adapter.CreateSurface(hwnd, width, height);
        const auto target = adapter.CreateRenderTarget(width, height);
        if (!surface.IsValid() || !target.IsValid())
        {
            std::cerr << "no surface or target\n";
            return 1;
        }

        BrainView view(adapter);
        const auto cloudStart = Clock::now();
        if (!view.Create(*connectome))
        {
            std::cerr << "the point cloud could not be made\n";
            return 1;
        }
        std::cout << "cloud: " << view.NeuronCount() << " points (" << view.Model().withoutPosition << " without a position: " << view.Model().estimated << " drawn at their targets, " << view.Model().hidden << " not drawn), groups:";
        for (std::size_t g = 0; g < view.Groups().size(); ++g)
        {
            std::cout << " " << view.Groups()[g].label << "=" << view.Activity().GroupSize(g);
        }
        std::cout << ", made in " << std::chrono::duration<double>(Clock::now() - cloudStart).count() << " s\n";
        {
            // where the dataset puts the neurons: the extent of every axis of the raw positions and of the normalised cloud (the layout is checked against this)
            double lo[3] = {1e30, 1e30, 1e30};
            double hi[3] = {-1e30, -1e30, -1e30};
            double nlo[3] = {1e30, 1e30, 1e30};
            double nhi[3] = {-1e30, -1e30, -1e30};
            for (std::uint32_t i = 0; i < connectome->NeuronCount(); ++i)
            {
                const float* p = connectome->Position(i);
                if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f)
                {
                    continue;
                }
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = std::min<double>(lo[k], p[k]);
                    hi[k] = std::max<double>(hi[k], p[k]);
                    const double n = view.Model().positions[3ull * i + static_cast<std::size_t>(k)];
                    nlo[k] = std::min(nlo[k], n);
                    nhi[k] = std::max(nhi[k], n);
                }
            }
            std::printf("raw extent x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            std::printf("normalised extent x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", nlo[0], nhi[0], nlo[1], nhi[1], nlo[2], nhi[2]);
            std::map<std::string, std::uint32_t> missingBySuperclass;
            for (std::uint32_t i = 0; i < connectome->NeuronCount(); ++i)
            {
                const float* p = connectome->Position(i);
                if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f)
                {
                    ++missingBySuperclass[std::string(connectome->Superclass(i))];
                }
            }
            for (const auto& [name, count] : missingBySuperclass)
            {
                std::printf("  without a position: %-24s %u\n", name.c_str(), count);
            }
            std::map<std::string, std::uint32_t> bySuperclass;
            for (std::uint32_t i = 0; i < connectome->NeuronCount(); ++i)
            {
                ++bySuperclass[std::string(connectome->Superclass(i))];
            }
            for (const auto& [name, count] : bySuperclass)
            {
                std::printf("  superclass %-24s %u\n", name.c_str(), count);
            }
        }
        view.Style() = style;
        view.Camera().yaw = yaw;
        view.Camera().distance = distance;
        view.Camera().autoRotate = out.empty() || rotate;

        std::unique_ptr<BrainRunner> runner;
        if (!noBrain)
        {
            runner = std::make_unique<BrainRunner>(connectome);
            runner->SetPreset(ParsePreset(preset));
            runner->Start();
        }

        if (!out.empty())
        {
            fs::create_directories(out);
        }

        std::vector<double> frameMs;
        std::vector<double> gpuMs;
        std::vector<double> cloudMs;
        std::vector<BrainRunner::Spike> spikes;
        std::uint64_t flashedTotal = 0;
        std::uint64_t frames = 0;
        int saved = 0;
        double lastSaved = -1.0e9;
        const auto begin = Clock::now();
        auto last = begin;
        while (true)
        {
            PumpMessages();
            const auto frameStart = Clock::now();
            const double elapsed = std::chrono::duration<double>(frameStart - begin).count();
            if (elapsed >= warm + seconds)
            {
                break;
            }
            const double dt = std::chrono::duration<double>(frameStart - last).count();
            last = frameStart;
            view.Advance(dt);
            if (runner != nullptr)
            {
                double simNow = 0.0;
                runner->Drain(spikes, simNow);
                for (const BrainRunner::Spike& spike : spikes)
                {
                    view.Flash(spike.neuron, simNow - spike.simSeconds);
                }
                flashedTotal += spikes.size();
            }
            if (!adapter.BeginFrame(surface, myengine::core::Color{0.0f, 0.0f, 0.0f, 1.0f}))
            {
                std::cerr << "BeginFrame failed\n";
                result = 1;
                break;
            }
            if (!view.Render(surface, target, width, height))
            {
                std::cerr << "the cloud was not drawn\n";
                result = 1;
                adapter.EndFrame(surface);
                break;
            }
            adapter.EndFrame(surface);
            ++frames;
            if (elapsed >= warm)
            {
                frameMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
                myengine::render::GpuFrameStats stats;
                if (adapter.GetLastFrameStats(stats, false) && stats.gpuMs >= 0.0)
                {
                    gpuMs.push_back(stats.gpuMs);
                    for (const auto& pass : stats.passes)
                    {
                        if (pass.renderTarget && pass.gpuMs >= 0.0)
                        {
                            cloudMs.push_back(pass.gpuMs);
                        }
                    }
                }
            }
            if (!out.empty() && elapsed >= warm && (interval > 0.0 ? elapsed - lastSaved >= interval : static_cast<int>(frames) % every == 0))
            {
                std::vector<std::uint8_t> pixels;
                if (adapter.ReadRenderTargetPixels(target, pixels))
                {
                    lastSaved = elapsed;
                    char name[64];
                    std::snprintf(name, sizeof(name), "frame_%04d.tga", saved++);
                    WriteTga(fs::path(out) / name, pixels, width, height);
                }
            }
        }

        const double wall = std::chrono::duration<double>(Clock::now() - begin).count() - warm;
        std::sort(frameMs.begin(), frameMs.end());
        const auto percentile = [](const std::vector<double>& sorted, const double p) { return sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(sorted.size() * p))]; };
        double sum = 0.0;
        for (const double ms : frameMs)
        {
            sum += ms;
        }
        const double average = frameMs.empty() ? 0.0 : sum / static_cast<double>(frameMs.size());
        std::sort(gpuMs.begin(), gpuMs.end());
        std::sort(cloudMs.begin(), cloudMs.end());
        std::printf("frames measured: %zu in %.1f s -> %.1f FPS\n", frameMs.size(), wall, static_cast<double>(frameMs.size()) / std::max(wall, 1e-9));
        std::printf("CPU frame (loop body): avg %.2f ms, median %.2f, p95 %.2f, p99 %.2f, max %.2f\n", average, percentile(frameMs, 0.5), percentile(frameMs, 0.95), percentile(frameMs, 0.99),
                    frameMs.empty() ? 0.0 : frameMs.back());
        std::printf("GPU frame: median %.2f ms, p95 %.2f, max %.2f; the cloud pass: median %.2f ms, p95 %.2f\n", percentile(gpuMs, 0.5), percentile(gpuMs, 0.95), gpuMs.empty() ? 0.0 : gpuMs.back(),
                    percentile(cloudMs, 0.5), percentile(cloudMs, 0.95));
        std::printf("flashes fed: %llu (%.0f per frame)\n", static_cast<unsigned long long>(flashedTotal), static_cast<double>(flashedTotal) / std::max<std::uint64_t>(frames, 1));
        if (runner != nullptr)
        {
            const BrainRunner::Stats stats = runner->GetStats();
            std::printf("brain: %.2f s simulated, %.2fx real time, %.0f spikes/s, %u awake neurons\n", stats.simSeconds, stats.realtimeRatio, stats.spikesPerSecond, stats.awakeNeurons);
            runner->Stop();
        }
        if (!out.empty())
        {
            std::printf("saved %d frames to %s\n", saved, out.c_str());
        }
        view.Destroy();
        adapter.DestroyRenderTarget(target);
        adapter.Shutdown();
        DestroyWindow(hwnd);
    }
    myengine::jobs::Shutdown();
    return result;
}
