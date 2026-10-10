#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    void SceneEditor::BuildHierarchyPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showHierarchy)
        {
            return;
        }

        ecs::World& world = *services_.world;
        if (ImGui::Begin(kHierarchyWindowName))
        {
            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            if (!editEnabled)
            {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("Create Empty"))
            {
                const std::string beforeSnapshot = services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string();
                const ecs::EntityId entity = world.CreateEntity();
                world.Emplace<ecs::components::TagComponent>(entity).name = "Empty_" + std::to_string(entity);
                world.Emplace<ecs::components::TransformComponent>(entity);
                world.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowContext.windowId;
                editorState.selectedEntity = entity;
                RecordSceneMutationImmediate("Create Empty Entity", beforeSnapshot);
            }

            ImGui::SameLine();
            if (ImGui::Button("Delete Selected") && editorState.selectedEntity != ecs::kInvalidEntity)
            {
                DeleteSelectedEntity();
            }

            if (!editEnabled)
            {
                ImGui::EndDisabled();
            }

            ImGui::Separator();

            const auto visibleEntities = CollectVisibleEntities(world, windowContext.windowId);
            std::vector<ecs::EntityId> rootEntities;
            rootEntities.reserve(visibleEntities.size());

            for (const ecs::EntityId entity : visibleEntities)
            {
                const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
                const bool hasVisibleParent =
                    hierarchy != nullptr &&
                    hierarchy->parent != ecs::kInvalidEntity &&
                    world.IsAlive(hierarchy->parent) &&
                    MatchesWindowBinding(world, hierarchy->parent, windowContext.windowId);
                if (!hasVisibleParent)
                {
                    rootEntities.push_back(entity);
                }
            }

            for (const ecs::EntityId entity : rootEntities)
            {
                DrawEntityNode(windowContext.windowId, entity);
            }
        }
        ImGui::End();
    }

    void SceneEditor::DrawEntityNode(const core::WindowId windowId, const ecs::EntityId entity)
    {
        ecs::World& world = *services_.world;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();

        const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
        const bool hasChildren = hierarchy != nullptr && !hierarchy->children.empty();

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAvailWidth |
            (editorState.selectedEntity == entity ? ImGuiTreeNodeFlags_Selected : 0) |
            (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);

        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity)),
            flags,
            "%s",
            EntityDisplayName(world, entity).c_str());

        if (ImGui::IsItemClicked())
        {
            editorState.selectedEntity = entity;
            editorState.lastInteractedWindowId = windowId;
        }

        if (open)
        {
            if (hierarchy != nullptr)
            {
                std::vector<ecs::EntityId> children = hierarchy->children;
                std::sort(
                    children.begin(),
                    children.end(),
                    [&](const ecs::EntityId lhs, const ecs::EntityId rhs)
                    {
                        return EntityDisplayName(world, lhs) < EntityDisplayName(world, rhs);
                    });

                for (const ecs::EntityId child : children)
                {
                    if (world.IsAlive(child) && MatchesWindowBinding(world, child, windowId))
                    {
                        DrawEntityNode(windowId, child);
                    }
                }
            }

            ImGui::TreePop();
        }
    }

    void SceneEditor::DeleteSelectedEntity()
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.selectedEntity == ecs::kInvalidEntity || editorState.mode != editor::RuntimeMode::Edit)
        {
            return;
        }

        const std::string beforeSnapshot = services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string();
        if (services_.world->DestroyEntity(editorState.selectedEntity))
        {
            editorState.selectedEntity = ecs::kInvalidEntity;
            RecordSceneMutationImmediate("Delete Entity", beforeSnapshot);
        }
    }
}
