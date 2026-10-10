#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    void SceneEditor::BuildStatisticsPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showStatistics)
        {
            return;
        }

        const auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();

        if (ImGui::Begin(kStatisticsWindowName))
        {
            ImGui::Text("Mode: %s", editorState.mode == editor::RuntimeMode::Edit ? "Edit" : "Play");
            ImGui::Text("FPS: %.1f", windowState.timings.averageFps);
            ImGui::Text("Frame: %.2f ms", windowState.timings.frameMs);
            ImGui::Text("Render: %.2f ms", windowState.timings.renderMs);
            ImGui::Text("World update: %.2f ms", windowState.timings.worldUpdateMs);
            ImGui::Text("Scripts: %.3f ms (part of World update)", windowState.timings.scriptsMs);
            ImGui::Separator();
            ImGui::Text("Entities: %u", windowState.renderStats.totalEntities);
            ImGui::Text("Renderable: %u", windowState.renderStats.renderableEntities);
            ImGui::Text("Drawn: %u", windowState.renderStats.renderedEntities);
            ImGui::Text("Active collisions: %u", physicsState.stats.collisionPairs);
            ImGui::Text("Resource memory: %s", FormatBytes(windowState.renderStats.resourceMemoryBytes).c_str());
            ImGui::Separator();
            const auto& scriptStats = editorState.scriptStats;
            ImGui::Text("Scripts: %u (active %u, faulted %u)", scriptStats.instances, scriptStats.activeInstances, scriptStats.faultedInstances);
            ImGui::Text("Script modules: %u", scriptStats.scriptModules);
            ImGui::Text("Python objects: %u", scriptStats.pythonObjects);
            ImGui::Text("Script errors: %u", scriptStats.errors);
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
        if (ImGui::Begin(kScriptConsoleWindowName, &editorState.showScriptConsole))
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
        if (ImGui::Begin(kAssistantWindowName, &editorState.showAssistant))
        {
            assistantPanel_->Draw();
        }
        ImGui::End();
    }
}
