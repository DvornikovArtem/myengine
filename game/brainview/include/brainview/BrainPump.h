// BrainPump.h
// One frame of "the brain runs on its thread, the view shows it": the clock of the view moves, the spikes the runner collected become flashes, the buffer of a GPU brain
// becomes the source of the flashes, the counts of the groups go to the HUD. The window Fly Brain, the menu of FlyDoom and the panel of the game all do exactly this.

#pragma once

#include <brainview/BrainRunner.h>
#include <brainview/BrainView.h>

#include <vector>

namespace brainview
{
    struct BrainPumpScratch
    {
        std::vector<BrainRunner::Spike> spikes;
        std::vector<BrainRunner::GroupCount> counts;
    };

    void PumpBrain(BrainRunner& runner, BrainView& view, double deltaSeconds, BrainPumpScratch& scratch);
}
