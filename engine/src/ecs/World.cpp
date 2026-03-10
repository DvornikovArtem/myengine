// World.cpp

#include <algorithm>
#include <unordered_set>

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/HierarchyComponent.h>

namespace myengine::ecs
{
    EntityId World::CreateEntity()
    {
        const EntityId entity = nextEntityId_++;
        entities_.insert(entity);
        return entity;
    }

    EntityId World::CreateEntityWithId(const EntityId entity)
    {
        if (entity == kInvalidEntity)
        {
            return kInvalidEntity;
        }

        entities_.insert(entity);
        if (entity >= nextEntityId_)
        {
            nextEntityId_ = entity + 1;
        }
        return entity;
    }

    bool World::DestroyEntity(const EntityId entity)
    {
        if (!IsAlive(entity))
        {
            return false;
        }

        ClearParent(entity);

        if (auto* hierarchy = registry_.TryGet<components::HierarchyComponent>(entity); hierarchy != nullptr)
        {
            const auto children = hierarchy->children;
            for (const EntityId child : children)
            {
                if (IsAlive(child))
                {
                    ClearParent(child);
                }
            }
        }

        registry_.RemoveEntity(entity);
        entities_.erase(entity);
        return true;
    }

    bool World::IsAlive(const EntityId entity) const
    {
        return entities_.find(entity) != entities_.end();
    }

    std::vector<EntityId> World::GetEntities() const
    {
        return std::vector<EntityId>(entities_.begin(), entities_.end());
    }

    void World::ClearEntities()
    {
        entities_.clear();
        registry_.Clear();
        nextEntityId_ = 1;
    }

    void World::AddUpdateSystem(std::unique_ptr<IUpdateSystem> system)
    {
        if (system != nullptr)
        {
            updateSystems_.push_back(std::move(system));
        }
    }

    void World::AddRenderSystem(std::unique_ptr<IRenderSystem> system)
    {
        if (system != nullptr)
        {
            renderSystems_.push_back(std::move(system));
        }
    }

    void World::UpdateSystems(const float deltaTime)
    {
        for (auto& system : updateSystems_)
        {
            system->Update(*this, deltaTime);
        }
    }

    void World::RenderSystems(const RenderFrameContext& context)
    {
        for (auto& system : renderSystems_)
        {
            system->Render(*this, context);
        }
    }

    bool World::SetParent(const EntityId child, const EntityId parent)
    {
        if (!IsAlive(child) || !IsAlive(parent) || child == parent)
        {
            return false;
        }

        if (IsParentChainContains(parent, child))
        {
            return false;
        }

        auto& childHierarchy = registry_.Emplace<components::HierarchyComponent>(child);
        if (childHierarchy.parent == parent)
        {
            return true;
        }

        if (childHierarchy.parent != kInvalidEntity)
        {
            RemoveChildReference(childHierarchy.parent, child);
        }

        auto& parentHierarchy = registry_.Emplace<components::HierarchyComponent>(parent);
        if (std::find(parentHierarchy.children.begin(), parentHierarchy.children.end(), child) == parentHierarchy.children.end())
        {
            parentHierarchy.children.push_back(child);
        }

        childHierarchy.parent = parent;
        return true;
    }

    bool World::ClearParent(const EntityId child)
    {
        if (!IsAlive(child))
        {
            return false;
        }

        auto* childHierarchy = registry_.TryGet<components::HierarchyComponent>(child);
        if (childHierarchy == nullptr || childHierarchy->parent == kInvalidEntity)
        {
            return false;
        }

        RemoveChildReference(childHierarchy->parent, child);
        childHierarchy->parent = kInvalidEntity;
        return true;
    }

    bool World::IsParentChainContains(const EntityId start, const EntityId target) const
    {
        EntityId current = start;
        std::unordered_set<EntityId> visited;

        while (current != kInvalidEntity)
        {
            if (current == target)
            {
                return true;
            }

            if (!visited.insert(current).second)
            {
                return false;
            }

            const auto* hierarchy = registry_.TryGet<components::HierarchyComponent>(current);
            if (hierarchy == nullptr)
            {
                return false;
            }

            current = hierarchy->parent;
        }

        return false;
    }

    void World::RemoveChildReference(const EntityId parent, const EntityId child)
    {
        if (!IsAlive(parent))
        {
            return;
        }

        auto* parentHierarchy = registry_.TryGet<components::HierarchyComponent>(parent);
        if (parentHierarchy == nullptr)
        {
            return;
        }

        parentHierarchy->children.erase(
            std::remove(parentHierarchy->children.begin(), parentHierarchy->children.end(), child),
            parentHierarchy->children.end());
    }
}
