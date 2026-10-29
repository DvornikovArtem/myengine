// AudioSystem.h
//
// Sound on XAudio2 (docs/engine/audio.md): buses, a pool of voices, 3D, loops with pitch, events from a sound bank. No XAudio2
// or Windows type is visible here. Without a sound device (or with AudioConfig::useDevice = false) everything works the same:
// the voices are simulated in time, so the game and the tests run on a machine without sound.
//
// With MYENGINE_WWISE=ON (cmake/Wwise.cmake) the same class is implemented on Wwise instead (engine/src/audio/wwise/WwiseAudioSystem.cpp, docs/engine/wwise.md):
// the events of the bank are the events of the Wwise project, the volume and the pitch of a voice are its game parameters. The calls that have no meaning for
// Wwise (LoadSound, CreateSound, UnloadSound, GetSoundInfo, Play, GetVoiceGain) do nothing there and say so in the log. The header is the same in both builds.
//
// One thread: the game thread calls everything, Update(dt) once per frame.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <myengine/audio/AudioTypes.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::audio
{
    class IAudioBackend;
    class IAudioVoice;

    class AudioSystem
    {
    public:
        AudioSystem();
        ~AudioSystem();
        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        // The messages (device, loaded sounds, errors) go to the logger when there is one
        void SetLogger(core::Logger* logger) { logger_ = logger; }

        // Always succeeds (without a device the system is a simulation); HasDevice() tells which one it is
        bool Initialize(const AudioConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return initialized_; }
        bool HasDevice() const;

        // ---- sounds ----
        // WAV from a file (cached by path); an invalid handle when it cannot be read
        SoundHandle LoadSound(const std::filesystem::path& path);
        // A sound made in memory (tests, procedural sounds); `name` identifies it
        SoundHandle CreateSound(const std::string& name, const WavData& data);
        // Stops the voices that play it first. The handle is invalid afterwards.
        void UnloadSound(SoundHandle sound);
        bool GetSoundInfo(SoundHandle sound, SoundInfo& out) const;

        // ---- the sound bank: events ----
        // assets/data/audio_bank.json: { "directory": "assets/audio", "events": { "ball_hit": { "files": [...], "bus": "sfx", ... } } }.
        // Loads every file of the events. false: the file cannot be read; an event with a missing sound is skipped with a warning.
        bool LoadBank(const std::filesystem::path& bankFile);
        bool HasEvent(const std::string& name) const;
        // A random variation of the event, with its random pitch and volume, its bus, 3D settings and instance limit
        VoiceHandle PlayEvent(const std::string& name, const EventParams& params = {});

        // ---- voices ----
        VoiceHandle Play(SoundHandle sound, const PlayParams& params = {});
        // fadeSeconds > 0: the volume goes to 0 first
        void Stop(VoiceHandle voice, float fadeSeconds = 0.0f);
        void StopAll(float fadeSeconds = 0.0f);
        void StopBus(Bus bus, float fadeSeconds = 0.0f);
        bool IsPlaying(VoiceHandle voice) const;
        // On the fly: the engine loop follows the revolutions with the pitch, the boost with the volume, a moving source with the position
        void SetVoiceVolume(VoiceHandle voice, float volume);
        void SetVoicePitch(VoiceHandle voice, float pitch);
        void SetVoicePosition(VoiceHandle voice, const Vec3& position, const Vec3& velocity = {});

        // ---- buses ----
        // Master multiplies everything. 0..1 and above (up to 4)
        void SetBusVolume(Bus bus, float volume);
        float GetBusVolume(Bus bus) const;
        void SetBusMuted(Bus bus, bool muted);
        bool IsBusMuted(Bus bus) const;
        // The volume a voice has now: its own, the fade, the 3D, its bus and the master (0 for a handle that is not valid)
        float GetVoiceGain(VoiceHandle voice) const;

        // ---- 3D ----
        void SetListener(const ListenerState& listener) { listener_ = listener; }
        const ListenerState& GetListener() const { return listener_; }

        // Once per frame: the finished voices go back to the pool, fades and 3D are applied
        void Update(float deltaTime);

        AudioStats GetStats() const;
        std::uint32_t ActiveVoiceCount() const;
        std::uint32_t VoiceCount() const;

    private:
        struct Sound
        {
            SoundInfo info;
            WavData data;
            bool alive = false;
        };

        struct Voice
        {
            std::unique_ptr<IAudioVoice> impl;
            std::uint16_t channels = 1;
            std::uint32_t generation = 1;
            bool active = false;
            std::uint32_t sound = 0;           // index of the sound in sounds_
            PlayParams params;
            float fade = 1.0f;
            float fadeRate = 0.0f;             // per second, > 0 while fading out
            bool fadingOut = false;
            std::uint64_t order = 0;           // the start order (the oldest is stolen first among the equal)
            std::int32_t eventIndex = -1;
            float lastGain = -1.0f;
            float lastPitch = -1.0f;
            float lastPan = 2.0f;
        };

        struct Event
        {
            std::string name;
            std::vector<SoundHandle> sounds;
            PlayParams base;
            float pitchMin = 1.0f;
            float pitchMax = 1.0f;
            float volumeMin = 1.0f;
            float volumeMax = 1.0f;
            std::uint32_t maxInstances = 0;    // 0: unlimited
            std::uint32_t lastVariation = 0xFFFFFFFFu;
        };

        VoiceHandle StartVoice(SoundHandle sound, const PlayParams& params, std::int32_t eventIndex);
        Voice* ResolveVoice(VoiceHandle handle);
        const Voice* ResolveVoice(VoiceHandle handle) const;
        VoiceHandle MakeHandle(std::size_t index) const;
        float ComputeGain(const Voice& voice, SpatialResult* outSpatial) const;
        void ApplyVoice(Voice& voice, bool force);
        void ReleaseVoice(Voice& voice);
        std::uint32_t NextRandom();
        float RandomRange(float low, float high);
        void Log(int level, const std::string& message) const;

        core::Logger* logger_ = nullptr;
        bool initialized_ = false;
        AudioConfig config_;
        std::unique_ptr<IAudioBackend> backend_;
        std::vector<Voice> voices_;
        std::vector<Sound> sounds_;            // index 0 is not used: handles start at 1
        std::unordered_map<std::string, std::uint32_t> soundsByName_;
        std::vector<Event> events_;
        std::unordered_map<std::string, std::uint32_t> eventsByName_;
        std::array<float, kBusCount> busVolume_{1.0f, 1.0f, 1.0f, 1.0f};
        std::array<bool, kBusCount> busMuted_{false, false, false, false};
        ListenerState listener_;
        std::uint64_t startCounter_ = 0;
        std::uint32_t randomState_ = 1;
        AudioStats stats_;

        // The state of the Wwise implementation (defined in WwiseAudioSystem.cpp; empty, and unused, in the XAudio2 build): the same layout in both builds
        struct WwiseState;
        std::unique_ptr<WwiseState> wwise_;
    };

    // The audio system of the process: Application initializes it (device, sound bank), the scene systems and the game play through it
    AudioSystem& GetAudioSystem();
}
