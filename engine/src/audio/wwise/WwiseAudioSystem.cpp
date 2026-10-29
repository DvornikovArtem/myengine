// WwiseAudioSystem.cpp
//
// audio::AudioSystem on Wwise (MYENGINE_WWISE=ON, docs/engine/wwise.md, "AudioSystem на Wwise"). The same class and the same calls as the XAudio2 build
// (AudioSystem.cpp, which this file replaces in the build), so RlFeedback, FxSystem, RlGameSystem and Application do not change.
//
//   - an event of audio_bank.json is the event "Play_<name>" of the Wwise project (the banks Init and RocketArena are in the folder "wwise" of the sounds);
//   - a voice is one game object of Wwise with one posted event: its game parameters are Volume (SetVoiceVolume divided by the loudness that the project
//     gives the event, so that the volume asked for is the volume of the voice, as with XAudio2) and Pitch (SetVoicePitch x the Doppler factor); its
//     position is the position of the object;
//   - the buses are the global game parameters Bus_Master, Bus_Sfx, Bus_Music, Bus_Ui;
//   - the end of an event comes from the callback of Wwise (any thread), goes through a queue and is taken in Update on the game thread: everything else of
//     the sound engine is called from the game thread only.
// Without a device the engine renders offline: the time passes in Update(dt) (WwiseRuntime), the tests do not wait for the clock.
//
// The logic is in `Engine` below (it keeps references to what the class owns); the methods of AudioSystem only hand over.

#include <myengine/audio/AudioSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <myengine/audio/AudioSpatial.h>
#include <myengine/core/Logger.h>

#include <AK/SoundEngine/Common/AkCallback.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkTypes.h>

#include "../AudioBackend.h"   // IAudioVoice: a member of the class (unused here), complete for its destructor
#include "WwiseRuntime.h"

#include <tracy/Tracy.hpp>

namespace myengine::audio
{
    namespace
    {
        // The game parameters of the project: Pitch 0.25 .. 4, Volume 0 .. 4, the buses 0 .. 1 (docs/engine/wwise-project.md)
        constexpr float kMinPitch = 0.25f;
        constexpr float kMaxPitch = 4.0f;
        constexpr float kMaxVolume = 4.0f;
        constexpr AkGameObjectID kListenerObject = 1;
        constexpr AkGameObjectID kFirstVoiceObject = 100;
        constexpr const char* kBankNames[] = {"Init", "RocketArena"};   // Init first
        constexpr const char* kBusParameters[kBusCount] = {"Bus_Master", "Bus_Sfx", "Bus_Music", "Bus_Ui"};

