// FramePacer.cpp

#include <myengine/core/FramePacer.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::core
{
    namespace
    {
        constexpr auto kSpin = std::chrono::microseconds(1500);  // the timer wakes up this much early: the rest is a spin (the timer is late by up to about a millisecond)
    }

    FramePacer::FramePacer()
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }

    FramePacer::~FramePacer()
    {
        if (timer_ != nullptr)
        {
            CloseHandle(timer_);
        }
    }

    void FramePacer::Wait(const int framesPerSecond)
    {
        if (framesPerSecond <= 0)
        {
            limit_ = 0;
            return;
        }

        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / static_cast<double>(framesPerSecond)));
        const auto now = std::chrono::steady_clock::now();
        if (limit_ != framesPerSecond || next_ == std::chrono::steady_clock::time_point{})
        {
            limit_ = framesPerSecond;
            next_ = now;   // a new grid: this frame ends here, the next one is one period away
        }
        next_ += period;
        if (next_ + period < now)
        {
            next_ = now;   // more than a frame late (a hitch, a loading): start over instead of running frames without a wait to catch up
        }
        WaitUntil(next_);
    }

    void FramePacer::WaitUntil(const std::chrono::steady_clock::time_point target)
    {
        auto remaining = target - std::chrono::steady_clock::now();
        if (remaining > kSpin)
        {
            const auto sleep = remaining - kSpin;
            if (timer_ != nullptr)
            {
                LARGE_INTEGER due;
                due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(sleep).count() / 100;   // negative: relative, in 100 ns
                if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(timer_, INFINITE);
                }
            }
            else
            {
                // no high resolution timer: Sleep(1) lasts a clock tick or more, so only a long wait uses it
                if (sleep > std::chrono::milliseconds(16))
                {
                    Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(sleep).count() - 15));
                }
            }
        }
        while (std::chrono::steady_clock::now() < target)
        {
            YieldProcessor();
        }
    }
}
