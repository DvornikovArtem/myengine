// AudioTypes.h
//
// The types of the audio module (docs/engine/audio.md): no XAudio2 or Windows type is visible here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace myengine::audio
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // The buses: every voice belongs to one; its volume is multiplied by the bus volume and by the master volume
    enum class Bus : std::uint8_t
    {
        Master = 0,   // only SetBusVolume / GetBusVolume: a voice plays on Sfx, Music or Ui
        Sfx = 1,
        Music = 2,
        Ui = 3,
    };
    constexpr std::size_t kBusCount = 4;

    struct SoundHandle
    {
        std::uint32_t value = 0;
        bool IsValid() const { return value != 0; }
        bool operator==(const SoundHandle& other) const { return value == other.value; }
    };

    // A playing sound. The value says the slot of the pool and its generation: a handle of a voice that has finished (and whose
    // slot is used by another sound now) is not valid any more, the calls with it do nothing.
    struct VoiceHandle
    {
        std::uint32_t value = 0;
        bool IsValid() const { return value != 0; }
        bool operator==(const VoiceHandle& other) const { return value == other.value; }
    };

    // Decoded sound: PCM 16 bit, interleaved
    struct WavData
    {
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::vector<std::int16_t> samples;

        std::uint64_t Frames() const { return channels == 0 ? 0 : samples.size() / channels; }
        double Seconds() const { return sampleRate == 0 ? 0.0 : static_cast<double>(Frames()) / sampleRate; }
    };

    struct SoundInfo
    {
        std::string name;                 // the path it was loaded from, or the name it was created with
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        double seconds = 0.0;
        std::uint64_t bytes = 0;
    };

    struct ListenerState
    {
        Vec3 position{};
        Vec3 forward{0.0f, 0.0f, 1.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
        Vec3 velocity{};                  // for the Doppler effect
    };

    // How a sound is played
    struct PlayParams
    {
        Bus bus = Bus::Sfx;
        float volume = 1.0f;              // 0..: before the buses
        float pitch = 1.0f;               // 0.25..4: a frequency ratio (1 = as recorded)
        bool loop = false;
        std::int32_t priority = 0;        // a full pool gives the voice of the lowest priority (then the quietest) to a sound of a higher or equal one
        // 3D: the volume falls with the distance (full up to minDistance, nothing at maxDistance), the sound is panned by the side
        // it comes from, and the pitch follows the Doppler effect (the velocities)
        bool spatial = false;
        Vec3 position{};
        Vec3 velocity{};
        float minDistance = 2.0f;
        float maxDistance = 80.0f;
        float rolloff = 1.0f;             // 1: the volume is minDistance / distance
        float dopplerFactor = 1.0f;       // 0: off
    };

    // An event of the sound bank (assets/data/audio_bank.json): a name for the game code, the variations and the settings behind it
    struct EventParams
    {
        bool hasPosition = false;         // false: not spatial (even when the event says it is)
        Vec3 position{};
        Vec3 velocity{};
        float volumeScale = 1.0f;         // e.g. the strength of a ball hit
        float pitchScale = 1.0f;          // e.g. the revolutions of the engine
    };

    struct AudioConfig
    {
        bool useDevice = true;            // false: no device at all (tests, machines without sound): voices are simulated in time only
        std::uint32_t monoVoices = 48;
        std::uint32_t stereoVoices = 16;
        float masterVolume = 1.0f;
        float dopplerFactor = 1.0f;
        float speedOfSound = 343.0f;
        std::uint32_t randomSeed = 12345; // the choice of the variations and the random pitch
    };

    struct AudioStats
    {
        bool deviceAvailable = false;
        std::string deviceName;
        std::uint32_t voicesTotal = 0;
        std::uint32_t voicesActive = 0;
        std::uint32_t voicesPeak = 0;      // the most that were active at once
        std::uint64_t played = 0;          // sounds started
        std::uint64_t stolen = 0;          // voices taken from a playing sound
        std::uint64_t dropped = 0;         // sounds not played: no free voice, no weaker one to take
        std::uint32_t soundsLoaded = 0;
        std::uint64_t soundBytes = 0;
        // What the sound engine behind holds (Wwise only; 0 with XAudio2 and in the Release libraries of Wwise, which do not count): the memory of its memory
        // manager, and the game objects that are registered for voices (the playing ones and the stopped ones that wait for the end of their event). A leak shows
        // here: after every voice has ended both go back to what they were.
        std::uint64_t engineMemoryBytes = 0;
        std::uint32_t engineObjects = 0;
    };

    // The result of the 3D calculation (a pure function: tested without a device)
    struct SpatialResult
    {
        float gain = 1.0f;                // distance attenuation, 0..1
        float pan = 0.0f;                 // -1 left .. +1 right (in the frame of the listener)
        float doppler = 1.0f;             // the factor of the pitch
        float distance = 0.0f;
    };
}
