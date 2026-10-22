// The limit of the frames per second (Video > Frame rate limit): the rate is the one that was asked for, on average and in the usual frame, and a frame that
// came late does not make the next ones run without a wait.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <myengine/core/FramePacer.h>

namespace
{
    using Clock = std::chrono::steady_clock;
    int g_failures = 0;

    void Check(const bool condition, const char* text)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("  FAIL %s\n", text);
        }
    }

    double Milliseconds(const Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

    // `frames` frames of a pretend 0.5 ms of work and the wait of the pacer; returns the length of every frame
    std::vector<double> RunFrames(myengine::core::FramePacer& pacer, const int limit, const int frames)
    {
        std::vector<double> times;
        auto previous = Clock::now();
        for (int i = 0; i < frames; ++i)
        {
            const auto work = Clock::now() + std::chrono::microseconds(500);
            while (Clock::now() < work)
            {
            }
            pacer.Wait(limit);
            const auto now = Clock::now();
            times.push_back(Milliseconds(now - previous));
            previous = now;
        }
        return times;
    }

    double Sum(const std::vector<double>& values)
    {
        double total = 0.0;
        for (const double v : values)
        {
            total += v;
        }
        return total;
    }

    void TestRate(const int limit)
    {
        myengine::core::FramePacer pacer;
        const int frames = limit * 2;
        const std::vector<double> times = RunFrames(pacer, limit, frames);
        const double total = Sum(times);
        const double rate = static_cast<double>(frames) / (total / 1000.0);
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        const double worst = sorted.back();
        std::printf("  limit %d: %.1f frames per second over %.1f s, the median frame %.2f ms (ask %.2f), the worst %.2f ms\n", limit, rate, total / 1000.0, median, 1000.0 / limit, worst);
        Check(rate > limit * 0.97 && rate < limit * 1.03, "the rate is the one asked for (within 3 %)");
        Check(std::fabs(median - 1000.0 / limit) < 0.5, "the usual frame lasts as long as it should (within half a millisecond)");
    }
}

int main()
{
    std::printf("frame pacer\n");
    for (const int limit : {60, 144, 240})
    {
        TestRate(limit);
    }

    {
        // no limit: no wait at all
        myengine::core::FramePacer pacer;
        const auto start = Clock::now();
        for (int i = 0; i < 1000; ++i)
        {
            pacer.Wait(0);
        }
        const double ms = Milliseconds(Clock::now() - start);
        std::printf("  no limit: 1000 calls in %.2f ms\n", ms);
        Check(ms < 20.0, "without a limit the pacer does not wait");
    }

    {
        // a frame that came late (a hitch of 100 ms) does not make the next ones run in a burst to catch up
        myengine::core::FramePacer pacer;
        RunFrames(pacer, 144, 30);
        const auto hitch = Clock::now() + std::chrono::milliseconds(100);
        while (Clock::now() < hitch)
        {
        }
        const std::vector<double> after = RunFrames(pacer, 144, 12);
        double shortest = 1e9;
        for (size_t i = 1; i < after.size(); ++i)
        {
            shortest = std::min(shortest, after[i]);
        }
        std::printf("  after a hitch of 100 ms the shortest frame is %.2f ms\n", shortest);
        Check(shortest > 5.5, "after a hitch the frames keep their length (no burst)");
    }

    {
        // the limit changes: the grid starts over (no wait for a time that belongs to the old rate)
        myengine::core::FramePacer pacer;
        RunFrames(pacer, 60, 20);
        const std::vector<double> times = RunFrames(pacer, 240, 60);
        const double rate = 60.0 / (Sum(times) / 1000.0);
        std::printf("  from 60 to 240: %.1f frames per second after the change\n", rate);
        Check(rate > 240 * 0.95 && rate < 240 * 1.05, "after a change of the limit the new rate holds");
    }

    std::printf(g_failures == 0 ? "frame_pacer: all passed\n" : "frame_pacer: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
