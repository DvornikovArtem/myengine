// FlySwarmBrain.cpp

#include <flybrain/FlySwarmBrain.h>

#include <algorithm>
#include <cmath>

namespace flybrain
{
    namespace
    {
        Group Join(const Connectome& cn, std::initializer_list<const char*> types, Side side)
        {
            Group all;
            for (const char* type : types)
            {
                const Group g = cn.Find(type, side);
                all.neurons.insert(all.neurons.end(), g.neurons.begin(), g.neurons.end());
            }
            std::sort(all.neurons.begin(), all.neurons.end());
            return all;
        }
    }

    std::unique_ptr<FlySwarmBrain> FlySwarmBrain::Create(std::shared_ptr<const Connectome> circuit, uint32_t flies, const SwarmOptions& options,
        std::string* error)
    {
        auto fail = [&](const std::string& message) -> std::unique_ptr<FlySwarmBrain>
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return nullptr;
        };
        if (circuit == nullptr || flies == 0)
        {
            return fail("no circuit or no flies");
        }
        std::unique_ptr<FlySwarmBrain> swarm(new FlySwarmBrain());
        const Connectome& cn = *circuit;
        swarm->options_ = options;
        swarm->flies_ = flies;
        swarm->loomLeft_ = Join(cn, { "LPLC2", "LC4" }, Side::Left);
        swarm->loomRight_ = Join(cn, { "LPLC2", "LC4" }, Side::Right);
        swarm->inputs_ = Join(cn, { "LPLC2", "LC4" }, Side::Any);
        swarm->readouts_[SteerLeft] = cn.Find("DNa02", Side::Left);
        swarm->readouts_[SteerLeftSlow] = cn.Find("DNa01", Side::Left);
        swarm->readouts_[SteerRight] = cn.Find("DNa02", Side::Right);
        swarm->readouts_[SteerRightSlow] = cn.Find("DNa01", Side::Right);
        swarm->readouts_[Forward] = cn.Find("DNp09");
        swarm->readouts_[Backward] = cn.Find("MDN");
        swarm->readouts_[Escape] = cn.Find("DNp01");
        if (swarm->loomLeft_.Empty() || swarm->loomRight_.Empty())
        {
            return fail("the circuit has no LPLC2/LC4 on both sides");
        }
        for (const Group& g : swarm->readouts_)
        {
            if (g.Empty())
            {
                return fail("the circuit has no DNa01/DNa02 on both sides, DNp09, MDN or DNp01");
            }
        }

        swarm->brain_ = std::make_unique<FlyBrain>(std::move(circuit), ModelParams{}, flies);
        FlyBrain& brain = *swarm->brain_;
        brain.SetParallel(options.parallel);
        brain.SetSpikeRingCapacity(1u << 16);
        for (uint32_t fly = 0; fly < flies; ++fly)
        {
            for (const Group& g : swarm->readouts_)
            {
                brain.Track(brain.ForReplica(g, fly));
            }
        }
        if (options.backend != BackendChoice::Cpu)
        {
            std::string backendError;
            if (brain.SetBackend(options.backend, options.gpu, &backendError) != Backend::Gpu && options.backend == BackendChoice::Gpu)
            {
                return fail("no GPU backend: " + backendError);
            }
        }
        swarm->inputsNow_.assign(flies, SwarmInput{});
        swarm->outputs_.assign(flies, SwarmOutput{});
        swarm->lastTotals_.assign(size_t(flies) * ReadoutCount, 0);
        swarm->smoothed_.assign(size_t(flies) * ReadoutCount, 0.0);
        return swarm;
    }

    void FlySwarmBrain::SetInput(uint32_t fly, const SwarmInput& input)
    {
        FlyBrain& brain = *brain_;
        SwarmInput& now = inputsNow_[fly];
        auto rate = [&](float loom) { return std::clamp(loom, 0.0f, 1.0f) * options_.loomRateHz; };
        if (input.loomLeft != now.loomLeft)
        {
            for (uint32_t neuron : loomLeft_.neurons)
            {
                brain.SetPoissonRate(brain.Unit(neuron, fly), rate(input.loomLeft));
            }
        }
        if (input.loomRight != now.loomRight)
        {
            for (uint32_t neuron : loomRight_.neurons)
            {
                brain.SetPoissonRate(brain.Unit(neuron, fly), rate(input.loomRight));
            }
        }
        if (input.brightness != now.brightness)
        {
            const float drive = std::clamp(input.brightness, 0.0f, 1.0f) * options_.brightnessMv;
            for (uint32_t neuron : inputs_.neurons)
            {
                brain.SetDrive(brain.Unit(neuron, fly), drive);
            }
        }
        now = input;
    }

    void FlySwarmBrain::AdvanceMs(double ms)
    {
        brain_->AdvanceMs(ms);
        UpdateOutputs();
    }

    void FlySwarmBrain::Synchronize()
    {
        brain_->Synchronize();
        UpdateOutputs();
    }

    void FlySwarmBrain::UpdateOutputs()
    {
        const FlyBrain& brain = *brain_;
        const uint64_t completed = brain.CompletedStep();
        if (completed <= lastStep_)
        {
            return;
        }
        const double seconds = double(completed - lastStep_) * brain.Params().dtMs / 1000.0;
        // Exponential smoothing over the finished interval
        const double keep = std::exp(-double(completed - lastStep_) * brain.Params().dtMs / options_.smoothingMs);
        for (uint32_t fly = 0; fly < flies_; ++fly)
        {
            double* rate = &smoothed_[size_t(fly) * ReadoutCount];
            uint64_t* last = &lastTotals_[size_t(fly) * ReadoutCount];
            for (int r = 0; r < ReadoutCount; ++r)
            {
                uint64_t total = 0;
                for (uint32_t neuron : readouts_[r].neurons)
                {
                    total += brain.TrackedSpikes(brain.Unit(neuron, fly));
                }
                const double hz = double(total - last[r]) / (seconds * double(readouts_[r].Size()));
                rate[r] = keep * rate[r] + (1.0 - keep) * hz;
                last[r] = total;
            }
            SwarmOutput& out = outputs_[fly];
            out.steerLeftHz = static_cast<float>(rate[SteerLeft] + 0.5 * rate[SteerLeftSlow]);
            out.steerRightHz = static_cast<float>(rate[SteerRight] + 0.5 * rate[SteerRightSlow]);
            out.forwardHz = static_cast<float>(rate[Forward]);
            out.backwardHz = static_cast<float>(rate[Backward]);
            out.escapeHz = static_cast<float>(rate[Escape]);
            out.escapeSpikes = static_cast<uint32_t>(last[Escape]);
            const double scale = options_.commandScaleHz;
            out.turn = static_cast<float>(std::tanh((out.steerRightHz - out.steerLeftHz) / scale));
            out.forward = static_cast<float>(std::tanh((out.forwardHz - out.backwardHz) / scale));
            out.escape = static_cast<float>(std::tanh(out.escapeHz / scale));
        }
        lastStep_ = completed;
    }
}
