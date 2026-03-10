// Registry.h

#pragma once

#include <memory>
#include <stdexcept>
#include <tuple>
#include <typeindex>
#include <type_traits>
#include <unordered_map>

#include <myengine/ecs/Component.h>
#include <myengine/ecs/Entity.h>

namespace myengine::ecs
{
    class Registry
    {
    public:
        template <typename T, typename... Args>
        T& Emplace(EntityId entity, Args&&... args)
        {
            ValidateComponentType<T>();
            auto& storage = GetOrCreateStorage<T>();
            auto [it, inserted] = storage.components.try_emplace(entity, std::forward<Args>(args)...);
            if (!inserted)
            {
                it->second = T(std::forward<Args>(args)...);
            }
            return it->second;
        }

        template <typename T>
        bool Has(const EntityId entity) const
        {
            ValidateComponentType<T>();
            const auto* storage = FindStorage<T>();
            return storage != nullptr && storage->components.find(entity) != storage->components.end();
        }

        template <typename T>
        T& Get(const EntityId entity)
        {
            ValidateComponentType<T>();
            auto* component = TryGet<T>(entity);
            if (component == nullptr)
            {
                throw std::runtime_error("Requested component is missing");
            }
            return *component;
        }

        template <typename T>
        const T& Get(const EntityId entity) const
        {
            ValidateComponentType<T>();
            const auto* component = TryGet<T>(entity);
            if (component == nullptr)
            {
                throw std::runtime_error("Requested component is missing");
            }
            return *component;
        }

        template <typename T>
        T* TryGet(const EntityId entity)
        {
            ValidateComponentType<T>();
            auto* storage = FindStorage<T>();
            if (storage == nullptr)
            {
                return nullptr;
            }

            const auto it = storage->components.find(entity);
            if (it == storage->components.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        template <typename T>
        const T* TryGet(const EntityId entity) const
        {
            ValidateComponentType<T>();
            const auto* storage = FindStorage<T>();
            if (storage == nullptr)
            {
                return nullptr;
            }

            const auto it = storage->components.find(entity);
            if (it == storage->components.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        template <typename T>
        bool Remove(const EntityId entity)
        {
            ValidateComponentType<T>();
            auto* storage = FindStorage<T>();
            if (storage == nullptr)
            {
                return false;
            }
            return storage->components.erase(entity) > 0;
        }

        void RemoveEntity(EntityId entity)
        {
            for (auto& [_, storage] : storages_)
            {
                storage->Remove(entity);
            }
        }

        void Clear()
        {
            storages_.clear();
        }

        template <typename... Components, typename Fn>
        void ForEach(Fn&& fn)
        {
            static_assert(sizeof...(Components) > 0, "ForEach requires at least one component type");
            static_assert((std::is_base_of_v<Component, Components> && ...), "ForEach accepts only types derived from ecs::Component");

            using Primary = std::tuple_element_t<0, std::tuple<Components...>>;
            auto* primaryStorage = FindStorage<Primary>();
            if (primaryStorage == nullptr)
            {
                return;
            }

            for (auto& [entity, _] : primaryStorage->components)
            {
                if ((Has<Components>(entity) && ...))
                {
                    fn(entity, Get<Components>(entity)...);
                }
            }
        }

    private:
        template <typename T>
        static constexpr void ValidateComponentType()
        {
            static_assert(std::is_base_of_v<Component, T>,
                "Registry accepts only types derived from ecs::Component");
        }

        struct IStorage
        {
            virtual ~IStorage() = default;
            virtual void Remove(EntityId entity) = 0;
        };

        template <typename T>
        struct ComponentStorage final : IStorage
        {
            std::unordered_map<EntityId, T> components;

            void Remove(const EntityId entity) override
            {
                components.erase(entity);
            }
        };

        template <typename T>
        ComponentStorage<T>* FindStorage()
        {
            const auto it = storages_.find(std::type_index(typeid(T)));
            if (it == storages_.end())
            {
                return nullptr;
            }
            return static_cast<ComponentStorage<T>*>(it->second.get());
        }

        template <typename T>
        const ComponentStorage<T>* FindStorage() const
        {
            const auto it = storages_.find(std::type_index(typeid(T)));
            if (it == storages_.end())
            {
                return nullptr;
            }
            return static_cast<const ComponentStorage<T>*>(it->second.get());
        }

        template <typename T>
        ComponentStorage<T>& GetOrCreateStorage()
        {
            const auto index = std::type_index(typeid(T));
            const auto it = storages_.find(index);
            if (it != storages_.end())
            {
                return *static_cast<ComponentStorage<T>*>(it->second.get());
            }

            auto storage = std::make_unique<ComponentStorage<T>>();
            auto* raw = storage.get();
            storages_.emplace(index, std::move(storage));
            return *raw;
        }

        std::unordered_map<std::type_index, std::unique_ptr<IStorage>> storages_;
    };
}