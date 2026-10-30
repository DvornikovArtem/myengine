// flybrain.hlsl
// The LIF fly brain of FlyBrain (game/flybrain) on the GPU: the same model, the same order of operations and the same
// double arithmetic as the CPU code (FlyBrain.cpp, StepBlock and CatchUp), so the spikes are the same bit for bit.
// docs/flybrain/flybrain.md, section "GPU". Compiled by fxc (cs_5_0): doubles are stored as uint2 and read with asdouble,
// 64-bit integers do not exist, so steps are uint (enough for 119 hours of simulated time).
//
// A window of up to 18 steps (the synaptic delay) is
//   CSUpdate    stimuli and drives that changed since the last window (catch-up under the old drive, then the new one)
//   CSCompact   the neurons that have input or may fire, packed into a list (about a fifth of the brain in a window)
//   CSIntegrate one thread per neuron of that list: the steps of the window, spikes into the window list, the GPU spike ring and the
//               per-step counts of the watched groups
//   CSDeliver   the spikes of the window along the CSR: integer atomic adds into the input ring, in 1/256 of a contact; a
//               plastic synapse (N3, KC -> MBON) adds its learned weight of the replica
//   CSPlastic   learned weights that changed since the last window (the learning step runs on the CPU, FlyBrainPlasticity)
// CSReset clears the counters at the start of a submission.

struct NeuronState
{
    uint2 u;               // double: v - v_0, mV
    uint2 g;               // double: synaptic input, mV
    uint lastStep;         // u, g are valid at this step
    uint refractoryUntil;  // the first step that integrates again
    uint awake;            // 1: may fire without input, stepped every window
    uint unused;
};

struct NeuronParams
{
    float driveExternal;   // SetDrive
    float driveStimulus;   // StimulateCurrent
    uint poissonThreshold; // P(event per step) * 2^32
    uint refractorySteps;
    uint watchMask;        // bit w: the neuron belongs to watch w
    uint unused0;
    uint unused1;
    uint unused2;
};

struct NeuronUpdate
{
    uint neuron;
    uint wake;             // 1: settle the state under the old drive and wake the neuron (FlyBrain::Wake)
    float driveExternal;
    float driveStimulus;
    uint poissonThreshold;
    uint refractorySteps;
    uint watchMask;
    uint unused;
};

cbuffer Model : register(b0)
{
    uint2 kDecayU;         // exp(-dt / t_mbr)
    uint2 kDecayG;         // exp(-dt / tau)
    uint2 kStepCoupling;   // K (decayG - decayU): input g -> u over one step
    uint2 kCouplingK;      // tau / (tau - t_mbr)
    uint2 kWeight;         // w_syn, mV per contact
    uint2 kKick;           // w_syn * f_poi
    uint2 kThreshold;      // v_th - v_0
    uint2 kReset;          // v_rst - v_0
    uint kNeuronCount;
    uint kDelaySteps;
    uint kTableSize;
    uint kSpikeCapacity;
    uint kRingMask;        // ring capacity - 1, a power of two
    uint kSeedHash;        // the seed and the high half of the step, mixed as in PoissonDraw
    uint kWatchStride;     // 33: 32 watches and the whole brain
    uint kMaxWindows;      // windows of a submission: Counters[2 + kMaxWindows + window] counts its active neurons
    uint kReplicas;        // independent brains on the CSR: unit = neuron * kReplicas + replica (kNeuronCount counts units)
    uint kPlastic;         // 1: PlasticRowOffset, EdgePlastic and PlasticWeights are bound
    uint2 kUnused;
};

cbuffer Window : register(b1)
{
    uint gWindowBegin;
    uint gSteps;
    uint gFirstSlot;       // gWindowBegin % kDelaySteps
    uint gWindowIndex;     // window of the submission: its spike counter
    uint gStepOffset;      // gWindowBegin - the first step of the submission: row of the watch counts
    uint gUpdateOffset;
    uint gUpdateCount;
    uint gGroupCount;      // CSDeliver: groups of the dispatch; CSReset: values to clear
};

