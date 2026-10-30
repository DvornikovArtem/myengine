#include <brainview/BrainPump.h>

#include <algorithm>

namespace brainview
{
    void PumpBrain(BrainRunner& runner, BrainView& view, const double deltaSeconds, BrainPumpScratch& scratch)
    {
        view.Advance(deltaSeconds);
        double simNow = 0.0;
        runner.Drain(scratch.spikes, scratch.counts, simNow);
        for (const BrainRunner::Spike& spike : scratch.spikes)
        {
            view.Flash(spike.neuron, simNow - spike.simSeconds);
        }
        // a brain on the GPU: the flashes come from its buffer, the HUD from the counts of the groups
        myengine::render::GpuFlashSource gpuFlash;
        view.SetGpuFlash(runner.GpuFlash(gpuFlash) ? gpuFlash : myengine::render::GpuFlashSource{});
        for (const BrainRunner::GroupCount& count : scratch.counts)
        {
            view.Activity().RecordCount(count.group, view.Clock() - std::max(simNow - count.simSeconds, 0.0), count.count);
        }
    }
}
