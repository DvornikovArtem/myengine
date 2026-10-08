// Behaviour.h
// Private header of the scripting subsystem: the C++ base class of script behaviours and its trampoline.

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/trampoline_self_life_support.h>

#include <myengine/ecs/components/Vector3.h>

#include "EntityRef.h"

namespace myengine::scripting::detail
{
    // A script is a Python class derived from this one:
    //     class Coin(myengine.Behaviour):
    //         def OnUpdate(self, dt): ...
    // The engine calls the virtual methods; for a Python object the trampoline forwards them to Python.
    class Behaviour
    {
    public:
        virtual ~Behaviour() = default;

        virtual void OnStart() {}
        virtual void OnUpdate(float /*deltaTime*/) {}
        virtual void OnCollision(EntityRef /*other*/, ecs::components::Vec3 /*point*/, ecs::components::Vec3 /*normal*/, float /*impulse*/) {}
        virtual void OnTrigger(EntityRef /*other*/) {}
        virtual void OnDestroy() {}
        virtual void OnReload() {}

        EntityRef entity; // set by the engine before OnStart, read-only from Python
    };

    // Trampoline: one override per virtual method. PYBIND11_OVERRIDE looks for a Python method with the same name
    // and calls it; if the Python class does not define it, the empty C++ version above runs.
    // trampoline_self_life_support keeps the Python part alive as long as C++ holds the object.
    class PyBehaviour : public Behaviour, public pybind11::trampoline_self_life_support
    {
    public:
        using Behaviour::Behaviour;

        void OnStart() override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnStart, );
        }

        void OnUpdate(const float deltaTime) override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnUpdate, deltaTime);
        }

        void OnCollision(const EntityRef other, const ecs::components::Vec3 point, const ecs::components::Vec3 normal, const float impulse) override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnCollision, other, point, normal, impulse);
        }

        void OnTrigger(const EntityRef other) override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnTrigger, other);
        }

        void OnDestroy() override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnDestroy, );
        }

        void OnReload() override
        {
            PYBIND11_OVERRIDE(void, Behaviour, OnReload, );
        }
    };

    // Registers myengine.Behaviour in the module (called from ScriptBindings.cpp)
    void BindBehaviour(pybind11::module_& module);
}