StructuredBuffer<uint> RowStart : register(t0);
StructuredBuffer<uint> Targets : register(t1);
StructuredBuffer<uint> Weights : register(t2);   // int16 pairs, the even edge in the low half
StructuredBuffer<uint2> PowU : register(t3);     // double: decayU^k, k = 0..kTableSize
StructuredBuffer<uint2> PowG : register(t4);
StructuredBuffer<NeuronUpdate> Updates : register(t5);
StructuredBuffer<uint> PlasticRowOffset : register(t6); // per neuron: offset of its row into EdgePlastic, 0xFFFFFFFF: none
StructuredBuffer<int> EdgePlastic : register(t7);       // per edge of those rows: the plastic synapse or -1
StructuredBuffer<uint2> PlasticUpdates : register(t8);  // (synapse * kReplicas + replica, weight)
StructuredBuffer<uint> SilentOutput : register(t9);     // per neuron: 1 when its spikes are not delivered (modulatory, N3)

RWStructuredBuffer<NeuronState> State : register(u0);
RWStructuredBuffer<NeuronParams> Params : register(u1);
RWStructuredBuffer<uint> Pending : register(u2);
RWStructuredBuffer<uint> SpikeTotals : register(u3);
RWStructuredBuffer<int> Arrivals : register(u4);     // [step % kDelaySteps][neuron], contacts
RWStructuredBuffer<uint2> SpikeList : register(u5);  // spikes of the window: neuron, step
RWStructuredBuffer<uint> Counters : register(u6);    // [0] ring head, [1] lost spikes, [2 + window] spikes of the window,
                                                     // [2 + kMaxWindows + window] active neurons of the window
RWStructuredBuffer<uint> WatchCounts : register(u7); // [step of the submission][kWatchStride]
RWStructuredBuffer<uint2> Ring : register(u8);       // the last spikes of the brain: neuron, step (BrainView reads it)
RWStructuredBuffer<uint> Active : register(u9);      // the neurons CSIntegrate steps in this window
RWStructuredBuffer<uint> LastSpike : register(u10);  // step of the last spike of every neuron, 0xFFFFFFFF: none (BrainView)
RWStructuredBuffer<int> PlasticWeights : register(u11); // [synapse][replica], 1/256 of a contact

groupshared uint gsActiveCount;
groupshared uint gsActiveBase;

double ToDouble(uint2 v)
{
    return asdouble(v.x, v.y);
}

uint2 FromDouble(double d)
{
    uint lo;
    uint hi;
    asuint(d, lo, hi);
    return uint2(lo, hi);
}

double DriveOf(NeuronParams p)
{
    return (double)p.driveExternal + (double)p.driveStimulus;
}

uint Mix32(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint PoissonDraw(uint neuron, uint step)
{
    return Mix32(Mix32(kSeedHash ^ step) ^ neuron);
}

// FlyBrain::CatchUp: closed form over the steps since lastStep, frozen while refractory
void CatchUp(inout double u, inout double g, NeuronState s, double drive, uint step)
{
    const uint from = max(s.lastStep, s.refractoryUntil);
    if (step <= from)
    {
        return;
    }
    const uint k = step - from;
    double a = ToDouble(PowU[k % kTableSize]);
    double b = ToDouble(PowG[k % kTableSize]);
    const double aTable = ToDouble(PowU[kTableSize]);
    const double bTable = ToDouble(PowG[kTableSize]);
    for (uint q = k / kTableSize; q > 0 && a != 0.0; --q)
    {
        a = a * aTable;
        b = b * bTable;
    }
    precise double relaxed = drive + (u - drive) * a;
    precise double coupled = g * ToDouble(kCouplingK) * (b - a);
    u = relaxed + coupled;
    g = g * b;
}

[numthreads(64, 1, 1)]
void CSReset(uint3 id : SV_DispatchThreadID)
{
    // Counters[1..] and the watch counts of the submission; the ring head keeps counting
    if (id.x < gGroupCount)
    {
        WatchCounts[id.x] = 0;
    }
    if (id.x + 1 < gUpdateCount)
    {
        Counters[id.x + 1] = 0;
    }
}

[numthreads(64, 1, 1)]
void CSUpdate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gUpdateCount)
    {
        return;
    }
    const NeuronUpdate update = Updates[gUpdateOffset + id.x];
    const uint i = update.neuron;
    if (update.wake != 0)
    {
        NeuronState s = State[i];
        double u = ToDouble(s.u);
        double g = ToDouble(s.g);
        CatchUp(u, g, s, DriveOf(Params[i]), gWindowBegin);
        s.u = FromDouble(u);
        s.g = FromDouble(g);
        s.lastStep = max(s.lastStep, gWindowBegin);
        s.awake = 1;
        State[i] = s;
    }
    NeuronParams p;
    p.driveExternal = update.driveExternal;
    p.driveStimulus = update.driveStimulus;
    p.poissonThreshold = update.poissonThreshold;
    p.refractorySteps = update.refractorySteps;
    p.watchMask = update.watchMask;
    p.unused0 = 0;
    p.unused1 = 0;
    p.unused2 = 0;
    Params[i] = p;
}

