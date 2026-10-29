// Connectome.h
// The wiring of a fly brain: neurons with their annotations and the synapses between them in CSR form. Read from a
// .fbrain file (tools/flybrain/convert_malecns.py writes it from MaleCNS v1.0, docs/flybrain/flybrain.md describes the
// format) or built in code by ConnectomeBuilder (tests, small circuits). Immutable after loading: one connectome serves
// any number of FlyBrain simulations.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace flybrain
{
    enum class Side : uint8_t
    {
        Unknown = 0,
        Left = 1,
        Right = 2,
        Center = 3,
        Any = 255 // only for Find
    };

    // Predicted fast neurotransmitter of the neuron (MaleCNS consensus prediction). It gives the sign of its synapses
    enum class Transmitter : uint8_t
    {
        Unknown = 0,
        Acetylcholine = 1,
        Gaba = 2,
        Glutamate = 3,
        Histamine = 4,
        Dopamine = 5,
        Octopamine = 6,
        Serotonin = 7,
        Unclear = 8
    };

    const char* ToString(Side side);
    const char* ToString(Transmitter transmitter);

    // No column in the optic lobe (assignedOlHex1/2 is empty)
    constexpr int16_t kNoHex = INT16_MIN;

    // A set of neurons: indices into the connectome
    struct Group
    {
        std::vector<uint32_t> neurons;

        bool Empty() const { return neurons.empty(); }
        size_t Size() const { return neurons.size(); }
    };

    struct NeuronInfo
    {
        uint64_t bodyId = 0;
        std::string type; // cell type ("DNa02", "R1-R6", ...), empty when untyped
        std::string cellClass; // "class" column
        std::string superclass; // "descending_neuron", "ol_sensory", ...
        Side side = Side::Unknown;
        Transmitter transmitter = Transmitter::Unknown;
        float position[3] = { 0.0f, 0.0f, 0.0f }; // soma (or another point of the body), nm in the dataset space
        int16_t hex1 = kNoHex; // optic lobe column (assignedOlHex1/2), kNoHex for neurons outside the columns
        int16_t hex2 = kNoHex;
    };

    class Connectome
    {
    public:
        // Reads a .fbrain file. Returns nullptr and fills error when the file is missing or broken
        static std::shared_ptr<const Connectome> Load(const std::string& path, std::string* error = nullptr);
        // The same from an image of the file in memory (ConnectomeBuilder::Build)
        static std::shared_ptr<const Connectome> FromImage(std::vector<std::byte> image, std::string* error = nullptr);

        uint32_t NeuronCount() const { return neuronCount_; }
        uint64_t EdgeCount() const { return edgeCount_; }
        uint32_t FormatVersion() const { return version_; }
        // Dataset name, citation and license (CC BY 4.0 for MaleCNS), as the converter wrote them
        const std::string& Attribution() const { return attribution_; }

        // The synapses of neuron i: targets [EdgeBegin(i), EdgeEnd(i)) with signed weights (contacts x sign of i)
        uint32_t EdgeBegin(uint32_t neuron) const { return rowStart_[neuron]; }
        uint32_t EdgeEnd(uint32_t neuron) const { return rowStart_[neuron + 1]; }
        const uint32_t* RowStart() const { return rowStart_; }
        const uint32_t* Targets() const { return targets_; }
        const int16_t* Weights() const { return weights_; }

        uint64_t BodyId(uint32_t neuron) const { return bodyIds_[neuron]; }
        std::string_view Type(uint32_t neuron) const { return String(typeIds_[neuron]); }
        std::string_view CellClass(uint32_t neuron) const { return String(classIds_[neuron]); }
        std::string_view Superclass(uint32_t neuron) const { return String(superclassIds_[neuron]); }
        Side NeuronSide(uint32_t neuron) const { return static_cast<Side>(sides_[neuron]); }
        Transmitter NeuronTransmitter(uint32_t neuron) const { return static_cast<Transmitter>(transmitters_[neuron]); }
        const float* Position(uint32_t neuron) const { return positions_ + 3ull * neuron; }
        int16_t Hex1(uint32_t neuron) const { return hex1_[neuron]; }
        int16_t Hex2(uint32_t neuron) const { return hex2_[neuron]; }
        NeuronInfo Info(uint32_t neuron) const;

        // All neurons of a type ("DNa02") on a side; Side::Any takes both sides and the unknown ones. Sorted by index
        Group Find(std::string_view type, Side side = Side::Any) const;
        // Neurons whose type starts with prefix ("R7" finds R7d, R7p, R7y, R7_unclear)
        Group FindPrefix(std::string_view prefix, Side side = Side::Any) const;
        Group FindSuperclass(std::string_view superclass, Side side = Side::Any) const;
        // Index of a body id, or UINT32_MAX
        uint32_t FindBodyId(uint64_t bodyId) const;
        // Distinct cell types
        uint32_t TypeCount() const { return typeCount_; }

        // Bytes taken by the loaded image
        size_t MemoryBytes() const { return image_.size(); }

    private:
        Connectome() = default;
        bool Parse(std::string* error);
        std::string_view String(uint32_t id) const;
        void AppendType(uint32_t typeIndex, Side side, std::vector<uint32_t>& out) const;

        std::vector<std::byte> image_;
        uint32_t version_ = 0;
        uint32_t neuronCount_ = 0;
        uint64_t edgeCount_ = 0;
        uint32_t stringCount_ = 0;
        uint32_t typeCount_ = 0;
        std::string attribution_;

        const uint32_t* stringOffsets_ = nullptr; // stringCount + 1
        const char* stringChars_ = nullptr;
        const uint64_t* bodyIds_ = nullptr;
        const uint32_t* typeIds_ = nullptr; // string ids
        const uint32_t* classIds_ = nullptr;
        const uint32_t* superclassIds_ = nullptr;
        const uint8_t* sides_ = nullptr;
        const uint8_t* transmitters_ = nullptr;
        const int16_t* hex1_ = nullptr;
        const int16_t* hex2_ = nullptr;
        const float* positions_ = nullptr;
        const uint32_t* rowStart_ = nullptr; // neuronCount + 1
        const uint32_t* targets_ = nullptr;
        const int16_t* weights_ = nullptr;
        const uint32_t* typeStringIds_ = nullptr; // typeCount: string id of each type
        const uint32_t* typeStart_ = nullptr; // typeCount + 1
        const uint32_t* typeNeurons_ = nullptr; // neurons grouped by type

        std::unordered_map<std::string_view, uint32_t> typeByName_;
    };

    // Builds the image of a .fbrain file in memory: synthetic graphs of the tests and small hand-made circuits
    class ConnectomeBuilder
    {
    public:
        uint32_t AddNeuron(const NeuronInfo& info);
        // A synapse with a signed weight in contacts: +n excitatory, -n inhibitory. Several calls for one pair add up
        void AddEdge(uint32_t pre, uint32_t post, int32_t weight);
        uint32_t NeuronCount() const { return static_cast<uint32_t>(neurons_.size()); }

        std::vector<std::byte> Build(std::string_view attribution) const;
        bool Save(const std::string& path, std::string_view attribution, std::string* error = nullptr) const;

    private:
        struct Edge
        {
            uint32_t pre;
            uint32_t post;
            int32_t weight;
        };
        std::vector<NeuronInfo> neurons_;
        std::vector<Edge> edges_;
    };
}
