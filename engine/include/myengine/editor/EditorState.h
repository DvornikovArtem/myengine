#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <myengine/core/Types.h>
#include <myengine/ecs/Entity.h>
#include <myengine/render/RenderTypes.h>

namespace myengine::editor
{
    enum class RuntimeMode : std::uint8_t
    {
        Edit = 0,
        Play = 1,
    };

    enum class GizmoOperation : std::uint8_t
    {
        Translate = 0,
        Rotate = 1,
        Scale = 2,
    };

    enum class GizmoSpace : std::uint8_t
    {
        Local = 0,
        World = 1,
    };

    enum class MaterialPreviewShape : std::uint8_t
    {
        Sphere = 0,
        Cube = 1,
    };

    struct ViewportRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        bool IsValid() const
        {
            return width > 1.0f && height > 1.0f;
        }
    };

    struct CameraFrameState
    {
        bool available = false;
        render::Matrix4 view = render::Matrix4::Identity();
        render::Matrix4 projection = render::Matrix4::Identity();
    };

    struct FrameTimingStats
    {
        float deltaTime = 0.0f;
        float averageFps = 0.0f;
        double frameMs = 0.0;
        double worldUpdateMs = 0.0;
        double scriptsMs = 0.0; // part of worldUpdateMs, not an additional frame stage
        double stateUpdateMs = 0.0;
        double hotReloadMs = 0.0;
        double uiUpdateMs = 0.0;
        double renderMs = 0.0;
    };

    // Written by ScriptSystem, shown in Statistics. On a long session the numbers must not grow (no leaked state)
    struct ScriptStats
    {
        double updateMs = 0.0; // full ScriptSystem::Update, including hot reload and bookkeeping
        std::uint32_t instances = 0;
        std::uint32_t activeInstances = 0;
        std::uint32_t faultedInstances = 0;
        std::uint32_t scriptModules = 0; // modules loaded from assets/scripts, refreshed once per second
        std::uint32_t pythonObjects = 0; // len(gc.get_objects()), refreshed once per second
        std::uint32_t errors = 0; // total script errors since start
    };

    struct RenderStats
    {
        std::uint32_t totalEntities = 0;
        std::uint32_t renderableEntities = 0;
        std::uint32_t renderedEntities = 0;
        std::uint32_t activeCollisions = 0;
        std::uint64_t resourceMemoryBytes = 0;
        CameraFrameState camera{};
    };

    struct WindowEditorState
    {
        ViewportRect viewport{};
        ViewportRect viewportToolbar{};      // bounds of the left group of the viewport toolbar
        ViewportRect viewportToolbarRight{}; // bounds of the right group; the gap between them stays clickable for the scene
        bool viewportHovered = false;
        bool viewportFocused = false;
        bool viewportAcceptsCameraNavigation = false;
        bool gizmoHovered = false;
        bool gizmoActive = false;
        bool dockLayoutInitialized = false;
        MaterialPreviewShape materialPreviewShape = MaterialPreviewShape::Sphere;
        FrameTimingStats timings{};
        RenderStats renderStats{};
    };

    struct EditorRuntimeState
    {
        RuntimeMode mode = RuntimeMode::Edit;
        ecs::EntityId selectedEntity = ecs::kInvalidEntity;
        GizmoOperation gizmoOperation = GizmoOperation::Translate;
        GizmoSpace gizmoSpace = GizmoSpace::Local;
        bool showHierarchy = true;
        bool showInspector = true;
        bool showStatistics = true;
        bool showViewport = true;
        bool showMaterialEditor = true;
        bool showAssetBrowser = true;
        bool showPrefabs = true;
        bool showScriptConsole = true;
        bool showAssistant = true;
        bool showImGuiDemo = false;
        bool showStatsOverlay = true;   // FPS / ms in the corner of the viewport (viewport toolbar > Show)
        bool viewportWireframe = false; // view mode Wireframe (viewport toolbar > Lit)
        bool showProjectBrowser = false; // the Project Browser window (File > Open Project, --project-browser)
        std::string mapPath; // the open map as a project path ("Maps/Main.json")
        bool selectionLocked = false;
        bool sceneDirty = false;
        core::WindowId lastInteractedWindowId = 0;
        std::string playModeSnapshot;
        ScriptStats scriptStats{};
        // Hot reload in Play: true - the new object keeps the old one's state (L2), false - it starts over (L1)
        bool scriptReloadKeepsState = true;
        std::unordered_map<core::WindowId, WindowEditorState> windows;

        WindowEditorState& GetOrCreateWindowState(const core::WindowId windowId)
        {
            return windows[windowId];
        }

        const WindowEditorState* FindWindowState(const core::WindowId windowId) const
        {
            const auto it = windows.find(windowId);
            return it != windows.end() ? &it->second : nullptr;
        }

        void ResetWindowFrameState(const core::WindowId windowId)
        {
            auto& windowState = GetOrCreateWindowState(windowId);
            windowState.viewportHovered = false;
            windowState.viewportFocused = false;
            windowState.viewportAcceptsCameraNavigation = false;
            windowState.gizmoHovered = false;
            windowState.gizmoActive = false;
            windowState.viewport = {};
            windowState.viewportToolbar = {};
            windowState.viewportToolbarRight = {};
        }
    };
}
