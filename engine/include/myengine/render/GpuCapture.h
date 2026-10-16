// GpuCapture.h: frame capture through RenderDoc's in-application API (and loading PIX's capturer).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace myengine::render
{
    // The tools are optional: nothing here fails the engine when they are not installed, Load just says why.
    // Load must come before the D3D12 device is created: RenderDoc hooks the API from the moment its DLL is in the
    // process (when the engine is started from RenderDoc, it is there already).
    class GpuCapture
    {
    public:
        GpuCapture();
        ~GpuCapture();

        GpuCapture(const GpuCapture&) = delete;
        GpuCapture& operator=(const GpuCapture&) = delete;

        // Finds RenderDoc (injected, RENDERDOC_DLL, Program Files, PATH). `message` is a line for the log / the user:
        // what was found, or what to install.
        bool LoadRenderDoc(std::string& message);
        // Loads WinPixGpuCapturer.dll of the newest PIX install so that PIX can take GPU captures of this process
        bool LoadPix(std::string& message);

        bool IsRenderDocAvailable() const { return api_ != nullptr; }
        bool IsPixLoaded() const { return pixLoaded_; }

        // Captures go to <directory>/<name>_<time>.rdc
        void SetOutput(const std::filesystem::path& directory, const std::string& name);

        // The next presented frame is captured. false: RenderDoc is not there (message says what to do).
        bool TriggerNextFrame(std::string& message);
        bool IsCapturing() const;
        std::uint32_t CaptureCount() const;
        // The newest capture file; empty if none
        std::string LastCapturePath() const;

        // Hints for the user: how to get the tools
        static const char* InstallHint();

    private:
        void* api_ = nullptr; // RENDERDOC_API_1_6_0*
        bool pixLoaded_ = false;
        std::filesystem::path directory_;
    };
}
