// GpuStats.cpp

#include <myengine/render/GpuStats.h>

#include <cstdio>
#include <sstream>

#include <nlohmann/json.hpp>

namespace myengine::render
{
    std::string GpuFrameStatsToJson(const GpuFrameStats& stats, const std::string& map, const std::string& adapter)
    {
        using json = nlohmann::json;
        json root;
        root["map"] = map;
        root["adapter"] = adapter;
        root["frame"] = stats.frame;
        root["gpuMs"] = stats.gpuMs;
        root["totals"] = {
            {"draws", stats.draws},
            {"triangles", stats.triangles},
            {"psoSwitches", stats.psoSwitches},
            {"textureSwitches", stats.textureSwitches},
            {"barriers", stats.barriers},
            {"renderTargetPasses", stats.renderTargetPasses},
        };

        json passes = json::array();
        for (const GpuPassStats& pass : stats.passes)
        {
            json entry;
            entry["name"] = pass.name;
            entry["depth"] = pass.depth;
            entry["draws"] = pass.draws;
            entry["triangles"] = pass.triangles;
            entry["lines"] = pass.lines;
            entry["psoSwitches"] = pass.psoSwitches;
            entry["textureSwitches"] = pass.textureSwitches;
            entry["barriers"] = pass.barriers;
            entry["gpuMs"] = pass.gpuMs;
            if (pass.renderTarget)
            {
                entry["renderTarget"] = {{"width", pass.width}, {"height", pass.height}};
            }
            passes.push_back(std::move(entry));
        }
        root["passes"] = std::move(passes);
        return root.dump(2);
    }

    std::string GpuFrameStatsToMarkdown(const GpuFrameStats& stats, const std::string& map, const std::string& adapter)
    {
        const auto millis = [](const double value)
        {
            if (value < 0.0)
            {
                return std::string("-");
            }
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f", value);
            return std::string(text);
        };

        std::ostringstream out;
        out << "# GPU frame " << stats.frame << " of `" << map << "`\n\n";
        out << "- adapter: " << adapter << "\n";
        out << "- GPU time: " << millis(stats.gpuMs) << " ms\n";
        out << "- draws: " << stats.draws << ", triangles: " << stats.triangles << ", PSO switches: " << stats.psoSwitches
            << ", texture switches: " << stats.textureSwitches << ", barriers: " << stats.barriers << "\n";
        out << "- offscreen passes (thumbnails, previews): " << stats.renderTargetPasses << "\n\n";
        out << "| Pass | Draws | Triangles | Lines | PSO | Tex | Barriers | GPU ms | Target |\n";
        out << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";
        for (const GpuPassStats& pass : stats.passes)
        {
            out << "| " << std::string(static_cast<std::size_t>(pass.depth) * 2, ' ') << pass.name << " | " << pass.draws << " | "
                << pass.triangles << " | " << pass.lines << " | " << pass.psoSwitches << " | " << pass.textureSwitches << " | "
                << pass.barriers << " | " << millis(pass.gpuMs) << " | ";
            if (pass.renderTarget)
            {
                out << pass.width << "x" << pass.height;
            }
            out << " |\n";
        }
        return out.str();
    }
}
