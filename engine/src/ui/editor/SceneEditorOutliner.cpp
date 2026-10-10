#include "SceneEditorInternal.h"
#include "SceneEditorEntityKind.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        constexpr float kVisibilityColumn = 28.0f;
        constexpr float kTypeColumn = 92.0f;

        // The row menu runs while the tree is being drawn: adding or deleting entities there would invalidate
        // the component pointers of the rows that follow, so the panel does it after the tree
        thread_local bool g_createRequested = false;
        thread_local bool g_deleteRequested = false;

        // Text in a role font at a fixed position (the draw list gets the font and size explicitly)
        void AddRoleText(ImDrawList* drawList, const FontRole role, const ImVec2 position, const ImU32 color, const char* text)
        {
            PushFontRole(role);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            PopFontRole();
            drawList->AddText(font, size, position, color, text);
        }

        float RoleTextHeight(const FontRole role)
        {
            PushFontRole(role);
            const float height = ImGui::GetFontSize();
            PopFontRole();
            return height;
        }
    }

    void SceneEditor::BuildHierarchyPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showHierarchy)
        {
            return;
        }

        ecs::World& world = *services_.world;

        // Edge to edge: the tools row, the column header and the tree well draw their own margins
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool shown = ImGui::Begin(kHierarchyWindowName);
        ImGui::PopStyleVar();
        if (shown)
        {
            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            const auto visibleEntities = CollectVisibleEntities(world, windowContext.windowId);
            const bool selectionListed = editorState.selectedEntity != ecs::kInvalidEntity &&
                std::find(visibleEntities.begin(), visibleEntities.end(), editorState.selectedEntity) != visibleEntities.end();

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // 1. Tools row: how many entities on the left, "+" (Create Empty) on the right
            {
                const std::size_t count = visibleEntities.size();
                std::string counter = std::to_string(count) + (count == 1 ? " entity" : " entities");
                if (selectionListed)
                {
                    counter += " (1 selected)";
                }

                const float textHeight = RoleTextHeight(FontRole::Secondary);
                AddRoleText(
                    drawList,
                    FontRole::Secondary,
                    ImVec2(origin.x + 12.0f, std::floor(origin.y + (style::kPanelToolsHeight - textHeight) * 0.5f)),
                    style::kTextDim,
                    counter.c_str());

                ImGui::SetCursorScreenPos(ImVec2(
                    origin.x + width - 8.0f - style::kPanelIconButton,
                    origin.y + (style::kPanelToolsHeight - style::kPanelIconButton) * 0.5f));
                if (IconButton("##outliner_add", ICON_PLUS, "Create Empty", false, editEnabled))
                {
                    CreateEmptyEntity(windowContext.windowId);
                }
            }

            // 2. Column header: visibility, Item Label, Type
            const float headerTop = origin.y + style::kPanelToolsHeight;
            {
                drawList->AddRectFilled(
                    ImVec2(origin.x, headerTop),
                    ImVec2(origin.x + width, headerTop + style::kRowHeight),
                    style::kHeader);
                const float centerY = headerTop + style::kRowHeight * 0.5f;
                DrawIcon(drawList, IconSize::Row14, ICON_EYE, ImVec2(origin.x + kVisibilityColumn * 0.5f, centerY), style::kTextDim);

                const float textHeight = RoleTextHeight(FontRole::Secondary);
                const float textY = std::floor(centerY - textHeight * 0.5f);
                AddRoleText(drawList, FontRole::Secondary, ImVec2(origin.x + kVisibilityColumn + 12.0f, textY), style::kTextDim, "Item Label");
                AddRoleText(drawList, FontRole::Secondary, ImVec2(origin.x + width - kTypeColumn + 8.0f, textY), style::kTextDim, "Type");
            }

            // 3. The tree well
            ImGui::SetCursorScreenPos(ImVec2(origin.x, headerTop + style::kRowHeight));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            const bool wellShown = ImGui::BeginChild("##outliner_rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (wellShown)
            {
                if (visibleEntities.empty())
                {
                    EmptyState(
                        ICON_LIST_TREE,
                        "No entities",
                        "Add one with + Add or drag a mesh from the Content Browser.");
                }
                else
                {
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

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
                    if (ImGui::BeginTable("##outliner_tree", 3, ImGuiTableFlags_NoSavedSettings))
                    {
                        ImGui::TableSetupColumn("visibility", ImGuiTableColumnFlags_WidthFixed, kVisibilityColumn);
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, kTypeColumn);

                        for (const ecs::EntityId entity : rootEntities)
                        {
                            DrawEntityNode(windowContext.windowId, entity);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();
                }

                if (g_createRequested)
                {
                    g_createRequested = false;
                    CreateEmptyEntity(windowContext.windowId);
                }
                if (g_deleteRequested)
                {
                    g_deleteRequested = false;
                    DeleteSelectedEntity();
                }

                // Right click on the empty part of the well
                if (ImGui::BeginPopupContextWindow("##outliner_empty_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
                {
                    if (ImGui::MenuItem("Create Empty", nullptr, false, editEnabled))
                    {
                        CreateEmptyEntity(windowContext.windowId);
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void SceneEditor::DrawEntityNode(const core::WindowId windowId, const ecs::EntityId entity)
    {
        ecs::World& world = *services_.world;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;

        const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
        const bool hasChildren = hierarchy != nullptr && !hierarchy->children.empty();
        const bool selected = editorState.selectedEntity == entity;

        ImGui::TableNextRow(ImGuiTableRowFlags_None, style::kRowHeight);
        if ((ImGui::TableGetRowIndex() & 1) != 0)
        {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, style::WithAlpha(IM_COL32_WHITE, 0.018f));
        }

        ImGui::TableSetColumnIndex(1);
        const float nodeX = ImGui::GetCursorScreenPos().x;

        const ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAllColumns |
            ImGuiTreeNodeFlags_FramePadding |
            (selected ? ImGuiTreeNodeFlags_Selected : 0) |
            (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);

        // The row (hit area, hover, selection) comes from the tree node; its text and arrow are drawn below
        const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        PushSelectionColors(panelFocused);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, (style::kRowHeight - ImGui::GetFontSize()) * 0.5f));
        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity)),
            flags,
            "%s",
            "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        PopSelectionColors();

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            editorState.selectedEntity = entity;
            editorState.lastInteractedWindowId = windowId;
        }

        if (ImGui::BeginPopupContextItem("##entity_menu"))
        {
            if (ImGui::MenuItem("Create Empty", nullptr, false, editEnabled))
            {
                g_createRequested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del", false, editEnabled))
            {
                g_deleteRequested = true;
            }
            ImGui::EndPopup();
        }

        // Row contents on top of the node. The label column is current, so its text, icon and chevron are not
        // clipped by another column; the visibility and type cells are drawn after switching to their columns.
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float centerY = std::floor((rowMin.y + rowMax.y) * 0.5f);
        const bool selectedNow = editorState.selectedEntity == entity;
        const ImU32 textColor = selectedNow ? style::kTextStrong : style::kText;
        const EntityKindInfo kind = ClassifyEntity(world, entity);
        const float typeColumnX = rowMax.x - kTypeColumn;

        if (hasChildren)
        {
            DrawIcon(
                drawList,
                IconSize::Chevron12,
                open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                ImVec2(nodeX + 9.0f, centerY),
                style::kTextDim);
        }

        // The chevron space is always reserved, so names of leaves line up with names of parents
        DrawIcon(drawList, IconSize::Row14, kind.icon, ImVec2(nodeX + 29.0f, centerY), selectedNow ? style::kTextStrong : kind.color);

        const std::string name = EntityDisplayName(world, entity);
        const float bodyHeight = ImGui::GetFontSize();
        drawList->PushClipRect(ImVec2(nodeX, rowMin.y), ImVec2(typeColumnX - 4.0f, rowMax.y), true);
        drawList->AddText(ImVec2(nodeX + 42.0f, std::floor(centerY - bodyHeight * 0.5f)), textColor, name.c_str());
        drawList->PopClipRect();

        ImGui::TableSetColumnIndex(0);
        const float visibilityX = ImGui::GetCursorScreenPos().x;
        if (const auto* renderer = world.TryGet<ecs::components::MeshRendererComponent>(entity); renderer != nullptr)
        {
            DrawIcon(
                drawList,
                IconSize::Row14,
                renderer->visible ? ICON_EYE : ICON_EYE_OFF,
                ImVec2(visibilityX + kVisibilityColumn * 0.5f, centerY),
                selectedNow ? style::kTextStrong : style::kTextDim);
        }

        ImGui::TableSetColumnIndex(2);
        const float typeX = ImGui::GetCursorScreenPos().x;
        const float secondaryHeight = RoleTextHeight(FontRole::Secondary);
        AddRoleText(
            drawList,
            FontRole::Secondary,
            ImVec2(typeX + 8.0f, std::floor(centerY - secondaryHeight * 0.5f)),
            selectedNow ? style::kTextStrong : style::kTextDim,
            kind.name);

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

    void SceneEditor::CreateEmptyEntity(const core::WindowId windowId)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        ecs::World& world = *services_.world;
        const std::string beforeSnapshot = services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string();
        const ecs::EntityId entity = world.CreateEntity();
        world.Emplace<ecs::components::TagComponent>(entity).name = "Empty_" + std::to_string(entity);
        world.Emplace<ecs::components::TransformComponent>(entity);
        world.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;
        editorState.selectedEntity = entity;
        RecordSceneMutationImmediate("Create Empty Entity", beforeSnapshot);
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
