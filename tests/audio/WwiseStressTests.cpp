// The stability of audio::AudioSystem on Wwise (W4, docs/measurements/wwise/stability.md): more than 10 000 events over several simulated minutes with the pool
// full and empty again, handles that are old or not ours, two LoadBank calls, banks that are empty or cut, 20 starts and stops in a row. No device and no
// window: Wwise renders offline, the time passes in Update(dt). The numbers it prints go to the document. Built only with MYENGINE_WWISE=ON.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <nlohmann/json.hpp>

#include <myengine/audio/AudioSystem.h>
#include <myengine/core/Logger.h>

#pragma comment(lib, "psapi.lib")

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;
    using Clock = std::chrono::steady_clock;

    int g_failures = 0;

    void Check(const bool condition, const std::string& text)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("  FAIL %s\n", text.c_str());
        }
    }

    double Milliseconds(const Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

    // the private bytes of the process (what it has committed for itself)
    std::uint64_t PrivateBytes()
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
        return counters.PrivateUsage;
    }

    double Megabytes(const std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

    struct EventInfo
    {
        std::string name;
        bool loop = false;
        bool spatial = false;
        std::string bus;
    };

    std::vector<EventInfo> ReadEvents(const fs::path& json)
    {
        nlohmann::json bank;
        std::ifstream stream(json);
        stream >> bank;
        std::vector<EventInfo> events;
        for (const auto& entry : bank["events"].items())
        {
            EventInfo info;
            info.name = entry.key();
            info.loop = entry.value().value("loop", false);
            info.spatial = entry.value().value("spatial", false);
            info.bus = entry.value().value("bus", "sfx");
            events.push_back(info);
        }
        return events;
    }

    void Settle(audio::AudioSystem& audio, const float seconds, const float step = 0.03f)
    {
        for (float t = 0.0f; t < seconds; t += step)
        {
            audio.Update(step);
        }
    }

    // every voice is over and every game object has gone (the loops are stopped by the caller)
    bool Drain(audio::AudioSystem& audio, const float limitSeconds = 12.0f)
    {
        float elapsed = 0.0f;
        while (elapsed < limitSeconds && (audio.GetStats().voicesActive > 0 || audio.GetStats().engineObjects > 0))
        {
            audio.Update(0.05f);
            elapsed += 0.05f;
        }
        return audio.GetStats().voicesActive == 0 && audio.GetStats().engineObjects == 0;
    }

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // a tree like the one of the project: <root>/assets/data/audio_bank.json, <root>/assets/audio/wwise/*.bnk, "directory" of the json is the absolute folder (so that
    // the search for the banks finds this tree and not the one next to the executable)
    fs::path MakeProjectCopy(const fs::path& root, const fs::path& source, const bool copyInit, const std::int64_t bankBytes /* -1: whole, 0: no bank */)
    {
        std::error_code error;
        fs::remove_all(root, error);
        fs::create_directories(root / "assets/data", error);
        fs::create_directories(root / "assets/audio/wwise", error);
        nlohmann::json bank;
        {
            std::ifstream stream(source / "assets/data/audio_bank.json");
            stream >> bank;
        }
        bank["directory"] = (root / "assets/audio").generic_string();
        std::ofstream(root / "assets/data/audio_bank.json") << bank.dump(2);
        if (copyInit)
        {
            fs::copy_file(source / "assets/audio/wwise/Init.bnk", root / "assets/audio/wwise/Init.bnk", error);
        }
        if (bankBytes != 0)
        {
            const fs::path from = source / "assets/audio/wwise/RocketArena.bnk";
            const fs::path to = root / "assets/audio/wwise/RocketArena.bnk";
            if (bankBytes < 0)
            {
                fs::copy_file(from, to, error);
            }
            else
            {
                std::ifstream in(from, std::ios::binary);
                std::vector<char> data(static_cast<std::size_t>(bankBytes));
                in.read(data.data(), static_cast<std::streamsize>(data.size()));
                std::ofstream(to, std::ios::binary).write(data.data(), in.gcount());
            }
        }
        return root / "assets/data/audio_bank.json";
    }

    audio::AudioConfig OfflineConfig()
    {
        audio::AudioConfig config;
        config.useDevice = false;
        return config;
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
    const fs::path folder = fs::temp_directory_path() / "myengine_wwise_stress_tests";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    core::Logger logger;
    const fs::path logFile = folder / "stress.log";
    logger.Initialize(logFile);
    const fs::path bankJson = source / "assets/data/audio_bank.json";
    const std::vector<EventInfo> events = ReadEvents(bankJson);
    const auto started = Clock::now();

    std::printf("  phase 1\n");
    // ---- 1. ten thousand events over several minutes, the pool full and empty again
    {
        audio::AudioSystem audio;
        audio.SetLogger(&logger);
        Check(audio.Initialize(OfflineConfig()) && audio.LoadBank(bankJson), "the audio system starts with the banks");
        std::mt19937 random(12345);
        const auto pick = [&](const bool loops) -> const EventInfo&
        {
            for (;;)
            {
                const EventInfo& event = events[random() % events.size()];
                if (event.loop == loops)
                {
                    return event;
                }
            }
        };
        const auto position = [&]() { return audio::Vec3{static_cast<float>(static_cast<int>(random() % 160) - 80), static_cast<float>(random() % 20), static_cast<float>(static_cast<int>(random() % 240) - 120)}; };

        // a warm-up: Wwise allocates what it needs for its voices on first use; the memory is compared after it
        std::vector<audio::VoiceHandle> live;
        for (int i = 0; i < 1500; ++i)
        {
            audio::EventParams params;
            params.hasPosition = (random() & 1u) != 0;
            params.position = position();
            const audio::VoiceHandle handle = audio.PlayEvent(pick(false).name, params);
            if (handle.IsValid() && i % 3 == 0)
            {
                live.push_back(handle);
            }
            if (i % 10 == 9)
            {
                audio.Update(0.03f);
            }
        }
        audio.StopAll(0.0f);
        Check(Drain(audio), "the warm-up drains: no voice, no game object");
        const std::uint64_t memoryBefore = audio.GetStats().engineMemoryBytes;
        const std::uint64_t processBefore = PrivateBytes();
        const audio::AudioStats before = audio.GetStats();
        std::printf("  stat: warm-up done, Wwise memory %.2f MB, process private bytes %.1f MB\n", Megabytes(memoryBefore), Megabytes(processBefore));

        std::uint64_t asked = 0;
        std::uint64_t accepted = 0;
        std::uint64_t memoryPeak = memoryBefore;
        std::uint32_t objectsPeak = 0;
        double playMilliseconds = 0.0;
        double updateMilliseconds = 0.0;
        double updateWorst = 0.0;
        live.clear();
        const int steps = 9000;   // 4.5 simulated minutes at 30 ms
        for (int step = 0; step < steps; ++step)
        {
            // events: one or two a step, a burst of sixty now and then (fills the pool), loops now and then
            int count = 1 + static_cast<int>(random() % 2);
            if (step % 400 == 399)
            {
                count = 60;
            }
            for (int i = 0; i < count; ++i)
            {
                const bool loop = (random() % 12) == 0;
                const EventInfo& event = pick(loop);
                audio::EventParams params;
                params.hasPosition = event.spatial && (random() % 3) != 0;
                params.position = position();
                params.velocity = {static_cast<float>(static_cast<int>(random() % 40) - 20), 0.0f, static_cast<float>(static_cast<int>(random() % 40) - 20)};
                params.volumeScale = 0.3f + static_cast<float>(random() % 100) / 100.0f;
                params.pitchScale = 0.6f + static_cast<float>(random() % 100) / 60.0f;
                const auto t0 = Clock::now();
                const audio::VoiceHandle handle = audio.PlayEvent(event.name, params);
                playMilliseconds += Milliseconds(Clock::now() - t0);
                ++asked;
                if (handle.IsValid())
                {
                    ++accepted;
                    live.push_back(handle);
                }
            }
            // what the game does to a voice: pitch, volume, position, a stop with and without a fade, a stop of a handle that has been stopped, a stop during a fade
            for (int i = 0; i < 3 && !live.empty(); ++i)
            {
                const std::size_t index = random() % live.size();
                const audio::VoiceHandle handle = live[index];
                switch (random() % 9)
                {
                    case 0: audio.SetVoicePitch(handle, 0.5f + static_cast<float>(random() % 100) / 40.0f); break;
                    case 1: audio.SetVoiceVolume(handle, static_cast<float>(random() % 120) / 100.0f); break;
                    case 2: audio.SetVoicePosition(handle, position(), {1.0f, 0.0f, 2.0f}); break;
                    case 3: audio.Stop(handle, 0.0f); audio.Stop(handle, 0.0f); break;                                  // twice
                    case 4: audio.Stop(handle, 0.2f); audio.Stop(handle, 0.05f); break;                                 // a stop during a fade
                    case 5: audio.Stop(handle, 0.15f); audio.SetVoiceVolume(handle, 1.0f); break;                       // a call during a fade
                    case 6: audio.SetBusVolume(static_cast<audio::Bus>(1 + random() % 3), static_cast<float>(random() % 100) / 100.0f); break;
                    default: break;
                }
            }
            // the loops that nobody stops are stopped when there are many
            if (live.size() > 300)
            {
                live.erase(live.begin(), live.begin() + 150);
            }
            const auto t1 = Clock::now();
            audio.Update(0.03f);
            const double update = Milliseconds(Clock::now() - t1);
            updateMilliseconds += update;
            updateWorst = std::max(updateWorst, update);
            if (step % 250 == 0)
            {
                const audio::AudioStats stats = audio.GetStats();
                Check(stats.voicesActive <= stats.voicesTotal, "never more voices than the pool has");
                memoryPeak = std::max(memoryPeak, stats.engineMemoryBytes);
                objectsPeak = std::max(objectsPeak, stats.engineObjects);
            }
        }
        // the loops and everything else stop; at the end nothing is left
        audio.SetBusVolume(audio::Bus::Sfx, 1.0f);
        audio.SetBusVolume(audio::Bus::Music, 1.0f);
        audio.SetBusVolume(audio::Bus::Ui, 1.0f);
        audio.StopAll(0.1f);
        Settle(audio, 0.3f);
        const bool drained = Drain(audio);
        const audio::AudioStats after = audio.GetStats();
        Check(asked >= 10000, "more than 10 000 events were asked for (" + std::to_string(asked) + ")");
        Check(accepted > asked / 2, "most of them were accepted (" + std::to_string(accepted) + " of " + std::to_string(asked) + ")");
        Check(after.played - before.played == accepted, "played counts what was accepted");
        Check(after.dropped + after.stolen > 0, "the pool was full: sounds were dropped or taken from weaker voices (dropped " + std::to_string(after.dropped) + ", stolen " + std::to_string(after.stolen) + ")");
        Check(after.voicesPeak == after.voicesTotal, "the pool filled up (peak " + std::to_string(after.voicesPeak) + " of " + std::to_string(after.voicesTotal) + ")");
        Check(drained && after.voicesActive == 0, "every voice ends and the pool is empty");
        Check(after.engineObjects == 0, "no game object is left registered (" + std::to_string(after.engineObjects) + ")");
        Check(after.engineMemoryBytes <= memoryBefore + 512 * 1024, "the memory of Wwise is where it was after the warm-up (" + std::to_string(Megabytes(after.engineMemoryBytes)) + " MB, was " + std::to_string(Megabytes(memoryBefore)) + " MB)");
        const std::uint64_t processAfter = PrivateBytes();
        Check(processAfter <= processBefore + 24ull * 1024 * 1024, "the process does not grow (" + std::to_string(Megabytes(processAfter)) + " MB, was " + std::to_string(Megabytes(processBefore)) + " MB)");
        std::printf("  stat: %llu events asked, %llu accepted, played %llu, dropped %llu, stolen %llu, peak %u voices of %u, objects peak %u\n", static_cast<unsigned long long>(asked),
                    static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(after.played), static_cast<unsigned long long>(after.dropped),
                    static_cast<unsigned long long>(after.stolen), after.voicesPeak, after.voicesTotal, objectsPeak);
        std::printf("  stat: Wwise memory before %.2f MB, peak %.2f MB, after %.2f MB; process private bytes %.1f -> %.1f MB\n", Megabytes(memoryBefore), Megabytes(memoryPeak),
                    Megabytes(after.engineMemoryBytes), Megabytes(processBefore), Megabytes(processAfter));
        std::printf("  stat: PlayEvent %.1f us on average (%llu calls), Update %.3f ms on average, %.3f ms at the worst (%d steps of 30 ms, %.1f s of sound)\n",
                    1000.0 * playMilliseconds / static_cast<double>(asked), static_cast<unsigned long long>(asked), updateMilliseconds / steps, updateWorst, steps, steps * 0.03);
    }

    std::printf("  phase 2\n");
    // ---- 2. handles that are old, that nobody gave, empty; calls in the wrong order
    {
        audio::AudioSystem audio;
        audio.SetLogger(&logger);
        Check(audio.Initialize(OfflineConfig()) && audio.LoadBank(bankJson), "the second scenario starts");
        const audio::VoiceHandle first = audio.PlayEvent("ui_click");
        Check(first.IsValid(), "a click plays");
        audio.Stop(first, 0.0f);
        audio.Stop(first, 0.0f);
        audio.Stop(first, 0.3f);
        audio.SetVoiceVolume(first, 2.0f);
        audio.SetVoicePitch(first, 0.0f);
        audio.SetVoicePosition(first, {1.0f, 2.0f, 3.0f});
        Check(!audio.IsPlaying(first), "a handle that was stopped is dead");
        const audio::VoiceHandle second = audio.PlayEvent("ui_click");   // the slot of the first one
        audio.Stop(first, 0.0f);
        Check(audio.IsPlaying(second) && !(first == second), "a handle that is old does not touch the voice that has its slot");
        for (const std::uint32_t value : {0u, 1u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu, 0x7FFF0001u, 0x00010040u, 0x0001FFFFu})
        {
            const audio::VoiceHandle odd{value};
            audio.Stop(odd, 0.1f);
            audio.SetVoiceVolume(odd, 1.0f);
            audio.SetVoicePosition(odd, {});
            Check(!audio.IsPlaying(odd) || odd == second, "a handle that nobody gave is not playing (" + std::to_string(value) + ")");
        }
        audio.Stop(second, 0.5f);
        audio.Stop(second, 0.5f);   // during the fade
        audio.SetVoiceVolume(second, 3.0f);
        Check(!audio.IsPlaying(second), "a voice that fades out is not playing");
        Settle(audio, 0.7f);
        Check(Drain(audio), "the faded voice is gone");
        const audio::VoiceHandle mine = audio.PlayEvent("boost");
        Check(mine.IsValid() && audio.IsPlaying(mine), "a voice plays before the stop");
        audio.StopAll(0.0f);
        // after the stop: nothing plays, nothing fails
        audio.Shutdown();
        audio.Stop(mine, 0.1f);
        audio.SetVoiceVolume(mine, 1.0f);
        audio.StopAll(0.0f);
        audio.StopBus(audio::Bus::Music, 0.0f);
        audio.Update(0.03f);
        Check(!audio.PlayEvent("ui_click").IsValid() && !audio.IsPlaying(mine), "after Shutdown nothing plays");
        audio.SetBusVolume(audio::Bus::Music, 0.5f);
        Check(audio.GetBusVolume(audio::Bus::Music) > 0.4f, "the settings of the buses are kept without the engine");
        // and the same system starts again
        Check(audio.Initialize(OfflineConfig()) && audio.LoadBank(bankJson) && audio.PlayEvent("ui_click").IsValid(), "after Shutdown the system starts again");
        audio.Shutdown();
    }

    std::printf("  phase 3\n");
    // ---- 3. LoadBank twice, a bank that is not there, a bank that is cut
    {
        audio::AudioSystem audio;
        audio.SetLogger(&logger);
        Check(audio.Initialize(OfflineConfig()), "a start");
        Check(audio.LoadBank(bankJson), "LoadBank");
        const audio::AudioStats once = audio.GetStats();
        Check(audio.LoadBank(bankJson), "LoadBank again");
        const audio::AudioStats twice = audio.GetStats();
        Check(twice.soundsLoaded == once.soundsLoaded && twice.soundBytes == once.soundBytes, "the banks are counted once (" + std::to_string(twice.soundsLoaded) + " banks)");
        Check(twice.engineMemoryBytes <= once.engineMemoryBytes + 64 * 1024, "and not loaded again (memory " + std::to_string(Megabytes(twice.engineMemoryBytes)) + " MB, was " + std::to_string(Megabytes(once.engineMemoryBytes)) + " MB)");
        Check(audio.HasEvent("ball_hit") && audio.PlayEvent("ball_hit").IsValid(), "the events work after the second call");
        const audio::VoiceHandle playing = audio.PlayEvent("engine");
        Check(audio.LoadBank(bankJson) && audio.IsPlaying(playing), "a LoadBank while a voice plays does not touch it");
        audio.StopAll(0.0f);
        Check(Drain(audio), "and everything ends");
        audio.Shutdown();

        // the offset of the chunk HIRC (the events and the structure) in the bank: a file cut there is whole by chunks and has no events
        std::int64_t withoutEvents = 0;
        {
            const std::string data = ReadAll(source / "assets/audio/wwise/RocketArena.bnk");
            std::size_t position = 0;
            while (position + 8 <= data.size())
            {
                std::uint32_t size = 0;
                std::memcpy(&size, data.data() + position + 4, sizeof(size));
                if (data.compare(position, 4, "HIRC") == 0)
                {
                    withoutEvents = static_cast<std::int64_t>(position);
                    break;
                }
                position += 8 + size;
            }
        }
        Check(withoutEvents > 0, "the chunk HIRC is found in the bank");

        struct Case
        {
            const char* name;
            bool init;
            std::int64_t bytes;
            bool loads;   // the bank is whole by its chunks: Wwise loads it (and has no events in it)
        };
        for (const Case& test : {Case{"an empty folder of banks", false, 0, false}, Case{"no Init bank", false, -1, false}, Case{"a bank that is cut in half", true, 3000000, false},
                                 Case{"a bank that is cut after the header", true, 200, false}, Case{"a bank of one byte", true, 1, false},
                                 Case{"no bank next to the Init", true, 0, false}, Case{"a bank without its events (cut before HIRC)", true, withoutEvents, true}})
        {
            const fs::path copy = MakeProjectCopy(folder / "broken", source, test.init, test.bytes);
            std::printf("  case: %s\n", test.name);
            audio::AudioSystem broken;
            broken.SetLogger(&logger);
            Check(broken.Initialize(OfflineConfig()), std::string("the system starts for ") + test.name);
            const bool loaded = broken.LoadBank(copy);
            const std::uint64_t dropped = broken.GetStats().dropped;
            if (!test.loads)
            {
                Check(!loaded, std::string("LoadBank says no for ") + test.name);
                Check(!broken.HasEvent("ball_hit") && !broken.PlayEvent("ball_hit").IsValid(), std::string("no event plays for ") + test.name);
            }
            else
            {
                // Wwise has taken the bank and does not know the events: it refuses to play them, the voice is not kept
                Check(loaded, std::string("LoadBank loads ") + test.name);
                Check(!broken.PlayEvent("ball_hit").IsValid() && broken.GetStats().dropped == dropped + 1, std::string("Wwise refuses its events, the sound is dropped: ") + test.name);
            }
            broken.SetBusVolume(audio::Bus::Music, 0.5f);
            broken.SetListener({});
            for (int i = 0; i < 20; ++i)
            {
                broken.Update(0.03f);
            }
            Check(broken.GetStats().voicesActive == 0 && broken.GetStats().engineObjects == 0, std::string("the game goes on without sound for ") + test.name);
            if (!test.loads)
            {
                // the real banks afterwards: the system recovers
                Check(broken.LoadBank(bankJson), std::string("the real banks load after ") + test.name);
                const audio::VoiceHandle click = broken.PlayEvent("ui_click");
                Check(click.IsValid() && Drain(broken), std::string("and play after ") + test.name);
            }
            broken.Shutdown();
        }
        std::error_code ignore;
        fs::remove_all(folder / "broken", ignore);
    }

    std::printf("  phase 4\n");
    // ---- 4. twenty starts and stops in a row: the process does not grow
    {
        std::uint64_t afterThird = 0;
        std::uint64_t afterLast = 0;
        std::uint64_t wwiseAfterFirst = 0;
        std::uint64_t wwiseWorst = 0;
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            audio::AudioSystem audio;
            audio.SetLogger(&logger);
            const bool ok = audio.Initialize(OfflineConfig()) && audio.LoadBank(bankJson);
            Check(ok, "cycle " + std::to_string(cycle) + " starts");
            if (!ok)
            {
                break;
            }
            for (int i = 0; i < 40; ++i)
            {
                audio.PlayEvent(events[static_cast<std::size_t>(i) % events.size()].name);
                audio.Update(0.03f);
            }
            Settle(audio, 0.5f);
            const std::uint64_t wwise = audio.GetStats().engineMemoryBytes;
            if (cycle == 0)
            {
                wwiseAfterFirst = wwise;
            }
            wwiseWorst = std::max(wwiseWorst, wwise);
            audio.StopAll(0.0f);
            audio.Shutdown();
            if (cycle == 2)
            {
                afterThird = PrivateBytes();
            }
            afterLast = PrivateBytes();
        }
        Check(afterLast <= afterThird + 12ull * 1024 * 1024, "twenty cycles do not grow the process (" + std::to_string(Megabytes(afterLast)) + " MB after the last, " + std::to_string(Megabytes(afterThird)) + " MB after the third)");
        Check(wwiseWorst <= wwiseAfterFirst + 1024 * 1024, "and not the memory of Wwise (" + std::to_string(Megabytes(wwiseWorst)) + " MB at the most, " + std::to_string(Megabytes(wwiseAfterFirst)) + " MB in the first cycle)");
        std::printf("  stat: 20 cycles: process private bytes %.1f MB after the third, %.1f MB after the last; Wwise memory %.2f MB in the first, %.2f MB at the most\n", Megabytes(afterThird),
                    Megabytes(afterLast), Megabytes(wwiseAfterFirst), Megabytes(wwiseWorst));
    }

    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("  stat: the test took %.1f s\n", seconds);
    const std::string log = ReadAll(logFile);
    Check(log.find("did not load") != std::string::npos || log.find("Wwise banks did not load") != std::string::npos, "the broken banks left errors in the log");
    std::printf(g_failures == 0 ? "audio_wwise_stress: all passed\n" : "audio_wwise_stress: FAILED\n");
    if (g_failures != 0)
    {
        std::printf("--- log (the last 6000 characters) ---\n%s\n", log.substr(log.size() > 6000 ? log.size() - 6000 : 0).c_str());
    }
    fs::remove_all(folder, error);
    return g_failures == 0 ? 0 : 1;
}
