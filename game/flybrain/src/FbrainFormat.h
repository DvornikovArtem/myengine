// FbrainFormat.h
// The layout of a .fbrain file (version 1), little-endian. tools/flybrain/convert_malecns.py writes the same layout:
// keep both in sync and raise kVersion on any change.
//
//   FileHeader (40 bytes)
//   SectionEntry[sectionCount] (24 bytes each): an 8-character tag, offset and size in bytes
//   sections, each starting at a multiple of 16 bytes

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace flybrain::format
{
    constexpr char kMagic[8] = { 'F', 'L', 'Y', 'B', 'R', 'A', 'I', 'N' };
    constexpr uint32_t kVersion = 1;
    constexpr size_t kTagSize = 8;
    constexpr size_t kAlignment = 16;
    constexpr int64_t kMaxWeight = 32767;

    struct FileHeader
    {
        char magic[8];
        uint32_t version;
        uint32_t sectionCount;
        uint32_t neuronCount;
        uint32_t stringCount;
        uint64_t edgeCount;
        uint32_t typeCount;
        uint32_t reserved;
    };
    static_assert(sizeof(FileHeader) == 40);

    struct SectionEntry
    {
        char tag[8];
        uint64_t offset;
        uint64_t size;
    };
    static_assert(sizeof(SectionEntry) == 24);

    // Tags are 8 characters, padded with spaces
    constexpr const char* kTagAttribution = "ATTRIB  "; // UTF-8 text: dataset, citation, license, what was changed
    constexpr const char* kTagStringOffsets = "STROFFS "; // uint32[stringCount + 1]; string 0 is ""
    constexpr const char* kTagStringChars = "STRCHARS"; // UTF-8 bytes of all strings, no terminators
    constexpr const char* kTagBodyIds = "BODYID  "; // uint64[N], ascending
    constexpr const char* kTagTypes = "TYPE    "; // uint32[N] string ids of the cell type
    constexpr const char* kTagClasses = "CLASS   "; // uint32[N] string ids of the class
    constexpr const char* kTagSuperclasses = "SUPERCLS"; // uint32[N] string ids of the superclass
    constexpr const char* kTagSides = "SIDE    "; // uint8[N] Side
    constexpr const char* kTagTransmitters = "NT      "; // uint8[N] Transmitter
    constexpr const char* kTagHex1 = "HEX1    "; // int16[N] assignedOlHex1, INT16_MIN when empty
    constexpr const char* kTagHex2 = "HEX2    "; // int16[N] assignedOlHex2
    constexpr const char* kTagPositions = "POS     "; // float[3N] x, y, z
    constexpr const char* kTagRowStart = "ROWSTART"; // uint32[N + 1] CSR offsets by presynaptic neuron
    constexpr const char* kTagTargets = "TARGETS "; // uint32[E] postsynaptic neuron, ascending inside a row
    constexpr const char* kTagWeights = "WEIGHTS "; // int16[E] contacts x sign of the presynaptic neuron
    constexpr const char* kTagTypeNames = "TYPENAME"; // uint32[T] string ids of the types, sorted by name
    constexpr const char* kTagTypeStart = "TYPESTRT"; // uint32[T + 1] offsets into TYPENEUR
    constexpr const char* kTagTypeNeurons = "TYPENEUR"; // uint32[] neurons of each type, ascending

    // Collects the sections and lays the file out
    struct ImageWriter
    {
        FileHeader header{};
        struct Pending
        {
            std::string tag;
            std::vector<std::byte> bytes;
        };
        std::vector<Pending> sections;

        void Add(const char* tag, const void* data, size_t size)
        {
            Pending pending{ std::string(tag, kTagSize), std::vector<std::byte>(size) };
            if (size > 0)
            {
                std::memcpy(pending.bytes.data(), data, size);
            }
            sections.push_back(std::move(pending));
        }

        template <typename T>
        void Add(const char* tag, const std::vector<T>& values)
        {
            Add(tag, values.data(), values.size() * sizeof(T));
        }

        std::vector<std::byte> Finish()
        {
            std::memcpy(header.magic, kMagic, sizeof(header.magic));
            header.version = kVersion;
            header.sectionCount = static_cast<uint32_t>(sections.size());

            auto align = [](uint64_t value) { return (value + kAlignment - 1) / kAlignment * kAlignment; };
            uint64_t offset = align(sizeof(FileHeader) + sections.size() * sizeof(SectionEntry));
            std::vector<SectionEntry> table(sections.size());
            for (size_t i = 0; i < sections.size(); ++i)
            {
                std::memcpy(table[i].tag, sections[i].tag.data(), kTagSize);
                table[i].offset = offset;
                table[i].size = sections[i].bytes.size();
                offset = align(offset + table[i].size);
            }

            std::vector<std::byte> image(static_cast<size_t>(offset));
            std::memcpy(image.data(), &header, sizeof(header));
            std::memcpy(image.data() + sizeof(header), table.data(), table.size() * sizeof(SectionEntry));
            for (size_t i = 0; i < sections.size(); ++i)
            {
                if (!sections[i].bytes.empty())
                {
                    std::memcpy(image.data() + table[i].offset, sections[i].bytes.data(), sections[i].bytes.size());
                }
            }
            return image;
        }
    };
}