void EmitSpike(uint i, uint t, uint watchMask)
{
    uint index;
    InterlockedAdd(Counters[2 + gWindowIndex], 1, index);
    if (index < kSpikeCapacity)
    {
        SpikeList[index] = uint2(i, t);
    }
    else
    {
        InterlockedAdd(Counters[1], 1);
    }
    uint ring;
    InterlockedAdd(Counters[0], 1, ring);
    Ring[ring & kRingMask] = uint2(i, t);

    const uint row = (t - gWindowBegin + gStepOffset) * kWatchStride;
    InterlockedAdd(WatchCounts[row + kWatchStride - 1], 1);
    while (watchMask != 0)
    {
        const uint w = firstbitlow(watchMask);
        InterlockedAdd(WatchCounts[row + w], 1);
        watchMask &= watchMask - 1;
    }
}

[numthreads(64, 1, 1)]
void CSCompact(uint3 id : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID)
{
    // A sleeping neuron cannot reach the threshold before its next input: only the others go to the list. One global
    // atomic per group; the order inside the list does not matter (every neuron is independent in a window)
    if (thread.x == 0)
    {
        gsActiveCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint i = id.x;
    const bool active = i < kNeuronCount && (Pending[i] != 0 || State[i].awake != 0);
    uint local = 0;
    if (active)
    {
        InterlockedAdd(gsActiveCount, 1, local);
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread.x == 0)
    {
        InterlockedAdd(Counters[2 + kMaxWindows + gWindowIndex], gsActiveCount, gsActiveBase);
    }
    GroupMemoryBarrierWithGroupSync();
    if (active)
    {
        Active[gsActiveBase + local] = i;
    }
}

[numthreads(64, 1, 1)]
void CSIntegrate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Counters[2 + kMaxWindows + gWindowIndex])
    {
        return;
    }
    const uint i = Active[id.x];

    NeuronState s = State[i];
    const NeuronParams p = Params[i];
    const double drive = DriveOf(p);
    const double a = ToDouble(kDecayU);
    const double b = ToDouble(kDecayG);
    const double c = ToDouble(kStepCoupling);
    const double weight = ToDouble(kWeight);
    const double threshold = ToDouble(kThreshold);
    double u = ToDouble(s.u);
    double g = ToDouble(s.g);
    CatchUp(u, g, s, drive, gWindowBegin);
    uint refractoryUntil = s.refractoryUntil;

    // Brian2 order inside a step: state update, threshold, synaptic input (and Poisson input), reset. Input that lands
    // on a refractory neuron is lost
    const uint windowEnd = gWindowBegin + gSteps;
    uint slot = gFirstSlot;
    for (uint t = gWindowBegin; t < windowEnd; ++t)
    {
        const uint cell = slot * kNeuronCount + i;
        const int contacts = Arrivals[cell];
        if (contacts != 0)
        {
            Arrivals[cell] = 0;
        }
        if (t >= refractoryUntil)
        {
            precise double next = drive + (u - drive) * a + g * c;
            // contacts are small integers: int -> float -> double is exact, as int -> double on the CPU
            precise double input = weight * (double)(float)contacts;
            precise double decayed = g * b;
            g = decayed + input;
            u = next;
            if (p.poissonThreshold != 0 && PoissonDraw(i, t) < p.poissonThreshold)
            {
                u = u + ToDouble(kKick);
            }
            if (next > threshold)
            {
                u = ToDouble(kReset);
                g = 0.0;
                refractoryUntil = t + p.refractorySteps;
                SpikeTotals[i] = SpikeTotals[i] + 1;
                LastSpike[i] = t;
                EmitSpike(i, t, p.watchMask);
            }
        }
        slot = slot + 1 == kDelaySteps ? 0 : slot + 1;
    }

    // A window shorter than the delay leaves the input of its later steps for the next window
    uint inputLeft = 0;
    for (uint k = gSteps; k < kDelaySteps; ++k)
    {
        if (Arrivals[((gFirstSlot + k) % kDelaySteps) * kNeuronCount + i] != 0)
        {
            inputLeft = 1;
        }
    }
    Pending[i] = inputLeft;

    // FlyBrain::CanFire: u relaxes towards drive + g(t), g(t) decays to 0
    const bool canFire = p.poissonThreshold != 0 || u > threshold || drive > threshold || drive + g > threshold;
    s.u = FromDouble(u);
    s.g = FromDouble(g);
    s.lastStep = windowEnd;
    s.refractoryUntil = refractoryUntil;
    s.awake = canFire ? 1 : 0;
    State[i] = s;
}

