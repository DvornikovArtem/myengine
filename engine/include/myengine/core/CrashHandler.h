// CrashHandler.h
// A crash of the game leaves a file the player can send: a minidump (.dmp) and a short text next to it (.txt: version, the
// exception, the last lines of the log). They go to <UserData>\crashes (%LOCALAPPDATA%\RocketArena\crashes in a shipping build).
// The next start finds the marker the handler left and the game says so (docs/game/packaging.md, "Падения").

#pragma once

#include <filesystem>
#include <optional>
#include <string>

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
    // `crashDirectory` is created at the crash; `logFile` is read for its last lines.
    void Install(const std::filesystem::path& crashDirectory, const std::filesystem::path& logFile);

    // The crash of the previous run if it has not been reported yet (the marker is removed: it is told once)
    std::optional<Report> TakePending(const std::filesystem::path& crashDirectory);

    // A library may take the handlers over after Install (SteamAPI_Init installs its own breakpad): called every frame by the screens of the game,
    // it puts ours back as soon as another one is found (one atomic exchange when nothing changed)
    void Reassert();

    // MYENGINE_CRASH_TEST=1 in the environment: crashes right here, on purpose (the check of the handler, tools/check_package.ps1);
    // MYENGINE_CRASH_TEST=2: crashes 10 s after the first Reassert, when the game runs and Steam has started (the check that Steam did not take the handler)
    void CrashIfRequested();
}
