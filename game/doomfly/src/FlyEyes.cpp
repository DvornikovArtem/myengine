// FlyEyes.cpp

#include <doomfly/FlyEyes.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace doomfly
{
    namespace
    {
        struct Axes
        {
            float up;
            float front;
        };

        // the lattice of the columns to the directions of the visual field, see FlyEyes.h
        Axes ColumnAxes(const int hex1, const int hex2)
        {
            const float hx = static_cast<float>(hex2) - 0.5f * static_cast<float>(hex1);
            const float hy = static_cast<float>(hex1) * 0.8660254f;
            return {0.6f * hx + 0.8f * hy, -0.8f * hx + 0.6f * hy};
        }

        float Srgb8ToLinear(const uint32_t value)
        {
            // a table: 256 entries
            static const std::vector<float> table = []
            {
                std::vector<float> t(256);
                for (int i = 0; i < 256; ++i)
                {
                    const float c = static_cast<float>(i) / 255.0f;
                    t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                }
                return t;
            }();
            return table[value & 0xFF];
        }
    }

    bool FlyEyes::Build(const flybrain::Connectome& connectome, const EyesConfig& config, std::string& error)
    {
        config_ = config;
        receptors_.clear();
        info_ = {};
        lamina_ = {};

        const flybrain::Group retina = connectome.Find("R1-R6");
        if (retina.Empty())
        {
            error = "the connectome has no R1-R6";
            return false;
        }

        // the lamina cells that get the constant drive
        for (const char* type : {"L1", "L2", "L3", "L5"})
        {
            const flybrain::Group cells = connectome.Find(type);
            lamina_.neurons.insert(lamina_.neurons.end(), cells.neurons.begin(), cells.neurons.end());
        }

        // the columns of each eye: the extent in the visual field, from L1
        struct Extent
        {
            float upMin = 1e9f, upMax = -1e9f, frontMin = 1e9f, frontMax = -1e9f;
        };
        Extent extent[2];
        std::map<int, bool> columnSeen[2];
        for (int eye = 0; eye < 2; ++eye)
        {
            const flybrain::Group l1 = connectome.Find("L1", eye == 0 ? flybrain::Side::Left : flybrain::Side::Right);
            for (const uint32_t neuron : l1.neurons)
            {
                if (connectome.Hex1(neuron) == flybrain::kNoHex)
                {
                    continue;
                }
                const Axes axes = ColumnAxes(connectome.Hex1(neuron), connectome.Hex2(neuron));
                extent[eye].upMin = std::min(extent[eye].upMin, axes.up);
                extent[eye].upMax = std::max(extent[eye].upMax, axes.up);
                extent[eye].frontMin = std::min(extent[eye].frontMin, axes.front);
                extent[eye].frontMax = std::max(extent[eye].frontMax, axes.front);
                const int key = connectome.Hex1(neuron) * 1000 + connectome.Hex2(neuron);
                if (!columnSeen[eye][key])
                {
                    columnSeen[eye][key] = true;
                    ++info_.columns[eye];
                }
            }
            if (info_.columns[eye] == 0)
            {
                error = "the connectome has no lamina columns on one side";
                return false;
            }
        }

        std::map<int, bool> covered[2];
        for (const uint32_t neuron : retina.neurons)
        {
            const flybrain::Side side = connectome.NeuronSide(neuron);
            if (side != flybrain::Side::Left && side != flybrain::Side::Right)
            {
                continue;
            }
            const int eye = side == flybrain::Side::Left ? 0 : 1;

            // the most common column of the lamina cells it feeds
            std::map<int, int> votes;
            for (uint32_t edge = connectome.EdgeBegin(neuron); edge < connectome.EdgeEnd(neuron); ++edge)
            {
                const uint32_t target = connectome.Targets()[edge];
                const std::string_view type = connectome.Type(target);
                if ((type == "L1" || type == "L2" || type == "L3") && connectome.Hex1(target) != flybrain::kNoHex)
                {
                    votes[connectome.Hex1(target) * 1000 + connectome.Hex2(target)] += std::abs(static_cast<int>(connectome.Weights()[edge]));
                }
            }
            if (votes.empty())
            {
                ++info_.withoutColumn[eye];
                continue;
            }
            const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

            Photoreceptor receptor;
            receptor.neuron = neuron;
            receptor.eye = static_cast<uint8_t>(eye);
            receptor.hex1 = static_cast<int16_t>(best->first / 1000);
            receptor.hex2 = static_cast<int16_t>(best->first % 1000);

            const Axes axes = ColumnAxes(receptor.hex1, receptor.hex2);
            const Extent& e = extent[eye];
            const float front = std::clamp((axes.front - e.frontMin) / std::max(e.frontMax - e.frontMin, 1e-3f), 0.0f, 1.0f);   // 1: the front edge of the eye
            const float up = std::clamp((axes.up - e.upMin) / std::max(e.upMax - e.upMin, 1e-3f), 0.0f, 1.0f);
            if (eye == 0)
            {
                receptor.x = config_.leftEyeFrom + front * (config_.leftEyeTo - config_.leftEyeFrom);
            }
            else
            {
                receptor.x = config_.rightEyeTo - front * (config_.rightEyeTo - config_.rightEyeFrom);
            }
            receptor.y = 1.0f - up;
            receptors_.push_back(receptor);
            ++info_.receptors[eye];
            covered[eye][best->first] = true;
        }
        for (int eye = 0; eye < 2; ++eye)
        {
            info_.columnsCovered[eye] = static_cast<uint32_t>(covered[eye].size());
        }
        if (receptors_.empty())
        {
            error = "no photoreceptor has a column";
            return false;
        }
        return true;
    }

    float FlyEyes::ResponseMv(const float luminance) const
    {
        const float l = std::max(luminance, 0.0f);
        return config_.maxDriveMv * l / (config_.halfLuminance + l);
    }

    void FlyEyes::SampleLuminance(const uint32_t* frame, const int width, const int height, std::vector<float>& luminance) const
    {
        luminance.resize(receptors_.size());
        const int viewHeight = std::max(1, static_cast<int>(static_cast<float>(height) * config_.viewHeightFraction));
        const int half = std::max(0, config_.boxPixels / 2);
        for (size_t i = 0; i < receptors_.size(); ++i)
        {
            const float px = receptors_[i].x * static_cast<float>(width - 1);
            const float py = receptors_[i].y * static_cast<float>(viewHeight - 1);
            const int cx = static_cast<int>(std::lround(px));
            const int cy = static_cast<int>(std::lround(py));
            float sum = 0.0f;
            int count = 0;
            for (int dy = -half; dy <= half; ++dy)
            {
                for (int dx = -half; dx <= half; ++dx)
                {
                    const int x = std::clamp(cx + dx, 0, width - 1);
                    const int y = std::clamp(cy + dy, 0, viewHeight - 1);
                    const uint32_t pixel = frame[y * width + x];
                    const float r = Srgb8ToLinear(pixel >> 16);
                    const float g = Srgb8ToLinear(pixel >> 8);
                    const float b = Srgb8ToLinear(pixel);
                    sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    ++count;
                }
            }
            luminance[i] = sum / static_cast<float>(count);
        }
    }
}