int WeightOf(uint edge)
{
    const uint word = Weights[edge >> 1];
    return (edge & 1) != 0 ? ((int)word >> 16) : (((int)(word << 16)) >> 16);
}

[numthreads(64, 1, 1)]
void CSPlastic(uint3 id : SV_DispatchThreadID)
{
    if (id.x < gUpdateCount)
    {
        const uint2 update = PlasticUpdates[gUpdateOffset + id.x];
        PlasticWeights[update.x] = asint(update.y);
    }
}

[numthreads(64, 1, 1)]
void CSDeliver(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    // Every group takes every gGroupCount-th spike, its threads share the synapses of the spike
    const uint count = min(Counters[2 + gWindowIndex], kSpikeCapacity);
    for (uint s = group.x; s < count; s += gGroupCount)
    {
        const uint2 spike = SpikeList[s];
        // The spike of step t lands at step t + delay: the same slot, already consumed in this window. A spike of a
        // replica reaches the units of that replica only
        const uint slotBase = (spike.y % kDelaySteps) * kNeuronCount;
        const uint neuron = spike.x / kReplicas;
        const uint replica = spike.x - neuron * kReplicas;
        if (SilentOutput[neuron] != 0)
        {
            continue;
        }
        const uint e0 = RowStart[neuron];
        const uint e1 = RowStart[neuron + 1];
        // Not ?: - fxc evaluates both sides of it, and a read of PlasticRowOffset has no bounds check (a root descriptor)
        uint rowPlastic = 0xFFFFFFFFu;
        [branch] if (kPlastic != 0)
        {
            rowPlastic = PlasticRowOffset[neuron];
        }
        for (uint e = e0 + thread.x; e < e1; e += 64)
        {
            const uint post = Targets[e] * kReplicas + replica;
            int weight = WeightOf(e) * 256;
            [branch] if (rowPlastic != 0xFFFFFFFFu)
            {
                const int plastic = EdgePlastic[rowPlastic + (e - e0)];
                [branch] if (plastic >= 0)
                {
                    weight = PlasticWeights[plastic * kReplicas + replica];
                }
            }
            InterlockedAdd(Arrivals[slotBase + post], weight);
            Pending[post] = 1;
        }
    }
}
