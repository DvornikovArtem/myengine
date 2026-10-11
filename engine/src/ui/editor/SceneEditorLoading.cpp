// The loading window of long operations (a map, a project), the task button of the status bar and the error
// dialog of a map that could not be opened (spec: .team/design/ld1-spec.md).
// What runs and for how long is the business of editor::LoadingService; this file only draws it.

#include "SceneEditorInternal.h"

#include <cstdio>

#include <myengine/editor/LoadingService.h>

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        constexpr char kLoadingPopup[] = "##loading_modal";
        constexpr char kBlockerPopup[] = "##loading_blocker";
        constexpr char kErrorPopup[] = "Cannot open##load_error";
        constexpr char kTasksPopup[] = "##background_tasks";

        std::string PercentText(const float progress)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(progress * 100.0f + 0.5f));
            return text;
        }

        std::string ElapsedText(const double seconds)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "%.1f s", seconds);
            return text;
        }

        // An invisible modal popup: the input is taken without a window or a dimmed screen
        void DrawInputBlocker(const bool block)
        {
            if (block)
            {
                if (!ImGui::IsPopupOpen(kBlockerPopup))
                {
                    ImGui::OpenPopup(kBlockerPopup);
                }
            }
            else if (!ImGui::IsPopupOpen(kBlockerPopup))
            {
                return;
            }

            ImGui::SetNextWindowPos(ImVec2(-100.0f, -100.0f));
            ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool open = ImGui::BeginPopupModal(
                kBlockerPopup, nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoInputs);
            ImGui::PopStyleColor();
            if (open)
            {
                if (!block)
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }

    void SceneEditor::ReportLoadError(const std::string& title, const std::string& text)
    {
        loadErrorTitle_ = title;
        loadErrorText_ = text;
    }

    bool SceneEditor::DrawStartupSplash()
    {
        if (services_.loading == nullptr || !services_.loading->HasSplash())
        {
            return false;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 min = viewport->Pos;
        const ImVec2 max(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList(const_cast<ImGuiViewport*>(viewport));
        drawList->AddRectFilled(min, max, style::kTitle);

        // Before the threshold only the dark window: a quick start goes straight to the editor
        const editor::LoadingOperation* splash = nullptr;
        std::vector<editor::LoadingOperation> visible = services_.loading->Visible();
        for (const auto& operation : visible)
        {
            if (operation.mode == editor::LoadingMode::Splash)
            {
                splash = &operation;
                break;
            }
        }
        if (splash == nullptr)
        {
            return true;
        }

        constexpr float kBarWidth = 320.0f;
        const float centerX = (min.x + max.x) * 0.5f;
        const float centerY = (min.y + max.y) * 0.5f;
        float y = centerY - 92.0f;

        DrawIcon(drawList, IconSize::Tile40, ICON_CUBOID, ImVec2(centerX, y + 24.0f), style::kTextStrong);
        y += 62.0f;

        ImFont* strong = ImGui::GetFont();
        {
            PushFontRole(FontRole::Strong);
            strong = ImGui::GetFont();
            PopFontRole();
        }
        const float titleSize = 20.0f;
        const ImVec2 titleExtent = strong->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, splash->label.c_str());
        drawList->AddText(strong, titleSize, ImVec2(centerX - titleExtent.x * 0.5f, y), style::kTextStrong, splash->label.c_str());
        y += 30.0f;

        PushFontRole(FontRole::Body);
        ImFont* body = ImGui::GetFont();
        PopFontRole();
        const ImVec2 subtitleExtent = body->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, splash->subtitle.c_str());
        drawList->AddText(body, 14.0f, ImVec2(centerX - subtitleExtent.x * 0.5f, y), style::kTextDim, splash->subtitle.c_str());
        y += 14.0f + 22.0f;

        // The bar, then the stage on the left and the percent on the right
        ImGui::SetNextWindowPos(ImVec2(centerX - kBarWidth * 0.5f, y));
        ImGui::SetNextWindowSize(ImVec2(kBarWidth, 40.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::Begin("##splash_progress", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing))
        {
            LoadingBar(kBarWidth, splash->progress, 4.0f);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            const ImVec2 textPosition = ImGui::GetCursorScreenPos();
            ImDrawList* windowDraw = ImGui::GetWindowDrawList();
            PushFontRole(FontRole::Tiny);
            windowDraw->AddText(textPosition, style::kTextDim, splash->detail.empty() ? " " : splash->detail.c_str());
            PopFontRole();
            if (splash->progress >= 0.0f)
            {
                PushFontRole(FontRole::Mono);
                const std::string percent = PercentText(splash->progress);
                const ImVec2 percentSize = ImGui::CalcTextSize(percent.c_str());
                windowDraw->AddText(ImVec2(textPosition.x + kBarWidth - percentSize.x, textPosition.y), style::kTextDim, percent.c_str());
                PopFontRole();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // The project file, bottom right
        const auto& project = core::ServiceLocator::GetProjectContext();
        const std::string projectPath = project.ProjectFile().u8string();
        if (!projectPath.empty())
        {
            PushFontRole(FontRole::Tiny);
            const ImVec2 pathSize = ImGui::CalcTextSize(projectPath.c_str());
            drawList->AddText(ImVec2(max.x - 16.0f - pathSize.x, max.y - 14.0f - pathSize.y), style::kTextDim, projectPath.c_str());
            PopFontRole();
        }
        return true;
    }

    void SceneEditor::DrawLoadingUi()
    {
        if (services_.loading == nullptr)
        {
            return;
        }

        const auto operations = services_.loading->Visible();
        const editor::LoadingOperation* modal = nullptr;
        for (const auto& operation : operations)
        {
            if (operation.mode == editor::LoadingMode::Modal)
            {
                modal = &operation;
                break;
            }
        }

        if (modal != nullptr)
        {
            if (!ImGui::IsPopupOpen(kLoadingPopup))
            {
                ImGui::OpenPopup(kLoadingPopup);
            }

            constexpr float kWidth = 440.0f;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRoundingDialog);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, style::ToVec4(style::kPanel));
            ImGui::PushStyleColor(ImGuiCol_Border, style::ToVec4(style::kBorderLight));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
            const bool open = ImGui::BeginPopupModal(
                kLoadingPopup, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
            if (open)
            {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float contentWidth = kWidth - 40.0f;

                // 1. The spinner, the title and the elapsed time (after the first second)
                DrawSpinner(drawList, ImVec2(start.x + 8.0f, start.y + 10.0f), 7.0f, style::kPrimary, 2.0f);
                PushFontRole(FontRole::Strong);
                const ImVec2 titleSize = ImGui::CalcTextSize(modal->label.c_str());
                drawList->AddText(ImVec2(start.x + 26.0f, std::floor(start.y + 10.0f - titleSize.y * 0.5f)), style::kTextStrong,
                    modal->label.c_str());
                PopFontRole();
                if (modal->elapsedSeconds >= 1.0)
                {
                    PushFontRole(FontRole::Mono);
                    const std::string elapsed = ElapsedText(modal->elapsedSeconds);
                    const ImVec2 elapsedSize = ImGui::CalcTextSize(elapsed.c_str());
                    drawList->AddText(ImVec2(start.x + contentWidth - elapsedSize.x, std::floor(start.y + 10.0f - elapsedSize.y * 0.5f)),
                        style::kTextDim, elapsed.c_str());
                    PopFontRole();
                }
                ImGui::Dummy(ImVec2(contentWidth, 22.0f));

                // 2. What exactly is loading
                PushFontRole(FontRole::Secondary);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 26.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                ImGui::TextUnformatted(modal->subtitle.empty() ? " " : modal->subtitle.c_str());
                ImGui::PopStyleColor();
                PopFontRole();

                // 3. The bar, 4. the detail and the percent
                ImGui::Dummy(ImVec2(0.0f, 8.0f));
                const bool cancelling = modal->cancelRequested;
                LoadingBar(contentWidth, cancelling ? -1.0f : modal->progress, 4.0f);
                ImGui::Dummy(ImVec2(0.0f, 6.0f));

                const ImVec2 detailPosition = ImGui::GetCursorScreenPos();
                PushFontRole(FontRole::Tiny);
                const std::string detail = cancelling ? std::string("Cancelling...") : modal->detail;
                drawList->AddText(detailPosition, style::kTextDim, detail.empty() ? " " : detail.c_str());
                PopFontRole();
                if (modal->progress >= 0.0f && !cancelling)
                {
                    PushFontRole(FontRole::Mono);
                    const std::string percent = PercentText(modal->progress);
                    const ImVec2 percentSize = ImGui::CalcTextSize(percent.c_str());
                    drawList->AddText(ImVec2(detailPosition.x + contentWidth - percentSize.x, detailPosition.y), style::kTextDim,
                        percent.c_str());
                    PopFontRole();
                }
                ImGui::Dummy(ImVec2(contentWidth, 16.0f));

                // 5. Only a cancellable operation has a footer
                if (modal->cancellable)
                {
                    ImGui::Dummy(ImVec2(0.0f, 10.0f));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + contentWidth - 90.0f);
                    if (Button("Cancel", ICON_X, !cancelling, 90.0f))
                    {
                        services_.loading->RequestCancel(modal->id);
                    }
                }
                ImGui::EndPopup();
            }
        }
        else
        {
            // The operation has not run long enough to deserve a window, but the editor must not be used meanwhile
            DrawInputBlocker(services_.loading->HasModal());

            if (ImGui::IsPopupOpen(kLoadingPopup))
            {
                // The window leaves with its operation
                ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                const bool open = ImGui::BeginPopupModal(
                    kLoadingPopup, nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoInputs);
                ImGui::PopStyleColor();
                if (open)
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
        }

        // A map that could not be opened: the usual error dialog, after the loading window has gone
        if (!loadErrorTitle_.empty() && modal == nullptr)
        {
            if (!ImGui::IsPopupOpen(kErrorPopup))
            {
                ImGui::OpenPopup(kErrorPopup);
            }
            if (BeginDialog(kErrorPopup, loadErrorTitle_.c_str(), ICON_CIRCLE_X, 460.0f))
            {
                {
                    const ImVec2 position = ImGui::GetWindowPos();
                    DrawIcon(ImGui::GetWindowDrawList(), IconSize::Row14, ICON_CIRCLE_X,
                        ImVec2(position.x + 16.0f + 7.5f, position.y + 19.0f), style::kError);
                }
                ImGui::PushTextWrapPos(460.0f - 16.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kText));
                ImGui::TextUnformatted(loadErrorText_.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();

                DialogFooter();
                DialogAlignRight(80.0f);
                if (Button("Close", nullptr, true, 80.0f))
                {
                    loadErrorTitle_.clear();
                    loadErrorText_.clear();
                    ImGui::CloseCurrentPopup();
                }
                EndDialog();
            }
        }
    }

    void SceneEditor::DrawBackgroundTasks(ImDrawList* drawList, const float rightEdge, const float centerY)
    {
        if (services_.loading == nullptr)
        {
            return;
        }

        std::vector<editor::LoadingOperation> tasks;
        for (auto& operation : services_.loading->Visible())
        {
            if (operation.mode == editor::LoadingMode::Background)
            {
                tasks.push_back(std::move(operation));
            }
        }
        if (tasks.empty())
        {
            return;
        }

        const editor::LoadingOperation& oldest = tasks.front();

        // [spinner] name [N tasks] [bar]
        constexpr float kHeight = 22.0f;
        constexpr float kBarWidth = 60.0f;
        PushFontRole(FontRole::Secondary);
        const ImVec2 nameSize = ImGui::CalcTextSize(oldest.label.c_str());
        PopFontRole();
        PushFontRole(FontRole::Tiny);
        const std::string chipText = std::to_string(tasks.size()) + " tasks";
        const float chipWidth = tasks.size() > 1 ? ImGui::CalcTextSize(chipText.c_str()).x + 12.0f : 0.0f;
        PopFontRole();
        const float width = 8.0f + 13.0f + 6.0f + nameSize.x + 8.0f + (chipWidth > 0.0f ? chipWidth + 8.0f : 0.0f) + kBarWidth + 8.0f;

        const ImVec2 min(rightEdge - width, centerY - kHeight * 0.5f);
        const ImVec2 max(rightEdge, centerY + kHeight * 0.5f);
        ImGui::SetCursorScreenPos(min);
        const bool pressed = ImGui::InvisibleButton("##backgroundTasks", ImVec2(width, kHeight));
        const bool hovered = ImGui::IsItemHovered();
        drawList->AddRectFilled(min, max, hovered ? style::kControlHover : style::kControl, style::kRoundingSmall);

        float x = min.x + 8.0f;
        DrawSpinner(drawList, ImVec2(x + 6.5f, centerY), 5.5f, style::kPrimary, 1.6f);
        x += 13.0f + 6.0f;
        PushFontRole(FontRole::Secondary);
        drawList->AddText(ImVec2(x, std::floor(centerY - nameSize.y * 0.5f)), style::kTextStrong, oldest.label.c_str());
        PopFontRole();
        x += nameSize.x + 8.0f;
        if (chipWidth > 0.0f)
        {
            ImGui::SetCursorScreenPos(ImVec2(x, centerY - 9.0f));
            Chip(chipText.c_str(), ChipKind::Blue);
            x += chipWidth + 8.0f;
        }
        // The mini bar: 60 x 3
        {
            const ImVec2 barMin(x, centerY - 1.5f);
            const ImVec2 barMax(x + kBarWidth, centerY + 1.5f);
            drawList->AddRectFilled(barMin, barMax, style::kInput, 1.5f);
            if (oldest.progress >= 0.0f)
            {
                drawList->AddRectFilled(barMin, ImVec2(barMin.x + kBarWidth * std::min(oldest.progress, 1.0f), barMax.y), style::kPrimary, 1.5f);
            }
            else
            {
                const float segment = kBarWidth * 0.3f;
                const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) / 1.2f, 1.0f);
                const float segmentX = barMin.x + (kBarWidth + segment) * phase - segment;
                drawList->PushClipRect(barMin, barMax, true);
                drawList->AddRectFilled(ImVec2(segmentX, barMin.y), ImVec2(segmentX + segment, barMax.y), style::kPrimary, 1.5f);
                drawList->PopClipRect();
            }
        }

        if (hovered)
        {
            std::string list;
            for (const auto& task : tasks)
            {
                list += task.label + (task.detail.empty() ? std::string() : "  \xC2\xB7  " + task.detail) + "\n";
            }
            if (!list.empty())
            {
                list.pop_back();
            }
            Tooltip(list.c_str());
        }

        if (pressed)
        {
            ImGui::OpenPopup(kTasksPopup);
        }

        // The list of the tasks, above the status bar
        ImGui::SetNextWindowPos(ImVec2(max.x, min.y - 6.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, style::ToVec4(style::kRecessed));
        ImGui::PushStyleColor(ImGuiCol_Border, style::ToVec4(style::kBorderLight));
        const bool open = ImGui::BeginPopup(kTasksPopup, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        if (open)
        {
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            ImGui::TextUnformatted("BACKGROUND TASKS");
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            const float rowWidth = ImGui::GetContentRegionAvail().x;
            for (const auto& task : tasks)
            {
                ImGui::PushID(static_cast<int>(task.id));
                ImDrawList* popupDraw = ImGui::GetWindowDrawList();
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                DrawSpinner(popupDraw, ImVec2(rowMin.x + 7.0f, rowMin.y + 9.0f), 5.5f, style::kPrimary, 1.6f);
                PushFontRole(FontRole::Secondary);
                popupDraw->AddText(ImVec2(rowMin.x + 22.0f, rowMin.y + 1.0f), style::kTextStrong, task.label.c_str());
                PopFontRole();
                if (!task.detail.empty())
                {
                    PushFontRole(FontRole::Tiny);
                    popupDraw->AddText(ImVec2(rowMin.x + 22.0f, rowMin.y + 19.0f), style::kTextDim, task.detail.c_str());
                    PopFontRole();
                }
                if (task.cancellable)
                {
                    ImGui::SetCursorScreenPos(ImVec2(rowMin.x + rowWidth - 26.0f, rowMin.y));
                    if (IconButton("##cancelTask", ICON_X, "Cancel", false, !task.cancelRequested, 0, 22.0f, nullptr, IconSize::Row14))
                    {
                        services_.loading->RequestCancel(task.id);
                    }
                }
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + 38.0f));
                LoadingBar(rowWidth, task.progress, 3.0f);
                ImGui::Dummy(ImVec2(0.0f, 8.0f));
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
}
