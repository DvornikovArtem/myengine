#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <myengine/ecs/Entity.h>

namespace myengine::spatial
{
    class UniformGrid3D
    {
    public:
        explicit UniformGrid3D(float cellSize = 1.0f) : cellSize_(std::max(cellSize, 0.1f)) {}

        void Clear()
        {
            cells_.clear();
        }

        void Insert(const ecs::EntityId entity, const float minX, const float minY, const float minZ, const float maxX, const float maxY, const float maxZ)
        {
            const std::int32_t minCellX = ToCell(minX);
            const std::int32_t minCellY = ToCell(minY);
            const std::int32_t minCellZ = ToCell(minZ);
            const std::int32_t maxCellX = ToCell(maxX);
            const std::int32_t maxCellY = ToCell(maxY);
            const std::int32_t maxCellZ = ToCell(maxZ);

            for (std::int32_t z = minCellZ; z <= maxCellZ; ++z)
            {
                for (std::int32_t y = minCellY; y <= maxCellY; ++y)
                {
                    for (std::int32_t x = minCellX; x <= maxCellX; ++x)
                    {
                        cells_[CellKey{x, y, z}].push_back(entity);
                    }
                }
            }
        }

        std::vector<std::pair<ecs::EntityId, ecs::EntityId>> BuildCandidatePairs() const
        {
            std::vector<std::pair<ecs::EntityId, ecs::EntityId>> result;
            std::unordered_set<std::uint64_t> uniquePairs;

            for (const auto& [_, cellEntities] : cells_)
            {
                for (std::size_t i = 0; i < cellEntities.size(); ++i)
                {
                    for (std::size_t j = i + 1; j < cellEntities.size(); ++j)
                    {
                        const ecs::EntityId a = std::min(cellEntities[i], cellEntities[j]);
                        const ecs::EntityId b = std::max(cellEntities[i], cellEntities[j]);
                        const std::uint64_t key = (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
                        if (uniquePairs.insert(key).second)
                        {
                            result.emplace_back(a, b);
                        }
                    }
                }
            }

            return result;
        }

    private:
        struct CellKey
        {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;

            bool operator==(const CellKey& other) const
            {
                return x == other.x && y == other.y && z == other.z;
            }
        };

        struct CellKeyHasher
        {
            std::size_t operator()(const CellKey& key) const
            {
                const std::uint64_t packedA =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32) |
                    static_cast<std::uint32_t>(key.y);
                const std::uint64_t packedB = static_cast<std::uint32_t>(key.z);
                return static_cast<std::size_t>((packedA * 1315423911ull) ^ packedB);
            }
        };

        std::int32_t ToCell(const float value) const
        {
            return static_cast<std::int32_t>(std::floor(value / cellSize_));
        }

        float cellSize_ = 1.0f;
        std::unordered_map<CellKey, std::vector<ecs::EntityId>, CellKeyHasher> cells_;
    };
}