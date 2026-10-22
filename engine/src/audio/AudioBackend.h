// AudioBackend.h
//
// The device behind AudioSystem: a voice that plays a buffer, and the thing that makes voices. Two implementations: XAudio2
// (the real one) and a null one that only counts time (no device, tests). Internal: nothing outside engine/src/audio includes it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace myengine::audio
{
    class IAudioVoice
    {
    public:
        virtual ~IAudioVoice() = default;

        // Starts the buffer from its beginning (the samples stay valid while the voice plays); loop: until Stop
        virtual void Start(const std::int16_t* samples, std::uint64_t frames, std::uint32_t sampleRate, bool loop) = 0;
        virtual void Stop() = 0;
        // true from Start until the buffer has played to its end (never for a loop)
        virtual bool IsPlaying() = 0;
        // The volume of the left and of the right ear (a mono voice is panned by them, a stereo one is attenuated)
        virtual void SetGain(float left, float right) = 0;
        // A frequency ratio against the recording
        virtual void SetPitch(float ratio) = 0;
    };

    class IAudioBackend
    {
    public:
        virtual ~IAudioBackend() = default;
        virtual bool HasDevice() const = 0;
        virtual std::string DeviceName() const = 0;
        // channels 1 or 2; nullptr when the device cannot make one more
        virtual std::unique_ptr<IAudioVoice> CreateVoice(std::uint16_t channels) = 0;
        virtual void Update(float deltaTime) = 0;
    };

    std::unique_ptr<IAudioBackend> CreateNullBackend();
    // nullptr and `error` when there is no XAudio2 or no device
    std::unique_ptr<IAudioBackend> CreateXAudio2Backend(std::string& error);
}
