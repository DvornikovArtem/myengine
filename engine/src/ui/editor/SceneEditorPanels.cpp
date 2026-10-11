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

        // One statistics group: a collapsible category, then label (left) / mono value (right) property rows
        void DrawStatsGroup(const char* title, const std::vector<StatRow>& rows)
        {
            if (!BeginCategory(title))
            {
                return;
            }
            if (BeginPropertyGrid(title))
            {
                for (const StatRow& row : rows)
                {
                    // Label and value are centred in the row by hand: PropertyLabel pins the text to the cell top,
                    // which suits rows with a 24 px field but leaves a text-only row top-heavy
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, style::kPropRowHeight);
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 labelPosition = ImGui::GetCursorScreenPos();
                    const ImVec2 labelSize = ImGui::CalcTextSize(row.label);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(labelPosition.x, std::floor(labelPosition.y + (style::kFrameHeight - labelSize.y) * 0.5f)),
                        style::kText, row.label);
                    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, style::kFrameHeight));
                    ImGui::TableSetColumnIndex(1);
                    const ImVec2 position = ImGui::GetCursorScreenPos();
                    const float available = ImGui::GetContentRegionAvail().x;
                    PushFontRole(FontRole::Mono);
                    // The mono role font carries the icon font's taller line box, so centre by the measured height
                    const ImVec2 valueSize = ImGui::CalcTextSize(row.value.c_str());
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(std::floor(position.x + std::max(available - valueSize.x, 0.0f)),
                               std::floor(position.y + (style::kFrameHeight - valueSize.y) * 0.5f)),
                        style::kTextStrong, row.value.c_str());
                    PopFontRole();
                    ImGui::Dummy(ImVec2(available, style::kFrameHeight));
                }
                EndPropertyGrid();
            }
            EndCategory();
        }

        std::string Fixed(const double value, const int decimals, const char* unit = nullptr)
        {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "%.*f%s%s", decimals, value, unit != nullptr ? " " : "", unit != nullptr ? unit : "");
            return buffer;
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

            DrawStatsGroup("Frame", frame);
            DrawStatsGroup("Scene", scene);
            DrawStatsGroup("Scripts", scripts);
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
