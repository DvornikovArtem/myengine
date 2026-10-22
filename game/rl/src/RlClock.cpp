// RlClock.cpp

#include <rl/RlClock.h>

#include <algorithm>
#include <chrono>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <profileapi.h>

namespace rl
{
    namespace
    {
        // Cycles of the thread per millisecond of CPU time. Measured against the wall clock while the thread spins; a thread that is put aside during a
        // spin sees fewer cycles than wall time, so the best of several short spins is the rate of the CPU. 0: the cycle counter does not work.
        double CyclesPerMs()
        {
            static const double rate = []
            {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                double best = 0.0;
                for (int trial = 0; trial < 6; ++trial)
                {
                    LARGE_INTEGER t0{}, t1{};
                    ULONG64 c0 = 0, c1 = 0;
                    QueryPerformanceCounter(&t0);
                    if (!QueryThreadCycleTime(GetCurrentThread(), &c0))
                    {
                        return 0.0;
                    }
                    volatile double sink = 0.0;
                    do
                    {
                        for (int i = 0; i < 2000; ++i)
                        {
                            sink = sink + static_cast<double>(i) * 1e-9;
                        }
                        QueryPerformanceCounter(&t1);
                    } while (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) < 4.0);
                    QueryThreadCycleTime(GetCurrentThread(), &c1);
                    const double wallMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
                    best = std::max(best, static_cast<double>(c1 - c0) / wallMs);
                }
                return best;
            }();
            return rate;
        }
    }

    double ThreadCpuMs()
    {
        const double rate = CyclesPerMs();
        ULONG64 cycles = 0;
        if (rate > 0.0 && QueryThreadCycleTime(GetCurrentThread(), &cycles))
        {
            return static_cast<double>(cycles) / rate;
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}
