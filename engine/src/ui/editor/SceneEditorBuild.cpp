// The Build window (docs/editor/build-panel.md): choose a game, the configuration and the options, press Build & Package, watch the log and the progress of
// tools/package_game.ps1 (a child process: the editor stays alive), then Open folder / Run / Check package. The logic (the manifests of the games, the lines
// of the script, the state of a run, the command line) is editor/BuildPanelModel.h; the process is editor/BuildRunner.h; here is only what is drawn.

#include <cstdio>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <myengine/core/AppPaths.h>

#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        const std::chrono::steady_clock::time_point kNever{};

        std::wstring Wide(const std::string& utf8)
        {
            if (utf8.empty())
            {
                return {};
            }
            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
            return wide;
        }

        std::string FormatDuration(double seconds)
        {
            const int total = static_cast<int>(seconds);
            char text[32];
            std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
            return text;
        }

        const editor::GameEntry* SelectedEntry(const BuildPanelState& state)
        {
            if (state.selected < 0 || state.selected >= static_cast<int>(state.games.size()))
            {
                return nullptr;
            }
            return &state.games[static_cast<std::size_t>(state.selected)];
        }

        const editor::GameManifest* SelectedManifest(const BuildPanelState& state)
        {
            const editor::GameEntry* entry = SelectedEntry(state);
            return entry != nullptr && entry->manifest.has_value() ? &*entry->manifest : nullptr;
        }

        void ReloadGames(BuildPanelState& state)
        {
            state.root = core::apppaths::SourceRoot();
            state.games = editor::LoadGameManifests(state.root);
            state.loaded = true;
            state.selected = 0;
            for (std::size_t i = 0; i < state.games.size(); ++i)
            {
                if (state.games[i].folder == "rocketarena")
                {
                    state.selected = static_cast<int>(i);
                }
            }
            state.checkedAt = -100.0;
        }

        void SelectGame(BuildPanelState& state, int index)
        {
            state.selected = index;
            const editor::GameManifest* manifest = SelectedManifest(state);
            if (manifest != nullptr)
            {
                state.options.gameId = manifest->id;
                state.options.zip = manifest->zipByDefault;
            }
            state.checkedAt = -100.0;
        }

        // the things that are looked at from time to time: the target, the external data, the free place, the package
        void RefreshChecks(BuildPanelState& state, double now)
        {
            if (now - state.checkedAt < 2.0)
            {
                return;
            }
            state.checkedAt = now;
            const editor::GameManifest* manifest = SelectedManifest(state);
            state.dataPresent.clear();
            state.targetFound = true;
            state.packageExists = false;
            std::error_code ec;
            const auto space = std::filesystem::space(state.root, ec);
            state.freeBytes = ec ? 0 : static_cast<std::uint64_t>(space.available);
            if (manifest == nullptr)
            {
                return;
            }
            state.targetFound = !manifest->cmakeTarget.empty() && editor::CmakeTargetExists(state.root, manifest->cmakeTarget);
            for (const editor::ExternalDataItem& item : manifest->externalData)
            {
                state.dataPresent.push_back(editor::ExternalDataPresent(state.root, item));
            }
            const std::filesystem::path folder = editor::PackageFolder(state.root, state.options, *manifest);
            state.packageExists = std::filesystem::exists(folder / std::filesystem::u8path(manifest->exe), ec);
        }

        // the lines that the process wrote since the last frame, and its end
        void Pump(BuildPanelState& state)
        {
            for (const std::string& line : state.runner.TakeLines())
            {
                state.session.AddLine(line);
            }
            int exitCode = 0;
            if (state.runner.TakeFinished(exitCode))
            {
                state.session.Finish(exitCode);
                state.endedAt = std::chrono::steady_clock::now();
                state.checkedAt = -100.0;   // the package is there now (or is not)
                if (state.finishActions)
                {
                    state.finishedAt = ImGui::GetTime();
                }
            }
        }

        bool StartJob(BuildPanelState& state, editor::BuildJob job)
        {
            const editor::GameManifest* manifest = SelectedManifest(state);
            if (manifest == nullptr || state.session.IsRunning())
            {
                return false;
            }
            state.options.gameId = manifest->id;
            std::string command;
            std::string title;
            if (job == editor::BuildJob::Package)
            {
                command = editor::MakeBuildCommandLine(state.root, state.options, *manifest);
                title = "Build & Package: " + manifest->displayName;
            }
            else
            {
                if (manifest->checkScript.empty())
                {
                    return false;
                }
                command = editor::MakeCheckCommandLine(state.root, *manifest, editor::PackageFolder(state.root, state.options, *manifest));
                title = "Check package: " + manifest->displayName;
            }
            state.session.Begin(job, title);
            state.session.AddLine("> " + command);
            std::string error;
            state.startedAt = std::chrono::steady_clock::now();
            state.endedAt = kNever;
            if (!state.runner.Start(command, state.root, error))
            {
                state.session.FailToStart(error);
                state.endedAt = std::chrono::steady_clock::now();
                return false;
            }
            state.autoScroll = true;
            return true;
        }

        void OpenInShell(const std::filesystem::path& path, const std::filesystem::path& workingDirectory)
        {
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, workingDirectory.empty() ? nullptr : workingDirectory.wstring().c_str(), SW_SHOWNORMAL);
        }

        ImU32 LineColor(editor::BuildLineKind kind)
        {
            switch (kind)
            {
                case editor::BuildLineKind::Error: return style::kErrorText;
                case editor::BuildLineKind::Warning: return style::kWarning;
                case editor::BuildLineKind::Progress: return style::kTextDim;
                case editor::BuildLineKind::Result: return style::kPlay;
                case editor::BuildLineKind::Zip: return style::kPlay;
                case editor::BuildLineKind::Plain: break;
            }
            return style::kText;
        }
    }

    // ---------------------------------------------------------------- the commands of the scripted input

    std::string SceneEditor::RunEditorCommand(const std::string& command)
    {
        if (!buildPanel_)
        {
            return "no Build window";
        }
        BuildPanelState& state = *buildPanel_;
        if (!state.loaded)
        {
            ReloadGames(state);
            SelectGame(state, state.selected);
        }
        std::istringstream words(command);
        std::string verb;
        words >> verb;
        if (verb == "build-open")
        {
            showBuild_ = true;
            return "the Build window is open";
        }
        if (verb == "build-finish")
        {
            words >> state.finishShot >> state.finishQuitAfter;
            state.finishActions = true;
            state.finishShotTaken = false;
            state.finishedAt = -1.0;
            return "after the run: a screenshot and the quit";
        }
        if (verb == "build-cancel")
        {
            if (!state.session.IsRunning())
            {
                return "no run to cancel";
            }
            state.session.RequestCancel();
            state.runner.Cancel();
            return "cancel pressed";
        }
        if (verb == "build" || verb == "check")
        {
            std::string id;
            words >> id;
            int found = -1;
            for (std::size_t i = 0; i < state.games.size(); ++i)
            {
                if (state.games[i].folder == id)
                {
                    found = static_cast<int>(i);
                }
            }
            if (found < 0)
            {
                return "unknown game '" + id + "'";
            }
            SelectGame(state, found);
            showBuild_ = true;
            std::string option;
            while (words >> option)
            {
                if (option == "rwdi")
                {
                    state.configChoice = 1;
                    state.options.config = editor::BuildConfig::RelWithDebInfo;
                }
                else if (option == "nosteam")
                {
                    state.steamChoice = 2;
                    state.options.steam = editor::SdkChoice::Off;
                }
                else if (option == "nowwise")
                {
                    state.wwiseChoice = 1;
                    state.options.wwise = editor::SdkChoice::Off;
                }
                else if (option == "nozip")
                {
                    state.options.zip = false;
                }
                else if (option.rfind("out=", 0) == 0)
                {
                    state.options.outputFolder = option.substr(4);
                }
                else if (option.rfind("builddir=", 0) == 0)
                {
                    state.options.buildFolder = option.substr(9);
                }
            }
            return StartJob(state, verb == "build" ? editor::BuildJob::Package : editor::BuildJob::Check) ? "started" : "could not start (see the log of the window)";
        }
        return "unknown command '" + verb + "'";
    }

    // ---------------------------------------------------------------- the window

    void SceneEditor::DrawBuildWindow()
    {
        if (!buildPanel_)
        {
            return;
        }
        BuildPanelState& state = *buildPanel_;
        Pump(state);   // also while the window is closed: the run goes on and its log is kept
        if (state.finishActions && state.finishedAt >= 0.0)
        {
            const double since = ImGui::GetTime() - state.finishedAt;
            if (!state.finishShotTaken && since > 1.0 && services_.screenshot && !state.finishShot.empty())
            {
                state.finishShotTaken = true;
                services_.screenshot(state.finishShot);
            }
            if (since > state.finishQuitAfter && services_.requestQuit)
            {
                state.finishActions = false;
                services_.requestQuit();
            }
        }
        if (!showBuild_)
        {
            return;
        }
        if (!state.loaded)
        {
            ReloadGames(state);
            SelectGame(state, state.selected);
        }
        const double now = ImGui::GetTime();
        RefreshChecks(state, now);

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(mainViewport->Pos.x + 80.0f, mainViewport->Pos.y + 110.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(720.0f, 640.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Build###Build", &showBuild_))
        {
            ImGui::End();
            return;
        }

        const editor::GameManifest* manifest = SelectedManifest(state);
        const bool running = state.session.IsRunning();

        // ---- the game
        if (state.games.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kWarning));
            ImGui::TextWrapped("There is no game: a folder games/<id> with game.json is expected in the repository (%s).", core::apppaths::SourceRoot().string().c_str());
            ImGui::PopStyleColor();
            ImGui::End();
            return;
        }
        ImGui::BeginDisabled(running);
        {
            const editor::GameEntry* entry = SelectedEntry(state);
            const std::string preview = entry == nullptr ? std::string() : (manifest != nullptr ? manifest->displayName : entry->folder + " (broken game.json)");
            ImGui::TextUnformatted("Game");
            ImGui::SameLine(110.0f);
            ImGui::SetNextItemWidth(280.0f);
            if (BeginCombo("##game", preview.c_str()))
            {
                for (std::size_t i = 0; i < state.games.size(); ++i)
                {
                    const editor::GameEntry& game = state.games[i];
                    const std::string label = (game.manifest.has_value() ? game.manifest->displayName : game.folder + " (broken game.json)") + "##g" + std::to_string(i);
                    if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == state.selected))
                    {
                        SelectGame(state, static_cast<int>(i));
                        manifest = SelectedManifest(state);
                    }
                }
                EndCombo();
            }
        }
        ImGui::EndDisabled();

        const editor::GameEntry* entry = SelectedEntry(state);
        if (entry != nullptr && !entry->error.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kErrorText));
            ImGui::TextWrapped("games/%s/game.json: %s", entry->folder.c_str(), entry->error.c_str());
            ImGui::PopStyleColor();
        }
        if (manifest != nullptr)
        {
            ImGui::TextDisabled("%s, the target \"%s\", the package %s/%s", manifest->exe.c_str(), manifest->cmakeTarget.empty() ? "(none)" : manifest->cmakeTarget.c_str(),
                                state.options.outputFolder.c_str(), manifest->name.c_str());
            if (!state.targetFound)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kWarning));
                ImGui::TextWrapped("The CMake target \"%s\" was not found in the repository: the game cannot be built yet (its code is not there).",
                                   manifest->cmakeTarget.empty() ? "(none)" : manifest->cmakeTarget.c_str());
                ImGui::PopStyleColor();
            }
            for (std::size_t i = 0; i < manifest->externalData.size() && i < state.dataPresent.size(); ++i)
            {
                const editor::ExternalDataItem& item = manifest->externalData[i];
                const bool present = state.dataPresent[i];
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(present ? style::kPlay : style::kWarning));
                ImGui::TextUnformatted(present ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (present)
                {
                    ImGui::Text("%s: found (%s)", item.name.c_str(), item.check.c_str());
                }
                else
                {
                    ImGui::TextWrapped("%s: not found (%s)%s%s%s", item.name.c_str(), item.check.c_str(), item.optional ? "; the package goes without it" : "",
                                       item.fetch.empty() ? "" : "; to get it run ", item.fetch.c_str());
                }
            }
        }
        ImGui::Spacing();

        // ---- the options
        ImGui::BeginDisabled(running || manifest == nullptr);
        {
            static const char* const kConfigs[] = {"Release (shipping)", "RelWithDebInfo"};
            ImGui::TextUnformatted("Configuration");
            ImGui::SameLine(110.0f);
            const int config = Segmented("##config", kConfigs, 2, state.configChoice, 140.0f);
            if (config != state.configChoice)
            {
                state.configChoice = config;
                state.options.config = config == 1 ? editor::BuildConfig::RelWithDebInfo : editor::BuildConfig::Shipping;
            }
            if (manifest != nullptr && manifest->steam == editor::GameFeature::Optional)
            {
                static const char* const kSteam[] = {"Auto", "On", "Off"};
                ImGui::TextUnformatted("Steam");
                ImGui::SameLine(110.0f);
                const int steam = Segmented("##steam", kSteam, 3, state.steamChoice, 70.0f);
                if (steam != state.steamChoice)
                {
                    state.steamChoice = steam;
                    state.options.steam = steam == 1 ? editor::SdkChoice::On : (steam == 2 ? editor::SdkChoice::Off : editor::SdkChoice::Auto);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Auto: with the SDK when it is found");
            }
            if (manifest != nullptr && manifest->wwise == editor::GameFeature::Optional)
            {
                static const char* const kWwise[] = {"Auto", "Off"};
                ImGui::TextUnformatted("Wwise sound");
                ImGui::SameLine(110.0f);
                const int wwise = Segmented("##wwise", kWwise, 2, state.wwiseChoice, 70.0f);
                if (wwise != state.wwiseChoice)
                {
                    state.wwiseChoice = wwise;
                    state.options.wwise = wwise == 1 ? editor::SdkChoice::Off : editor::SdkChoice::Auto;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Off: the XAudio2 sound");
            }
            ImGui::TextUnformatted("Zip");
            ImGui::SameLine(110.0f);
            Checkbox("Write the zip for players##zip", &state.options.zip);
            ImGui::TextUnformatted("Output folder");
            ImGui::SameLine(110.0f);
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::InputText("##out", &state.options.outputFolder))
            {
                state.checkedAt = -100.0;
            }
            ImGui::TextUnformatted("Build folder");
            ImGui::SameLine(110.0f);
            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputTextWithHint("##builddir", "default: build-shipping in the repository (a full path may be on another drive)", &state.options.buildFolder);
        }
        ImGui::EndDisabled();

        // ---- the place on the disk
        const bool lowDisk = editor::IsDiskSpaceLow(state.freeBytes);
        if (lowDisk)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kWarning));
            ImGui::TextWrapped("%s Only %s is free on the drive of the repository (%s): a shipping build takes about a gigabyte and the build folder stays. Free some place first (below 3 GB the series stops).",
                               ICON_TRIANGLE_ALERT, editor::FormatSize(state.freeBytes).c_str(), state.root.root_name().string().c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("Free on the drive: %s", editor::FormatSize(state.freeBytes).c_str());
        }
        ImGui::Spacing();

        // ---- the buttons
        const bool canBuild = manifest != nullptr && state.targetFound && !running;
        if (PrimaryButton(lowDisk ? "Build anyway" : "Build & Package", ICON_PACKAGE, canBuild, 170.0f))
        {
            StartJob(state, editor::BuildJob::Package);
        }
        ImGui::SameLine();
        if (Button("Cancel", ICON_X, running && !state.session.CancelRequested(), 100.0f))
        {
            state.session.RequestCancel();
            state.runner.Cancel();
        }
        ImGui::SameLine();
        if (Button("Clear log", nullptr, !running && state.session.State() != editor::BuildState::Idle, 100.0f))
        {
            state.session.Clear();
        }

        // ---- the progress
        const editor::BuildState phase = state.session.State();
        if (phase != editor::BuildState::Idle)
        {
            const auto end = phase == editor::BuildState::Running ? std::chrono::steady_clock::now() : state.endedAt;
            const double seconds = state.startedAt == kNever ? 0.0 : std::chrono::duration<double>(end - state.startedAt).count();
            char overlay[96];
            std::snprintf(overlay, sizeof(overlay), "%d %%  %s  %s", state.session.Percent(), state.session.Stage().c_str(), FormatDuration(seconds).c_str());
            ImU32 barColor = style::kPrimary;
            if (phase == editor::BuildState::Done) { barColor = style::kPlay; }
            else if (phase == editor::BuildState::Failed) { barColor = style::kError; }
            else if (phase == editor::BuildState::Cancelled) { barColor = style::kWarning; }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, style::ToVec4(barColor));
            ImGui::ProgressBar(static_cast<float>(state.session.Percent()) / 100.0f, ImVec2(-1.0f, 20.0f), overlay);
            ImGui::PopStyleColor();

            const char* word = "";
            ImU32 wordColor = style::kText;
            switch (phase)
            {
                case editor::BuildState::Running: word = state.session.CancelRequested() ? "Cancelling..." : "Running"; break;
                case editor::BuildState::Done: word = "Done"; wordColor = style::kPlay; break;
                case editor::BuildState::Failed: word = "Failed"; wordColor = style::kErrorText; break;
                case editor::BuildState::Cancelled: word = "Cancelled"; wordColor = style::kWarning; break;
                case editor::BuildState::Idle: break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(wordColor));
            ImGui::TextUnformatted(word);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%s  -  %zu warning(s), %zu error(s)", state.session.Title().c_str(), state.session.WarningCount(), state.session.ErrorCount());
            if (phase == editor::BuildState::Failed && !state.session.FailureText().empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kErrorText));
                ImGui::TextWrapped("%s", state.session.FailureText().c_str());
                ImGui::PopStyleColor();
            }
            if (phase == editor::BuildState::Done && state.session.Job() == editor::BuildJob::Package)
            {
                if (!state.session.ResultPath().empty())
                {
                    ImGui::TextWrapped("Package: %s", state.session.ResultPath().c_str());
                }
                if (!state.session.ZipPath().empty())
                {
                    ImGui::TextWrapped("Zip: %s", state.session.ZipPath().c_str());
                }
            }
        }

        // ---- after the build: the folder, the game, the check
        if (manifest != nullptr)
        {
            const std::filesystem::path folder = editor::PackageFolder(state.root, state.options, *manifest);
            const bool havePackage = state.packageExists && !running;
            if (Button("Open folder", ICON_FOLDER_OPEN, havePackage || (std::filesystem::exists(folder.parent_path()) && !running), 130.0f))
            {
                OpenInShell(std::filesystem::exists(folder) ? folder : folder.parent_path(), {});
            }
            ImGui::SameLine();
            if (Button("Run", ICON_PLAY, havePackage, 100.0f))
            {
                OpenInShell(folder / std::filesystem::u8path(manifest->exe), folder);
            }
            ImGui::SameLine();
            if (Button("Check package", ICON_CIRCLE_CHECK, havePackage && !manifest->checkScript.empty(), 150.0f))
            {
                StartJob(state, editor::BuildJob::Check);
            }
            if (manifest->checkScript.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(no check script for this game)");
            }
            else if (!havePackage && !running)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(no package in the output folder yet)");
            }
        }

        // ---- the log
        ImGui::Spacing();
        if (Button("Copy log", ICON_COPY, !state.session.Log().empty(), 110.0f))
        {
            std::string text;
            for (const editor::BuildLogLine& line : state.session.Log())
            {
                text += line.text;
                text += '\n';
            }
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
        Checkbox("Scroll to the end##autoscroll", &state.autoScroll);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
        ImGui::BeginChild("##buildlog", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            PushFontRole(FontRole::Mono);
            const auto& log = state.session.Log();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(log.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const editor::BuildLogLine& line = log[static_cast<std::size_t>(i)];
                    ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(LineColor(line.kind)));
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
            PopFontRole();
            if (state.session.LogVersion() != state.seenLogVersion)
            {
                state.seenLogVersion = state.session.LogVersion();
                if (state.autoScroll)
                {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
    }
}
