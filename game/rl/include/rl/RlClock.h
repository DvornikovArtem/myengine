// RlClock.h
// The clock of the measurements of the netcode and its tests (docs/game/rl-netcode.md, "Время"): the CPU time of the calling thread, not the wall time. A
// check of a budget ("a rollback of 20 ticks takes less than 6 ms") that reads the wall clock fails whenever the PC is busy with other work (a parallel
// ctest, a build next door): the thread is put aside and the clock keeps running. The CPU time of the thread counts only what the thread itself did.

#pragma once

namespace rl
{
    // Milliseconds of CPU time that the calling thread has used since it began (the difference of two calls is what a piece of work cost). Windows:
    // QueryThreadCycleTime, converted with the rate measured at the first call; any other case or a failure: the wall clock.
    double ThreadCpuMs();
}
