// XAudio2Backend.cpp
//
// The real device: XAudio2 2.9 (Windows 10+, no external DLL). One mastering voice; the source voices are made up front by the
// AudioSystem (the pool) in the format the sounds have, 44.1 kHz 16 bit mono or stereo; other sample rates play through the
// frequency ratio. Panning and volume go through the output matrix of the voice (to the channels of the device).

#include "AudioBackend.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <xaudio2.h>

namespace myengine::audio
{
    namespace
    {
        constexpr std::uint32_t kVoiceSampleRate = 44100;
        constexpr float kMaxFrequencyRatio = 4.0f;

        class XAudio2Voice final : public IAudioVoice
        {
        public:
            XAudio2Voice(IXAudio2SourceVoice* voice, const std::uint16_t channels, const std::uint32_t deviceChannels)
                : voice_(voice)
                , channels_(channels)
                , deviceChannels_(std::max(deviceChannels, 2u))
            {
            }

            ~XAudio2Voice() override
            {
                if (voice_ != nullptr)
                {
                    voice_->DestroyVoice();
                }
            }

            void Start(const std::int16_t* samples, const std::uint64_t frames, const std::uint32_t sampleRate, const bool loop) override
            {
                voice_->Stop();
                voice_->FlushSourceBuffers();
                rateRatio_ = static_cast<float>(sampleRate) / static_cast<float>(kVoiceSampleRate);
                voice_->SetFrequencyRatio(std::clamp(rateRatio_ * pitch_, XAUDIO2_MIN_FREQ_RATIO, kMaxFrequencyRatio));

                XAUDIO2_BUFFER buffer{};
                buffer.AudioBytes = static_cast<UINT32>(frames * channels_ * sizeof(std::int16_t));
                buffer.pAudioData = reinterpret_cast<const BYTE*>(samples);
                buffer.Flags = XAUDIO2_END_OF_STREAM;
                buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
                if (SUCCEEDED(voice_->SubmitSourceBuffer(&buffer)))
                {
                    voice_->Start(0);
                    started_ = true;
                }
            }

            void Stop() override
            {
                voice_->Stop();
                voice_->FlushSourceBuffers();
                started_ = false;
            }

            bool IsPlaying() override
            {
                if (!started_)
                {
                    return false;
                }
                XAUDIO2_VOICE_STATE state{};
                voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                return state.BuffersQueued > 0;
            }

            void SetGain(const float left, const float right) override
            {
                std::vector<float> matrix(static_cast<std::size_t>(channels_) * deviceChannels_, 0.0f);
                if (channels_ == 1)
                {
                    matrix[0] = left;
                    matrix[1] = right;
                }
                else
                {
                    matrix[0] = left;                    // source channel 0 -> device left
                    matrix[deviceChannels_ + 1] = right; // source channel 1 -> device right
                }
                voice_->SetOutputMatrix(nullptr, channels_, deviceChannels_, matrix.data());
            }

            void SetPitch(const float ratio) override
            {
                pitch_ = std::max(ratio, 0.01f);
                voice_->SetFrequencyRatio(std::clamp(rateRatio_ * pitch_, XAUDIO2_MIN_FREQ_RATIO, kMaxFrequencyRatio));
            }

        private:
            IXAudio2SourceVoice* voice_ = nullptr;
            std::uint16_t channels_ = 1;
            std::uint32_t deviceChannels_ = 2;
            float rateRatio_ = 1.0f;
            float pitch_ = 1.0f;
            bool started_ = false;
        };

        class XAudio2Backend final : public IAudioBackend
        {
        public:
            ~XAudio2Backend() override
            {
                if (master_ != nullptr)
                {
                    master_->DestroyVoice();
                }
                engine_.Reset();
                if (comInitialized_)
                {
                    CoUninitialize();
                }
            }

            bool Initialize(std::string& error)
            {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                comInitialized_ = SUCCEEDED(com); // RPC_E_CHANGED_MODE: COM is there already, in another mode: fine, not ours to uninitialize
                HRESULT hr = XAudio2Create(engine_.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
                if (FAILED(hr))
                {
                    error = "XAudio2Create failed (0x" + Hex(hr) + ")";
                    return false;
                }
                hr = engine_->CreateMasteringVoice(&master_);
                if (FAILED(hr) || master_ == nullptr)
                {
                    error = "no audio device: CreateMasteringVoice failed (0x" + Hex(hr) + ")";
                    return false;
                }
                XAUDIO2_VOICE_DETAILS details{};
                master_->GetVoiceDetails(&details);
                deviceChannels_ = details.InputChannels;
                deviceName_ = "default output, " + std::to_string(deviceChannels_) + " channels, " + std::to_string(details.InputSampleRate) + " Hz";
                return true;
            }

            bool HasDevice() const override { return true; }
            std::string DeviceName() const override { return deviceName_; }

            std::unique_ptr<IAudioVoice> CreateVoice(const std::uint16_t channels) override
            {
                WAVEFORMATEX format{};
                format.wFormatTag = WAVE_FORMAT_PCM;
                format.nChannels = channels;
                format.nSamplesPerSec = kVoiceSampleRate;
                format.wBitsPerSample = 16;
                format.nBlockAlign = static_cast<WORD>(channels * 2);
                format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
                IXAudio2SourceVoice* voice = nullptr;
                if (FAILED(engine_->CreateSourceVoice(&voice, &format, 0, kMaxFrequencyRatio)) || voice == nullptr)
                {
                    return nullptr;
                }
                return std::make_unique<XAudio2Voice>(voice, channels, deviceChannels_);
            }

            void Update(float) override {}

        private:
            static std::string Hex(const HRESULT hr)
            {
                char text[16];
                std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(hr));
                return text;
            }

            Microsoft::WRL::ComPtr<IXAudio2> engine_;
            IXAudio2MasteringVoice* master_ = nullptr;
            bool comInitialized_ = false;
            std::uint32_t deviceChannels_ = 2;
            std::string deviceName_;
        };
    }

    std::unique_ptr<IAudioBackend> CreateXAudio2Backend(std::string& error)
    {
        auto backend = std::make_unique<XAudio2Backend>();
        if (!backend->Initialize(error))
        {
            return nullptr;
        }
        return backend;
    }
}
