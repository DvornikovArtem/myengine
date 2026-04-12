#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <DirectXCollision.h>

namespace myengine::spatial
{
    template <typename TValue>
    class Octree
    {
    public:
        struct Item
        {
            TValue value{};
            DirectX::BoundingSphere bounds{};
        };

        explicit Octree(
            const DirectX::BoundingBox& rootBounds,
            const std::size_t maxItemsPerNode = 8,
            const std::size_t maxDepth = 6)
            : root_(std::make_unique<Node>(rootBounds, 0)),
              maxItemsPerNode_(std::max<std::size_t>(maxItemsPerNode, 1)),
              maxDepth_(std::max<std::size_t>(maxDepth, 1))
        {
        }

        void Insert(const TValue& value, const DirectX::BoundingSphere& bounds)
        {
            if (root_ == nullptr)
            {
                return;
            }

            Insert(*root_, Item{value, bounds});
        }

        template <typename TVisitor>
        void Query(const DirectX::BoundingFrustum& frustum, TVisitor&& visitor) const
        {
            if (root_ == nullptr)
            {
                return;
            }

            Query(*root_, frustum, std::forward<TVisitor>(visitor));
        }

    private:
        struct Node
        {
            explicit Node(const DirectX::BoundingBox& inBounds, const std::size_t inDepth)
                : bounds(inBounds), depth(inDepth)
            {
            }

            DirectX::BoundingBox bounds{};
            std::vector<Item> items;
            std::array<std::unique_ptr<Node>, 8> children;
            std::size_t depth = 0;

            bool IsLeaf() const
            {
                for (const auto& child : children)
                {
                    if (child != nullptr)
                    {
                        return false;
                    }
                }
                return true;
            }
        };

        static std::array<DirectX::BoundingBox, 8> BuildChildBounds(const DirectX::BoundingBox& parentBounds)
        {
            std::array<DirectX::BoundingBox, 8> childBounds{};

            const DirectX::XMFLOAT3& center = parentBounds.Center;
            const DirectX::XMFLOAT3 childExtents{
                parentBounds.Extents.x * 0.5f,
                parentBounds.Extents.y * 0.5f,
                parentBounds.Extents.z * 0.5f,
            };

            std::size_t index = 0;
            for (int xSign = -1; xSign <= 1; xSign += 2)
            {
                for (int ySign = -1; ySign <= 1; ySign += 2)
                {
                    for (int zSign = -1; zSign <= 1; zSign += 2)
                    {
                        childBounds[index].Center = {
                            center.x + childExtents.x * static_cast<float>(xSign),
                            center.y + childExtents.y * static_cast<float>(ySign),
                            center.z + childExtents.z * static_cast<float>(zSign),
                        };
                        childBounds[index].Extents = childExtents;
                        ++index;
                    }
                }
            }

            return childBounds;
        }

        void Subdivide(Node& node)
        {
            if (!node.IsLeaf() || node.depth >= maxDepth_)
            {
                return;
            }

            const auto childBounds = BuildChildBounds(node.bounds);
            for (std::size_t index = 0; index < childBounds.size(); ++index)
            {
                node.children[index] = std::make_unique<Node>(childBounds[index], node.depth + 1);
            }
        }

        bool TryInsertIntoChildren(Node& node, const Item& item)
        {
            for (auto& child : node.children)
            {
                if (child != nullptr && child->bounds.Contains(item.bounds) == DirectX::ContainmentType::CONTAINS)
                {
                    Insert(*child, item);
                    return true;
                }
            }

            return false;
        }

        void Insert(Node& node, const Item& item)
        {
            if (!node.IsLeaf() && TryInsertIntoChildren(node, item))
            {
                return;
            }

            node.items.push_back(item);

            if (node.items.size() <= maxItemsPerNode_ || node.depth >= maxDepth_)
            {
                return;
            }

            Subdivide(node);
            if (node.IsLeaf())
            {
                return;
            }

            std::vector<Item> remainingItems;
            remainingItems.reserve(node.items.size());

            for (const Item& existingItem : node.items)
            {
                if (!TryInsertIntoChildren(node, existingItem))
                {
                    remainingItems.push_back(existingItem);
                }
            }

            node.items = std::move(remainingItems);
        }

        template <typename TVisitor>
        void Query(const Node& node, const DirectX::BoundingFrustum& frustum, TVisitor&& visitor) const
        {
            if (frustum.Contains(node.bounds) == DirectX::ContainmentType::DISJOINT)
            {
                return;
            }

            for (const Item& item : node.items)
            {
                if (frustum.Contains(item.bounds) != DirectX::ContainmentType::DISJOINT)
                {
                    visitor(item.value);
                }
            }

            for (const auto& child : node.children)
            {
                if (child != nullptr)
                {
                    Query(*child, frustum, std::forward<TVisitor>(visitor));
                }
            }
        }

        std::unique_ptr<Node> root_;
        std::size_t maxItemsPerNode_ = 8;
        std::size_t maxDepth_ = 6;
    };
}