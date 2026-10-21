// RlJolt.h
// Jolt glue of the Rocket League simulation: layers, the flat state recorder, the global init. Not included outside game/rl/src.

#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/RegisterTypes.h>

#include <cstdint>
#include <vector>

namespace rl::detail
{
    namespace Layers
    {
        constexpr JPH::ObjectLayer kStatic = 0;  // the arena
        constexpr JPH::ObjectLayer kCar = 1;
        constexpr JPH::ObjectLayer kBall = 2;
        constexpr JPH::ObjectLayer kGhost = 3;   // a demolished car: collides with nothing
        constexpr JPH::uint kCount = 4;
    }

    namespace BroadPhase
    {
        constexpr JPH::BroadPhaseLayer kStatic(0);
        constexpr JPH::BroadPhaseLayer kMoving(1);
        constexpr JPH::uint kCount = 2;
    }

    class LayerPairFilter final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;
    };

    class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
    {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhase::kCount; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override;
#endif
    };

    class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broadPhase) const override;
    };

    // The state recorder of Jolt over one flat byte buffer: written once, read once from the start. A snapshot of the
    // simulation is this buffer; it is reused (cleared, not freed) so a rollback buffer does not allocate per tick.
    class FlatRecorder final : public JPH::StateRecorder
    {
    public:
        void WriteBytes(const void* data, size_t size) override;
        void ReadBytes(void* data, size_t size) override;
        bool IsEOF() const override { return readPosition_ > bytes_.size(); }
        bool IsFailed() const override { return false; }

        std::vector<uint8_t>& Bytes() { return bytes_; }
        const std::vector<uint8_t>& Bytes() const { return bytes_; }
        void BeginRead() { readPosition_ = 0; }
        bool ReadOk() const { return readPosition_ <= bytes_.size(); }

        template <typename T>
        void Put(const T& value)
        {
            WriteBytes(&value, sizeof(T));
        }
        template <typename T>
        void Get(T& value)
        {
            ReadBytes(&value, sizeof(T));
        }

    private:
        std::vector<uint8_t> bytes_;
        size_t readPosition_ = 0;
    };

    // Reference-counted Jolt globals (allocator, factory, types): the first simulation creates them, the last one frees them.
    void JoltAcquire();
    void JoltRelease();
}
