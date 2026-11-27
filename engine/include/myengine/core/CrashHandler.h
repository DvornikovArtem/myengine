// CrashHandler.h
// A crash of the game leaves a report the player can send: three files with one name crash-<date>-<time>-<reportId> in
// <UserData>\crashes (%LOCALAPPDATA%\RocketArena\crashes in a shipping build):
//   .dmp  the minidump: the exception, its context and the thread that crashed, the stacks of all threads, the modules;
//   .txt  what can be read without a debugger: reportId, the time, buildId, version and commit, the scene, the last action, the exception;
//   .log  the log of the run that crashed (the next start truncates myengine.log, so it is copied).
// The next start finds the marker the handler left and the game says so (docs/game/packaging.md, "Падения"; LR5: docs/crash/README.md).

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace myengine::core::crash
{
    // What the previous run left: the files and the line of the exception ("access violation 0xC0000005 at RocketArena.exe+0x1234")
    struct Report
    {
        std::filesystem::path dump;
        std::filesystem::path text;
        std::string summary;
    };

    // Sets the filter of the unhandled exceptions (and std::terminate, abort, a pure virtual call, a bad CRT parameter) of the process.
    // `crashDirectory` is created at the crash; `logFile` is copied into the report. The reportId of this run is made here (a crash ends the
    // process, so one run has at most one report): 16 random hex digits.
    void Install(const std::filesystem::path& crashDirectory, const std::filesystem::path& logFile, const wchar_t* productName = L"Rocket Arena");

    // The reportId a crash of this run would have ("" before Install)
    std::string ReportId();

    // The context a report keeps (and the log says): the scene that is open and the last thing the player did ("screen main", "crash test:
    // access violation"). Copied into fixed buffers (a crashed process must not allocate to read them); a longer text is cut.
    void SetScene(std::string_view scene);
    void SetLastAction(std::string_view action);

    // The crash of the previous run if it has not been reported yet (the marker is removed: it is told once)
    std::optional<Report> TakePending(const std::filesystem::path& crashDirectory);

    // What the game tells the player after a crash: "The game crashed last time. Send crash-<date>-<time>-<reportId>.* from <folder>". In a shipping build the folder is written
    // by %LOCALAPPDATA% (the folder of the product, RocketArena or FlyDoom), otherwise it is the real one. `crashDirectory` is the one given to TakePending.
    std::string PlayerNote(const Report& report, const std::filesystem::path& crashDirectory);

    // A library may take the handlers over after Install (SteamAPI_Init installs its own breakpad): called every frame by the screens of the game,
    // it puts ours back as soon as another one is found (one atomic exchange when nothing changed)
    void Reassert();

    // MYENGINE_CRASH_TEST=1 in the environment: crashes right here, on purpose (the check of the handler, tools/check_package.ps1);
    // MYENGINE_CRASH_TEST=2: crashes 10 s after the first Reassert, when the game runs and Steam has started (the check that Steam did not take the handler)
    void CrashIfRequested();

    // The crashes of the CRASH TEST buttons of the main menu (LR5, docs/crash/tester-guide.md). Each is a function of its own that is never inlined,
    // so the stack of the dump names it with its file and line.
    namespace test
    {
        void DivideByZero();       // EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094
        void AccessViolation();    // EXCEPTION_ACCESS_VIOLATION 0xC0000005: a write to the address 0
        void AssertionFailure();   // kAssertionFailure 0xE0000003: MYENGINE_CHECK of a false condition
    }

    // The code of the exception of a failed MYENGINE_CHECK
    constexpr unsigned long kAssertionFailure = 0xE0000003;

    // A failed MYENGINE_CHECK: the condition, the file and the line go to the report ("assertion: ..."), then the exception kAssertionFailure is raised
    // here, so the crash handler writes the report with the context of this call (the stack goes on to the function with the check)
    [[noreturn]] void AssertionFailed(const char* condition, const char* file, int line);
}

// An assertion that stays in a release build (the standard assert() is off there: NDEBUG). A false condition crashes the game with a report.
#define MYENGINE_CHECK(condition) ((condition) ? static_cast<void>(0) : ::myengine::core::crash::AssertionFailed(#condition, __FILE__, __LINE__))
