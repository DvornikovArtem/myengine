// BehaviourBindings.cpp
// myengine.Behaviour: the base class of every script behaviour (engine -> script calls).

#include "Behaviour.h"

namespace py = pybind11;

namespace myengine::scripting::detail
{
    void BindBehaviour(py::module_& module)
    {
        // classh (smart_holder) + the trampoline: the Python part of the object can not die before the C++ part.
        // ScriptSystem keeps the py::object anyway, this is the second line of defence.
        py::classh<Behaviour, PyBehaviour>(module, "Behaviour",
            "Base class of script behaviours. Override OnStart / OnUpdate / OnCollision / OnTrigger / OnDestroy / OnReload.\n"
            "Do not override __init__ (use OnStart); if you do, call super().__init__().\n"
            "Fields with a type annotation (speed: float = 3.0) are editable and get their values from the scene / prefab.")
            .def(py::init<>())
            .def_property_readonly("entity", [](const Behaviour& behaviour) { return behaviour.entity; },
                "The entity this behaviour is attached to")
            .def("OnStart", &Behaviour::OnStart,
                "Called once before the first OnUpdate (the next frame after the entity appears)")
            .def("OnUpdate", &Behaviour::OnUpdate, py::arg("dt"),
                "Called every frame in Play, after physics")
            .def("OnCollision", &Behaviour::OnCollision, py::arg("other"), py::arg("point"), py::arg("normal"), py::arg("impulse"),
                "Collision started; normal points from this entity to the other one")
            .def("OnTrigger", &Behaviour::OnTrigger, py::arg("other"),
                "This entity and the other one started overlapping, one of them is a trigger")
            .def("OnDestroy", &Behaviour::OnDestroy,
                "Called before the entity is destroyed (entity.destroy() or a destroyed parent)")
            .def("OnReload", &Behaviour::OnReload,
                "Called after hot reload replaced this object with a new one");
    }
}