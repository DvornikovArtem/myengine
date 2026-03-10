// World.h

#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include <myengine/ecs/Registry.h>
#include <myengine/ecs/System.h>

namespace myengine::ecs
{
    class World
    {
    public:
        EntityId CreateEntity();
        EntityId CreateEntityWithId(EntityId entity);
        bool DestroyEntity(EntityId entity);
        bool IsAlive(EntityId entity) const;
        std::vector<EntityId> GetEntities() const;
        void ClearEntities();

        template <typename T, typename... Args>
        T& Emplace(EntityId entity, Args&&... args)
        {
            return registry_.Emplace<T>(entity, std::forward<Args>(args)...);
        }

        template <typename T>
        bool Has(EntityId entity) const
        {
            return registry_.Has<T>(entity);
        }

        template <typename T>
        T& Get(EntityId entity)
        {
            return registry_.Get<T>(entity);
        }

        template <typename T>
        const T& Get(EntityId entity) const
        {
            return registry_.Get<T>(entity);
        }

        template <typename T>
        T* TryGet(EntityId entity)
        {
            return registry_.TryGet<T>(entity);
        }

        template <typename T>
        const T* TryGet(EntityId entity) const
        {
            return registry_.TryGet<T>(entity);
        }

        template <typename T>
        bool Remove(EntityId entity)
        {
            return registry_.Remove<T>(entity);
        }

        template <typename... Components, typename Fn>
        void ForEach(Fn&& fn)
        {
            registry_.ForEach<Components...>(std::forward<Fn>(fn));
        }

        void AddUpdateSystem(std::unique_ptr<IUpdateSystem> system);
        void AddRenderSystem(std::unique_ptr<IRenderSystem> system);

        void UpdateSystems(float deltaTime);
        void RenderSystems(const RenderFrameContext& context);

        bool SetParent(EntityId child, EntityId parent);
        bool ClearParent(EntityId child);

    private:
        bool IsParentChainContains(EntityId start, EntityId target) const;
        void RemoveChildReference(EntityId parent, EntityId child);

        EntityId nextEntityId_ = 1;
        std::unordered_set<EntityId> entities_;
        Registry registry_;
        std::vector<std::unique_ptr<IUpdateSystem>> updateSystems_;
        std::vector<std::unique_ptr<IRenderSystem>> renderSystems_;
    };
}