        // A SoundBank is a row of chunks (a name of four characters, a size of four bytes, the content) that has to end where the file ends, and begins with BKHD.
        // A file that is cut is refused here: Wwise given a bank that is cut after its header waits for the rest of it for ever (the bank manager does not answer
        // and the sound engine cannot stop). A bank that is whole and wrong inside is Wwise's to refuse.
        bool BankFileIsWhole(const std::filesystem::path& file, std::string& why)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open())
            {
                why = "the file cannot be opened";
                return false;
            }
            stream.seekg(0, std::ios::end);
            const std::uint64_t size = static_cast<std::uint64_t>(stream.tellg());
            std::uint64_t position = 0;
            bool first = true;
            while (position < size)
            {
                char header[8];
                stream.seekg(static_cast<std::streamoff>(position));
                if (size - position < sizeof(header) || !stream.read(header, sizeof(header)))
                {
                    why = "a chunk header is cut at " + std::to_string(position) + " of " + std::to_string(size) + " bytes";
                    return false;
                }
                if (first && std::string(header, 4) != "BKHD")
                {
                    why = "the file does not begin with a BKHD chunk";
                    return false;
                }
                first = false;
                std::uint32_t chunk = 0;
                std::memcpy(&chunk, header + 4, sizeof(chunk));   // little endian, as the machine
                position += sizeof(header) + chunk;
                if (position > size)
                {
                    why = "the chunk '" + std::string(header, 4) + "' needs " + std::to_string(position) + " bytes and the file has " + std::to_string(size);
                    return false;
                }
            }
            if (first)
            {
                why = "the file is empty";
                return false;
            }
            return true;
        }

        Bus BusFromName(const std::string& name, const Bus fallback)
        {
            if (name == "sfx") return Bus::Sfx;
            if (name == "music") return Bus::Music;
            if (name == "ui") return Bus::Ui;
            return fallback;
        }

        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        Vec3 Normalized(const Vec3& a, const Vec3& fallback)
        {
            const float length = std::sqrt(Dot(a, a));
            return length > 1e-6f ? Vec3{a.x / length, a.y / length, a.z / length} : fallback;
        }

        // Wwise wants an orthonormal front and top (else SetPosition is refused): the listener of the game may be any camera
        void Orthonormal(const Vec3& forwardIn, const Vec3& upIn, Vec3& front, Vec3& top)
        {
            front = Normalized(forwardIn, Vec3{0.0f, 0.0f, 1.0f});
            // The world of the game is left-handed like the one of Wwise: X right, Y up, Z forward (up x forward = right, the sign of the pan in AudioSpatial.cpp)
            const Vec3 side = Cross(upIn, front);
            if (Dot(side, side) < 1e-8f)
            {
                // the up vector is along the front (or empty): any perpendicular one
                const Vec3 axis = std::fabs(front.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
                top = Normalized(Cross(front, Cross(axis, front)), Vec3{0.0f, 1.0f, 0.0f});
                return;
            }
            top = Normalized(Cross(front, side), Vec3{0.0f, 1.0f, 0.0f});
        }

        void SetObjectPosition(const AkGameObjectID object, const Vec3& position, const Vec3& forward, const Vec3& up)
        {
            Vec3 front;
            Vec3 top;
            Orthonormal(forward, up, front, top);
            AkSoundPosition transform;
            transform.Set(position.x, position.y, position.z, front.x, front.y, front.z, top.x, top.y, top.z);
            AK::SoundEngine::SetPosition(object, transform);
        }

        // The end of an event: the callback runs in a thread of Wwise (or inside RenderAudio): only a number goes into the queue
        struct EndQueue
        {
            std::mutex mutex;
            std::vector<AkGameObjectID> ended;
        };

        void AkEventCallback(AkCallbackType type, AkEventCallbackInfo* eventInfo, void*, void* cookie)
        {
            if (type != AK_EndOfEvent || eventInfo == nullptr || cookie == nullptr)
            {
                return;
            }
            auto* queue = static_cast<EndQueue*>(cookie);
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->ended.push_back(eventInfo->gameObjID);
        }

        struct Event
        {
            std::string name;
            AkUniqueID id = 0;                // GetIDFromString("Play_<name>")
            Bus bus = Bus::Sfx;
            bool loop = false;
            bool spatial = false;             // the event is 3D (its position is the one of the caller or the listener's)
            std::int32_t priority = 0;
            float dopplerFactor = 1.0f;
            float volume = 1.0f;              // "volume" of the json: the loudness that the Wwise project gives the event (the game parameter Volume multiplies it)
        };

        struct Slot
        {
            bool active = false;
            std::uint32_t generation = 1;
            AkPlayingID playing = AK_INVALID_PLAYING_ID;
            AkGameObjectID object = 0;
            std::int32_t event = -1;
            std::int32_t priority = 0;
            std::uint64_t order = 0;
            bool positional = false;          // has its own position (the caller gave one): the Doppler effect applies
            bool followsListener = false;     // a 3D event without a position: it sits at the listener
            Vec3 position{};
            Vec3 velocity{};
            float volume = 1.0f;              // the game parameters as the game asked for them
            float pitch = 1.0f;
            float appliedVolume = -1.0f;
            float appliedPitch = -1.0f;
            bool fadingOut = false;
            float fadeLeft = 0.0f;
        };

        // One warning for a call or a name: the log is not flooded by a game that calls a thing 60 times a second
        std::unordered_set<std::string>& Warned()
        {
            static std::unordered_set<std::string> warned;
            return warned;
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------------------ the engine

    class WwiseEngine
    {
    public:
        WwiseEngine(core::Logger* logger, const AudioConfig& config, const ListenerState& listener, AudioStats& stats, std::array<float, kBusCount>& busVolume,
                    std::array<bool, kBusCount>& busMuted)
            : logger_(logger), config_(config), listener_(listener), stats_(stats), busVolume_(busVolume), busMuted_(busMuted), runtime_(logger)
        {
        }

        bool Start(bool communication)
        {
            running_ = runtime_.Initialize({}, config_.useDevice, communication);
            if (running_)
            {
                AK::SoundEngine::SetRandomSeed(config_.randomSeed);
                AK::SoundEngine::RegisterGameObj(kListenerObject, "Listener");
                const AkGameObjectID listener = kListenerObject;
                AK::SoundEngine::SetDefaultListeners(&listener, 1);
            }
            const std::size_t count = std::clamp<std::size_t>(static_cast<std::size_t>(config_.monoVoices) + config_.stereoVoices, 8, 1024);
            slots_.assign(count, Slot{});
            return running_;
        }

        void Stop()
        {
            runtime_.Shutdown();   // Wwise ends the callbacks before it returns
            running_ = false;
        }

        bool Running() const { return running_; }
        std::size_t SlotCount() const { return slots_.size(); }

        void Warn(const std::string& key, const std::string& text)
        {
            if (logger_ != nullptr && Warned().insert(key).second)
            {
                logger_->Warning(text);
            }
        }

        // ---- the bank

        bool LoadBanks(const std::filesystem::path& bankDirectory, const nlohmann::json& events)
        {
            if (!running_)
            {
                return false;
            }
            runtime_.SetBankDirectory(bankDirectory);
            for (const char* name : kBankNames)
            {
                const std::filesystem::path file = bankDirectory / (std::string(name) + ".bnk");
                const std::string key = file.lexically_normal().generic_string();
                if (loadedBanks_.count(key) > 0)
                {
                    continue;   // a second LoadBank of the same file: loaded already
                }
                std::string why;
                if (!BankFileIsWhole(file, why))
                {
                    if (logger_ != nullptr)
                    {
                        logger_->Error("Wwise: the bank file " + file.generic_string() + " is not whole: " + why);
                    }
                    return false;
                }
                if (!runtime_.LoadBank(name))
                {
                    return false;
                }
                std::error_code ec;
                const auto size = std::filesystem::file_size(file, ec);
                if (!ec)
                {
                    stats_.soundBytes += size;
                }
                ++stats_.soundsLoaded;
                loadedBanks_.insert(key);
            }
            for (const auto& entry : events.items())
            {
                const nlohmann::json& body = entry.value();
                Event event;
                event.name = entry.key();
                event.id = AK::SoundEngine::GetIDFromString(("Play_" + event.name).c_str());
                event.bus = BusFromName(body.value("bus", std::string("sfx")), Bus::Sfx);
                event.loop = body.value("loop", false);
                event.spatial = body.value("spatial", false);
                event.priority = body.value("priority", 0);
                event.dopplerFactor = body.value("doppler", 1.0f);
                event.volume = std::max(body.value("volume", 1.0f), 0.01f);
                const auto known = byName_.find(event.name);
                if (known != byName_.end())
                {
                    events_[known->second] = std::move(event);   // (the same json again)
                    continue;
                }
                byName_[event.name] = static_cast<std::uint32_t>(events_.size());
                events_.push_back(std::move(event));
            }
            // The settings of the menu that were made before the banks: the game parameters exist only when the banks are loaded
            for (std::size_t bus = 0; bus < kBusCount; ++bus)
            {
                ApplyBus(static_cast<Bus>(bus));
            }
            return true;
        }

        std::size_t EventCount() const { return events_.size(); }
        bool HasEvent(const std::string& name) const { return byName_.find(name) != byName_.end(); }

        // ---- voices

        static VoiceHandle MakeHandle(const Slot& slot, const std::size_t index)
        {
            return VoiceHandle{(slot.generation << 16) | static_cast<std::uint32_t>(index + 1)};
        }

        Slot* Resolve(const VoiceHandle handle)
        {
            const std::uint32_t index = handle.value & 0xFFFFu;
            if (index == 0 || index > slots_.size())
            {
                return nullptr;
            }
            Slot& slot = slots_[index - 1];
            return slot.active && slot.generation == (handle.value >> 16) ? &slot : nullptr;
        }

        const Slot* Resolve(const VoiceHandle handle) const
        {
            return const_cast<WwiseEngine*>(this)->Resolve(handle);
        }

        // The slot is free for the next sound (its handle is not valid any more); the game object stays until its event has ended
        void Release(Slot& slot, const bool objectEnded)
        {
            if (objectEnded)
            {
                AK::SoundEngine::UnregisterGameObj(slot.object);
                orphans_.erase(slot.object);
            }
            else
            {
                orphans_.insert(slot.object);
            }
            slot.active = false;
            slot.fadingOut = false;
            slot.event = -1;
            ++slot.generation;
            if ((slot.generation & 0xFFFFu) == 0)
            {
                slot.generation = 1;
            }
        }

        VoiceHandle PlayEvent(const std::string& name, const EventParams& params)
        {
            if (!running_)
            {
                return {};
            }
            const auto found = byName_.find(name);
            if (found == byName_.end())
            {
                return {};
            }
            const Event& event = events_[found->second];

            // A free slot, else the weakest playing one that may be taken: the lowest priority (not higher than the new sound's), then the oldest
            std::size_t chosen = slots_.size();
            for (std::size_t i = 0; i < slots_.size(); ++i)
            {
                if (!slots_[i].active)
                {
                    chosen = i;
                    break;
                }
            }
            if (chosen == slots_.size())
            {
                for (std::size_t i = 0; i < slots_.size(); ++i)
                {
                    const Slot& candidate = slots_[i];
                    if (candidate.priority > event.priority)
                    {
                        continue;
                    }
                    if (chosen == slots_.size() || candidate.priority < slots_[chosen].priority ||
                        (candidate.priority == slots_[chosen].priority && candidate.order < slots_[chosen].order))
                    {
                        chosen = i;
                    }
                }
                if (chosen == slots_.size())
                {
                    ++stats_.dropped;
                    return {};
                }
                AK::SoundEngine::StopPlayingID(slots_[chosen].playing, 0);
                Release(slots_[chosen], false);
                ++stats_.stolen;
            }

            Slot& slot = slots_[chosen];
            slot.object = nextObject_++;
            AK::SoundEngine::RegisterGameObj(slot.object);
            slot.positional = event.spatial && params.hasPosition;
            slot.followsListener = event.spatial && !params.hasPosition;
            slot.position = params.hasPosition ? params.position : listener_.position;
            slot.velocity = params.velocity;
            slot.volume = std::clamp(params.volumeScale, 0.0f, kMaxVolume);
            slot.pitch = std::clamp(params.pitchScale, kMinPitch, kMaxPitch);
            slot.appliedVolume = -1.0f;
            slot.appliedPitch = -1.0f;
            slot.event = found->second;
            slot.priority = event.priority;
            slot.order = ++order_;
            slot.fadingOut = false;
            slot.fadeLeft = 0.0f;
            SetObjectPosition(slot.object, slot.position, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f});
            ApplyParameters(slot, true);

            slot.playing = AK::SoundEngine::PostEvent(event.id, slot.object, AK_EndOfEvent, &AkEventCallback, &queue_);
            if (slot.playing == AK_INVALID_PLAYING_ID)
            {
                AK::SoundEngine::UnregisterGameObj(slot.object);
                ++stats_.dropped;
                Warn("post:" + name, "Audio: Wwise did not take the event Play_" + name + " (the bank has no such event?)");
                return {};
            }
            slot.active = true;
            ++stats_.played;
            stats_.voicesPeak = std::max(stats_.voicesPeak, ActiveCount());
            return MakeHandle(slot, chosen);
        }

        // Volume and Pitch (with the Doppler effect of a voice that has a position) are the game parameters of the game object
        void ApplyParameters(Slot& slot, const bool force)
        {
            const float volume = std::clamp(slot.volume, 0.0f, kMaxVolume);
            if (force || std::fabs(volume - slot.appliedVolume) > 1e-4f)
            {
                AK::SoundEngine::SetRTPCValue("Volume", volume, slot.object);
                slot.appliedVolume = volume;
            }
            float pitch = slot.pitch;
            if (slot.positional && slot.event >= 0)
            {
                PlayParams source;
                source.spatial = true;
                source.position = slot.position;
                source.velocity = slot.velocity;
                source.dopplerFactor = events_[static_cast<std::size_t>(slot.event)].dopplerFactor;
                pitch *= ComputeSpatial(listener_, source, config_.speedOfSound, config_.dopplerFactor).doppler;
            }
            pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
            if (force || std::fabs(pitch - slot.appliedPitch) > 1e-4f)
            {
                AK::SoundEngine::SetRTPCValue("Pitch", pitch, slot.object);
                slot.appliedPitch = pitch;
            }
        }

        void Stop(const VoiceHandle handle, const float fadeSeconds)
        {
            Slot* slot = Resolve(handle);
            if (slot == nullptr)
            {
                return;
            }
            if (fadeSeconds > 0.0f)
            {
                if (!slot->fadingOut)
                {
                    slot->fadingOut = true;
                    slot->fadeLeft = fadeSeconds;
                    AK::SoundEngine::StopPlayingID(slot->playing, static_cast<AkTimeMs>(std::ceil(fadeSeconds * 1000.0f)));
                }
                return;
            }
            AK::SoundEngine::StopPlayingID(slot->playing, 0);
            Release(*slot, false);
        }

        void StopAll(const float fadeSeconds)
        {
            for (std::size_t i = 0; i < slots_.size(); ++i)
            {
                if (slots_[i].active)
                {
                    Stop(MakeHandle(slots_[i], i), fadeSeconds);
                }
            }
        }

        void StopBus(const Bus bus, const float fadeSeconds)
        {
            for (std::size_t i = 0; i < slots_.size(); ++i)
            {
                if (slots_[i].active && slots_[i].event >= 0 && events_[static_cast<std::size_t>(slots_[i].event)].bus == bus)
                {
                    Stop(MakeHandle(slots_[i], i), fadeSeconds);
                }
            }
        }

        bool IsPlaying(const VoiceHandle handle) const
        {
            const Slot* slot = Resolve(handle);
            return slot != nullptr && !slot->fadingOut;
        }

        // The volume that the game asks for is the volume of the voice, as in the XAudio2 build (there it replaces the volume of the event): the game
        // parameter Volume multiplies the loudness that the project gives the event, so it is the asked volume divided by that loudness
        void SetVoiceVolume(const VoiceHandle handle, const float volume)
        {
            if (Slot* slot = Resolve(handle))
            {
                const float base = slot->event >= 0 ? events_[static_cast<std::size_t>(slot->event)].volume : 1.0f;
                slot->volume = std::clamp(std::max(volume, 0.0f) / base, 0.0f, kMaxVolume);
                ApplyParameters(*slot, false);
            }
        }

        void SetVoicePitch(const VoiceHandle handle, const float pitch)
        {
            if (Slot* slot = Resolve(handle))
            {
                slot->pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
                ApplyParameters(*slot, false);
            }
        }

        void SetVoicePosition(const VoiceHandle handle, const Vec3& position, const Vec3& velocity)
        {
            Slot* slot = Resolve(handle);
            if (slot == nullptr)
            {
                return;
            }
            slot->position = position;
            slot->velocity = velocity;
            if (slot->event >= 0 && events_[static_cast<std::size_t>(slot->event)].spatial)
            {
                slot->positional = true;
                slot->followsListener = false;
            }
            SetObjectPosition(slot->object, position, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f});
            ApplyParameters(*slot, false);
        }

        // ---- the buses: the global game parameters, 0..1 (what is above 1 is kept for GetBusVolume, the sound is not louder than the project)

        void ApplyBus(const Bus bus)
        {
            if (!running_)
            {
                return;
            }
            const std::size_t index = static_cast<std::size_t>(bus);
            const float value = busMuted_[index] ? 0.0f : std::clamp(busVolume_[index], 0.0f, 1.0f);
            AK::SoundEngine::SetRTPCValue(kBusParameters[index], value);
        }

        // ---- the frame

        void Update(const float deltaTime)
        {
            if (!running_)
            {
                return;
            }
            // the listener
            const bool moved = !listenerApplied_ || listener_.position.x != appliedListener_.position.x || listener_.position.y != appliedListener_.position.y ||
                               listener_.position.z != appliedListener_.position.z || listener_.forward.x != appliedListener_.forward.x ||
                               listener_.forward.y != appliedListener_.forward.y || listener_.forward.z != appliedListener_.forward.z ||
                               listener_.up.x != appliedListener_.up.x || listener_.up.y != appliedListener_.up.y || listener_.up.z != appliedListener_.up.z;
            if (moved)
            {
                SetObjectPosition(kListenerObject, listener_.position, listener_.forward, listener_.up);
                appliedListener_ = listener_;
                listenerApplied_ = true;
            }

            for (Slot& slot : slots_)
            {
                if (!slot.active)
                {
                    continue;
                }
                if (slot.fadingOut)
                {
                    // the fade is Wwise's; the slot is free when its time has passed (not later: the number of the voices does not wait for the callback)
                    slot.fadeLeft -= deltaTime;
                    if (slot.fadeLeft <= 0.0f)
                    {
                        Release(slot, false);
                        continue;
                    }
                }
                if (slot.followsListener && moved)
                {
                    // a 3D sound without a position is at the listener: the pan is in the middle, the distance is nothing
                    slot.position = listener_.position;
                    SetObjectPosition(slot.object, slot.position, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f});
                }
                if (slot.positional)
                {
                    ApplyParameters(slot, false);   // the Doppler effect follows the listener
                }
            }

            {
                ZoneScopedN("Wwise::RenderAudio");
                runtime_.RenderAudio(deltaTime);
            }
            DrainEnded();
            if ((++frame_ % 30) == 0)
            {
                TracyPlot("Wwise memory, bytes", static_cast<std::int64_t>(runtime_.MemoryUsed()));
            }
            TracyPlot("Wwise voices", static_cast<std::int64_t>(ActiveCount()));

            stats_.voicesActive = ActiveCount();
            stats_.voicesPeak = std::max(stats_.voicesPeak, stats_.voicesActive);
        }

        // The events that have ended: the slot is free, the game object is gone
        void DrainEnded()
        {
            std::vector<AkGameObjectID> ended;
            {
                std::lock_guard<std::mutex> lock(queue_.mutex);
                ended.swap(queue_.ended);
            }
            for (const AkGameObjectID object : ended)
            {
                if (orphans_.erase(object) > 0)
                {
                    AK::SoundEngine::UnregisterGameObj(object);
                    continue;
                }
                for (Slot& slot : slots_)
                {
                    if (slot.active && slot.object == object)
                    {
                        Release(slot, true);
                        break;
                    }
                }
            }
        }

        void FillStats(AudioStats& stats) const
        {
            stats.engineMemoryBytes = runtime_.MemoryUsed();
            stats.engineObjects = ActiveCount() + static_cast<std::uint32_t>(orphans_.size());
        }

        std::uint32_t ActiveCount() const
        {
            std::uint32_t count = 0;
            for (const Slot& slot : slots_)
            {
                count += slot.active ? 1u : 0u;
            }
            return count;
        }

    private:
        core::Logger* logger_;
        const AudioConfig& config_;
        const ListenerState& listener_;
        AudioStats& stats_;
        std::array<float, kBusCount>& busVolume_;
        std::array<bool, kBusCount>& busMuted_;
        wwise::WwiseRuntime runtime_;
        bool running_ = false;
        std::vector<Event> events_;
        std::unordered_map<std::string, std::uint32_t> byName_;
        std::vector<Slot> slots_;
        std::unordered_set<AkGameObjectID> orphans_;   // objects whose slot is already free, waiting for the end of their event to be unregistered
        std::unordered_set<std::string> loadedBanks_;  // the files of the banks that Wwise has loaded
        EndQueue queue_;
        AkGameObjectID nextObject_ = kFirstVoiceObject;
        std::uint64_t order_ = 0;
        std::uint64_t frame_ = 0;
        ListenerState appliedListener_;
        bool listenerApplied_ = false;
    };

    struct AudioSystem::WwiseState
    {
        WwiseState(core::Logger* logger, const AudioConfig& config, const ListenerState& listener, AudioStats& stats, std::array<float, kBusCount>& busVolume,
                   std::array<bool, kBusCount>& busMuted)
            : engine(logger, config, listener, stats, busVolume, busMuted)
        {
        }
        WwiseEngine engine;
    };

    // ------------------------------------------------------------------------------------------------------------------------------------ the class

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
        stats_ = {};
        busVolume_ = {std::clamp(config.masterVolume, 0.0f, 4.0f), 1.0f, 1.0f, 1.0f};
        busMuted_ = {false, false, false, false};
        wwise_ = std::make_unique<WwiseState>(logger_, config_, listener_, stats_, busVolume_, busMuted_);

        // The link with the Wwise authoring (Profiler, Remote Connect) opens a network port: the firewall of Windows would ask at every start of a build
        // that has it, so it is on only when it is asked for (and exists in the Debug and Profile libraries of the SDK only)
        bool communication = false;
        {
            char* value = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&value, &length, "MYENGINE_WWISE_PROFILER") == 0 && value != nullptr)
            {
                communication = std::string(value) == "1";
                std::free(value);
            }
        }
        const bool running = wwise_->engine.Start(communication);
        if (!running)
        {
            Log(1, "Audio: the Wwise sound engine did not start: nothing is heard");
        }
        initialized_ = true;
        stats_.deviceAvailable = running && config.useDevice;
        stats_.deviceName = !running ? "none (Wwise did not start)" : (config.useDevice ? "Wwise: the default device" : "Wwise: offline, no device");
        stats_.voicesTotal = static_cast<std::uint32_t>(wwise_->engine.SlotCount());
        Log(0, "Audio: " + stats_.deviceName + ", " + std::to_string(stats_.voicesTotal) + " voices");
        return true;
    }

    void AudioSystem::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }
        if (wwise_ != nullptr)
        {
            wwise_->engine.Stop();
            wwise_.reset();
        }
        initialized_ = false;
    }

    bool AudioSystem::HasDevice() const
    {
        return initialized_ && wwise_ != nullptr && wwise_->engine.Running() && config_.useDevice;
    }

    // The XAudio2 build plays WAV files that the game loads; Wwise plays the events of its banks. The game never calls these (its sounds are events), so they
    // do nothing: an invalid handle, false, and one warning.
    SoundHandle AudioSystem::LoadSound(const std::filesystem::path&)
    {
        if (wwise_ != nullptr) wwise_->engine.Warn("LoadSound", "Audio: LoadSound has no meaning with Wwise (the sounds are the events of the banks): it does nothing");
        return {};
    }

    SoundHandle AudioSystem::CreateSound(const std::string&, const WavData&)
    {
        if (wwise_ != nullptr) wwise_->engine.Warn("CreateSound", "Audio: CreateSound has no meaning with Wwise (the sounds are the events of the banks): it does nothing");
        return {};
    }

    void AudioSystem::UnloadSound(SoundHandle)
    {
        if (wwise_ != nullptr) wwise_->engine.Warn("UnloadSound", "Audio: UnloadSound has no meaning with Wwise (the sounds are the events of the banks): it does nothing");
    }

    bool AudioSystem::GetSoundInfo(SoundHandle, SoundInfo&) const
    {
        return false;
    }

    VoiceHandle AudioSystem::Play(SoundHandle, const PlayParams&)
    {
        if (wwise_ != nullptr) wwise_->engine.Warn("Play", "Audio: Play(SoundHandle) has no meaning with Wwise (the sounds are the events of the banks): it does nothing");
        return {};
    }

    float AudioSystem::GetVoiceGain(VoiceHandle) const
    {
        return 0.0f;
    }

    bool AudioSystem::LoadBank(const std::filesystem::path& bankFile)
    {
        if (!initialized_ || wwise_ == nullptr)
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
        const auto eventsIt = bank.find("events");
        if (eventsIt == bank.end() || !eventsIt->is_object())
        {
            Log(1, "Audio: the sound bank has no \"events\" object");
            return false;
        }

        // The banks of Wwise are in the folder "wwise" of the sounds of the project: the "directory" of the file as it is given (from the working directory),
        // or from the project root above assets/data (the same search as the XAudio2 build makes for its files)
        const std::string directoryText = bank.value("directory", std::string("assets/audio"));
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(std::filesystem::path(directoryText));
        std::filesystem::path root = bankFile.parent_path();
        for (int i = 0; i < 3; ++i)
        {
            candidates.push_back(root / directoryText);
            root = root.parent_path();
        }
        std::filesystem::path bankDirectory;
        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate / "wwise", ec))
            {
                bankDirectory = candidate / "wwise";
                break;
            }
        }
        if (bankDirectory.empty())
        {
            Log(1, "Audio: the folder of the Wwise banks (" + directoryText + "/wwise) is not found next to " + bankFile.generic_string());
            return false;
        }
        if (!wwise_->engine.LoadBanks(bankDirectory, *eventsIt))
        {
            Log(1, "Audio: the Wwise banks did not load from " + bankDirectory.generic_string());
            return false;
        }
        Log(0, "Audio: Wwise banks from " + bankDirectory.generic_string() + ": " + std::to_string(wwise_->engine.EventCount()) + " events");
        return true;
    }

    bool AudioSystem::HasEvent(const std::string& name) const
    {
        return wwise_ != nullptr && wwise_->engine.HasEvent(name);
    }

    VoiceHandle AudioSystem::PlayEvent(const std::string& name, const EventParams& params)
    {
        ZoneScopedN("Audio::PlayEvent");
        return initialized_ && wwise_ != nullptr ? wwise_->engine.PlayEvent(name, params) : VoiceHandle{};
    }

    void AudioSystem::Stop(const VoiceHandle voice, const float fadeSeconds)
    {
        if (wwise_ != nullptr) wwise_->engine.Stop(voice, fadeSeconds);
    }

    void AudioSystem::StopAll(const float fadeSeconds)
    {
        if (wwise_ != nullptr) wwise_->engine.StopAll(fadeSeconds);
    }

    void AudioSystem::StopBus(const Bus bus, const float fadeSeconds)
    {
        if (wwise_ != nullptr) wwise_->engine.StopBus(bus, fadeSeconds);
    }

    bool AudioSystem::IsPlaying(const VoiceHandle voice) const
    {
        return wwise_ != nullptr && wwise_->engine.IsPlaying(voice);
    }

    void AudioSystem::SetVoiceVolume(const VoiceHandle voice, const float volume)
    {
        if (wwise_ != nullptr) wwise_->engine.SetVoiceVolume(voice, volume);
    }

    void AudioSystem::SetVoicePitch(const VoiceHandle voice, const float pitch)
    {
        if (wwise_ != nullptr) wwise_->engine.SetVoicePitch(voice, pitch);
    }

    void AudioSystem::SetVoicePosition(const VoiceHandle voice, const Vec3& position, const Vec3& velocity)
    {
        if (wwise_ != nullptr) wwise_->engine.SetVoicePosition(voice, position, velocity);
    }

    void AudioSystem::SetBusVolume(const Bus bus, const float volume)
    {
        busVolume_[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.0f, 4.0f);
        if (wwise_ != nullptr) wwise_->engine.ApplyBus(bus);
    }

    float AudioSystem::GetBusVolume(const Bus bus) const
    {
        return busVolume_[static_cast<std::size_t>(bus)];
    }

    void AudioSystem::SetBusMuted(const Bus bus, const bool muted)
    {
        busMuted_[static_cast<std::size_t>(bus)] = muted;
        if (wwise_ != nullptr) wwise_->engine.ApplyBus(bus);
    }

    bool AudioSystem::IsBusMuted(const Bus bus) const
    {
        return busMuted_[static_cast<std::size_t>(bus)];
    }

    void AudioSystem::Update(const float deltaTime)
    {
        if (!initialized_ || wwise_ == nullptr)
        {
            return;
        }
        ZoneScopedN("Audio::Update");
        wwise_->engine.Update(deltaTime);
    }

    AudioStats AudioSystem::GetStats() const
    {
        AudioStats stats = stats_;
        stats.voicesActive = ActiveVoiceCount();
        if (wwise_ != nullptr)
        {
            wwise_->engine.FillStats(stats);
        }
        return stats;
    }

    std::uint32_t AudioSystem::ActiveVoiceCount() const
    {
        return wwise_ != nullptr ? wwise_->engine.ActiveCount() : 0u;
    }

    std::uint32_t AudioSystem::VoiceCount() const
    {
        return wwise_ != nullptr ? static_cast<std::uint32_t>(wwise_->engine.SlotCount()) : 0u;
    }
}
