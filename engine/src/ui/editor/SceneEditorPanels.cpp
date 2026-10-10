#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        struct StatRow
        {
            const char* label;
            std::string value;
        };

        // One statistics group: a category band with the title, then label (left) / mono value (right) rows
        void DrawStatsGroup(const char* title, const std::vector<StatRow>& rows)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const float x0 = window->DC.CursorPos.x;
            const float x1 = x0 + ImGui::GetContentRegionAvail().x;
            const float y = ImGui::GetCursorScreenPos().y;
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + style::kCategoryHeight), style::kHeader);
            drawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), style::kInput, 1.0f);
            PushFontRole(FontRole::Strong);
            drawList->AddText(ImGui::GetFont(), style::kFontStrong,
                              ImVec2(x0 + 12.0f, std::floor(y + (style::kCategoryHeight - style::kFontStrong) * 0.5f - 0.5f)),
                              style::kTextStrong, title);
            PopFontRole();
            ImGui::Dummy(ImVec2(0.0f, style::kCategoryHeight));

            for (const StatRow& row : rows)
            {
                const ImVec2 position = ImGui::GetCursorScreenPos();
                const float centerY = position.y + style::kPropRowHeight * 0.5f;
                drawList->AddText(ImVec2(x0 + 12.0f, std::floor(centerY - style::kFontBody * 0.5f - 0.5f)), style::kText, row.label);
                PushFontRole(FontRole::Mono);
                const float valueWidth = ImGui::CalcTextSize(row.value.c_str()).x;
                drawList->AddText(ImVec2(x1 - 12.0f - valueWidth, std::floor(centerY - style::kFontMono * 0.5f - 0.5f)),
                                  style::kTextStrong, row.value.c_str());
                PopFontRole();
                ImGui::Dummy(ImVec2(0.0f, style::kPropRowHeight));
            }
        }

        std::string Fixed(const double value, const int decimals, const char* unit = nullptr)
        {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "%.*f%s%s", decimals, value, unit != nullptr ? " " : "", unit != nullptr ? unit : "");
            return buffer;
        }

        void DrawStatsGrid(const char* id, const std::vector<std::pair<const char*, const std::vector<StatRow>*>>& groups, const bool columns)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
            if (columns)
            {
                if (ImGui::BeginTable(id, static_cast<int>(groups.size()), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
                {
                    ImGui::TableNextRow();
                    for (std::size_t index = 0; index < groups.size(); ++index)
                    {
                        ImGui::TableSetColumnIndex(static_cast<int>(index));
                        DrawStatsGroup(groups[index].first, *groups[index].second);
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                for (const auto& group : groups)
                {
                    DrawStatsGroup(group.first, *group.second);
                }
            }
            ImGui::PopStyleVar();
        }
    }

    void SceneEditor::BuildStatisticsPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showStatistics)
        {
            return;
        }

        const auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(kStatisticsWindowName, &editorState.showStatistics);
        ImGui::PopStyleVar();
        if (visible)
        {
            const auto& scriptStats = editorState.scriptStats;
            const std::vector<StatRow> frame = {
                {"FPS", Fixed(windowState.timings.averageFps, 1)},
                {"Frame", Fixed(windowState.timings.frameMs, 2, "ms")},
                {"Render", Fixed(windowState.timings.renderMs, 2, "ms")},
                {"World update", Fixed(windowState.timings.worldUpdateMs, 2, "ms")},
                {"Scripts (in World update)", Fixed(windowState.timings.scriptsMs, 3, "ms")},
            };
            const std::vector<StatRow> scene = {
                {"Entities", std::to_string(windowState.renderStats.totalEntities)},
                {"Renderable", std::to_string(windowState.renderStats.renderableEntities)},
                {"Drawn", std::to_string(windowState.renderStats.renderedEntities)},
                {"Active collisions", std::to_string(physicsState.stats.collisionPairs)},
                {"Resource memory", FormatBytes(windowState.renderStats.resourceMemoryBytes)},
            };
            const std::vector<StatRow> scripts = {
                {"Instances", std::to_string(scriptStats.instances) + " (" + std::to_string(scriptStats.activeInstances) + " active, " +
                                  std::to_string(scriptStats.faultedInstances) + " faulted)"},
                {"Modules", std::to_string(scriptStats.scriptModules)},
                {"Python objects", std::to_string(scriptStats.pythonObjects)},
                {"Errors", std::to_string(scriptStats.errors)},
            };

            // Three columns when there is room, one group under another otherwise
            const bool wide = ImGui::GetContentRegionAvail().x >= 700.0f;
            DrawStatsGrid("##stats", {{"Frame", &frame}, {"Scene", &scene}, {"Scripts", &scripts}}, wide);
        }
        ImGui::End();
    }

    void SceneEditor::BuildPrefabsPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showPrefabs)
        {
            return;
        }
        if (ImGui::Begin(kPrefabsWindowName, &editorState.showPrefabs))
        {
            if (services_.prefabLibrary != nullptr)
            {
                prefabInspector_->Draw(*services_.prefabLibrary, services_.describeScriptFields);
            }
            else
            {
                ImGui::TextDisabled("Prefab library is unavailable.");
            }
        }
        ImGui::End();
    }

    void SceneEditor::BuildScriptConsolePanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showScriptConsole)
        {
            return;
        }
        // The tools row, the output well and the input row paint their own padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(kScriptConsoleWindowName, &editorState.showScriptConsole);
        ImGui::PopStyleVar();
        if (visible)
        {
            scriptConsole_->Draw(services_.scriptConsole, editorState.mode == editor::RuntimeMode::Play);
        }
        ImGui::End();
    }

    void SceneEditor::BuildAssistantPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (assistantPanel_ == nullptr)
        {
            return;
        }
        assistantPanel_->Update(); // pumps the pipe even while the window is hidden
        if (!editorState.showAssistant)
        {
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(kAssistantWindowName, &editorState.showAssistant);
        ImGui::PopStyleVar();
        if (visible)
        {
            assistantPanel_->Draw();
        }
        ImGui::End();
    }
}
