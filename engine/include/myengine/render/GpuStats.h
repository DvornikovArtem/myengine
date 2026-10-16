// GpuStats.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace myengine::render
{
    // One pass of a frame (a marker group of the GPU capture): what the renderer recorded in it
    struct GpuPassStats
    {
        std::string name;
        int depth = 0;               // 0: the frame itself, 1: a pass of the frame, 2: inside a pass
        bool renderTarget = false;   // an offscreen pass (thumbnail, preview)
        std::uint32_t width = 0;     // render targets: the size
        std::uint32_t height = 0;
        std::uint32_t draws = 0;     // draw calls
        std::uint64_t triangles = 0;
        std::uint64_t lines = 0;     // debug lines
        std::uint32_t psoSwitches = 0;
        std::uint32_t textureSwitches = 0;
        std::uint32_t barriers = 0;
        double gpuMs = -1.0;         // from timestamp queries; negative: not measured
    };

    struct GpuFrameStats
    {
        std::uint64_t frame = 0;     // the number of the frame (the first rendered frame is 1)
        double gpuMs = -1.0;         // the whole frame on the GPU; negative: not measured
        std::uint32_t draws = 0;
        std::uint64_t triangles = 0;
        std::uint32_t psoSwitches = 0;
        std::uint32_t textureSwitches = 0;
        std::uint32_t barriers = 0;
        std::uint32_t renderTargetPasses = 0;
        std::vector<GpuPassStats> passes; // in recording order; a pass is inside the nearest earlier pass of a lower depth
    };

    // JSON (pretty-printed) and a markdown table of one frame: what the agents and the CLI write
    std::string GpuFrameStatsToJson(const GpuFrameStats& stats, const std::string& map, const std::string& adapter);
    std::string GpuFrameStatsToMarkdown(const GpuFrameStats& stats, const std::string& map, const std::string& adapter);
}
