// FlySwarmBrain.h
// The brains of the enemy flies of the swarm (N2): one small circuit (ExtractCircuit, assets/flybrain/swarm_circuit.fbrain)
// and a brain of it for every fly, all in one FlyBrain with a replica per fly: one CSR, the state [neuron][fly], one window
// for the whole swarm on the CPU (AVX2 over the flies of a neuron) or the GPU. The game gives every fly what it sees
// (looming on the left and right eye, brightness) and gets back steering, walking and the escape jump.
// docs/flybrain/flybrain.md, section "Swarm".

#pragma once

#include <flybrain/FlyBrain.h>

#include <memory>
#include <string>
#include <vector>

namespace flybrain
{
    // What a fly sees in a frame, 0..1
    struct SwarmInput
    {
        float loomLeft = 0.0f;   // something approaching on the left (angular size and speed of approach)
        float loomRight = 0.0f;
        float brightness = 0.0f;
    };

    // What the fly does: rates of the descending neurons smoothed over smoothingMs, and the commands made of them
    struct SwarmOutput
    {
        float turn = 0.0f;       // -1 left .. +1 right: DNa02 (+ half of DNa01) right minus left
        float forward = 0.0f;    // -1 back .. +1 forward: DNp09 minus MDN
        float escape = 0.0f;     // 0..1: the Giant Fiber DNp01
        float steerLeftHz = 0.0f;
        float steerRightHz = 0.0f;
        float forwardHz = 0.0f;
        float backwardHz = 0.0f;
        float escapeHz = 0.0f;
        uint32_t escapeSpikes = 0; // Giant Fiber spikes since the start: a new one is a jump
    };

    struct SwarmOptions
    {
        float loomRateHz = 200.0f;  // Poisson rate into LPLC2/LC4 of a side at loom 1
        float brightnessMv = 5.0f;  // tonic drive into the input neurons at brightness 1 (below the threshold)
        float smoothingMs = 100.0f; // as the readout of DOOMFLY
        float commandScaleHz = 50.0f; // turn = tanh(difference / scale)
        BackendChoice backend = BackendChoice::Cpu;
        GpuOptions gpu;
        bool parallel = true;       // CPU: windows on the job system
    };

    class FlySwarmBrain
    {
    public:
        // nullptr circuit or a circuit without the inputs / outputs: error says what is missing
        static std::unique_ptr<FlySwarmBrain> Create(std::shared_ptr<const Connectome> circuit, uint32_t flies, const SwarmOptions& options = {},
            std::string* error = nullptr);

        uint32_t FlyCount() const { return flies_; }
        Backend GetBackend() const { return brain_->GetBackend(); }
        FlyBrain& Brain() { return *brain_; }
        const FlyBrain& Brain() const { return *brain_; }

        void SetInput(uint32_t fly, const SwarmInput& input);
        // All flies; the outputs follow the spikes the backend has finished (on the GPU: the previous call)
        void AdvanceMs(double ms);
        void Synchronize();
        const SwarmOutput& Output(uint32_t fly) const { return outputs_[fly]; }

    private:
        enum Readout { SteerLeft, SteerLeftSlow, SteerRight, SteerRightSlow, Forward, Backward, Escape, ReadoutCount };

        FlySwarmBrain() = default;
        void UpdateOutputs();

        std::unique_ptr<FlyBrain> brain_;
        SwarmOptions options_;
        uint32_t flies_ = 0;
        Group loomLeft_;   // neurons (not units)
        Group loomRight_;
        Group inputs_;
        Group readouts_[ReadoutCount];
        std::vector<SwarmInput> inputsNow_;
        std::vector<SwarmOutput> outputs_;
        std::vector<uint64_t> lastTotals_; // [fly][readout]
        std::vector<double> smoothed_;     // [fly][readout], Hz per neuron
        uint64_t lastStep_ = 0;
    };
}
