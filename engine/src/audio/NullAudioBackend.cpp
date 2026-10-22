// NullAudioBackend.cpp
//
// No device: a voice plays for the length of its buffer (divided by the pitch) in the time that Update() hands out.

#include "AudioBackend.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace myengine::audio
{
    namespace
    {
        class NullVoice final : public IAudioVoice
        {
        public:
            void Start(const std::int16_t*, const std::uint64_t frames, const std::uint32_t sampleRate, const bool loop) override
            {
                remaining_ = loop ? std::numeric_limits<double>::infinity() : static_cast<double>(frames) / std::max(sampleRate, 1u);
                playing_ = frames > 0;
            }

            void Stop() override
            {
                playing_ = false;
                remaining_ = 0.0;
            }

            bool IsPlaying() override { return playing_; }
            void SetGain(const float left, const float right) override
            {
                left_ = left;
                right_ = right;
            }
            void SetPitch(const float ratio) override { pitch_ = std::max(ratio, 0.01f); }

            void Advance(const float deltaTime)
            {
                if (!playing_)
                {
                    return;
                }
                remaining_ -= static_cast<double>(deltaTime) * pitch_;
                if (remaining_ <= 0.0)
                {
                    playing_ = false;
                }
            }

        private:
            bool playing_ = false;
            double remaining_ = 0.0;
            float pitch_ = 1.0f;
            float left_ = 1.0f;
            float right_ = 1.0f;
        };

        class NullBackend final : public IAudioBackend
        {
        public:
            bool HasDevice() const override { return false; }
            std::string DeviceName() const override { return "none (simulated)"; }

            std::unique_ptr<IAudioVoice> CreateVoice(const std::uint16_t) override
            {
                auto voice = std::make_unique<NullVoice>();
                voices_.push_back(voice.get());
                return voice;
            }

            void Update(const float deltaTime) override
            {
                for (NullVoice* voice : voices_)
                {
                    voice->Advance(deltaTime);
                }
            }

        private:
            std::vector<NullVoice*> voices_; // the voices belong to the system that asked for them and go before the backend
        };
    }

    std::unique_ptr<IAudioBackend> CreateNullBackend()
    {
        return std::make_unique<NullBackend>();
    }
}
