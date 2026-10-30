// BrainLayout.h
// What the 3D view needs to know about a connectome before the first frame (docs/flybrain/brain-view.md): where each neuron is in a unit-sized cloud, which colour
// it has (by its class), and the named groups that are highlighted and counted (Turn L / Turn R / Forward / Back / Fire / Dopamine / Eyes). Pure data and rules, no
// GPU and no simulation, so it is tested without either.

#pragma once

#include <flybrain/Connectome.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace brainview
{
    constexpr int kPaletteSize = 16;
    constexpr uint8_t kNoGroup = 255;

    // The rows of the palette (the colour index of a point)
    enum ColorIndex : uint8_t
    {
        kColorCentral = 0,     // the rest of the brain: violet
        kColorVision = 1,      // optic lobes: cyan
        kColorDescending = 2,  // descending neurons: orange
        kColorMotor = 3,       // motor neurons: red
        kColorSensory = 4,     // other senses: green
        kColorAscending = 5,   // ascending neurons: teal
        kColorDopamine = 6,    // dopamine (the reinforcement, "pain"): gold
        kColorTurnLeft = 7,
        kColorTurnRight = 8,
        kColorForward = 9,
        kColorBack = 10,
        kColorFire = 11,
        kColorEyes = 12,
    };

    // The colour index of a neuron by its annotations (superclass, class, type); the groups below overwrite it for their neurons
    uint8_t ClassifyNeuron(std::string_view superclass, std::string_view cellClass, std::string_view type);

    // The palette (rgb, HDR: the highlighted groups are brighter than 1 and glow at rest)
    void FillPalette(float palette[kPaletteSize][4]);

    struct BrainGroup
    {
        std::string id;      // "turn_left"
        std::string label;   // "Turn L"
        uint8_t colorIndex = 0;
        std::vector<uint32_t> neurons;   // indices into the connectome
    };

    // The groups that the HUD shows: the descending neurons that steer (DNa01 and DNa02 by side), walk forward (DNp09) and back (MDN), the giant fibre that fires
    // (DNp01), the dopamine neurons of the PPL1 cluster (the pain signal of DOOM) and the photoreceptors. A group that the connectome does not have is left out.
    std::vector<BrainGroup> DefaultGroups(const flybrain::Connectome& connectome);

    // A neuron without a position is drawn this far from the middle (the cloud has a radius of about 1): out of the picture
    constexpr float kHiddenDistance = 100.0f;

    struct CloudModel
    {
        std::vector<float> positions;     // 3 floats per neuron: x across the head, y along the long axis (the brain above, the nerve cord below), z the depth
        std::vector<uint8_t> colorIndex;  // per neuron
        std::vector<uint8_t> groupOf;     // per neuron: the index of its group or kNoGroup (the first group that holds it)
        uint32_t withoutPosition = 0;     // neurons that have no position in the dataset (the cell body is outside the traced volume)
        uint32_t estimated = 0;           // ... of them those that are drawn where the neurons they talk to are (the endings of the photoreceptors and of other sensory cells)
        uint32_t hidden = 0;              // ... and those that have no neighbour with a position either: not drawn (they still count in the rates)
    };

    // Turns the axes so that the longest one (by the 0.5 to 99.5 percentile range) is y with its longer tail pointing down, the next one is x and the last one z, centres the
    // box of the percentiles on the origin, stands the line from the middle of the lower 30 % to the middle of the upper 30 % upright (the nerve cord of the dataset leans), and scales so that the 99.5th percentile of the distance from the centre is 1. The turn is a rotation, never a mirror image (left
    // stays left). Points that are not finite or all zero are placed kHiddenDistance away on y and counted in `withoutPosition`.
    void NormalizePositions(const float* xyz, std::size_t count, std::vector<float>& out, uint32_t* withoutPosition = nullptr);

    CloudModel BuildCloudModel(const flybrain::Connectome& connectome, const std::vector<BrainGroup>& groups);
}
