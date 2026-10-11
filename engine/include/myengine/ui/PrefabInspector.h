// PrefabInspector.h

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/scripting/ScriptRuntime.h>

namespace myengine::scene
{
    class PrefabLibrary;
}

namespace myengine::editor
{
    class ThumbnailService;
}

namespace myengine::ui
{
    // A separate draft: typing in the panel must not change the next spawn until Save is pressed.
    // No pointers into PrefabLibrary's cache survive a frame (the watcher can invalidate them).
    class PrefabInspector
    {
    public:
        using DescribeFields = std::function<std::vector<scripting::ScriptFieldInfo>(const std::string&, const std::string&)>;

        // The preview service for the template thumbnails (null: the package icon only)
        void SetThumbnails(editor::ThumbnailService* thumbnails) { thumbnails_ = thumbnails; }

        void Clear();
        bool Select(scene::PrefabLibrary& library, const std::string& name);
        bool Reload(scene::PrefabLibrary& library); // discard the draft and load the current template
        void Refresh(scene::PrefabLibrary& library); // notice external changes without losing unsaved props
        bool Save(scene::PrefabLibrary& library);
        void Draw(scene::PrefabLibrary& library, const DescribeFields& describeFields); // inside an ImGui window

        const std::string& GetSelectedName() const;
        const std::string& GetLastError() const;
        bool IsDirty() const;
        bool HasConflict() const;
        nlohmann::json& GetDraft();

    private:
        std::string selectedName_;
        nlohmann::json original_;
        nlohmann::json draft_;
        std::string lastError_;
        bool conflict_ = false;
        editor::ThumbnailService* thumbnails_ = nullptr;
    };
}
