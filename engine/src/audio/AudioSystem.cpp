// AudioSystem.cpp

#include <myengine/audio/AudioSystem.h>

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include <myengine/audio/AudioSpatial.h>
#include <myengine/audio/WavFile.h>
#include <myengine/core/Logger.h>

#include "AudioBackend.h"

#include <tracy/Tracy.hpp>

namespace myengine::audio
{
    namespace
    {
        constexpr float kMinPitch = 0.05f;
        constexpr float kMaxPitch = 4.0f;

        std::string KeyOf(const std::filesystem::path& path)
        {
            return path.lexically_normal().generic_string();
        }

        Bus BusFromName(const std::string& name, const Bus fallback)
        {
            if (name == "sfx") return Bus::Sfx;
            if (name == "music") return Bus::Music;
            if (name == "ui") return Bus::Ui;
            return fallback;
        }
    }

    AudioSystem& GetAudioSystem()
    {
        static AudioSystem instance;
        return instance;
    }

    AudioSystem::AudioSystem() = default;

    AudioSystem::~AudioSystem()
    {
        Shutdown();
    }

    void AudioSystem::Log(const int level, const std::string& message) const
    {
        if (logger_ == nullptr)
        {
            return;
        }
        if (level >= 2)
        {
            logger_->Error(message);
        }
        else if (level == 1)
        {
            logger_->Warning(message);
        }
        else
        {
            logger_->Info(message);
        }
    }

    bool AudioSystem::Initialize(const AudioConfig& config)
    {
        if (initialized_)
        {
            return true;
        }
        config_ = config;
        randomState_ = config.randomSeed | 1u;
        stats_ = {};

        if (config.useDevice)
        {
            std::string error;
            backend_ = CreateXAudio2Backend(error);
            if (backend_ == nullptr)
            {
                Log(1, "Audio: no sound device (" + error + "): the sounds are simulated, nothing is heard");
            }
        }
        if (backend_ == nullptr)
        {
            backend_ = CreateNullBackend();
        }

        // The pool: the voices are made once, in the two formats of the sounds
        const auto makeVoices = [this](const std::uint32_t count, const std::uint16_t channels)
        {
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto impl = backend_->CreateVoice(channels);
                if (impl == nullptr)
                {
                    break;
                }
                Voice voice;
                voice.impl = std::move(impl);
                voice.channels = channels;
                voices_.push_back(std::move(voice));
            }
        };
        makeVoices(std::min<std::uint32_t>(config.monoVoices, 1024), 1);
        makeVoices(std::min<std::uint32_t>(config.stereoVoices, 256), 2);

