// ScriptInspector.h

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/scripting/ScriptRuntime.h>

namespace myengine::ui
{
    // Undo hooks of the caller (SceneEditor). All optional: the prefab panel works without undo
    struct ScriptFieldsUndo
    {
        std::function<void(const char* label)> recordFromItem; // after a drag / input / checkbox widget
        std::function<std::string()> captureBefore; // before an instant change (Reset)
        std::function<void(const char* label, const std::string& before)> recordImmediate;
    };

    // Draws the editable fields of one script behaviour: float -> DragFloat, int -> DragInt, bool -> Checkbox,
    // str -> InputText. `props` are the values stored in the scene / prefab:
    //  - a field without a key shows the default from the script code (greyed out) and is not written;
    //  - editing a field writes the key; Reset removes it (the default from the code applies again).
    // Must be called inside an ImGui window. Returns true if props changed this frame
    bool DrawScriptFields(const std::vector<scripting::ScriptFieldInfo>& fields, nlohmann::json& props, const ScriptFieldsUndo& undo = {});

    // Read-only view of a running object's values (Play): no Reset, nothing is greyed out as a default
    void DrawScriptFieldValues(const std::vector<scripting::ScriptFieldInfo>& fields, const nlohmann::json& values);
}
