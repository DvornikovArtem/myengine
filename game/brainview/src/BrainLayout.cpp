#include <brainview/BrainLayout.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace brainview
{
    namespace
    {
        bool Contains(const std::string_view text, const std::string_view part)
        {
            return text.find(part) != std::string_view::npos;
        }

        bool StartsWith(const std::string_view text, const std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
        }

        bool Usable(const float* point)
        {
            return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]) && (point[0] != 0.0f || point[1] != 0.0f || point[2] != 0.0f);
        }

        void AddGroup(std::vector<BrainGroup>& groups, const char* id, const char* label, const uint8_t color, flybrain::Group members)
        {
            if (members.Empty())
            {
                return;
            }
            BrainGroup group;
            group.id = id;
            group.label = label;
            group.colorIndex = color;
            group.neurons = std::move(members.neurons);
            groups.push_back(std::move(group));
        }

        flybrain::Group Join(flybrain::Group a, const flybrain::Group& b)
        {
            a.neurons.insert(a.neurons.end(), b.neurons.begin(), b.neurons.end());
            std::sort(a.neurons.begin(), a.neurons.end());
            a.neurons.erase(std::unique(a.neurons.begin(), a.neurons.end()), a.neurons.end());
            return a;
        }
    }

    uint8_t ClassifyNeuron(const std::string_view superclass, const std::string_view cellClass, const std::string_view type)
    {
        (void)cellClass;
        if (Contains(superclass, "descending"))
        {
            return kColorDescending;
        }
        if (Contains(superclass, "ascending"))
        {
            return kColorAscending;
        }
        if (Contains(superclass, "motor") || Contains(superclass, "efferent") || Contains(superclass, "endocrine"))
        {
            return kColorMotor;
        }
        if (StartsWith(superclass, "ol_") || Contains(superclass, "optic") || Contains(superclass, "visual") || StartsWith(type, "R1-R6") || StartsWith(type, "R7") || StartsWith(type, "R8"))
        {
            return kColorVision;
        }
        if (Contains(superclass, "sensory"))
        {
            return kColorSensory;
        }
        return kColorCentral;
    }

    void FillPalette(float palette[kPaletteSize][4])
    {
        const auto set = [&](const int index, const float r, const float g, const float b)
        {
            palette[index][0] = r;
            palette[index][1] = g;
            palette[index][2] = b;
            palette[index][3] = 1.0f;
        };
        for (int i = 0; i < kPaletteSize; ++i)
        {
            set(i, 0.6f, 0.6f, 0.6f);
        }
        set(kColorCentral, 0.50f, 0.28f, 1.00f);
        set(kColorVision, 0.12f, 0.72f, 1.00f);
        set(kColorDescending, 1.00f, 0.52f, 0.10f);
        set(kColorMotor, 1.00f, 0.15f, 0.15f);
        set(kColorSensory, 0.30f, 1.00f, 0.42f);
        set(kColorAscending, 0.15f, 0.90f, 0.80f);
        // the highlighted groups glow at rest: their colour is above 1
        set(kColorDopamine, 3.2f, 2.5f, 0.35f);
        set(kColorTurnLeft, 0.9f, 3.0f, 3.0f);
        set(kColorTurnRight, 3.2f, 0.9f, 2.4f);
        set(kColorForward, 1.6f, 3.4f, 0.6f);
        set(kColorBack, 0.9f, 1.4f, 3.6f);
        set(kColorFire, 3.8f, 0.9f, 0.5f);
        set(kColorEyes, 0.35f, 1.6f, 2.8f);
    }

    std::vector<BrainGroup> DefaultGroups(const flybrain::Connectome& connectome)
    {
        using flybrain::Side;
        std::vector<BrainGroup> groups;
        AddGroup(groups, "turn_left", "Turn L", kColorTurnLeft, Join(connectome.Find("DNa01", Side::Left), connectome.Find("DNa02", Side::Left)));
        AddGroup(groups, "turn_right", "Turn R", kColorTurnRight, Join(connectome.Find("DNa01", Side::Right), connectome.Find("DNa02", Side::Right)));
        AddGroup(groups, "forward", "Forward", kColorForward, connectome.Find("DNp09"));
        AddGroup(groups, "back", "Back", kColorBack, connectome.Find("MDN"));
        AddGroup(groups, "fire", "Fire", kColorFire, connectome.Find("DNp01"));
        AddGroup(groups, "dopamine", "Dopamine (pain)", kColorDopamine, connectome.FindPrefix("PPL1"));
        AddGroup(groups, "eyes", "Eyes (R1-R6)", kColorEyes, connectome.Find("R1-R6"));
        return groups;
    }

    void NormalizePositions(const float* xyz, const std::size_t count, std::vector<float>& out, uint32_t* withoutPosition)
    {
        out.assign(count * 3, 0.0f);
        uint32_t missing = 0;
        std::vector<uint8_t> usable(count, 0);
        std::size_t valid = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (Usable(xyz + 3 * i))
            {
                usable[i] = 1;
                ++valid;
            }
            else
            {
                ++missing;
            }
        }
        if (withoutPosition != nullptr)
        {
            *withoutPosition = missing;
        }
        const auto hide = [&](const std::size_t i)
        {
            out[3 * i] = 0.0f;
            out[3 * i + 1] = kHiddenDistance;
            out[3 * i + 2] = 0.0f;
        };
        if (valid == 0)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                hide(i);
            }
            return;
        }

        // the range of every axis without the outliers, and the median
        double low[3];
        double high[3];
        double median[3];
        std::vector<float> values(valid);
        for (int axis = 0; axis < 3; ++axis)
        {
            std::size_t n = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (usable[i] != 0)
                {
                    values[n++] = xyz[3 * i + static_cast<std::size_t>(axis)];
                }
            }
            const auto at = [&](const double fraction)
            {
                const std::size_t nth = std::min(valid - 1, static_cast<std::size_t>(static_cast<double>(valid) * fraction));
                std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(nth), values.end());
                return static_cast<double>(values[nth]);
            };
            low[axis] = at(0.005);
            high[axis] = at(0.995);
            median[axis] = at(0.5);
        }
        std::array<int, 3> order = {0, 1, 2};
        std::sort(order.begin(), order.end(), [&](const int a, const int b) { return high[a] - low[a] > high[b] - low[b]; });
        const int longAxis = order[0];
        const int sideAxis = order[1];
        const int depthAxis = order[2];

        // the signs: the longer tail of the long axis goes down; the turn must stay a rotation, so the depth takes the sign that makes the determinant positive
        double signY = (high[longAxis] - median[longAxis]) > (median[longAxis] - low[longAxis]) ? -1.0 : 1.0;
        const double signX = 1.0;
        // the matrix with rows x, y, z: which raw axis each takes and with which sign
        double matrix[3][3] = {};
        matrix[0][sideAxis] = signX;
        matrix[1][longAxis] = signY;
        matrix[2][depthAxis] = 1.0;
        const double determinant = matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) - matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
                                   matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
        const double signZ = determinant < 0.0 ? -1.0 : 1.0;
        const double centre[3] = {(low[0] + high[0]) * 0.5, (low[1] + high[1]) * 0.5, (low[2] + high[2]) * 0.5};

        // the axes of the dataset are not those of the animal: the nerve cord leans. A turn about z stands the line from the middle of the lower part (the cord) to the middle
        // of the upper part (the brain) upright.
        std::vector<double> mapped(count * 3, 0.0);
        std::vector<double> ys;
        ys.reserve(valid);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (usable[i] == 0)
            {
                continue;
            }
            mapped[3 * i] = signX * (xyz[3 * i + static_cast<std::size_t>(sideAxis)] - centre[sideAxis]);
            mapped[3 * i + 1] = signY * (xyz[3 * i + static_cast<std::size_t>(longAxis)] - centre[longAxis]);
            mapped[3 * i + 2] = signZ * (xyz[3 * i + static_cast<std::size_t>(depthAxis)] - centre[depthAxis]);
            ys.push_back(mapped[3 * i + 1]);
        }
        {
            std::vector<double> sorted = ys;
            const std::size_t lowIndex = sorted.size() * 3 / 10;
            const std::size_t highIndex = sorted.size() * 7 / 10;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(lowIndex), sorted.end());
            const double lowCut = sorted[lowIndex];
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(highIndex), sorted.end());
            const double highCut = sorted[highIndex];
            double lowX = 0.0;
            double lowY = 0.0;
            double highX = 0.0;
            double highY = 0.0;
            std::size_t lowCount = 0;
            std::size_t highCount = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (usable[i] == 0)
                {
                    continue;
                }
                const double y = mapped[3 * i + 1];
                if (y <= lowCut)
                {
                    lowX += mapped[3 * i];
                    lowY += y;
                    ++lowCount;
                }
                else if (y >= highCut)
                {
                    highX += mapped[3 * i];
                    highY += y;
                    ++highCount;
                }
            }
            if (lowCount > 0 && highCount > 0)
            {
                const double ux = highX / static_cast<double>(highCount) - lowX / static_cast<double>(lowCount);
                const double uy = highY / static_cast<double>(highCount) - lowY / static_cast<double>(lowCount);
                const double length = std::sqrt(ux * ux + uy * uy);
                if (length > 0.0 && std::fabs(ux) < 0.7 * length)   // a lean of up to about 45 degrees; more than that is not a lean but another shape
                {
                    const double c = uy / length;
                    const double s = ux / length;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        if (usable[i] != 0)
                        {
                            const double x = mapped[3 * i];
                            const double y = mapped[3 * i + 1];
                            mapped[3 * i] = x * c - y * s;
                            mapped[3 * i + 1] = x * s + y * c;
                        }
                    }
                }
            }
        }

        std::vector<float> distances;
        distances.reserve(valid);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (usable[i] == 0)
            {
                hide(i);
                continue;
            }
            const double x = mapped[3 * i];
            const double y = mapped[3 * i + 1];
            const double z = mapped[3 * i + 2];
            out[3 * i] = static_cast<float>(x);
            out[3 * i + 1] = static_cast<float>(y);
            out[3 * i + 2] = static_cast<float>(z);
            distances.push_back(static_cast<float>(std::sqrt(x * x + y * y + z * z)));
        }
        const std::size_t nth = std::min(distances.size() - 1, static_cast<std::size_t>(static_cast<double>(distances.size()) * 0.995));
        std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(nth), distances.end());
        const float scale = distances[nth] > 0.0f ? 1.0f / distances[nth] : 1.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (usable[i] != 0)
            {
                out[3 * i] *= scale;
                out[3 * i + 1] *= scale;
                out[3 * i + 2] *= scale;
            }
        }
    }

    CloudModel BuildCloudModel(const flybrain::Connectome& connectome, const std::vector<BrainGroup>& groups)
    {
        CloudModel model;
        const uint32_t count = connectome.NeuronCount();
        std::vector<float> raw(3ull * count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const float* p = connectome.Position(i);
            raw[3ull * i] = p[0];
            raw[3ull * i + 1] = p[1];
            raw[3ull * i + 2] = p[2];
        }

        // Many sensory neurons (the photoreceptors, the bristles, the taste cells) have their cell body outside the traced volume and no position. They are drawn
        // where their endings are: the mean place of the neurons they send synapses to (those that have a position), moved a little by a number that depends on the
        // neuron only, so that thousands of them do not sit on one point.
        std::vector<uint8_t> missing(count, 0);
        for (uint32_t i = 0; i < count; ++i)
        {
            missing[i] = Usable(&raw[3ull * i]) ? 0 : 1;
        }
        const std::vector<float> original = raw;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (missing[i] == 0)
            {
                continue;
            }
            ++model.withoutPosition;
            double sum[3] = {0.0, 0.0, 0.0};
            double total = 0.0;
            for (uint32_t e = connectome.EdgeBegin(i); e < connectome.EdgeEnd(i); ++e)
            {
                const uint32_t target = connectome.Targets()[e];
                if (missing[target] != 0)
                {
                    continue;
                }
                const double weight = std::abs(static_cast<double>(connectome.Weights()[e]));
                sum[0] += weight * original[3ull * target];
                sum[1] += weight * original[3ull * target + 1];
                sum[2] += weight * original[3ull * target + 2];
                total += weight;
            }
            if (total <= 0.0)
            {
                ++model.hidden;
                continue;
            }
            uint32_t h = i * 2654435761u;
            const auto jitter = [&]() { h = h * 1664525u + 1013904223u; return (static_cast<float>(h >> 8) / 16777216.0f - 0.5f) * 2.0f; };
            raw[3ull * i] = static_cast<float>(sum[0] / total) + 1800.0f * jitter();
            raw[3ull * i + 1] = static_cast<float>(sum[1] / total) + 1800.0f * jitter();
            raw[3ull * i + 2] = static_cast<float>(sum[2] / total) + 1800.0f * jitter();
            ++model.estimated;
        }

        uint32_t unused = 0;
        NormalizePositions(raw.data(), count, model.positions, &unused);
        model.colorIndex.resize(count);
        model.groupOf.assign(count, kNoGroup);
        for (uint32_t i = 0; i < count; ++i)
        {
            model.colorIndex[i] = ClassifyNeuron(connectome.Superclass(i), connectome.CellClass(i), connectome.Type(i));
        }
        for (std::size_t g = 0; g < groups.size() && g < kNoGroup; ++g)
        {
            for (const uint32_t neuron : groups[g].neurons)
            {
                if (neuron < count && model.groupOf[neuron] == kNoGroup)
                {
                    model.groupOf[neuron] = static_cast<uint8_t>(g);
                    model.colorIndex[neuron] = groups[g].colorIndex;
                }
            }
        }
        return model;
    }
}
