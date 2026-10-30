// FlyModes.h
//
// The modes of FlyDoom in which the fly plays (docs/game/flydoom-fly.md): the entries of the menu in front of the game and the controller that runs the
// fly player in the window of the game, with the panel of the brain at the right of the picture.

#pragma once

#include <myengine/game/DoomController.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace doomfly
{
    struct FlyModeOptions
    {
        std::filesystem::path connectome;       // --fbrain; empty: assets/flybrain/malecns_v1.fbrain next to the exe, then in the sources
        std::filesystem::path exeDirectory;
        std::filesystem::path sourceDirectory;  // the sources of a development build (MYENGINE_SOURCE_DIR), empty in a package
        std::uint64_t seed = 1;
        bool reward = false;                    // --fly-reward: a current into PAM when the fly kills a monster or takes an item
        std::function<void*()> renderDevice;    // the ID3D12Device* of the renderer when the window has one (the brain of the fly lives there)
        std::function<void(const std::string&)> log;
    };

    // The modes of FlyDoom, in the order of the menu: human, fly, human-vs-fly, fly-vs-fly, swarm (DoomMode.id). The set of neurons and the backend come with the
    // DoomLaunchChoice. The modes that are not ready, or need the connectome that is not there, are in the list with available = false and the reason.
    // withFly false: the connectome file is not there.
    std::vector<myengine::game::DoomMode> MakeModes(const FlyModeOptions& options, bool withFly);

    // Is the connectome file where FlyModeOptions says (for the decision of the menu)
    bool ConnectomeAvailable(const FlyModeOptions& options);
}
