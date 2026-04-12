#pragma once

#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace myengine::events
{
    class EventBus
    {
    public:
        template <typename Event, typename Callback>
        void Subscribe(Callback&& callback)
        {
            auto& storage = GetOrCreateStorage<Event>();
            storage.listeners.emplace_back(std::forward<Callback>(callback));
        }

        template <typename Event>
        void Publish(const Event& event)
        {
            auto* storage = FindStorage<Event>();
            if (storage == nullptr)
            {
                return;
            }

            for (const auto& listener : storage->listeners)
            {
                listener(event);
            }
        }

    private:
        struct IEventStorage
        {
            virtual ~IEventStorage() = default;
        };

        template <typename Event>
        struct EventStorage final : IEventStorage
        {
            std::vector<std::function<void(const Event&)>> listeners;
        };

        template <typename Event>
        EventStorage<Event>* FindStorage()
        {
            const auto it = storages_.find(std::type_index(typeid(Event)));
            if (it == storages_.end())
            {
                return nullptr;
            }

            return static_cast<EventStorage<Event>*>(it->second.get());
        }

        template <typename Event>
        EventStorage<Event>& GetOrCreateStorage()
        {
            const auto key = std::type_index(typeid(Event));
            const auto it = storages_.find(key);
            if (it != storages_.end())
            {
                return *static_cast<EventStorage<Event>*>(it->second.get());
            }

            auto storage = std::make_unique<EventStorage<Event>>();
            auto* raw = storage.get();
            storages_.emplace(key, std::move(storage));
            return *raw;
        }

        std::unordered_map<std::type_index, std::unique_ptr<IEventStorage>> storages_;
    };
}