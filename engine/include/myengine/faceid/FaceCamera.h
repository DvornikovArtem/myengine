// FaceCamera.h
// The web camera of Face ID: Media Foundation (IMFSourceReader), 640 x 480 at 30 frames per second, RGB32 converted to RGB, a thread of its own. The camera is on only between
// Start and Stop (the light of the camera tells it). A camera that is missing, busy or forbidden by the privacy settings of Windows is a message in Status, not a crash.

#pragma once

#include <myengine/faceid/FaceTypes.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace myengine::faceid
{
    struct CameraInfo
    {
        std::string name;           // UTF-8, for the list of the settings
        std::wstring symbolicLink;  // what the camera is opened by
    };

    class FaceCamera
    {
    public:
        FaceCamera() = default;
        ~FaceCamera();
        FaceCamera(const FaceCamera&) = delete;
        FaceCamera& operator=(const FaceCamera&) = delete;

        // The cameras of the computer (may take a moment: ask once, not every frame)
        static std::vector<CameraInfo> List();

        // Opens the camera whose name is `name` (empty or not found: the first one). Returns at once: the opening happens on the thread, Status says how it went.
        void Start(const std::string& name, int width = 640, int height = 480, int fps = 30);
        void Stop();
        bool Running() const { return running_.load(); }

        // The newest frame if it is newer than `serial` (the serial of the last one that the caller took); false: nothing new
        bool Latest(FaceImage& out, std::uint64_t& serial);
        std::uint64_t FrameSerial() const { return serial_.load(); }

        enum class State
        {
            Stopped,
            Opening,
            Streaming,
            Failed
        };
        State GetState() const { return state_.load(); }
        // Why the camera is not streaming ("no camera found", "access denied: switch on the access of apps to the camera in the privacy settings of Windows", ...)
        std::string Status() const;

    private:
        void Run(std::string name, int width, int height, int fps);
        void SetStatus(State state, const std::string& text);

        std::thread thread_;
        std::atomic<bool> running_{false};
        std::atomic<bool> stopRequested_{false};
        std::atomic<State> state_{State::Stopped};
        std::atomic<std::uint64_t> serial_{0};
        mutable std::mutex mutex_;
        FaceImage latest_;
        std::string status_;
    };
}
