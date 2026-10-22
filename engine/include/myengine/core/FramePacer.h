// FramePacer.h

#pragma once

#include <chrono>

namespace myengine::core
{
    // The limit of the frames per second of the main loop (Video > Frame rate limit). The frames stand on a grid: each one ends one period after the
    // previous one, so a short frame does not shift the rest and the rate is exact on average. The wait is a high resolution timer (Sleep(1) lasts up to 15.6 ms
    // with the default clock of Windows and would turn 144 into 64) and a spin for the last bit.
    class FramePacer
    {
    public:
        FramePacer();
        ~FramePacer();
        FramePacer(const FramePacer&) = delete;
        FramePacer& operator=(const FramePacer&) = delete;

        // Waits until the end of the frame that the limit allows. 0 (or less): no limit, the grid is forgotten.
        void Wait(int framesPerSecond);

    private:
        void WaitUntil(std::chrono::steady_clock::time_point target);

        void* timer_ = nullptr;   // HANDLE of a high resolution waitable timer (nullptr: the system has none)
        int limit_ = 0;
        std::chrono::steady_clock::time_point next_{};
    };
}