        sounds_.assign(1, Sound{}); // index 0: "no sound"
        busVolume_ = {std::clamp(config.masterVolume, 0.0f, 4.0f), 1.0f, 1.0f, 1.0f};
        busMuted_ = {false, false, false, false};
        initialized_ = true;
        stats_.deviceAvailable = backend_->HasDevice();
        stats_.deviceName = backend_->DeviceName();
        stats_.voicesTotal = static_cast<std::uint32_t>(voices_.size());
        Log(0, "Audio: " + std::string(backend_->HasDevice() ? "device " : "simulated, ") + backend_->DeviceName() + ", " + std::to_string(voices_.size()) + " voices");
        return true;
    }

    void AudioSystem::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }
        for (Voice& voice : voices_)
        {
            if (voice.active)
            {
                ReleaseVoice(voice);
            }
        }
        voices_.clear();   // the voices go before their device
        backend_.reset();
        sounds_.clear();
        soundsByName_.clear();
        events_.clear();
        eventsByName_.clear();
        initialized_ = false;
    }

    bool AudioSystem::HasDevice() const
    {
        return initialized_ && backend_ != nullptr && backend_->HasDevice();
    }

    // --------------------------------------------------------------------------- sounds

    SoundHandle AudioSystem::LoadSound(const std::filesystem::path& path)
    {
        if (!initialized_)
        {
            return {};
        }
        const std::string key = KeyOf(path);
        if (const auto found = soundsByName_.find(key); found != soundsByName_.end() && sounds_[found->second].alive)
        {
            return SoundHandle{found->second};
        }
        WavData data;
        std::string error;
        if (!LoadWavFile(path, data, error))
        {
            Log(1, "Audio: cannot load " + path.generic_string() + ": " + error);
            return {};
        }
        return CreateSound(key, data);
    }

    SoundHandle AudioSystem::CreateSound(const std::string& name, const WavData& data)
    {
        if (!initialized_ || data.channels < 1 || data.channels > 2 || data.sampleRate == 0 || data.samples.empty())
        {
            return {};
        }
        if (const auto found = soundsByName_.find(name); found != soundsByName_.end() && sounds_[found->second].alive)
        {
            return SoundHandle{found->second};
        }
        Sound sound;
        sound.data = data;
        sound.alive = true;
        sound.info.name = name;
        sound.info.sampleRate = data.sampleRate;
        sound.info.channels = data.channels;
        sound.info.seconds = data.Seconds();
        sound.info.bytes = data.samples.size() * sizeof(std::int16_t);
        sounds_.push_back(std::move(sound));
        const std::uint32_t index = static_cast<std::uint32_t>(sounds_.size() - 1);
        soundsByName_[name] = index;
        ++stats_.soundsLoaded;
        stats_.soundBytes += sounds_[index].info.bytes;
        return SoundHandle{index};
    }

    void AudioSystem::UnloadSound(const SoundHandle handle)
    {
        if (!initialized_ || handle.value == 0 || handle.value >= sounds_.size() || !sounds_[handle.value].alive)
        {
            return;
        }
        for (Voice& voice : voices_)
        {
            if (voice.active && voice.sound == handle.value)
            {
                ReleaseVoice(voice);
            }
        }
        Sound& sound = sounds_[handle.value];
        soundsByName_.erase(sound.info.name);
        stats_.soundBytes -= std::min<std::uint64_t>(stats_.soundBytes, sound.info.bytes);
        if (stats_.soundsLoaded > 0)
        {
            --stats_.soundsLoaded;
        }
        sound = Sound{};
    }

    bool AudioSystem::GetSoundInfo(const SoundHandle handle, SoundInfo& out) const
    {
        if (handle.value == 0 || handle.value >= sounds_.size() || !sounds_[handle.value].alive)
        {
            return false;
        }
        out = sounds_[handle.value].info;
        return true;
    }

    // --------------------------------------------------------------------------- the bank

    bool AudioSystem::LoadBank(const std::filesystem::path& bankFile)
    {
        if (!initialized_)
        {
            return false;
        }
        nlohmann::json bank;
        {
            std::ifstream stream(bankFile);
            if (!stream.is_open())
            {
                Log(1, "Audio: cannot open the sound bank " + bankFile.generic_string());
                return false;
            }
            try
            {
                stream >> bank;
            }
            catch (const std::exception& exception)
            {
                Log(1, "Audio: the sound bank " + bankFile.generic_string() + " does not parse: " + exception.what());
                return false;
            }
        }

        // The folder of the sounds: as given (from the working directory), or from the project root above assets/data
        const std::string directoryText = bank.value("directory", std::string("assets/audio"));
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(std::filesystem::path(directoryText));
        std::filesystem::path root = bankFile.parent_path();
        for (int i = 0; i < 3; ++i)
        {
            candidates.push_back(root / directoryText);
            root = root.parent_path();
        }
        std::filesystem::path directory = candidates.front();
        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec))
            {
                directory = candidate;
                break;
            }
        }

        const auto eventsIt = bank.find("events");
        if (eventsIt == bank.end() || !eventsIt->is_object())
        {
            Log(1, "Audio: the sound bank has no \"events\" object");
            return false;
        }
        std::uint32_t loaded = 0;
        for (const auto& entry : eventsIt->items())
        {
            const nlohmann::json& body = entry.value();
            Event event;
            event.name = entry.key();
            std::vector<std::string> files;
            if (body.contains("files") && body["files"].is_array())
            {
                for (const auto& file : body["files"])
                {
                    files.push_back(file.get<std::string>());
                }
            }
            else if (body.contains("file"))
            {
                files.push_back(body["file"].get<std::string>());
            }
            for (const std::string& file : files)
            {
                const SoundHandle sound = LoadSound(directory / file);
                if (sound.IsValid())
                {
                    event.sounds.push_back(sound);
                }
            }
            if (event.sounds.empty())
            {
                Log(1, "Audio: the event '" + event.name + "' has no sound that loads: skipped");
                continue;
            }
            event.base.bus = BusFromName(body.value("bus", std::string("sfx")), Bus::Sfx);
            event.base.volume = body.value("volume", 1.0f);
            event.base.pitch = body.value("pitch", 1.0f);
            event.base.loop = body.value("loop", false);
            event.base.priority = body.value("priority", 0);
            event.base.spatial = body.value("spatial", false);
            event.base.minDistance = body.value("minDistance", 2.0f);
            event.base.maxDistance = body.value("maxDistance", 80.0f);
            event.base.rolloff = body.value("rolloff", 1.0f);
            event.base.dopplerFactor = body.value("doppler", 1.0f);
            event.pitchMin = body.value("pitchMin", 1.0f);
            event.pitchMax = body.value("pitchMax", event.pitchMin);
            event.volumeMin = body.value("volumeMin", 1.0f);
            event.volumeMax = body.value("volumeMax", event.volumeMin);
            event.maxInstances = body.value("maxInstances", 0u);
            eventsByName_[event.name] = static_cast<std::uint32_t>(events_.size());
            events_.push_back(std::move(event));
            ++loaded;
        }
        Log(0, "Audio: sound bank " + bankFile.generic_string() + ": " + std::to_string(loaded) + " events, " + std::to_string(stats_.soundsLoaded) + " sounds");
        return true;
    }

    bool AudioSystem::HasEvent(const std::string& name) const
    {
        return eventsByName_.find(name) != eventsByName_.end();
    }

    VoiceHandle AudioSystem::PlayEvent(const std::string& name, const EventParams& params)
    {
        const auto found = eventsByName_.find(name);
        if (!initialized_ || found == eventsByName_.end())
        {
            return {};
        }
        Event& event = events_[found->second];

        // An event that is already at its limit gives the oldest of its voices to the new one
        if (event.maxInstances > 0)
        {
            std::uint32_t count = 0;
            Voice* oldest = nullptr;
            for (Voice& voice : voices_)
            {
                if (voice.active && voice.eventIndex == static_cast<std::int32_t>(found->second) && !voice.fadingOut)
                {
                    ++count;
                    if (oldest == nullptr || voice.order < oldest->order)
                    {
                        oldest = &voice;
                    }
                }
            }
            if (count >= event.maxInstances && oldest != nullptr)
            {
                ReleaseVoice(*oldest);
                ++stats_.stolen;
            }
        }

        // A variation that is not the one that played last
        std::uint32_t variation = 0;
        if (event.sounds.size() > 1)
        {
            variation = NextRandom() % static_cast<std::uint32_t>(event.sounds.size());
            if (variation == event.lastVariation)
            {
                variation = (variation + 1) % static_cast<std::uint32_t>(event.sounds.size());
            }
        }
        event.lastVariation = variation;

        PlayParams play = event.base;
        play.volume = event.base.volume * RandomRange(event.volumeMin, event.volumeMax) * params.volumeScale;
        play.pitch = event.base.pitch * RandomRange(event.pitchMin, event.pitchMax) * params.pitchScale;
        play.spatial = event.base.spatial && params.hasPosition;
        play.position = params.position;
        play.velocity = params.velocity;
        return StartVoice(event.sounds[variation], play, static_cast<std::int32_t>(found->second));
    }

    // --------------------------------------------------------------------------- voices

    VoiceHandle AudioSystem::MakeHandle(const std::size_t index) const
    {
        return VoiceHandle{(voices_[index].generation << 16) | static_cast<std::uint32_t>(index + 1)};
    }

    AudioSystem::Voice* AudioSystem::ResolveVoice(const VoiceHandle handle)
    {
        return const_cast<Voice*>(static_cast<const AudioSystem*>(this)->ResolveVoice(handle));
    }

    const AudioSystem::Voice* AudioSystem::ResolveVoice(const VoiceHandle handle) const
    {
        const std::uint32_t slot = handle.value & 0xFFFFu;
        if (slot == 0 || slot > voices_.size())
        {
            return nullptr;
        }
        const Voice& voice = voices_[slot - 1];
        if (!voice.active || voice.generation != (handle.value >> 16))
        {
            return nullptr;
        }
        return &voice;
    }

    VoiceHandle AudioSystem::Play(const SoundHandle sound, const PlayParams& params)
    {
        return StartVoice(sound, params, -1);
    }

    VoiceHandle AudioSystem::StartVoice(const SoundHandle sound, const PlayParams& requested, const std::int32_t eventIndex)
    {
        if (!initialized_ || sound.value == 0 || sound.value >= sounds_.size() || !sounds_[sound.value].alive)
        {
            return {};
        }
        const Sound& data = sounds_[sound.value];

        // a free voice of the format of the sound, else the weakest playing one of that format that may be taken
        std::size_t chosen = voices_.size();
        for (std::size_t i = 0; i < voices_.size(); ++i)
        {
            if (!voices_[i].active && voices_[i].channels == data.info.channels)
            {
                chosen = i;
                break;
            }
        }
        if (chosen == voices_.size())
        {
            for (std::size_t i = 0; i < voices_.size(); ++i)
            {
                const Voice& candidate = voices_[i];
                if (!candidate.active || candidate.channels != data.info.channels || candidate.params.priority > requested.priority)
                {
                    continue;
                }
                if (chosen == voices_.size())
                {
                    chosen = i;
                    continue;
                }
                const Voice& best = voices_[chosen];
                const float candidateGain = ComputeGain(candidate, nullptr);
                const float bestGain = ComputeGain(best, nullptr);
                if (candidate.params.priority != best.params.priority ? candidate.params.priority < best.params.priority
                    : candidateGain != bestGain ? candidateGain < bestGain
                    : candidate.order < best.order)
                {
                    chosen = i;
                }
            }
            if (chosen == voices_.size())
            {
                ++stats_.dropped;
                return {};
            }
            ReleaseVoice(voices_[chosen]);
            ++stats_.stolen;
        }

        Voice& voice = voices_[chosen];
        voice.active = true;
        voice.sound = sound.value;
        voice.params = requested;
        voice.params.volume = std::max(requested.volume, 0.0f);
        voice.params.pitch = std::clamp(requested.pitch, kMinPitch, kMaxPitch);
        if (voice.params.bus == Bus::Master)
        {
            voice.params.bus = Bus::Sfx;
        }
        voice.fade = 1.0f;
        voice.fadeRate = 0.0f;
        voice.fadingOut = false;
        voice.order = ++startCounter_;
        voice.eventIndex = eventIndex;
        voice.lastGain = -1.0f;
        voice.lastPitch = -1.0f;
        voice.lastPan = 2.0f;
        ApplyVoice(voice, true);
        voice.impl->Start(data.data.samples.data(), data.data.Frames(), data.data.sampleRate, voice.params.loop);
        ++stats_.played;
        stats_.voicesPeak = std::max(stats_.voicesPeak, ActiveVoiceCount());
        return MakeHandle(chosen);
    }

    void AudioSystem::ReleaseVoice(Voice& voice)
    {
        if (voice.impl != nullptr)
        {
            voice.impl->Stop();
        }
        voice.active = false;
        voice.fadingOut = false;
        voice.eventIndex = -1;
        ++voice.generation;
        if ((voice.generation & 0xFFFFu) == 0)
        {
            voice.generation = 1;
        }
    }

    void AudioSystem::Stop(const VoiceHandle handle, const float fadeSeconds)
    {
        Voice* voice = ResolveVoice(handle);
        if (voice == nullptr)
        {
            return;
        }
        if (fadeSeconds > 0.0f)
        {
            voice->fadingOut = true;
            voice->fadeRate = voice->fade / fadeSeconds;
            return;
        }
        ReleaseVoice(*voice);
    }

    void AudioSystem::StopAll(const float fadeSeconds)
    {
        for (std::size_t i = 0; i < voices_.size(); ++i)
        {
            if (voices_[i].active)
            {
                Stop(MakeHandle(i), fadeSeconds);
            }
        }
    }

    void AudioSystem::StopBus(const Bus bus, const float fadeSeconds)
    {
        for (std::size_t i = 0; i < voices_.size(); ++i)
        {
            if (voices_[i].active && voices_[i].params.bus == bus)
            {
                Stop(MakeHandle(i), fadeSeconds);
            }
        }
    }

    bool AudioSystem::IsPlaying(const VoiceHandle handle) const
    {
        const Voice* voice = ResolveVoice(handle);
        return voice != nullptr && !voice->fadingOut;
    }

    void AudioSystem::SetVoiceVolume(const VoiceHandle handle, const float volume)
    {
        if (Voice* voice = ResolveVoice(handle))
        {
            voice->params.volume = std::max(volume, 0.0f);
            ApplyVoice(*voice, false);
        }
    }

    void AudioSystem::SetVoicePitch(const VoiceHandle handle, const float pitch)
    {
        if (Voice* voice = ResolveVoice(handle))
        {
            voice->params.pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
            ApplyVoice(*voice, false);
        }
    }

    void AudioSystem::SetVoicePosition(const VoiceHandle handle, const Vec3& position, const Vec3& velocity)
    {
        if (Voice* voice = ResolveVoice(handle))
        {
            voice->params.position = position;
            voice->params.velocity = velocity;
            ApplyVoice(*voice, false);
        }
    }

    // --------------------------------------------------------------------------- buses

    void AudioSystem::SetBusVolume(const Bus bus, const float volume)
    {
        busVolume_[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.0f, 4.0f);
    }

    float AudioSystem::GetBusVolume(const Bus bus) const
    {
        return busVolume_[static_cast<std::size_t>(bus)];
    }

    void AudioSystem::SetBusMuted(const Bus bus, const bool muted)
    {
        busMuted_[static_cast<std::size_t>(bus)] = muted;
    }

    bool AudioSystem::IsBusMuted(const Bus bus) const
    {
        return busMuted_[static_cast<std::size_t>(bus)];
    }

    float AudioSystem::ComputeGain(const Voice& voice, SpatialResult* outSpatial) const
    {
        const std::size_t bus = static_cast<std::size_t>(voice.params.bus);
        if (busMuted_[0] || busMuted_[bus])
        {
            if (outSpatial != nullptr)
            {
                *outSpatial = SpatialResult{};
            }
            return 0.0f;
        }
        SpatialResult spatial;
        if (voice.params.spatial)
        {
            spatial = ComputeSpatial(listener_, voice.params, config_.speedOfSound, config_.dopplerFactor);
        }
        if (outSpatial != nullptr)
        {
            *outSpatial = spatial;
        }
        return voice.params.volume * voice.fade * spatial.gain * busVolume_[bus] * busVolume_[0];
    }

    float AudioSystem::GetVoiceGain(const VoiceHandle handle) const
    {
        const Voice* voice = ResolveVoice(handle);
        return voice == nullptr ? 0.0f : ComputeGain(*voice, nullptr);
    }

    void AudioSystem::ApplyVoice(Voice& voice, const bool force)
    {
        SpatialResult spatial;
        const float gain = ComputeGain(voice, &spatial);
        const float pitch = std::clamp(voice.params.pitch * spatial.doppler, kMinPitch, kMaxPitch);
        const float pan = voice.channels == 1 ? spatial.pan : 0.0f;
        if (force || std::fabs(gain - voice.lastGain) > 1e-4f || std::fabs(pan - voice.lastPan) > 1e-3f)
        {
            float left = 1.0f;
            float right = 1.0f;
            if (voice.channels == 1 && voice.params.spatial)
            {
                PanGains(pan, left, right);
                left *= 1.41421356f; // equal power: the centre stays at the level of a sound that is not panned
                right *= 1.41421356f;
                left = std::min(left, 1.0f);
                right = std::min(right, 1.0f);
            }
            voice.impl->SetGain(gain * left, gain * right);
            voice.lastGain = gain;
            voice.lastPan = pan;
        }
        if (force || std::fabs(pitch - voice.lastPitch) > 1e-4f)
        {
            voice.impl->SetPitch(pitch);
            voice.lastPitch = pitch;
        }
    }

    // --------------------------------------------------------------------------- the frame

    void AudioSystem::Update(const float deltaTime)
    {
        if (!initialized_)
        {
            return;
        }
        ZoneScopedN("Audio::Update");
        backend_->Update(deltaTime);
        std::uint32_t active = 0;
        for (Voice& voice : voices_)
        {
            if (!voice.active)
            {
                continue;
            }
            if (voice.fadingOut)
            {
                voice.fade -= voice.fadeRate * deltaTime;
                if (voice.fade <= 0.0f)
                {
                    ReleaseVoice(voice);
                    continue;
                }
            }
            if (!voice.impl->IsPlaying())
            {
                ReleaseVoice(voice);
                continue;
            }
            ApplyVoice(voice, false);
            ++active;
        }
        stats_.voicesActive = active;
        stats_.voicesPeak = std::max(stats_.voicesPeak, active);
    }

    AudioStats AudioSystem::GetStats() const
    {
        AudioStats stats = stats_;
        stats.voicesActive = ActiveVoiceCount();
        return stats;
    }

    std::uint32_t AudioSystem::ActiveVoiceCount() const
    {
        std::uint32_t count = 0;
        for (const Voice& voice : voices_)
        {
            count += voice.active ? 1u : 0u;
        }
        return count;
    }

    std::uint32_t AudioSystem::NextRandom()
    {
        randomState_ ^= randomState_ << 13;
        randomState_ ^= randomState_ >> 17;
        randomState_ ^= randomState_ << 5;
        return randomState_;
    }

    float AudioSystem::RandomRange(const float low, const float high)
    {
        if (high <= low)
        {
            return low;
        }
        return low + (high - low) * (static_cast<float>(NextRandom() & 0xFFFFFFu) / static_cast<float>(0x1000000u));
    }
}
