// The start and the stop of the sound engine of Wwise (W1, docs/engine/wwise.md): Initialize without a device (the output "No_Output"), a few hundred audio frames,
// Shutdown, again and again; a folder of banks that does not exist; a bank that cannot be loaded; a second runtime while one runs. No window, no sound, no
// network (the link with the Wwise authoring listens on it: MYENGINE_WWISE_TEST_COMM=1 adds one cycle with it). Built only with MYENGINE_WWISE=ON.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <myengine/core/Logger.h>

#include "../../engine/src/audio/wwise/WwiseRuntime.h"

namespace
{
    namespace fs = std::filesystem;
    using myengine::audio::wwise::WwiseRuntime;

    int g_failures = 0;

    void Check(const bool condition, const char* text)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("  FAIL %s\n", text);
        }
    }

    void Frames(WwiseRuntime& runtime, const int count)
    {
        for (int i = 0; i < count; ++i)
        {
            runtime.RenderAudio();
            if (i % 20 == 19)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
}

int main()
{
    const fs::path folder = fs::temp_directory_path() / "myengine_wwise_runtime_tests";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder / "banks", error);

    myengine::core::Logger logger;
    const fs::path logFile = folder / "wwise.log";
    Check(logger.Initialize(logFile), "the log opens");

    {
        // nothing runs yet: the calls are harmless
        WwiseRuntime idle(&logger);
        idle.RenderAudio();
        Check(!idle.IsInitialized(), "a new runtime is not running");
        Check(!idle.LoadBank("Init"), "a bank cannot be loaded before the start");
        idle.Shutdown();
    }

    {
        // the cycle: start without a device, 200 audio frames, stop
        WwiseRuntime runtime(&logger);
        Check(runtime.Initialize(folder / "banks", false, false), "Initialize without a device");
        Check(runtime.IsInitialized(), "it runs");
        Frames(runtime, 200);
        Check(runtime.IsInitialized(), "it still runs after 200 frames");
        runtime.Shutdown();
        Check(!runtime.IsInitialized(), "Shutdown stops it");
        runtime.Shutdown();   // a second one is nothing
    }

    {
        // two cycles in a row, of the same runtime and of a new one
        WwiseRuntime runtime(&logger);
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            Check(runtime.Initialize(folder / "banks", false, false), "the cycle starts");
            Frames(runtime, 60);
            runtime.Shutdown();
            Check(!runtime.IsInitialized(), "and stops");
        }
        WwiseRuntime another(&logger);
        Check(another.Initialize(folder / "banks", false, false), "a new runtime after the old one stopped");
        Frames(another, 60);
        // (the destructor stops it)
    }

    {
        // the sound engine of Wwise is one for the process: a second runtime says no and leaves the first alone
        WwiseRuntime first(&logger);
        WwiseRuntime second(&logger);
        Check(first.Initialize(folder / "banks", false, false), "the first starts");
        Check(!second.Initialize(folder / "banks", false, false), "the second does not start while the first runs");
        Check(!second.IsInitialized(), "and is not running");
        Frames(first, 20);
        Check(first.IsInitialized(), "the first is not disturbed");
        first.Shutdown();
        Check(second.Initialize(folder / "banks", false, false), "the second starts when the first has stopped");
        second.Shutdown();
    }

    {
        // a folder of banks that does not exist, and banks that are not there: errors, not crashes
        WwiseRuntime runtime(&logger);
        Check(runtime.Initialize(folder / "no" / "such" / "folder", false, false), "a folder of banks that does not exist is not an error of the start");
        Frames(runtime, 20);
        Check(!runtime.LoadBank("NoSuchBank"), "a bank that does not exist does not load");
        Check(!runtime.LoadBank("Init"), "the Init bank of a folder that does not exist does not load");
        Check(!runtime.LoadBank(""), "an empty name does not load");
        Frames(runtime, 20);
        Check(runtime.IsInitialized(), "the engine runs after the failed loads");
        runtime.Shutdown();

        // an empty folder (the banks of the project are not made yet)
        Check(runtime.Initialize(folder / "banks", false, false), "an empty folder of banks");
        Check(!runtime.LoadBank("Init.bnk"), "no bank in it: the name with an extension too");
        runtime.Shutdown();
        Check(!runtime.LoadBank("Init"), "no loads after the stop");
    }

    char* comm = nullptr;
    std::size_t commLength = 0;
    const bool withComm = _dupenv_s(&comm, &commLength, "MYENGINE_WWISE_TEST_COMM") == 0 && comm != nullptr && comm[0] == '1';
    std::free(comm);
    if (withComm)
    {
        WwiseRuntime runtime(&logger);
        Check(runtime.Initialize(folder / "banks", false, true), "a start with the communication (Debug and Profile open a network port: Windows may ask)");
        Frames(runtime, 60);
        // (the Release libraries have no communication: false there)
        std::printf("  communication with the authoring: %s\n", runtime.CommunicationRunning() ? "up" : "not up");
        runtime.Shutdown();
    }

    const std::string log = ReadAll(logFile);
    Check(log.find("Wwise: started") != std::string::npos && log.find("Wwise: stopped") != std::string::npos, "the log has the start and the stop");
    Check(log.find("did not load") != std::string::npos, "and the failed bank loads");
    std::printf(g_failures == 0 ? "audio_wwise_runtime: all passed\n" : "audio_wwise_runtime: FAILED\n");
    if (g_failures != 0)
    {
        std::printf("--- log ---\n%s\n", log.c_str());
    }
    fs::remove_all(folder, error);
    return g_failures == 0 ? 0 : 1;
}
