#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace myengine::events
{
    // Returned by Subscribe, passed to Unsubscribe. 0 is never a valid id
    using SubscriptionId = std::uint64_t;

    class EventBus
    {
    public:
        template <typename Event, typename Callback>
        SubscriptionId Subscribe(Callback&& callback)
        {
            auto& storage = GetOrCreateStorage<Event>();
            const SubscriptionId id = ++lastSubscriptionId_;
            storage.listeners.push_back({id, std::function<void(const Event&)>(std::forward<Callback>(callback))});
            return id;
        }

        // Safe to call from inside a listener: the slot is cleared now and removed after the current Publish
        void Unsubscribe(const SubscriptionId id)
        {
            if (id == 0)
            {
                return;
            }

            for (auto& [type, storage] : storages_)
            {
                if (storage->Remove(id))
                {
                    return;
                }
            }
        }

        template <typename Event>
        void Publish(const Event& event)
        {
            auto* storage = FindStorage<Event>();
            if (storage == nullptr)
            {
                return;
            }

            ++storage->publishDepth;

            // By index and by copy: a listener may subscribe (the vector reallocates) or unsubscribe during the call.
            // Listeners added during this Publish get the next event, not this one
            const std::size_t count = storage->listeners.size();
            for (std::size_t index = 0; index < count; ++index)
            {
                auto callback = storage->listeners[index].callback;
                if (callback)
                {
                    callback(event);
                }
            }

            if (--storage->publishDepth == 0)
            {
                storage->RemoveCleared();
            }
        }

    private:
        struct IEventStorage
        {
            virtual ~IEventStorage() = default;
            virtual bool Remove(SubscriptionId id) = 0;
        };

        template <typename Event>
        struct EventStorage final : IEventStorage
        {
            struct Listener
            {
                SubscriptionId id = 0;
                std::function<void(const Event&)> callback;
            };

            std::vector<Listener> listeners;
            int publishDepth = 0;

            bool Remove(const SubscriptionId id) override
            {
                for (auto& listener : listeners)
                {
                    if (listener.id == id)
                    {
                        listener.callback = nullptr;
                        if (publishDepth == 0)
                        {
                            RemoveCleared();
                        }
                        return true;
                    }
                }
                return false;
            }

            void RemoveCleared()
            {
                listeners.erase(
                    std::remove_if(listeners.begin(), listeners.end(), [](const Listener& listener) { return !listener.callback; }),
                    listeners.end());
            }
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
        SubscriptionId lastSubscriptionId_ = 0;
    };
}