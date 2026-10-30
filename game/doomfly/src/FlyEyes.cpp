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
        columns_.clear();
        columnByKey_.assign(2 * 64 * 64, -1);

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
                    Column column;
                    column.hex1 = connectome.Hex1(neuron);
                    column.hex2 = connectome.Hex2(neuron);
                    column.eye = static_cast<uint8_t>(eye);
                    columns_.push_back(column);
                }
            }
            if (info_.columns[eye] == 0)
            {
                error = "the connectome has no lamina columns on one side";
                return false;
            }
        }

        // where each column looks
        for (Column& column : columns_)
        {
            const Axes axes = ColumnAxes(column.hex1, column.hex2);
            const Extent& e = extent[column.eye];
            const float front = std::clamp((axes.front - e.frontMin) / std::max(e.frontMax - e.frontMin, 1e-3f), 0.0f, 1.0f);
            const float up = std::clamp((axes.up - e.upMin) / std::max(e.upMax - e.upMin, 1e-3f), 0.0f, 1.0f);
            column.x = column.eye == 0 ? config_.leftEyeFrom + front * (config_.leftEyeTo - config_.leftEyeFrom)
                                       : config_.rightEyeTo - front * (config_.rightEyeTo - config_.rightEyeFrom);
            column.y = 1.0f - up;
            if (column.hex1 >= 0 && column.hex1 < 64 && column.hex2 >= 0 && column.hex2 < 64)
            {
                columnByKey_[(static_cast<size_t>(column.eye) * 64 + static_cast<size_t>(column.hex1)) * 64 + static_cast<size_t>(column.hex2)] = static_cast<int>(&column - columns_.data());
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

        // the neurons of the motion channel that sit in columns (Mi1, Mi4, Mi9, Tm1, Tm2, Tm4, Tm9 have a column in the file; Tm3 and T4/T5 do not)
        motion_.clear();
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const std::string& type : pass == 0 ? config_.motionOnTypes : config_.motionOffTypes)
            {
                for (const uint32_t neuron : connectome.Find(type).neurons)
                {
                    const int column = ColumnOf(connectome, neuron);
                    if (column >= 0)
                    {
                        motion_.push_back({neuron, column, pass == 0});
                    }
                }
            }
        }
        return true;
    }

    float FlyEyes::MotionDriveMv(const float luminance, const float slowLuminance, const bool on) const
    {
        const float contrast = std::clamp((luminance - slowLuminance) / (slowLuminance + 0.05f), -1.0f, 1.0f);
        return on ? config_.motionOnGainMv * std::max(contrast, 0.0f) : config_.motionOffGainMv * std::max(-contrast, 0.0f);
    }

    float FlyEyes::ResponseMv(const float luminance) const
    {
        const float l = std::max(luminance, 0.0f);
        return config_.maxDriveMv * l / (config_.halfLuminance + l);
    }

    float FlyEyes::SampleAt(const uint32_t* frame, const int width, const int height, const float fx, const float fy) const
    {
        const int viewHeight = std::max(1, static_cast<int>(static_cast<float>(height) * config_.viewHeightFraction));
        const int half = std::max(0, config_.boxPixels / 2);
        const int cx = static_cast<int>(std::lround(fx * static_cast<float>(width - 1)));
        const int cy = static_cast<int>(std::lround(fy * static_cast<float>(viewHeight - 1)));
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
        return sum / static_cast<float>(count);
    }

    void FlyEyes::SampleLuminance(const uint32_t* frame, const int width, const int height, std::vector<float>& luminance) const
    {
        luminance.resize(receptors_.size());
        for (size_t i = 0; i < receptors_.size(); ++i)
        {
            luminance[i] = SampleAt(frame, width, height, receptors_[i].x, receptors_[i].y);
        }
    }

    void FlyEyes::SampleColumns(const uint32_t* frame, const int width, const int height, std::vector<float>& luminance) const
    {
        luminance.resize(columns_.size());
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            luminance[i] = SampleAt(frame, width, height, columns_[i].x, columns_[i].y);
        }
    }

    int FlyEyes::ColumnOf(const flybrain::Connectome& connectome, const uint32_t neuron) const
    {
        const int16_t h1 = connectome.Hex1(neuron);
        const int16_t h2 = connectome.Hex2(neuron);
        const flybrain::Side side = connectome.NeuronSide(neuron);
        if (h1 == flybrain::kNoHex || h1 < 0 || h1 >= 64 || h2 < 0 || h2 >= 64 || (side != flybrain::Side::Left && side != flybrain::Side::Right))
        {
            return -1;
        }
        return columnByKey_[(static_cast<size_t>(side == flybrain::Side::Left ? 0 : 1) * 64 + static_cast<size_t>(h1)) * 64 + static_cast<size_t>(h2)];
    }
}
