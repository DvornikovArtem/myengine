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
        std::function<void(const char* label)> recordFromItem; // after a drag / input widget
        std::function<std::string()> captureBefore; // before an instant change (checkbox, Reset)
        std::function<void(const char* label, const std::string& before)> recordImmediate;
    };

    // The status chip on the right of a script sub-header
    enum class ScriptHeaderChip
    {
        None,
        AppliedOnPlay, // Edit: the stored values apply on the next Play
        Active,        // Play: the script object is running
        Faulted,       // Play: the script failed; the "Open log" link is shown next to the chip
        NotCreated,    // Play: no object for this entry yet
    };

    // The sub-header of one script behaviour: icon, module.Class and a status chip on the right.
    // Returns true when the "Open log" link of a Faulted script was clicked. Inside an ImGui window.
    bool DrawScriptHeader(const std::string& module, const std::string& className, ScriptHeaderChip chip = ScriptHeaderChip::None);

    // Draws the editable fields of one script behaviour as a property grid (name, value, reset): float -> DragFloat,
    // int -> DragInt, bool -> checkbox, str -> InputText. `props` are the values stored in the scene / prefab:
    //  - a field without a key shows the default from the script code (dim) and is not written;
    //  - editing a field writes the key; the reset arrow removes it (the default from the code applies again).
    // Must be called inside an ImGui window. Returns true if props changed this frame
    bool DrawScriptFields(const std::vector<scripting::ScriptFieldInfo>& fields, nlohmann::json& props, const ScriptFieldsUndo& undo = {});

    // Read-only view of a running object's values (Play): no reset, nothing is dimmed as a default
    void DrawScriptFieldValues(const std::vector<scripting::ScriptFieldInfo>& fields, const nlohmann::json& values);
}
