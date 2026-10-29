// FlyControls.h
//
// How the descending neurons of the fly brain steer the player of DOOM (docs/game/flydoom-fly.md): a set of controls is data
// (assets/flybrain/controls_*.json), so the neurons, the signs, the gains and the thresholds can be changed without a build.
//
// A channel is a weighted sum of the mean firing rates (Hz per neuron, over `smoothingMs`) of groups of neurons: a type and a side.
//   turn     degrees of turn per tic  = clamp(gain * (sum - offset - dead zone), +-limit); a positive sum turns to the right
//   forward  DOOM units of walking    = clamp(gain * (sum - offset - dead zone), +-limit)
//   fire     the weapon fires when the neurons fired at least `minSpikes` times in the last `windowMs` (or their rate is above thresholdHz)
//   unstick  (optional reflex, not of the brain) the turn away from a wall that the fly has pushed against for a long time
//   use      one press of the use button every `everyTics` while the player pushes forward and does not move (stuck at a door); on the
//            intermission screens (they wait for a key) the controller presses fire

#pragma once

#include <flybrain/Connectome.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace doomfly
{
    struct ControlTerm
    {
        std::string type;                       // "DNa02"
        flybrain::Side side = flybrain::Side::Any;
        float weight = 1.0f;
    };

    struct AnalogChannel
    {
        std::vector<ControlTerm> terms;
        float offsetHz = 0.0f;                  // taken from the sum first: the sum the model has when the picture is even (the right eye has more photoreceptors)
        float deadZoneHz = 0.0f;
        float gain = 0.0f;                      // turn: degrees per tic per Hz; forward: DOOM units per Hz
        float limit = 0.0f;                     // |turn| in degrees per tic; |forward| in DOOM units (50 is the run of DOOM)
    };

    struct FireChannel
    {
        std::vector<ControlTerm> terms;
        float windowMs = 100.0f;
        uint32_t minSpikes = 1;
        // the mean rate of the neurons (Hz) above which the weapon fires, when a single spike means little (0: use minSpikes only)
        float thresholdHz = 0.0f;
    };

    struct UseChannel
    {
        uint32_t everyTics = 35;                // the interval of the presses at a door
        uint32_t stuckTics = 12;                // the tics the player must push forward without moving before the first press
        float stuckUnits = 6.0f;                // "does not move": less than this many map units in stuckTics
    };

    // A reflex of the legs, outside the connectome: a fly that pushes against a wall for a long time turns away and backs up for a moment.
    // Without it the fly of DOOM stands at the first wall for good (the picture of a wall does not steer). afterTics 0: no reflex.
    struct UnstickReflex
    {
        uint32_t afterTics = 0;                 // the tics of pushing without moving before the reflex starts (the use button has been tried by then)
        float turnDegrees = 5.0f;               // the turn per tic; the side alternates
        uint32_t forTics = 18;
        float backUnits = 10.0f;                // walking backwards, DOOM units
    };

    struct FlyControls
    {
        std::string name;
        std::string description;
        float smoothingMs = 100.0f;
        AnalogChannel turn;
        AnalogChannel forward;
        FireChannel fire;
        UseChannel use;
        UnstickReflex unstick;

        // Reads a controls file. false and `error` when the file is missing or wrong. Side: "left", "right", "any"
        static bool Load(const std::filesystem::path& path, FlyControls& out, std::string& error);
        // The same from the text of the file
        static bool Parse(const std::string& text, FlyControls& out, std::string& error);
    };
}
