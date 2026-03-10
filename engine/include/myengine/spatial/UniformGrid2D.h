// UniformGrid2D.h

#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <myengine/ecs/Entity.h>

namespace myengine::spatial
{
    class UniformGrid2D
    {
    public:
        explicit UniformGrid2D(float cellSize = 0.5f) : cellSize_(cellSize) {}

        void Clear()
        {
            cells_.clear();
        }

        void Insert(const ecs::EntityId entity, const float x, const float y)
        {
            const CellKey key{ToCell(x), ToCell(y)};
            cells_[key].push_back(entity);
        }

        std::vector<ecs::EntityId> Query(const float minX, const float minY, const float maxX, const float maxY) const
        {
            std::vector<ecs::EntityId> result;

            const std::int32_t minCellX = ToCell(minX);
            const std::int32_t maxCellX = ToCell(maxX);
            const std::int32_t minCellY = ToCell(minY);
            const std::int32_t maxCellY = ToCell(maxY);

            for (std::int32_t y = minCellY; y <= maxCellY; ++y)
            {
                for (std::int32_t x = minCellX; x <= maxCellX; ++x)
                {
                    const CellKey key{x, y};
                    const auto it = cells_.find(key);
                    if (it == cells_.end())
                    {
                        continue;
                    }

                    result.insert(result.end(), it->second.begin(), it->second.end());
                }
            }

            return result;
        }

    private:
        struct CellKey
        {
            std::int32_t x = 0;
            std::int32_t y = 0;

            bool operator==(const CellKey& other) const
            {
                return x == other.x && y == other.y;
            }
        };

        struct CellKeyHasher
        {
            std::size_t operator()(const CellKey& key) const
            {
                const std::uint64_t packed =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32) |
                    static_cast<std::uint32_t>(key.y);
                return static_cast<std::size_t>(packed ^ (packed >> 33));
            }
        };

        std::int32_t ToCell(const float value) const
        {
            return static_cast<std::int32_t>(std::floor(value / cellSize_));
        }

        float cellSize_ = 0.5f;
        std::unordered_map<CellKey, std::vector<ecs::EntityId>, CellKeyHasher> cells_;
    };
}