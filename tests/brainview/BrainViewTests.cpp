// BrainViewTests.cpp
// V0 (FlyDoom): the 3D view of the fly brain (game/brainview).
//   - the layout: classes by annotation, the groups of a connectome, the normalised cloud (the long axis up, the tail down, unit size), neurons without a position
//   - the flash buffer (time of the last spike, the rebase of a long run) and the activity of the groups (rates, the raster, bins that leave the window)
//   - the camera (it turns by itself, waits after a touch, zoom limits)
//   - the runner on a small synthetic brain: the stimulus makes spikes, they arrive in order with the time of the simulation, silence makes none
//   - the picture: a cloud on a real DX12 device (or WARP) into a render target, the pixels read back: flashes are bright and fade, the colour of the class shows,
//     the bloom adds light around a flash, a target that changes size and a destroyed cloud do not leak. Without a device or a desktop this part is skipped
//     (exit code 77; the checks before it have run).

#include <brainview/BrainActivity.h>
#include <brainview/BrainLayout.h>
#include <brainview/BrainRunner.h>
#include <brainview/BrainView.h>
#include <brainview/OrbitCamera.h>

#include <flybrain/Connectome.h>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace brainview;
    namespace fs = std::filesystem;
    namespace render = myengine::render;

    constexpr int kSkipped = 77;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const double a, const double b, const double tolerance) { return std::fabs(a - b) <= tolerance; }

    // ------------------------------------------------------------------------------------------------ the layout

    flybrain::NeuronInfo Neuron(const uint64_t id, const char* type, const char* superclass, const flybrain::Side side, const float x, const float y, const float z)
    {
        flybrain::NeuronInfo info;
        info.bodyId = id;
        info.type = type;
        info.superclass = superclass;
        info.side = side;
        info.transmitter = flybrain::Transmitter::Acetylcholine;
        info.position[0] = x;
        info.position[1] = y;
        info.position[2] = z;
        return info;
    }

    std::shared_ptr<const flybrain::Connectome> SmallBrain()
    {
        flybrain::ConnectomeBuilder builder;
        uint64_t id = 1;
        // a head: the optic lobes at the sides, the central brain between them, descending neurons below, a nerve cord far down; nothing is on a grid
        std::mt19937 random(5);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (int i = 0; i < 300; ++i)
        {
            builder.AddNeuron(Neuron(id++, "Mi1", "ol_intrinsic", flybrain::Side::Left, -60000.0f + 20000.0f * unit(random), 20000.0f * unit(random), 20000.0f * unit(random)));
        }
        for (int i = 0; i < 300; ++i)
        {
            builder.AddNeuron(Neuron(id++, "Mi1", "ol_intrinsic", flybrain::Side::Right, 60000.0f + 20000.0f * unit(random), 20000.0f * unit(random), 20000.0f * unit(random)));
        }
        for (int i = 0; i < 200; ++i)
        {
            builder.AddNeuron(Neuron(id++, "PFL", "central", flybrain::Side::Center, 30000.0f * unit(random), 25000.0f * unit(random), 30000.0f * unit(random)));
        }
        const uint32_t dnaLeft = builder.AddNeuron(Neuron(id++, "DNa02", "descending_neuron", flybrain::Side::Left, -9000.0f, -45000.0f, 0.0f));
        const uint32_t dnaRight = builder.AddNeuron(Neuron(id++, "DNa02", "descending_neuron", flybrain::Side::Right, 9000.0f, -45000.0f, 0.0f));
        const uint32_t dna01 = builder.AddNeuron(Neuron(id++, "DNa01", "descending_neuron", flybrain::Side::Left, -8000.0f, -44000.0f, 0.0f));
        const uint32_t walk = builder.AddNeuron(Neuron(id++, "DNp09", "descending_neuron", flybrain::Side::Left, 0.0f, -46000.0f, 0.0f));
        const uint32_t giant = builder.AddNeuron(Neuron(id++, "DNp01", "descending_neuron", flybrain::Side::Left, 0.0f, -40000.0f, 0.0f));
        const uint32_t back = builder.AddNeuron(Neuron(id++, "MDN", "descending_neuron", flybrain::Side::Left, 0.0f, -47000.0f, 0.0f));
        builder.AddNeuron(Neuron(id++, "PPL101", "central", flybrain::Side::Left, 15000.0f, 10000.0f, 5000.0f));
        builder.AddNeuron(Neuron(id++, "PPL102", "central", flybrain::Side::Right, -15000.0f, 10000.0f, 5000.0f));
        const uint32_t photoLeft = builder.AddNeuron(Neuron(id++, "R1-R6", "ol_sensory", flybrain::Side::Left, -70000.0f, 0.0f, 0.0f));
        const uint32_t photoRight = builder.AddNeuron(Neuron(id++, "R1-R6", "ol_sensory", flybrain::Side::Right, 70000.0f, 0.0f, 0.0f));
        for (int i = 0; i < 100; ++i)
        {
            builder.AddNeuron(Neuron(id++, "IN", "ascending_neuron", flybrain::Side::Center, 4000.0f * unit(random), -120000.0f - 90000.0f * (unit(random) + 1.0f), 4000.0f * unit(random)));
        }
        builder.AddNeuron(Neuron(id++, "nowhere", "central", flybrain::Side::Center, 0.0f, 0.0f, 0.0f));   // no position and nobody to talk to: not drawn
        const uint32_t ghost = builder.AddNeuron(Neuron(id++, "SNta1", "vnc_sensory", flybrain::Side::Left, 0.0f, 0.0f, 0.0f));   // no position, but it talks to DNa02
        builder.AddEdge(ghost, dnaLeft, 10);
        builder.AddEdge(photoLeft, dnaLeft, 40);
        builder.AddEdge(photoRight, dnaRight, 40);
        builder.AddEdge(photoLeft, dna01, 40);
        builder.AddEdge(photoRight, walk, 40);
        builder.AddEdge(photoLeft, giant, 40);
        builder.AddEdge(photoRight, back, 40);
        std::string error;
        auto connectome = flybrain::Connectome::FromImage(builder.Build("test"), &error);
        Check(connectome != nullptr, "the synthetic brain: " + error);
        return connectome;
    }

    void TestLayout()
    {
        Check(ClassifyNeuron("descending_neuron", "DN", "DNa02") == kColorDescending, "descending");
        Check(ClassifyNeuron("ascending_neuron", "", "") == kColorAscending, "ascending");
        Check(ClassifyNeuron("motor_neuron", "", "") == kColorMotor && ClassifyNeuron("endocrine", "", "") == kColorMotor, "motor");
        Check(ClassifyNeuron("ol_sensory", "", "R1-R6") == kColorVision && ClassifyNeuron("ol_intrinsic", "", "Mi1") == kColorVision, "optic lobe");
        Check(ClassifyNeuron("vnc_sensory", "", "SNta") == kColorSensory && ClassifyNeuron("sensory", "", "") == kColorSensory, "other senses");
        Check(ClassifyNeuron("central", "", "PFL") == kColorCentral && ClassifyNeuron("", "", "") == kColorCentral, "the rest");

        float palette[kPaletteSize][4];
        FillPalette(palette);
        for (int i = 0; i < kPaletteSize; ++i)
        {
            Check(palette[i][0] >= 0.0f && palette[i][1] >= 0.0f && palette[i][2] >= 0.0f && palette[i][0] + palette[i][1] + palette[i][2] > 0.5f, "a palette row is a visible colour");
        }
        Check(palette[kColorVision][2] > palette[kColorVision][0], "vision is bluish");
        Check(palette[kColorDescending][0] > palette[kColorDescending][2], "descending is orange");
        Check(palette[kColorMotor][0] > palette[kColorMotor][1] * 3.0f, "motor is red");
        Check(palette[kColorTurnLeft][1] > 1.0f && palette[kColorFire][0] > 1.0f, "the highlighted groups are above 1: they glow at rest");

        const auto brain = SmallBrain();
        const std::vector<BrainGroup> groups = DefaultGroups(*brain);
        const auto find = [&](const std::string& id) -> const BrainGroup*
        {
            for (const BrainGroup& group : groups)
            {
                if (group.id == id)
                {
                    return &group;
                }
            }
            return nullptr;
        };
        Check(find("turn_left") != nullptr && find("turn_left")->neurons.size() == 2, "Turn L: DNa01 and DNa02 of the left");
        Check(find("turn_right") != nullptr && find("turn_right")->neurons.size() == 1, "Turn R: DNa02 of the right");
        Check(find("forward") != nullptr && find("forward")->neurons.size() == 1, "Forward: DNp09");
        Check(find("fire") != nullptr && find("back") != nullptr, "Fire (DNp01) and Back (MDN)");
        Check(find("dopamine") != nullptr && find("dopamine")->neurons.size() == 2, "Dopamine: the PPL1 neurons");
        Check(find("eyes") != nullptr && find("eyes")->neurons.size() == 2, "Eyes: R1-R6");
        Check(find("turn_left")->label == "Turn L" && find("dopamine")->label == "Dopamine (pain)", "the labels of the HUD");

        const CloudModel model = BuildCloudModel(*brain, groups);
        const uint32_t count = brain->NeuronCount();
        Check(model.positions.size() == 3ull * count && model.colorIndex.size() == count && model.groupOf.size() == count, "the model has one entry per neuron");
        Check(model.withoutPosition == 2 && model.estimated == 1 && model.hidden == 1, "two neurons have no position: one is drawn at its target, one is not drawn");
        {
            // the sensory neuron without a position sits near the neuron it talks to (DNa02 of the left)
            uint32_t ghostIndex = UINT32_MAX;
            uint32_t targetIndex = UINT32_MAX;
            uint32_t hiddenIndex = UINT32_MAX;
            for (uint32_t i = 0; i < count; ++i)
            {
                ghostIndex = brain->Type(i) == "SNta1" ? i : ghostIndex;
                hiddenIndex = brain->Type(i) == "nowhere" ? i : hiddenIndex;
                targetIndex = brain->Type(i) == "DNa02" && brain->NeuronSide(i) == flybrain::Side::Left ? i : targetIndex;
            }
            Check(ghostIndex != UINT32_MAX && targetIndex != UINT32_MAX && hiddenIndex != UINT32_MAX, "the neurons are there");
            const float* g = &model.positions[3ull * ghostIndex];
            const float* d = &model.positions[3ull * targetIndex];
            Check(std::sqrt((g[0] - d[0]) * (g[0] - d[0]) + (g[1] - d[1]) * (g[1] - d[1]) + (g[2] - d[2]) * (g[2] - d[2])) < 0.2f, "a neuron without a position is drawn near its target");
            Check(model.positions[3ull * hiddenIndex + 1] >= kHiddenDistance, "one with nobody to talk to is out of the picture");
        }
        uint32_t grouped = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (model.groupOf[i] != kNoGroup)
            {
                ++grouped;
                Check(model.colorIndex[i] == groups[model.groupOf[i]].colorIndex, "a grouped neuron has the colour of its group");
            }
            for (int k = 0; k < 3; ++k)
            {
                Check(std::isfinite(model.positions[3ull * i + static_cast<std::size_t>(k)]), "every position is finite");
            }
        }
        Check(grouped == 2 + 1 + 1 + 1 + 1 + 2 + 2, "the neurons of the groups are marked (a neuron is in one group)");
        // the first neuron is a neuron of the optic lobe: the cloud colours it by its class
        Check(model.colorIndex[0] == kColorVision && model.colorIndex[600] == kColorCentral, "the classes colour the rest");
    }

    void TestNormalize()
    {
        // an elongated cloud along x (a head and a long tail in the +x direction), the skew points to +x: the tail must come out going down
        std::mt19937 random(11);
        std::normal_distribution<float> normal(0.0f, 1.0f);
        std::vector<float> points;
        for (int i = 0; i < 4000; ++i)
        {
            const bool tail = i % 8 == 0;
            const float x = tail ? 30000.0f + 40000.0f * std::fabs(normal(random)) : 3000.0f * normal(random);
            points.push_back(5000.0f + x);
            points.push_back(-2000.0f + 3000.0f * normal(random));
            points.push_back(800.0f + 3000.0f * normal(random));
        }
        // some neurons without a position
        for (int i = 0; i < 5; ++i)
        {
            points.push_back(0.0f);
            points.push_back(0.0f);
            points.push_back(0.0f);
        }
        std::vector<float> out;
        uint32_t missing = 0;
        NormalizePositions(points.data(), points.size() / 3, out, &missing);
        Check(out.size() == points.size() && missing == 5, "five neurons without a position");

        double mean[3] = {0, 0, 0};
        double variance[3] = {0, 0, 0};
        double skew = 0;
        std::vector<float> distances;
        const std::size_t valid = points.size() / 3 - 5;
        for (std::size_t i = 0; i < valid; ++i)
        {
            for (int k = 0; k < 3; ++k)
            {
                mean[k] += out[3 * i + static_cast<std::size_t>(k)];
            }
        }
        for (double& m : mean)
        {
            m /= static_cast<double>(valid);
        }
        for (std::size_t i = 0; i < valid; ++i)
        {
            for (int k = 0; k < 3; ++k)
            {
                const double d = out[3 * i + static_cast<std::size_t>(k)] - mean[k];
                variance[k] += d * d;
            }
            const double y = out[3 * i + 1] - mean[1];
            skew += y * y * y;
            distances.push_back(std::sqrt(out[3 * i] * out[3 * i] + out[3 * i + 1] * out[3 * i + 1] + out[3 * i + 2] * out[3 * i + 2]));
        }
        Check(variance[1] > variance[0] * 2.0 && variance[1] > variance[2] * 2.0, "the long axis is y");
        Check(skew < 0.0, "the tail goes down");
        Check(std::fabs(mean[0]) < 0.3 && std::fabs(mean[2]) < 0.3, "the cloud is centred on the axis");
        {
            // the line from the middle of the lower part to the middle of the upper one is upright: the synthetic tail leans (y_raw = 0.3 * x_raw), the turn removes it
            std::vector<float> tilted;
            for (int i = 0; i < 4000; ++i)
            {
                const bool tail = i % 8 == 0;
                const float length = tail ? 30000.0f + 40000.0f * std::fabs(normal(random)) : 3000.0f * normal(random);
                tilted.push_back(5000.0f + length);                                      // long axis (raw x)
                tilted.push_back(-2000.0f + 0.3f * length + 800.0f * normal(random));    // leans with it
                tilted.push_back(800.0f + 800.0f * normal(random));
            }
            std::vector<float> upright;
            NormalizePositions(tilted.data(), tilted.size() / 3, upright, nullptr);
            std::vector<float> ysorted;
            for (std::size_t i = 0; i < upright.size() / 3; ++i)
            {
                ysorted.push_back(upright[3 * i + 1]);
            }
            std::sort(ysorted.begin(), ysorted.end());
            const float lowCut = ysorted[ysorted.size() * 3 / 10];
            const float highCut = ysorted[ysorted.size() * 7 / 10];
            double lowX = 0.0;
            double highX = 0.0;
            int lowCount = 0;
            int highCount = 0;
            for (std::size_t i = 0; i < upright.size() / 3; ++i)
            {
                if (upright[3 * i + 1] <= lowCut)
                {
                    lowX += upright[3 * i];
                    ++lowCount;
                }
                else if (upright[3 * i + 1] >= highCut)
                {
                    highX += upright[3 * i];
                    ++highCount;
                }
            }
            Check(std::fabs(lowX / lowCount - highX / highCount) < 0.03, "a leaning cloud is stood upright");
        }

        // a turn is never a mirror image (Turn L must stay on the left): the signed volume of four points of the head has the same sign before and after, for the cloud
        // as it is and for the same cloud with its axes permuted (a cyclic permutation is a rotation of the data)
        {
            const auto volume = [](const std::vector<float>& cloud)
            {
                const auto p = [&](const std::size_t k, const int axis) { return static_cast<double>(cloud[3 * k + static_cast<std::size_t>(axis)]); };
                const std::size_t a = 1;
                const std::size_t b = 2;
                const std::size_t c = 3;
                const std::size_t d = 5;
                double m[3][3];
                for (int k = 0; k < 3; ++k)
                {
                    m[0][k] = p(b, k) - p(a, k);
                    m[1][k] = p(c, k) - p(a, k);
                    m[2][k] = p(d, k) - p(a, k);
                }
                return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            };
            std::vector<float> swapped(points.size());
            for (std::size_t i = 0; i < points.size(); i += 3)
            {
                swapped[i] = points[i + 2];
                swapped[i + 1] = points[i];
                swapped[i + 2] = points[i + 1];
            }
            std::vector<float> turned;
            NormalizePositions(swapped.data(), swapped.size() / 3, turned, nullptr);
            Check(volume(points) * volume(out) > 0.0, "the turn keeps the handedness (no mirror image)");
            Check(volume(swapped) * volume(turned) > 0.0, "also when the axes of the data come in another order");
        }
        std::sort(distances.begin(), distances.end());
        Check(Near(distances[static_cast<std::size_t>(static_cast<double>(distances.size()) * 0.995)], 1.0, 0.02), "the 99.5th percentile of the distance is 1");
        for (std::size_t i = valid; i < points.size() / 3; ++i)
        {
            Check(out[3 * i + 1] >= kHiddenDistance, "a neuron without a position is out of the picture");
        }

        // no points, a single point, NaN: no crash
        NormalizePositions(nullptr, 0, out, &missing);
        Check(out.empty() && missing == 0, "no points");
        const float one[3] = {5.0f, 6.0f, 7.0f};
        NormalizePositions(one, 1, out, &missing);
        Check(out.size() == 3 && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]), "one point");
        const float nan[6] = {std::nanf(""), 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
        NormalizePositions(nan, 2, out, &missing);
        Check(missing == 1 && std::isfinite(out[3]), "a NaN is a missing position");
    }

    // ------------------------------------------------------------------------------------------------ the numbers of the HUD

    void TestFlashes()
    {
        SpikeFlashes flashes;
        flashes.Reset(5);
        Check(flashes.Count() == 5 && flashes.Times()[0] == SpikeFlashes::kNever, "a neuron that never spiked has the 'never' time");
        flashes.SetNow(10.0);
        flashes.Mark(2, 9.75);
        Check(Near(flashes.Now() - flashes.Times()[2], 0.25, 1e-4), "the age of a flash is now minus its time");
        flashes.Mark(99, 1.0);   // outside: ignored
        Check(flashes.Times()[0] == SpikeFlashes::kNever, "the others are untouched");

        // a view that runs for hours: the base moves, the ages stay, a neuron that never spiked still never did
        flashes.SetNow(3500.0);
        flashes.Mark(3, 3499.9);
        const double ageOld = flashes.Now() - flashes.Times()[2];
        Check(flashes.Base() > 0.0, "the base moved forward");
        Check(Near(flashes.Now() - flashes.Times()[3], 0.1, 1e-3), "a recent flash keeps its age across the rebase");
        Check(flashes.Times()[0] == SpikeFlashes::kNever, "never stays never");
        Check(ageOld > 3000.0 || flashes.Times()[2] <= SpikeFlashes::kNever * 0.5f, "an old flash is old (or forgotten)");
        // the milliseconds survive an hour: a spike 2 ms ago is still 2 ms ago
        flashes.SetNow(7000.0);
        flashes.Mark(4, 6999.998);
        Check(Near(flashes.Now() - flashes.Times()[4], 0.002, 5e-4), "two milliseconds are two milliseconds after an hour");
    }

    void TestActivity()
    {
        GroupActivity activity;
        activity.Configure({4, 200});
        Check(activity.GroupCount() == 2 && activity.Rows(0) == 4 && activity.Rows(1) == GroupActivity::kMaxRows, "a small group has a row per neuron, a big one is folded");
        activity.Advance(5.0);
        activity.Advance(5.31);
        // group 0: 4 neurons, each spikes at 50 Hz for 0.3 s before 5.31
        for (int step = 0; step < 15; ++step)
        {
            for (uint32_t n = 0; n < 4; ++n)
            {
                activity.Record(0, n, 5.0 + 0.02 * step + 0.0001 * n);
            }
        }
        Check(Near(activity.RateHz(0, 0.3), 50.0, 5.0), "the rate is spikes per neuron per second: 50 Hz");
        Check(activity.RateHz(1, 0.3) == 0.0f, "the other group is silent");
        Check(activity.TotalSpikes(0) == 60, "the total counts all");

        std::vector<uint16_t> raster;
        activity.Raster(0, raster);
        Check(raster.size() == 4u * GroupActivity::kBinCount, "the raster is rows x bins");
        // the newest bin is last: the spikes of 5.28 are 3 bins before the end (5.31 is bin 531)
        uint32_t sum = 0;
        for (uint16_t cell : raster)
        {
            sum += cell;
        }
        Check(sum == 60, "the raster holds the spikes that are in the window");
        const int lastBinWithSpikes = [&]() {
            int last = -1;
            for (int k = 0; k < GroupActivity::kBinCount; ++k)
            {
                for (uint32_t row = 0; row < 4; ++row)
                {
                    if (raster[static_cast<std::size_t>(row) * GroupActivity::kBinCount + static_cast<std::size_t>(k)] > 0)
                    {
                        last = k;
                    }
                }
            }
            return last;
        }();
        Check(lastBinWithSpikes >= GroupActivity::kBinCount - 6 && lastBinWithSpikes <= GroupActivity::kBinCount - 2, "the newest spikes are at the right edge");

        // a big group folds into rows: 200 neurons into 48 rows, nothing is lost
        for (uint32_t n = 0; n < 200; ++n)
        {
            activity.Record(1, n, 5.30);
        }
        activity.Raster(1, raster);
        uint32_t folded = 0;
        for (uint16_t cell : raster)
        {
            folded += cell;
        }
        Check(folded == 200, "folding keeps every spike");

        // the window moves: after 2.5 s the old spikes are gone
        activity.Advance(8.0);
        activity.Raster(0, raster);
        sum = 0;
        for (uint16_t cell : raster)
        {
            sum += cell;
        }
        Check(sum == 0 && activity.RateHz(0, 0.5) == 0.0f, "spikes older than two seconds leave the window");
        // a spike of the future or of long ago is ignored
        const uint64_t before = activity.TotalSpikes(0);
        activity.Record(0, 0, 100.0);
        activity.Record(0, 0, 1.0);
        Check(activity.TotalSpikes(0) == before, "a spike outside the window is not counted");
        // a jump of the clock by minutes clears the whole window
        activity.Record(0, 1, 7.95);
        activity.Advance(500.0);
        activity.Raster(0, raster);
        sum = 0;
        for (uint16_t cell : raster)
        {
            sum += cell;
        }
        Check(sum == 0, "a long gap clears everything");
        // the counts of a group without the neurons (the GPU brain counts per group): they fill the rows evenly and the rate follows them
        activity.Configure({100});
        activity.Advance(20.0);
        activity.Advance(20.25);
        activity.RecordCount(0, 20.2, 480);      // 480 spikes in a bin of 10 ms: 100 neurons, 48 rows
        activity.Advance(20.35);
        Check(activity.TotalSpikes(0) == 480, "the counts are in the total");
        std::vector<uint16_t> countedRaster;
        activity.Raster(0, countedRaster);
        std::uint32_t countedSum = 0;
        std::uint16_t countedMax = 0;
        for (const uint16_t cell : countedRaster)
        {
            countedSum += cell;
            countedMax = std::max(countedMax, cell);
        }
        Check(countedSum == 480 && countedMax == 10, "480 spikes are 10 in each of the 48 rows");
        Check(Near(activity.RateHz(0, 0.2), 480.0 / (0.2 * 100.0), 3.0), "and the rate is the count over the neurons and the time");
        activity.RecordCount(0, 99.0, 5);          // the future: not counted
        Check(activity.TotalSpikes(0) == 480, "a count of the future is not taken");
        // a group of size zero and an unknown group do not crash
        activity.Configure({0});
        activity.Advance(1.0);
        activity.Record(0, 0, 1.0);
        activity.Record(7, 0, 1.0);
        Check(activity.RateHz(0, 0.1) == 0.0f && activity.RateHz(7, 0.1) == 0.0f, "an empty group has no rate");
    }

    void TestCamera()
    {
        OrbitCamera camera;
        const float yaw = camera.yaw;
        camera.Advance(1.0);
        Check(camera.yaw > yaw, "the camera turns by itself");
        camera.Orbit(40.0f, 0.0f);
        const float touched = camera.yaw;
        camera.Advance(1.0);
        Check(camera.yaw == touched, "it waits after a touch");
        camera.Advance(OrbitCamera::kResumeAfterSeconds + 0.5f);
        Check(camera.yaw > touched, "and goes on after a few seconds");
        camera.autoRotate = false;
        const float stopped = camera.yaw;
        camera.Advance(10.0);
        Check(camera.yaw == stopped, "auto rotation can be switched off");
        for (int i = 0; i < 200; ++i)
        {
            camera.Zoom(1.0f);
        }
        Check(camera.distance >= OrbitCamera::kMinDistance - 1e-4f && camera.distance <= OrbitCamera::kMinDistance + 1e-3f, "the zoom stops near the cloud");
        for (int i = 0; i < 400; ++i)
        {
            camera.Zoom(-1.0f);
        }
        Check(Near(camera.distance, OrbitCamera::kMaxDistance, 1e-3), "and far away");
        camera.Orbit(0.0f, 100000.0f);
        Check(camera.pitch <= 1.46f, "the pitch stays under the pole");
        render::Matrix4 view;
        render::Matrix4 projection;
        camera.Matrices(1.5f, view, projection);
        for (int i = 0; i < 16; ++i)
        {
            Check(std::isfinite(view.data[static_cast<std::size_t>(i)]) && std::isfinite(projection.data[static_cast<std::size_t>(i)]), "the matrices are finite");
        }
        camera.Matrices(0.0f, view, projection);   // a zero aspect does not break the projection
        Check(std::isfinite(projection.data[0]), "a zero aspect is repaired");
    }

    // ------------------------------------------------------------------------------------------------ the runner

    std::shared_ptr<const flybrain::Connectome> WiredBrain(uint32_t& left, uint32_t& right, uint32_t& dnLeft)
    {
        flybrain::ConnectomeBuilder builder;
        flybrain::NeuronInfo info;
        info.transmitter = flybrain::Transmitter::Acetylcholine;
        info.position[0] = 1.0f;
        info.position[1] = 1.0f;
        info.position[2] = 1.0f;
        info.type = "R1-R6";
        info.side = flybrain::Side::Left;
        info.bodyId = 1;
        left = builder.AddNeuron(info);
        info.side = flybrain::Side::Right;
        info.bodyId = 2;
        right = builder.AddNeuron(info);
        info.type = "DNa02";
        info.side = flybrain::Side::Left;
        info.superclass = "descending_neuron";
        info.bodyId = 3;
        dnLeft = builder.AddNeuron(info);
        builder.AddEdge(left, dnLeft, 200);
        std::string error;
        auto connectome = flybrain::Connectome::FromImage(builder.Build("test"), &error);
        Check(connectome != nullptr, "the wired brain: " + error);
        return connectome;
    }

    void TestRunner()
    {
        myengine::jobs::Initialize();
        {
            uint32_t left = 0;
            uint32_t right = 0;
            uint32_t dnLeft = 0;
            const auto brain = WiredBrain(left, right, dnLeft);
            BrainRunner runner(brain);
            runner.SetPreset(StimulusPreset::Silence);
            runner.Start();
            Check(runner.Running(), "the runner is running");
            std::vector<BrainRunner::Spike> spikes;
            double simNow = 0.0;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            runner.Drain(spikes, simNow);
            Check(spikes.empty(), "a LIF network without input does not spike");
            Check(simNow > 0.1 && simNow < 0.6, "the simulation runs at about real time (" + std::to_string(simNow) + " s in 0.3 s)");

            runner.SetPreset(StimulusPreset::Vision);
            std::vector<BrainRunner::Spike> all;
            double last = 0.0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2600);
            while (std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                runner.Drain(spikes, last);
                all.insert(all.end(), spikes.begin(), spikes.end());
            }
            Check(!all.empty(), "the vision stimulus makes spikes");
            bool ordered = true;
            bool leftSpiked = false;
            bool rightSpiked = false;
            bool targetSpiked = false;
            for (std::size_t i = 0; i < all.size(); ++i)
            {
                ordered = ordered && (i == 0 || all[i].simSeconds >= all[i - 1].simSeconds - 1e-9);
                ordered = ordered && all[i].simSeconds <= last + 1e-6;
                leftSpiked = leftSpiked || all[i].neuron == left;
                rightSpiked = rightSpiked || all[i].neuron == right;
                targetSpiked = targetSpiked || all[i].neuron == dnLeft;
            }
            Check(ordered, "the spikes come in order and never from the future");
            Check(leftSpiked, "the left eye was stimulated first");
            Check(targetSpiked, "the descending neuron wired to the left eye fires");
            Check(rightSpiked, "after 1.5 s the right eye is stimulated");

            const BrainRunner::Stats stats = runner.GetStats();
            Check(stats.totalSpikes > 0 && stats.simSeconds > 1.0, "the stats count the run");
            runner.SetPaused(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            runner.Drain(spikes, last);
            const double pausedAt = last;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            runner.Drain(spikes, last);
            Check(Near(last, pausedAt, 1e-9) && spikes.empty(), "pause stops the simulation");
            runner.SetPaused(false);

            // a command runs on the thread of the brain
            std::atomic<bool> ran{false};
            runner.Post([&ran](flybrain::FlyBrain& flyBrain) { ran.store(flyBrain.TimeMs() >= 0.0); });
            for (int i = 0; i < 100 && !ran.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(ran.load(), "a posted command runs");
            runner.Stop();
            Check(!runner.Running(), "Stop joins the thread");
            runner.Stop();   // twice is fine
        }
        myengine::jobs::Shutdown();
    }

    // ------------------------------------------------------------------------------------------------ the picture

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    struct Rgb
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        double Sum() const { return r + g + b; }
    };

    Rgb MeanOf(const std::vector<std::uint8_t>& pixels, const uint32_t width, const uint32_t x0, const uint32_t y0, const uint32_t x1, const uint32_t y1)
    {
        Rgb total;
        uint32_t count = 0;
        for (uint32_t y = y0; y < y1; ++y)
        {
            for (uint32_t x = x0; x < x1; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                total.r += pixels[i];
                total.g += pixels[i + 1];
                total.b += pixels[i + 2];
                ++count;
            }
        }
        total.r /= count;
        total.g /= count;
        total.b /= count;
        return total;
    }

    // 0: passed, kSkipped: no device or desktop
    int TestPicture()
    {
        myengine::core::Logger logger;
        logger.Initialize(fs::temp_directory_path() / "myengine_brainview_test.log");
        myengine::render::dx12::Dx12RenderAdapter adapter(logger);
        if (!adapter.Initialize())
        {
            std::cout << "brainview: SKIP the picture (no DX12 device)\n";
            return kSkipped;
        }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MyEngineBrainViewTest";
        RegisterClassW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"brainview", WS_OVERLAPPEDWINDOW, 100, 100, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (hwnd == nullptr)
        {
            std::cout << "brainview: SKIP the picture (no desktop to make the window on)\n";
            adapter.Shutdown();
            return kSkipped;
        }
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        const render::RenderSurfaceHandle surface = adapter.CreateSurface(hwnd, 320, 240);
        Check(surface.IsValid(), "the surface");

        // a cloud: a hot patch of 400 points of colour 1 (cyan) in the middle, points of colour 2 (orange) around it
        render::PointCloudData data;
        std::mt19937 random(3);
        std::normal_distribution<float> normal(0.0f, 1.0f);
        const uint32_t patch = 400;
        const uint32_t ring = 1500;
        for (uint32_t i = 0; i < patch; ++i)
        {
            data.positions.push_back(0.04f * normal(random));
            data.positions.push_back(0.04f * normal(random));
            data.positions.push_back(0.04f * normal(random));
            data.colorIndex.push_back(1);
        }
        for (uint32_t i = 0; i < ring; ++i)
        {
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(ring);
            data.positions.push_back(0.9f * std::cos(angle));
            data.positions.push_back(0.9f * std::sin(angle));
            data.positions.push_back(0.0f);
            data.colorIndex.push_back(2);
        }
        const uint32_t count = patch + ring;
        const render::PointCloudHandle cloud = adapter.CreatePointCloud(data);
        if (!cloud.IsValid())
        {
            std::cout << "brainview: SKIP the picture (the point cloud shader did not build: assets/shaders/brain_cloud.hlsl is not next to the test)\n";
            adapter.Shutdown();
            return kSkipped;
        }

        constexpr uint32_t kSize = 128;
        const render::RenderTargetHandle target = adapter.CreateRenderTarget(kSize, kSize);
        Check(target.IsValid(), "the render target");

        std::vector<float> flashTimes(count, SpikeFlashes::kNever);
        const auto draw = [&](const std::vector<float>& times, const float now, const bool bloom, const float distance, std::vector<std::uint8_t>& pixels)
        {
            render::PointCloudDrawParams params;
            OrbitCamera camera;
            camera.distance = distance;
            camera.pitch = 0.0f;
            camera.yaw = 0.0f;
            camera.Matrices(1.0f, params.view, params.projection);
            params.flashTimes = times.data();
            params.time = now;
            params.decaySeconds = 0.15f;
            params.baseBrightness = 0.0f;
            params.flashBrightness = 0.04f;
            params.baseSizePixels = 2.0f;
            params.flashSizePixels = 4.0f;
            params.referenceDepth = distance;
            FillPalette(params.palette);
            params.bloom = bloom;
            params.bloomThreshold = 0.15f;
            params.bloomIntensity = 1.0f;
            Check(adapter.BeginFrame(surface, myengine::core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame");
            Check(adapter.BeginRenderTarget(target, myengine::core::Color{0.0f, 0.0f, 0.0f, 0.0f}), "BeginRenderTarget");
            Check(adapter.DrawPointCloud(surface, cloud, params), "DrawPointCloud");
            adapter.EndRenderTarget(target);
            adapter.EndFrame(surface);
            Check(adapter.ReadRenderTargetPixels(target, pixels) && pixels.size() == static_cast<std::size_t>(kSize) * kSize * 4, "the pixels of the target");
        };

        // nothing flashed and no base light: black
        std::vector<std::uint8_t> pixels;
        draw(flashTimes, 10.0f, true, 3.0f, pixels);
        Check(MeanOf(pixels, kSize, 0, 0, kSize, kSize).Sum() < 1.0, "points that never flashed give a black picture");

        // the patch flashes just now: it is bright in the middle and the colour is that of its class (cyan: blue above red)
        for (uint32_t i = 0; i < patch; ++i)
        {
            flashTimes[i] = 10.0f;
        }
        std::vector<std::uint8_t> flash;
        draw(flashTimes, 10.0f, true, 3.0f, flash);
        const Rgb centre = MeanOf(flash, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
        const Rgb corner = MeanOf(flash, kSize, 0, 0, 16, 16);
        Check(centre.Sum() > 60.0, "a flashing patch is bright in the middle (" + std::to_string(centre.Sum()) + ")");
        Check(corner.Sum() < 2.0, "and the corner stays dark");
        Check(centre.b > centre.r * 1.4, "the patch has the colour of its class: cyan (rgb " + std::to_string(centre.r) + " " + std::to_string(centre.g) + " " + std::to_string(centre.b) + ")");
        // the same patch a tenth of a second later is dimmer, and half a second later it is almost gone
        std::vector<std::uint8_t> faded;
        draw(flashTimes, 10.1f, true, 3.0f, faded);
        const Rgb fading = MeanOf(faded, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
        draw(flashTimes, 10.8f, true, 3.0f, faded);
        const Rgb gone = MeanOf(faded, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
        Check(fading.Sum() < centre.Sum() * 0.9 && fading.Sum() > gone.Sum(), "a flash fades with time");
        Check(gone.Sum() < centre.Sum() * 0.2, "and is nearly gone after half a second");

        // the bloom: light around the patch where no point is
        std::vector<std::uint8_t> noBloom;
        draw(flashTimes, 10.0f, false, 3.0f, noBloom);
        const Rgb halo = MeanOf(flash, kSize, kSize / 2 + 12, kSize / 2 - 3, kSize / 2 + 20, kSize / 2 + 3);
        const Rgb haloNoBloom = MeanOf(noBloom, kSize, kSize / 2 + 12, kSize / 2 - 3, kSize / 2 + 20, kSize / 2 + 3);
        Check(halo.Sum() > haloNoBloom.Sum() + 1.5, "the bloom adds light around a flash (" + std::to_string(halo.Sum()) + " against " + std::to_string(haloNoBloom.Sum()) + ")");

        // the ring flashes too: orange at the edge, and the cloud seen from far away is smaller (the ring is closer to the middle)
        for (uint32_t i = patch; i < count; ++i)
        {
            flashTimes[i] = 10.0f;
        }
        draw(flashTimes, 10.0f, true, 3.0f, pixels);
        const Rgb ringColour = MeanOf(pixels, kSize, kSize - 24, kSize / 2 - 4, kSize - 8, kSize / 2 + 4);
        const Rgb ringLeft = MeanOf(pixels, kSize, 8, kSize / 2 - 4, 24, kSize / 2 + 4);
        Check(ringColour.r > ringColour.b * 1.5 && ringLeft.r > ringLeft.b * 1.5, "the ring has the colour of its class: orange, at both sides");
        std::vector<std::uint8_t> distant;
        draw(flashTimes, 10.0f, true, 8.0f, distant);
        const Rgb farEdge = MeanOf(distant, kSize, kSize - 24, kSize / 2 - 4, kSize - 8, kSize / 2 + 4);
        Check(farEdge.Sum() < ringColour.Sum() * 0.5, "a camera that is farther away sees the ring smaller: the edge is dark");
        // a point behind the camera (a camera inside the cloud) must not crash or smear the picture
        draw(flashTimes, 10.0f, true, 0.5f, pixels);

        // the flashes from a buffer of the GPU brain (the step of the last spike of every point, 0xFFFFFFFF never): the same picture as from the floats
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> steps;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = static_cast<UINT64>(count) * sizeof(std::uint32_t);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Check(SUCCEEDED(adapter.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(steps.GetAddressOf()))),
                  "a buffer of steps");
            std::uint32_t* mapped = nullptr;
            Check(SUCCEEDED(steps->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "map the steps");
            const std::uint32_t flashStep = 5000;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                mapped[i] = i < patch ? flashStep : 0xFFFFFFFFu;   // the patch flashed at step 5000, the ring never
            }
            const auto drawGpu = [&](const std::uint32_t stepNow, const float fraction, std::vector<std::uint8_t>& out)
            {
                render::PointCloudDrawParams params;
                OrbitCamera camera;
                camera.distance = 3.0f;
                camera.pitch = 0.0f;
                camera.yaw = 0.0f;
                camera.Matrices(1.0f, params.view, params.projection);
                params.decaySeconds = 0.15f;
                params.baseBrightness = 0.0f;
                params.flashBrightness = 0.04f;
                params.baseSizePixels = 2.0f;
                params.flashSizePixels = 4.0f;
                params.referenceDepth = 3.0f;
                FillPalette(params.palette);
                params.bloom = true;
                params.bloomThreshold = 0.15f;
                params.bloomIntensity = 1.0f;
                params.gpuFlash.buffer = steps.Get();
                params.gpuFlash.stepWhole = stepNow;
                params.gpuFlash.stepFraction = fraction;
                params.gpuFlash.stepSeconds = 0.001f;   // 1 ms a step
                Check(adapter.BeginFrame(surface, myengine::core::Color{0.0f, 0.0f, 0.0f, 1.0f}), "BeginFrame");
                Check(adapter.BeginRenderTarget(target, myengine::core::Color{0.0f, 0.0f, 0.0f, 0.0f}), "BeginRenderTarget");
                Check(adapter.DrawPointCloud(surface, cloud, params), "DrawPointCloud from a buffer of the GPU");
                adapter.EndRenderTarget(target);
                adapter.EndFrame(surface);
                Check(adapter.ReadRenderTargetPixels(target, out), "the pixels");
            };
            // just flashed: the same as the floats at age 0 (the earlier picture had the whole patch at time 10 = now)
            std::vector<std::uint8_t> fromSteps;
            std::vector<std::uint8_t> reference;
            std::vector<float> patchOnly(count, SpikeFlashes::kNever);
            for (std::uint32_t i = 0; i < patch; ++i)
            {
                patchOnly[i] = 10.0f;
            }
            draw(patchOnly, 10.0f, true, 3.0f, reference);
            drawGpu(flashStep, 0.0f, fromSteps);
            const Rgb gpuCentre = MeanOf(fromSteps, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
            const Rgb refCentre = MeanOf(reference, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
            Check(Near(gpuCentre.Sum(), refCentre.Sum(), refCentre.Sum() * 0.03 + 2.0), "the picture from the steps of the GPU is the picture from the floats (" + std::to_string(gpuCentre.Sum()) + " against " +
                                                                                           std::to_string(refCentre.Sum()) + ")");
            Check(MeanOf(fromSteps, kSize, kSize - 24, kSize / 2 - 4, kSize - 8, kSize / 2 + 4).Sum() < 2.0, "points that never spiked are dark");
            // 150 steps (0.15 s = one decay time) later it is dimmer, as the floats at age 0.15
            std::vector<std::uint8_t> older;
            drawGpu(flashStep + 150, 0.0f, older);
            std::vector<std::uint8_t> olderReference;
            draw(patchOnly, 10.15f, true, 3.0f, olderReference);
            const Rgb gpuOlder = MeanOf(older, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
            const Rgb refOlder = MeanOf(olderReference, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
            Check(gpuOlder.Sum() < gpuCentre.Sum() * 0.9 && Near(gpuOlder.Sum(), refOlder.Sum(), refOlder.Sum() * 0.05 + 2.0), "the age in steps fades the flash as the age in seconds does");
            // the fraction of a step moves the age: 150.0 + 100 steps = 250 ms
            std::vector<std::uint8_t> fraction;
            drawGpu(flashStep + 100, 50.0f, fraction);
            const Rgb gpuFraction = MeanOf(fraction, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6);
            Check(Near(gpuFraction.Sum(), gpuOlder.Sum(), gpuOlder.Sum() * 0.02 + 1.0), "100 steps and half a hundred more are the same age as 150 steps");
            // a step that is not yet known (newer than 'now') is age 0, not a flash in the future
            std::vector<std::uint8_t> future;
            drawGpu(flashStep - 500, 0.0f, future);
            Check(MeanOf(future, kSize, kSize / 2 - 6, kSize / 2 - 6, kSize / 2 + 6, kSize / 2 + 6).Sum() > refCentre.Sum() * 0.9, "a spike newer than the clock counts as just now");
            // a buffer that is too small for the cloud is refused, not read past its end
            {
                render::PointCloudDrawParams params;
                params.gpuFlash.buffer = steps.Get();
                Microsoft::WRL::ComPtr<ID3D12Resource> tiny;
                D3D12_RESOURCE_DESC tinyDesc = desc;
                tinyDesc.Width = 64;
                Check(SUCCEEDED(adapter.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tinyDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(tiny.GetAddressOf()))),
                      "a small buffer");
                params.gpuFlash.buffer = tiny.Get();
                Check(adapter.BeginFrame(surface, myengine::core::Color{0, 0, 0, 1}), "BeginFrame");
                Check(adapter.BeginRenderTarget(target, myengine::core::Color{0, 0, 0, 0}), "BeginRenderTarget");
                Check(!adapter.DrawPointCloud(surface, cloud, params), "a buffer too small for the cloud is refused");
                adapter.EndRenderTarget(target);
                adapter.EndFrame(surface);
            }
            steps->Unmap(0, nullptr);
        }

        // the size of the target changes: the HDR targets are rebuilt, nothing leaks
        for (int round = 0; round < 6; ++round)
        {
            const uint32_t size = 64u + 32u * static_cast<uint32_t>(round);
            const render::RenderTargetHandle other = adapter.CreateRenderTarget(size, size);
            Check(other.IsValid(), "another target");
            render::PointCloudDrawParams params;
            OrbitCamera camera;
            camera.Matrices(1.0f, params.view, params.projection);
            params.flashTimes = flashTimes.data();
            params.time = 10.0f;
            FillPalette(params.palette);
            Check(adapter.BeginFrame(surface, myengine::core::Color{0, 0, 0, 1}), "BeginFrame");
            Check(adapter.BeginRenderTarget(other, myengine::core::Color{0, 0, 0, 0}), "BeginRenderTarget");
            Check(adapter.DrawPointCloud(surface, cloud, params), "DrawPointCloud into another size");
            adapter.EndRenderTarget(other);
            adapter.EndFrame(surface);
            adapter.DestroyRenderTarget(other);
        }
        // outside a render target and with bad arguments the call is refused, not a crash
        {
            render::PointCloudDrawParams params;
            params.flashTimes = flashTimes.data();
            Check(adapter.BeginFrame(surface, myengine::core::Color{0, 0, 0, 1}), "BeginFrame");
            Check(!adapter.DrawPointCloud(surface, cloud, params), "a cloud is drawn into a render target only");
            Check(adapter.BeginRenderTarget(target, myengine::core::Color{0, 0, 0, 0}), "BeginRenderTarget");
            Check(!adapter.DrawPointCloud(surface, render::PointCloudHandle{9999}, params), "an unknown cloud is refused");
            params.flashTimes = nullptr;
            Check(!adapter.DrawPointCloud(surface, cloud, params), "no flash times: refused");
            adapter.EndRenderTarget(target);
            adapter.EndFrame(surface);
        }
        adapter.DestroyPointCloud(cloud);
        adapter.DestroyPointCloud(cloud);   // twice is fine
        adapter.DestroyRenderTarget(target);
        std::cout << "brainview: the picture ok (patch " << centre.Sum() << ", halo " << halo.Sum() << " against " << haloNoBloom.Sum() << ")\n";
        adapter.Shutdown();
        DestroyWindow(hwnd);
        return 0;
    }
}

int main()
{
    try
    {
        TestLayout();
        TestNormalize();
        TestFlashes();
        TestActivity();
        TestCamera();
        TestRunner();
        std::cout << "brainview: layout, numbers, camera and runner ok\n";
        const int picture = TestPicture();
        if (picture == 0)
        {
            std::cout << "brainview: all passed\n";
        }
        return picture;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "brainview FAILED: " << exception.what() << "\n";
        return 1;
    }
}
