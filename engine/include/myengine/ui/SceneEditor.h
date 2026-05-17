#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <myengine/editor/EditorState.h>

namespace myengine::core
{
    class Logger;
}

struct ImFont;
struct ImVec2;

namespace myengine::ecs
{
    class World;
}

namespace myengine::editor
{
    class EditorCommandHistory;
    class TransformGizmo;
}

namespace myengine::resource
{
    class ResourceManager;
    struct MaterialAsset;
}

namespace myengine::ui
{
    struct SceneEditorServices
    {
        ecs::World* world = nullptr;
        resource::ResourceManager* resourceManager = nullptr;
        core::Logger* logger = nullptr;
        std::function<void()> requestQuit;
        std::function<bool()> saveScene;
        std::function<bool()> loadScene;
        std::function<std::string()> captureSceneSnapshot;
        std::function<bool(std::string_view)> restoreSceneSnapshot;
    };

    struct SceneEditorWindowContext
    {
        core::WindowId windowId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ImFont* monoFont = nullptr;
        std::string stateLabel;
    };

    class SceneEditor
    {
    public:
        SceneEditor();
        ~SceneEditor();

        void Initialize(SceneEditorServices services);
        void Shutdown();
        void BuildWindowUi(const SceneEditorWindowContext& windowContext);
        bool CanStartSceneNavigation(core::WindowId windowId, float mouseX, float mouseY) const;

    private:
        void BuildDockSpace(const SceneEditorWindowContext& windowContext);
        bool BuildToolbar(const SceneEditorWindowContext& windowContext, const editor::ViewportRect& viewportRect);
        void BuildHierarchyPanel(const SceneEditorWindowContext& windowContext);
        void BuildInspectorPanel(const SceneEditorWindowContext& windowContext);
        void BuildStatisticsPanel(const SceneEditorWindowContext& windowContext);
        void BuildViewportPanel(const SceneEditorWindowContext& windowContext);
        void BuildMaterialEditorPanel(const SceneEditorWindowContext& windowContext);
        void BuildAssetBrowserPanel(const SceneEditorWindowContext& windowContext);
        void HandleKeyboardShortcuts(const SceneEditorWindowContext& windowContext);
        void ValidateSelection() const;
        void CreateDefaultDockLayout(const SceneEditorWindowContext& windowContext);
        void DrawEntityNode(core::WindowId windowId, ecs::EntityId entity);
        void SpawnRenderableEntity(core::WindowId windowId, std::string meshPath, std::string materialPath);
        bool ApplyMaterialAsset(const std::string& materialPath, const resource::MaterialAsset& asset) const;
        bool PushMaterialAssetCommand(
            const char* label,
            const std::string& materialPath,
            const resource::MaterialAsset& beforeAsset,
            const resource::MaterialAsset& afterAsset);
        std::string ResolveSuggestedMaterialForMesh(const std::string& meshPath) const;
        std::string EnsureTexturePreviewMaterial(const std::string& texturePath) const;
        std::string CaptureSceneSnapshot() const;
        ecs::EntityId PickEntityAtScreenPosition(
            const SceneEditorWindowContext& windowContext,
            const editor::WindowEditorState& windowState,
            const ImVec2& mousePosition) const;
        void RecordSceneMutationFromItem(const char* label);
        void RecordSceneMutationImmediate(const char* label, const std::string& beforeSnapshot);
        void CommitPendingGizmoMutation();
        void DeleteSelectedEntity();

        SceneEditorServices services_{};
        std::unique_ptr<editor::EditorCommandHistory> history_;
        std::unique_ptr<editor::TransformGizmo> gizmo_;
        std::string pendingSceneMutationSnapshot_;
        std::string pendingGizmoMutationSnapshot_;
        bool gizmoWasUsing_ = false;
        bool initialized_ = false;
    };
}