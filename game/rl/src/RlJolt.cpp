// RlJolt.cpp

#include "RlJolt.h"

#include <Jolt/Core/Factory.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace rl::detail
{
    bool LayerPairFilter::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const
    {
        if (a == Layers::kGhost || b == Layers::kGhost)
        {
            return false;
        }
        if (a == Layers::kStatic && b == Layers::kStatic)
        {
            return false;
        }
        return true;  // car-car, car-ball, ball-static, car-static
    }

    JPH::BroadPhaseLayer BroadPhaseLayerMap::GetBroadPhaseLayer(JPH::ObjectLayer layer) const
    {
        return layer == Layers::kStatic ? BroadPhase::kStatic : BroadPhase::kMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* BroadPhaseLayerMap::GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const
    {
        return layer == BroadPhase::kStatic ? "static" : "moving";
    }
#endif

    bool ObjectVsBroadPhaseFilter::ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broadPhase) const
    {
        if (object == Layers::kGhost)
        {
            return false;
        }
        if (object == Layers::kStatic)
        {
            return broadPhase == BroadPhase::kMoving;
        }
        return true;
    }

    void FlatRecorder::WriteBytes(const void* data, size_t size)
    {
        const auto* begin = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), begin, begin + size);
    }

    void FlatRecorder::ReadBytes(void* data, size_t size)
    {
        if (readPosition_ + size > bytes_.size())
        {
            // a snapshot that is too short: hand back zeros and let IsEOF report it
            std::memset(data, 0, size);
            readPosition_ = bytes_.size() + 1;
            return;
        }
        std::memcpy(data, bytes_.data() + readPosition_, size);
        readPosition_ += size;
    }

    namespace
    {
        std::mutex g_joltMutex;
        int g_joltUsers = 0;
    }

    void JoltAcquire()
    {
        std::lock_guard<std::mutex> lock(g_joltMutex);
        if (g_joltUsers++ == 0)
        {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    void JoltRelease()
    {
        std::lock_guard<std::mutex> lock(g_joltMutex);
        if (--g_joltUsers == 0)
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }
}
