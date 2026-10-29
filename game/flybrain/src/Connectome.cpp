// Connectome.cpp

#include <flybrain/Connectome.h>

#include "FbrainFormat.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace flybrain
{
    const char* ToString(Side side)
    {
        switch (side)
        {
        case Side::Left: return "L";
        case Side::Right: return "R";
        case Side::Center: return "M";
        case Side::Any: return "any";
        default: return "?";
        }
    }

    const char* ToString(Transmitter transmitter)
    {
        switch (transmitter)
        {
        case Transmitter::Acetylcholine: return "acetylcholine";
        case Transmitter::Gaba: return "gaba";
        case Transmitter::Glutamate: return "glutamate";
        case Transmitter::Histamine: return "histamine";
        case Transmitter::Dopamine: return "dopamine";
        case Transmitter::Octopamine: return "octopamine";
        case Transmitter::Serotonin: return "serotonin";
        case Transmitter::Unclear: return "unclear";
        default: return "unknown";
        }
    }

    namespace
    {
        bool Fail(std::string* error, std::string message)
        {
            if (error != nullptr)
            {
                *error = std::move(message);
            }
            return false;
        }

        bool SideMatches(uint8_t neuronSide, Side side)
        {
            return side == Side::Any || neuronSide == static_cast<uint8_t>(side);
        }
    }

    std::shared_ptr<const Connectome> Connectome::Load(const std::string& path, std::string* error)
    {
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        if (!file)
        {
            Fail(error, "cannot open " + path);
            return nullptr;
        }
        const std::streamoff size = file.tellg();
        if (size <= 0)
        {
            Fail(error, "empty file " + path);
            return nullptr;
        }
        std::vector<std::byte> image(static_cast<size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(image.data()), size))
        {
            Fail(error, "cannot read " + path);
            return nullptr;
        }
        return FromImage(std::move(image), error);
    }

    std::shared_ptr<const Connectome> Connectome::FromImage(std::vector<std::byte> image, std::string* error)
    {
        std::shared_ptr<Connectome> connectome(new Connectome());
        connectome->image_ = std::move(image);
        if (!connectome->Parse(error))
        {
            return nullptr;
        }
        return connectome;
    }

    bool Connectome::Parse(std::string* error)
    {
        using namespace format;

        if (image_.size() < sizeof(FileHeader))
        {
            return Fail(error, "file is too small for a .fbrain header");
        }
        FileHeader header;
        std::memcpy(&header, image_.data(), sizeof(header));
        if (std::memcmp(header.magic, kMagic, sizeof(header.magic)) != 0)
        {
            return Fail(error, "not a .fbrain file (bad magic)");
        }
        if (header.version != kVersion)
        {
            return Fail(error, "unsupported .fbrain version " + std::to_string(header.version) + " (expected " + std::to_string(kVersion) + ")");
        }
        const uint64_t tableEnd = sizeof(FileHeader) + uint64_t(header.sectionCount) * sizeof(SectionEntry);
        if (tableEnd > image_.size())
        {
            return Fail(error, "section table is out of the file");
        }

        version_ = header.version;
        neuronCount_ = header.neuronCount;
        edgeCount_ = header.edgeCount;
        stringCount_ = header.stringCount;
        typeCount_ = header.typeCount;
        if (edgeCount_ > std::numeric_limits<uint32_t>::max())
        {
            return Fail(error, "more edges than uint32 row offsets can address");
        }

        const auto* table = reinterpret_cast<const SectionEntry*>(image_.data() + sizeof(FileHeader));
        // Finds a section and checks its size against the expected element count
        auto section = [&](const char* tag, uint64_t elementSize, uint64_t count, const void** out) -> bool
        {
            for (uint32_t i = 0; i < header.sectionCount; ++i)
            {
                if (std::memcmp(table[i].tag, tag, kTagSize) != 0)
                {
                    continue;
                }
                if (table[i].offset + table[i].size > image_.size() || table[i].offset % elementSize != 0)
                {
                    return Fail(error, std::string("section ") + std::string(tag, kTagSize) + " is out of the file or misaligned");
                }
                if (count != UINT64_MAX && table[i].size != elementSize * count)
                {
                    return Fail(error, std::string("section ") + std::string(tag, kTagSize) + " has a wrong size");
                }
                *out = image_.data() + table[i].offset;
                return true;
            }
            return Fail(error, std::string("section ") + std::string(tag, kTagSize) + " is missing");
        };
        auto sectionSize = [&](const char* tag) -> uint64_t
        {
            for (uint32_t i = 0; i < header.sectionCount; ++i)
            {
                if (std::memcmp(table[i].tag, tag, kTagSize) == 0)
                {
                    return table[i].size;
                }
            }
            return 0;
        };

        const uint64_t n = neuronCount_;
        const void* p = nullptr;
        if (!section(kTagAttribution, 1, UINT64_MAX, &p)) return false;
        attribution_.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(sectionSize(kTagAttribution)));
        if (!section(kTagStringOffsets, 4, uint64_t(stringCount_) + 1, &p)) return false;
        stringOffsets_ = static_cast<const uint32_t*>(p);
        if (!section(kTagStringChars, 1, UINT64_MAX, &p)) return false;
        stringChars_ = static_cast<const char*>(p);
        if (stringCount_ == 0 || stringOffsets_[stringCount_] > sectionSize(kTagStringChars))
        {
            return Fail(error, "string table is broken");
        }
        for (uint32_t i = 0; i < stringCount_; ++i)
        {
            if (stringOffsets_[i] > stringOffsets_[i + 1])
            {
                return Fail(error, "string table is broken");
            }
        }
        if (!section(kTagBodyIds, 8, n, &p)) return false;
        bodyIds_ = static_cast<const uint64_t*>(p);
        if (!section(kTagTypes, 4, n, &p)) return false;
        typeIds_ = static_cast<const uint32_t*>(p);
        if (!section(kTagClasses, 4, n, &p)) return false;
        classIds_ = static_cast<const uint32_t*>(p);
        if (!section(kTagSuperclasses, 4, n, &p)) return false;
        superclassIds_ = static_cast<const uint32_t*>(p);
        if (!section(kTagSides, 1, n, &p)) return false;
        sides_ = static_cast<const uint8_t*>(p);
        if (!section(kTagTransmitters, 1, n, &p)) return false;
        transmitters_ = static_cast<const uint8_t*>(p);
        if (!section(kTagHex1, 2, n, &p)) return false;
        hex1_ = static_cast<const int16_t*>(p);
        if (!section(kTagHex2, 2, n, &p)) return false;
        hex2_ = static_cast<const int16_t*>(p);
        if (!section(kTagPositions, 4, 3 * n, &p)) return false;
        positions_ = static_cast<const float*>(p);
        if (!section(kTagRowStart, 4, n + 1, &p)) return false;
        rowStart_ = static_cast<const uint32_t*>(p);
        if (!section(kTagTargets, 4, edgeCount_, &p)) return false;
        targets_ = static_cast<const uint32_t*>(p);
        if (!section(kTagWeights, 2, edgeCount_, &p)) return false;
        weights_ = static_cast<const int16_t*>(p);
        if (!section(kTagTypeNames, 4, typeCount_, &p)) return false;
        typeStringIds_ = static_cast<const uint32_t*>(p);
        if (!section(kTagTypeStart, 4, uint64_t(typeCount_) + 1, &p)) return false;
        typeStart_ = static_cast<const uint32_t*>(p);
        if (!section(kTagTypeNeurons, 4, UINT64_MAX, &p)) return false;
        typeNeurons_ = static_cast<const uint32_t*>(p);

        // Everything that indexes into other arrays is checked once here, so the simulation can trust it
        if (rowStart_[0] != 0 || rowStart_[n] != edgeCount_)
        {
            return Fail(error, "CSR row offsets do not cover the edges");
        }
        for (uint64_t i = 0; i < n; ++i)
        {
            if (rowStart_[i] > rowStart_[i + 1])
            {
                return Fail(error, "CSR row offsets are not sorted");
            }
            if (typeIds_[i] >= stringCount_ || classIds_[i] >= stringCount_ || superclassIds_[i] >= stringCount_)
            {
                return Fail(error, "neuron annotation points outside the string table");
            }
        }
        for (uint64_t e = 0; e < edgeCount_; ++e)
        {
            if (targets_[e] >= n)
            {
                return Fail(error, "edge target is out of range");
            }
        }
        const uint64_t typeNeuronCount = sectionSize(kTagTypeNeurons) / 4;
        if (typeStart_[0] != 0 || typeStart_[typeCount_] != typeNeuronCount)
        {
            return Fail(error, "type index does not cover its neurons");
        }
        for (uint32_t t = 0; t < typeCount_; ++t)
        {
            if (typeStart_[t] > typeStart_[t + 1] || typeStringIds_[t] >= stringCount_)
            {
                return Fail(error, "type index is broken");
            }
            typeByName_.emplace(String(typeStringIds_[t]), t);
        }
        for (uint64_t i = 0; i < typeNeuronCount; ++i)
        {
            if (typeNeurons_[i] >= n)
            {
                return Fail(error, "type index points to a missing neuron");
            }
        }
        return true;
    }

    std::string_view Connectome::String(uint32_t id) const
    {
        return std::string_view(stringChars_ + stringOffsets_[id], stringOffsets_[id + 1] - stringOffsets_[id]);
    }

    NeuronInfo Connectome::Info(uint32_t neuron) const
    {
        NeuronInfo info;
        info.bodyId = BodyId(neuron);
        info.type = std::string(Type(neuron));
        info.cellClass = std::string(CellClass(neuron));
        info.superclass = std::string(Superclass(neuron));
        info.side = NeuronSide(neuron);
        info.transmitter = NeuronTransmitter(neuron);
        std::memcpy(info.position, Position(neuron), sizeof(info.position));
        info.hex1 = Hex1(neuron);
        info.hex2 = Hex2(neuron);
        return info;
    }

    void Connectome::AppendType(uint32_t typeIndex, Side side, std::vector<uint32_t>& out) const
    {
        for (uint32_t k = typeStart_[typeIndex]; k < typeStart_[typeIndex + 1]; ++k)
        {
            const uint32_t neuron = typeNeurons_[k];
            if (SideMatches(sides_[neuron], side))
            {
                out.push_back(neuron);
            }
        }
    }

    Group Connectome::Find(std::string_view type, Side side) const
    {
        Group group;
        const auto it = typeByName_.find(type);
        if (it != typeByName_.end())
        {
            AppendType(it->second, side, group.neurons);
            std::sort(group.neurons.begin(), group.neurons.end());
        }
        return group;
    }

    Group Connectome::FindPrefix(std::string_view prefix, Side side) const
    {
        Group group;
        for (uint32_t t = 0; t < typeCount_; ++t)
        {
            const std::string_view name = String(typeStringIds_[t]);
            if (name.substr(0, prefix.size()) == prefix)
            {
                AppendType(t, side, group.neurons);
            }
        }
        std::sort(group.neurons.begin(), group.neurons.end());
        return group;
    }

    Group Connectome::FindSuperclass(std::string_view superclass, Side side) const
    {
        Group group;
        for (uint32_t i = 0; i < neuronCount_; ++i)
        {
            if (SideMatches(sides_[i], side) && String(superclassIds_[i]) == superclass)
            {
                group.neurons.push_back(i);
            }
        }
        return group;
    }

    uint32_t Connectome::FindBodyId(uint64_t bodyId) const
    {
        // The converter and the builder write neurons sorted by body id
        const uint64_t* end = bodyIds_ + neuronCount_;
        const uint64_t* it = std::lower_bound(bodyIds_, end, bodyId);
        return it != end && *it == bodyId ? static_cast<uint32_t>(it - bodyIds_) : UINT32_MAX;
    }

    // ---------------------------------------------------------------- builder

    uint32_t ConnectomeBuilder::AddNeuron(const NeuronInfo& info)
    {
        neurons_.push_back(info);
        return static_cast<uint32_t>(neurons_.size() - 1);
    }

    void ConnectomeBuilder::AddEdge(uint32_t pre, uint32_t post, int32_t weight)
    {
        edges_.push_back({ pre, post, weight });
    }

    std::vector<std::byte> ConnectomeBuilder::Build(std::string_view attribution) const
    {
        using namespace format;

        // Neurons go into the file sorted by body id (FindBodyId relies on it)
        const uint32_t n = static_cast<uint32_t>(neurons_.size());
        std::vector<uint32_t> order(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return neurons_[a].bodyId < neurons_[b].bodyId; });
        std::vector<uint32_t> newIndex(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            newIndex[order[i]] = i;
        }

        // Strings: id 0 is the empty string
        std::vector<std::string> strings{ std::string() };
        std::unordered_map<std::string, uint32_t> stringIds{ { std::string(), 0u } };
        auto intern = [&](const std::string& s) -> uint32_t
        {
            const auto it = stringIds.find(s);
            if (it != stringIds.end())
            {
                return it->second;
            }
            const uint32_t id = static_cast<uint32_t>(strings.size());
            strings.push_back(s);
            stringIds.emplace(s, id);
            return id;
        };

        std::vector<uint64_t> bodyIds(n);
        std::vector<uint32_t> types(n), classes(n), superclasses(n);
        std::vector<uint8_t> sides(n), transmitters(n);
        std::vector<int16_t> hex1(n), hex2(n);
        std::vector<float> positions(3ull * n);
        for (uint32_t i = 0; i < n; ++i)
        {
            const NeuronInfo& info = neurons_[order[i]];
            bodyIds[i] = info.bodyId;
            types[i] = intern(info.type);
            classes[i] = intern(info.cellClass);
            superclasses[i] = intern(info.superclass);
            sides[i] = static_cast<uint8_t>(info.side);
            transmitters[i] = static_cast<uint8_t>(info.transmitter);
            hex1[i] = info.hex1;
            hex2[i] = info.hex2;
            std::memcpy(&positions[3ull * i], info.position, sizeof(info.position));
        }

        // CSR: rows by pre, targets ascending inside a row, duplicates summed and clamped to int16
        std::vector<Edge> edges;
        edges.reserve(edges_.size());
        for (const Edge& e : edges_)
        {
            edges.push_back({ newIndex[e.pre], newIndex[e.post], e.weight });
        }
        std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.pre != b.pre ? a.pre < b.pre : a.post < b.post; });
        std::vector<uint32_t> rowStart(n + 1ull, 0);
        std::vector<uint32_t> targets;
        std::vector<int16_t> weights;
        for (size_t k = 0; k < edges.size();)
        {
            const Edge& first = edges[k];
            int64_t sum = 0;
            for (; k < edges.size() && edges[k].pre == first.pre && edges[k].post == first.post; ++k)
            {
                sum += edges[k].weight;
            }
            if (sum == 0)
            {
                continue;
            }
            sum = std::clamp<int64_t>(sum, -kMaxWeight, kMaxWeight);
            targets.push_back(first.post);
            weights.push_back(static_cast<int16_t>(sum));
            ++rowStart[first.pre + 1ull];
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            rowStart[i + 1ull] += rowStart[i];
        }

        // Type index: types sorted by name, neurons of a type by index
        std::vector<bool> isType(strings.size(), false);
        for (uint32_t i = 0; i < n; ++i)
        {
            isType[types[i]] = true;
        }
        std::vector<uint32_t> typeStringIds;
        for (uint32_t id = 1; id < strings.size(); ++id)
        {
            if (isType[id])
            {
                typeStringIds.push_back(id);
            }
        }
        std::sort(typeStringIds.begin(), typeStringIds.end(), [&](uint32_t a, uint32_t b) { return strings[a] < strings[b]; });
        std::unordered_map<uint32_t, uint32_t> typeIndexOfString;
        for (uint32_t t = 0; t < typeStringIds.size(); ++t)
        {
            typeIndexOfString[typeStringIds[t]] = t;
        }
        std::vector<uint32_t> typeStart(typeStringIds.size() + 1, 0);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (types[i] != 0)
            {
                ++typeStart[typeIndexOfString[types[i]] + 1ull];
            }
        }
        for (size_t t = 0; t < typeStringIds.size(); ++t)
        {
            typeStart[t + 1] += typeStart[t];
        }
        std::vector<uint32_t> typeNeurons(typeStart.back());
        std::vector<uint32_t> cursor(typeStart.begin(), typeStart.end() - 1);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (types[i] != 0)
            {
                typeNeurons[cursor[typeIndexOfString[types[i]]]++] = i;
            }
        }

        std::vector<uint32_t> stringOffsets(strings.size() + 1, 0);
        std::string stringChars;
        for (size_t s = 0; s < strings.size(); ++s)
        {
            stringChars += strings[s];
            stringOffsets[s + 1] = static_cast<uint32_t>(stringChars.size());
        }

        ImageWriter writer;
        writer.header.neuronCount = n;
        writer.header.edgeCount = targets.size();
        writer.header.stringCount = static_cast<uint32_t>(strings.size());
        writer.header.typeCount = static_cast<uint32_t>(typeStringIds.size());
        writer.Add(kTagAttribution, attribution.data(), attribution.size());
        writer.Add(kTagStringOffsets, stringOffsets);
        writer.Add(kTagStringChars, stringChars.data(), stringChars.size());
        writer.Add(kTagBodyIds, bodyIds);
        writer.Add(kTagTypes, types);
        writer.Add(kTagClasses, classes);
        writer.Add(kTagSuperclasses, superclasses);
        writer.Add(kTagSides, sides);
        writer.Add(kTagTransmitters, transmitters);
        writer.Add(kTagHex1, hex1);
        writer.Add(kTagHex2, hex2);
        writer.Add(kTagPositions, positions);
        writer.Add(kTagRowStart, rowStart);
        writer.Add(kTagTargets, targets);
        writer.Add(kTagWeights, weights);
        writer.Add(kTagTypeNames, typeStringIds);
        writer.Add(kTagTypeStart, typeStart);
        writer.Add(kTagTypeNeurons, typeNeurons);
        return writer.Finish();
    }

    bool ConnectomeBuilder::Save(const std::string& path, std::string_view attribution, std::string* error) const
    {
        const std::vector<std::byte> image = Build(attribution);
        std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size())))
        {
            return Fail(error, "cannot write " + path);
        }
        return true;
    }
}
