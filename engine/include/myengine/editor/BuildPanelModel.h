// BuildPanelModel.h
// The model of the Build window of the editor (docs/editor/build-panel.md): no window, no process, no ImGui here, so that a test can drive all of it.
//   - the manifests of the games, games/<id>/game.json (what the window lists and what tools/package_game.ps1 reads);
//   - the lines that tools/package_game.ps1 writes for a program: "##progress <0-100> <stage>", "##result ok|fail <path or what went wrong>", "##zip <path>";
//   - the state of one run: Idle -> Running -> Done / Failed / Cancelled, with the log, the progress and the result;
//   - the command line of the script for the chosen options.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myengine::editor
{
    // ---------------------------------------------------------------- the manifests of the games

    // What the game says about a feature of the package that depends on an SDK (Steam, Wwise)
    enum class GameFeature : std::uint8_t
    {
        Optional, // the switch is there; the script looks for the SDK and builds without it, with a warning, when it is not found
        No,       // the game has none: no switch, the SDK is not looked for
    };

    struct ExternalDataItem
    {
        std::string name;   // "Freedoom (the free data of the game, WAD)"
        std::string check;  // a file that is there when the data is there: relative to the repository, or with %NAME% (an environment variable: %LOCALAPPDATA%)
        std::string fetch;  // the script that downloads it (relative to the repository); empty: none
        bool optional = false;   // the package goes without it, with a warning
    };

    struct GameManifest
    {
        std::string id;              // the folder name under games/
        std::string displayName;     // "Rocket Arena"
        std::string name;            // the folder and the file prefix of the package: "RocketArena"
        std::string exe;             // "RocketArena.exe"
        std::string cmakeTarget;     // the target of the shipping build; empty: the manifest names none
        std::string exePath;         // where the exe of that target lands in the build folder: "app/Release/RocketArena.exe"
        std::string userDataFolder;  // the folder of the player's files under %LOCALAPPDATA%
        std::string icon;            // a path relative to the repository; may be empty
        std::string checkScript;     // the script that checks the package (tools/check_package.ps1); empty: there is none for this game
        GameFeature steam = GameFeature::Optional;
        GameFeature wwise = GameFeature::Optional;
        bool zipByDefault = true;
        std::vector<ExternalDataItem> externalData;
    };

    // One folder of games/: a manifest, or the reason that there is none (the window shows the reason instead of hiding the game)
    struct GameEntry
    {
        std::string folder;                 // the name of the folder: games/<folder>
        std::optional<GameManifest> manifest;
        std::string error;                  // readable; empty when the manifest is fine
    };

    // Parses the text of a game.json. `folderName` is the folder it lies in: the id must be the same. false: `error` says what is wrong and where.
    bool ParseGameManifest(std::string_view jsonText, std::string_view folderName, GameManifest& out, std::string& error);
    // Every games/<folder>/game.json under `repoRoot`, sorted by the folder name; a folder without game.json is skipped, a broken one is listed with its error
    std::vector<GameEntry> LoadGameManifests(const std::filesystem::path& repoRoot);

    // Is `target` made by a CMake file of the repository (add_executable / add_library)? The CMake files under external/ and build folders are not read.
    bool CmakeTargetExists(const std::filesystem::path& repoRoot, std::string_view target);
    // Is a file or a folder of the data there (relative to the repository; %NAME% in the path is an environment variable)?
    bool ExternalDataPresent(const std::filesystem::path& repoRoot, const ExternalDataItem& item);

    // ---------------------------------------------------------------- the lines of the script

    enum class BuildLineKind : std::uint8_t
    {
        Plain,
        Progress, // ##progress <percent> <stage>
        Result,   // ##result ok|fail <text>
        Zip,      // ##zip <path>
        Warning,  // "WARNING: ...", "warning C4100", "warning MSB..."
        Error,    // "error C2065", "fatal error", "CMake Error", "FAILED", "##result fail"
    };

    struct BuildLine
    {
        BuildLineKind kind = BuildLineKind::Plain;
        int percent = 0;       // Progress: 0..100
        std::string stage;     // Progress: the word after the number
        bool ok = false;       // Result
        std::string text;      // Result: the path (ok) or what went wrong (fail); Zip: the path; the others: the line itself
    };

    BuildLine ParseBuildLine(std::string_view line);

    // ---------------------------------------------------------------- the options and the command line

    enum class SdkChoice : std::uint8_t
    {
        Auto, // the script looks for the SDK and builds without it, with a warning, when it is not there
        On,   // required: the script stops when it is not there
        Off,  // not used
    };

    enum class BuildConfig : std::uint8_t
    {
        Shipping,        // Release: -Config Shipping
        RelWithDebInfo,  // the same shipping build with the symbols next to the exe: -Config RelWithDebInfo
    };

    struct BuildOptions
    {
        std::string gameId;
        BuildConfig config = BuildConfig::Shipping;
        SdkChoice steam = SdkChoice::Auto;
        SdkChoice wwise = SdkChoice::Auto;
        bool zip = true;
        std::string outputFolder = "dist";   // relative to the repository, or a full path
        std::string buildFolder;             // the build folder of the shipping build (relative or a full path, also on another drive); empty: the default of the script
    };

    // The arguments after "powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script>": the game, the configuration, the switches, the folders (-Out, -BuildDir).
    // A switch that the game does not have (`manifest`) is left out. Quotes are put where a value has a space.
    std::vector<std::string> MakeScriptArguments(const BuildOptions& options, const GameManifest& manifest);
    // The whole command line (UTF-8) of the run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<root>\tools\package_game.ps1" <arguments>
    std::string MakeBuildCommandLine(const std::filesystem::path& repoRoot, const BuildOptions& options, const GameManifest& manifest);
    // The same for the check of a package (the script of the manifest, -Dist <the folder of the package>)
    std::string MakeCheckCommandLine(const std::filesystem::path& repoRoot, const GameManifest& manifest, const std::filesystem::path& packageFolder);

    // The place of the package after a run: <output folder>/<name> (the folder the script reports in "##result ok"); full path
    std::filesystem::path PackageFolder(const std::filesystem::path& repoRoot, const BuildOptions& options, const GameManifest& manifest);

    // Free space below this (bytes) and the window warns before the run: a shipping build takes about a gigabyte, the editor and the others more
    constexpr std::uint64_t kLowDiskBytes = 3ull * 1024 * 1024 * 1024;
    bool IsDiskSpaceLow(std::uint64_t freeBytes);
    std::string FormatSize(std::uint64_t bytes);

    // ---------------------------------------------------------------- the state of a run

    enum class BuildState : std::uint8_t
    {
        Idle,       // nothing ran yet (or the window was cleared)
        Running,
        Done,       // the script said "##result ok" (or ended with the code 0 without a result line)
        Failed,     // "##result fail" or an exit code that is not 0
        Cancelled,  // the person pressed Cancel
    };

    enum class BuildJob : std::uint8_t
    {
        Package,    // tools/package_game.ps1
        Check,      // tools/check_package.ps1
    };

    struct BuildLogLine
    {
        std::string text;
        BuildLineKind kind = BuildLineKind::Plain;
    };

    // One run, as the window shows it. Not thread-safe: the window feeds it on the main thread (the runner hands the lines over through a queue).
    class BuildSession
    {
    public:
        static constexpr std::size_t kMaxLines = 20000;   // the log is bounded: the oldest lines go

        // Idle/Done/Failed/Cancelled -> Running. false while a run is going (a second run is not allowed). Clears the log and the result.
        bool Begin(BuildJob job, std::string title);
        // One line of the output; only while Running. The ##progress lines move the bar and the stage; ##result and ##zip fill the result.
        void AddLine(std::string_view line);
        // The process ended. Running -> Done / Failed by the result line and the code (Cancelled stays Cancelled if Cancel was requested).
        void Finish(int exitCode);
        // Cancel was pressed: the process is being killed; Finish() then makes it Cancelled
        void RequestCancel();
        // The process could not even start
        void FailToStart(std::string reason);
        void Clear();   // back to Idle with an empty log (not while Running)

        BuildState State() const { return state_; }
        bool IsRunning() const { return state_ == BuildState::Running; }
        BuildJob Job() const { return job_; }
        const std::string& Title() const { return title_; }
        int Percent() const { return percent_; }
        const std::string& Stage() const { return stage_; }
        const std::string& ResultPath() const { return resultPath_; }   // Done: the folder of the package
        const std::string& ZipPath() const { return zipPath_; }
        const std::string& FailureText() const { return failureText_; } // Failed: what the script (or the process) said
        int ExitCode() const { return exitCode_; }
        bool CancelRequested() const { return cancelRequested_; }
        std::size_t WarningCount() const { return warnings_; }
        std::size_t ErrorCount() const { return errors_; }
        const std::vector<BuildLogLine>& Log() const { return log_; }
        // Bumped whenever a line is added: the window scrolls to the end when it changes
        std::uint64_t LogVersion() const { return logVersion_; }

    private:
        BuildState state_ = BuildState::Idle;
        BuildJob job_ = BuildJob::Package;
        std::string title_;
        int percent_ = 0;
        std::string stage_;
        std::string resultPath_;
        std::string zipPath_;
        std::string failureText_;
        int exitCode_ = 0;
        bool cancelRequested_ = false;
        bool resultSeen_ = false;
        bool resultOk_ = false;
        std::size_t warnings_ = 0;
        std::size_t errors_ = 0;
        std::vector<BuildLogLine> log_;
        std::uint64_t logVersion_ = 0;
    };
}
