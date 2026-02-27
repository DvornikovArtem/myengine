// Timer.cpp

#include <myengine/core/Timer.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::core
{
    Timer::Timer()
        : secondsPerCount_(0.0),
            deltaTime_(-1.0),
            baseTime_(0),
            pausedTime_(0),
            stopTime_(0),
            prevTime_(0),
            currTime_(0),
            stopped_(false)
    {
        long long countsPerSec = 0;
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&countsPerSec));
        secondsPerCount_ = 1.0 / static_cast<double>(countsPerSec);
    }

    void Timer::Reset()
    {
        long long currentTime = 0;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentTime));

        baseTime_ = currentTime;
        prevTime_ = currentTime;
        stopTime_ = 0;
        stopped_ = false;
        pausedTime_ = 0;
    }

    void Timer::Start()
    {
        if (!stopped_)
        {
            return;
        }

        long long startTime = 0;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&startTime));

        pausedTime_ += (startTime - stopTime_);
        prevTime_ = startTime;
        stopTime_ = 0;
        stopped_ = false;
    }

    void Timer::Stop()
    {
        if (stopped_)
        {
            return;
        }

        long long currentTime = 0;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentTime));

        stopTime_ = currentTime;
        stopped_ = true;
    }

    void Timer::Tick()
    {
        if (stopped_)
        {
            deltaTime_ = 0.0;
            return;
        }

        long long currentTime = 0;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentTime));
        currTime_ = currentTime;

        deltaTime_ = (currTime_ - prevTime_) * secondsPerCount_;
        prevTime_ = currTime_;

        if (deltaTime_ < 0.0)
        {
            deltaTime_ = 0.0;
        }
    }

    float Timer::DeltaTime() const
    {
        return static_cast<float>(deltaTime_);
    }

    float Timer::TotalTime() const
    {
        if (stopped_)
        {
            return static_cast<float>(((stopTime_ - pausedTime_) - baseTime_) * secondsPerCount_);
        }

        return static_cast<float>(((currTime_ - pausedTime_) - baseTime_) * secondsPerCount_);
    }
}