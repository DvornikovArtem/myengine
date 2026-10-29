// WwiseRuntime.h
//
// The sound engine of Wwise (docs/engine/wwise.md) started and stopped in the order that the SDK asks for, and nothing else: no events, no game objects, no link
// with AudioSystem (that is a later step). Internal: nothing outside engine/src/audio includes it, and no type of the SDK is in this header. Built only with
// MYENGINE_WWISE=ON (the engine then defines MYENGINE_WWISE=1).
//
// Order of Initialize: Memory Manager, Stream Manager, a streaming device with the low-level IO of the samples of the SDK (reads the banks), the job workers,
// the Sound Engine (the music engine of the older versions is inside it since 2022), the communication with the authoring (Debug and Profile only; a failure
// there is a message, not an error). Shutdown goes the other way round. Errors of the SDK go to the log (core::Logger), there are no exceptions.
//
// Wwise has one sound engine in a process (its state is global): a second Initialize while one is running returns false.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace myengine::core
{
    class Logger;
}

namespace myengine::audio::wwise
{
    class WwiseRuntime
    {
    public:
        // `logger` may be null (nothing is logged) and has to outlive the runtime
        explicit WwiseRuntime(core::Logger* logger = nullptr);
        ~WwiseRuntime();   // Shutdown()

        WwiseRuntime(const WwiseRuntime&) = delete;
        WwiseRuntime& operator=(const WwiseRuntime&) = delete;

        // `bankDirectory`: where the files of the banks are (the folder GeneratedSoundBanks/Windows of the Wwise project); a folder that does not exist is not
        // an error here (the first LoadBank says so; SetBankDirectory can say it later). `withDevice` false: nothing is played and the engine has no clock of
        // its own: it OFFLINE-renders (AkInitSettings::bOfflineRendering, no engine thread, the "No_Output" device), and the audio time passes only inside
        // RenderAudio(seconds): tests and MYENGINE_AUDIO=0 do not depend on the real time, and a one-shot ends after as much Update(dt) as it lasts.
        // `withCommunication`: the link with the Wwise authoring (Profiler, Remote Connect), only in Debug and
        // Profile; it listens on the network, so the tests leave it off. false when something did not start; what did start is stopped again.
        bool Initialize(const std::filesystem::path& bankDirectory, bool withDevice, bool withCommunication = true);
        void Shutdown();
        bool IsInitialized() const { return initialized_; }

        // One audio frame of the sound engine: the events and the calls that were queued are processed. With a device the engine thread is Wwise's own and
        // `seconds` is ignored. Offline (no device): `seconds` of audio are rendered by this call (0: one audio buffer).
        void RenderAudio(float seconds = 0.0f);
        // Without a device the time is the one of RenderAudio
        bool IsOffline() const { return offline_; }

        // The memory that Wwise holds now (its memory manager; 0 in the Release libraries, which do not count) and whether the link with the authoring is up
        std::uint64_t MemoryUsed() const;
        bool CommunicationRunning() const;

        // Another folder of banks (the ones before stay searched; the newest is searched first). false when the runtime does not run.
        bool SetBankDirectory(const std::filesystem::path& bankDirectory);

        // A bank of the folder by its name ("Init", "Main"; with or without .bnk). false (and a line in the log) when it cannot be loaded.
        bool LoadBank(const std::string& name);

    private:
        struct Impl;

        core::Logger* logger_ = nullptr;
        std::unique_ptr<Impl> impl_;
        bool initialized_ = false;
        bool offline_ = false;
    };
}
