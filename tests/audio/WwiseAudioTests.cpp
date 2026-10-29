// audio::AudioSystem on Wwise (W3, docs/engine/wwise.md): the banks of the project load, every event of assets/data/audio_bank.json plays through Wwise and ends
// when its sound ends, a loop stops with a fade, the buses and the positions reach the sound engine. No device and no window: Wwise renders offline, the time
// passes in Update(dt), so the test does not wait for the clock. Built only with MYENGINE_WWISE=ON.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/audio/AudioSystem.h>
#include <myengine/audio/WavFile.h>
#include <myengine/core/Logger.h>

#include <AK/SoundEngine/Common/AkQueryParameters.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;

    int g_failures = 0;

    void Check(const bool condition, const std::string& text)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("  FAIL %s\n", text.c_str());
        }
    }

    bool Near(const float a, const float b, const float tolerance = 0.02f) { return std::fabs(a - b) <= tolerance; }

    // seconds of audio until the voice is free (an upper bound of `limit`)
    float SecondsUntilFree(audio::AudioSystem& audio, const audio::VoiceHandle voice, const float limit, const float step = 0.05f)
    {
        float elapsed = 0.0f;
        while (elapsed < limit && audio.GetStats().voicesActive > 0)
        {
            audio.Update(step);
            elapsed += step;
        }
        (void)voice;
        return elapsed;
    }

    void Settle(audio::AudioSystem& audio, const float seconds = 0.3f)
    {
        for (float t = 0.0f; t < seconds; t += 0.05f)
        {
            audio.Update(0.05f);
        }
    }

    bool StartAudio(audio::AudioSystem& audio, core::Logger& logger, const fs::path& source)
    {
        audio.SetLogger(&logger);
        audio::AudioConfig config;
        config.useDevice = false;
        Check(audio.Initialize(config), "the audio system starts");
        Check(!audio.HasDevice(), "without a device");
        const bool loaded = audio.LoadBank(source / "assets/data/audio_bank.json");
        Check(loaded, "the bank (the events of the json and the banks of Wwise) loads");
        return loaded;
    }
}

int main()
{
    const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
    const fs::path folder = fs::temp_directory_path() / "myengine_wwise_audio_tests";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    core::Logger logger;
    const fs::path logFile = folder / "audio.log";
    logger.Initialize(logFile);

    // the events of the json and the length of their sounds
    nlohmann::json bankJson;
    {
        std::ifstream stream(source / "assets/data/audio_bank.json");
        stream >> bankJson;
    }
    struct EventInfo
    {
        std::string name;
        bool loop = false;
        bool spatial = false;
        std::string bus;
        double seconds = 0.0;   // the longest of its files
    };
    std::vector<EventInfo> events;
    for (const auto& entry : bankJson["events"].items())
    {
        EventInfo info;
        info.name = entry.key();
        info.loop = entry.value().value("loop", false);
        info.spatial = entry.value().value("spatial", false);
        info.bus = entry.value().value("bus", "sfx");
        std::vector<std::string> files;
        if (entry.value().contains("files"))
        {
            for (const auto& file : entry.value()["files"])
            {
                files.push_back(file.get<std::string>());
            }
        }
        for (const std::string& file : files)
        {
            audio::WavData wav;
            std::string wavError;
            if (audio::LoadWavFile(source / bankJson.value("directory", "assets/audio") / file, wav, wavError))
            {
                info.seconds = std::max(info.seconds, wav.Seconds());
            }
        }
        events.push_back(info);
    }
    Check(events.size() >= 20, "the json has its events");

    {
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            const audio::AudioStats stats = audio.GetStats();
            Check(stats.soundsLoaded == 2 && stats.soundBytes > 1000000, "two banks are loaded (Init and RocketArena)");
            Check(stats.voicesTotal >= 8 && !stats.deviceAvailable && stats.deviceName.find("Wwise") != std::string::npos, "the statistics say Wwise and no device");

            // every event of the json is an event of the banks that plays
            for (const EventInfo& info : events)
            {
                Check(audio.HasEvent(info.name), "the event " + info.name + " is known");
            }
            Check(!audio.HasEvent("no_such_event"), "an event that is not in the json is not known");

            for (const EventInfo& info : events)
            {
                const audio::VoiceHandle voice = audio.PlayEvent(info.name);
                Check(voice.IsValid(), "the event " + info.name + " plays (a valid handle)");
                Check(audio.GetStats().voicesActive == 1, "one voice for " + info.name);
                if (!voice.IsValid())
                {
                    continue;
                }
                if (info.loop)
                {
                    // a loop plays for a long time, stops with a fade and the voice is free after the fade
                    Settle(audio, 1.0f);
                    Check(audio.IsPlaying(voice) && audio.GetStats().voicesActive == 1, "the loop " + info.name + " still plays after a second");
                    audio.Stop(voice, 0.2f);
                    Check(!audio.IsPlaying(voice), "a loop that is stopped with a fade is not playing at once");
                    Settle(audio, 0.4f);
                    Check(audio.GetStats().voicesActive == 0, "the loop " + info.name + " is gone after its fade");
                    Check(!audio.IsPlaying(voice), "and its handle is not valid");
                }
                else
                {
                    // a one-shot ends when its sound ends: not at once (it plays), not much later
                    const float elapsed = SecondsUntilFree(audio, voice, static_cast<float>(info.seconds) + 3.0f);
                    Check(audio.GetStats().voicesActive == 0, "the one-shot " + info.name + " ends by itself (" + std::to_string(elapsed) + " s of " + std::to_string(info.seconds) + " s)");
                    Check(elapsed >= info.seconds * 0.5 && elapsed <= info.seconds + 0.6, "and in about the length of its sound: " + info.name + " " + std::to_string(elapsed) + " s of " + std::to_string(info.seconds) + " s");
                    Check(!audio.IsPlaying(voice), "its handle is not valid after the end");
                }
            }
            Check(audio.GetStats().played >= events.size(), "played counts the sounds");
        }
        audio.Shutdown();
    }

    {
        // an event that is not there, a stop of a handle that is not valid, the other calls with it: nothing happens
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            const std::uint64_t dropped = audio.GetStats().dropped;
            Check(!audio.PlayEvent("no_such_event").IsValid(), "an unknown event gives an invalid handle");
            const audio::VoiceHandle none{};
            audio.Stop(none, 0.1f);
            audio.SetVoiceVolume(none, 0.5f);
            audio.SetVoicePitch(none, 2.0f);
            audio.SetVoicePosition(none, {1.0f, 2.0f, 3.0f});
            Check(!audio.IsPlaying(none), "an invalid handle does not play");
            audio.Update(0.05f);
            Check(audio.GetStats().voicesActive == 0 && audio.GetStats().dropped == dropped, "nothing was started, nothing dropped");

            // a handle of a voice that has ended is not valid even when its slot is used again
            const audio::VoiceHandle first = audio.PlayEvent("ui_click");
            SecondsUntilFree(audio, first, 3.0f);
            const audio::VoiceHandle second = audio.PlayEvent("ui_click");
            Check(second.IsValid() && !(first == second), "the slot is used again with another handle");
            Check(!audio.IsPlaying(first) && audio.IsPlaying(second), "the old handle is dead, the new one plays");
            audio.Stop(first, 0.0f);   // must not touch the new voice
            Check(audio.IsPlaying(second), "a stop with the old handle does not stop the new voice");
            audio.StopAll(0.0f);
            Check(audio.GetStats().voicesActive == 0 && !audio.IsPlaying(second), "StopAll stops at once without a fade");

            // the calls that Wwise has no meaning for
            Check(!audio.LoadSound(source / "assets/audio/ui_click.wav").IsValid(), "LoadSound gives an invalid handle with Wwise");
            audio::SoundInfo info;
            Check(!audio.GetSoundInfo({}, info), "GetSoundInfo is false");
            Check(!audio.Play({}).IsValid(), "Play of a sound is not played");
        }
    }

    {
        // StopBus: the music only
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            const audio::VoiceHandle music = audio.PlayEvent("menu_music");
            const audio::VoiceHandle engine = audio.PlayEvent("engine");
            const audio::VoiceHandle click = audio.PlayEvent("ui_click");
            Settle(audio, 0.1f);
            Check(audio.IsPlaying(music) && audio.IsPlaying(engine), "the music and the engine play");
            audio.StopBus(audio::Bus::Music, 0.0f);
            Check(!audio.IsPlaying(music) && audio.IsPlaying(engine), "StopBus(Music) stops the music and leaves the engine");
            audio.StopBus(audio::Bus::Sfx, 0.0f);
            Check(!audio.IsPlaying(engine), "StopBus(Sfx) stops the engine");
            (void)click;
            audio.StopAll(0.0f);
            Settle(audio, 0.2f);
            Check(audio.GetStats().voicesActive == 0, "no voices are left");
        }
    }

    {
        // the buses are the global game parameters of Wwise; the volume and the pitch of a voice are those of its game object; the position is its position
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            const auto globalValue = [](const char* name, float& out)
            {
                AK::SoundEngine::Query::RTPCValue_type type = AK::SoundEngine::Query::RTPCValue_Global;
                AkRtpcValue value = -1.0f;
                const AKRESULT result = AK::SoundEngine::Query::GetRTPCValue(AK::SoundEngine::GetIDFromString(name), AK_INVALID_GAME_OBJECT, AK_INVALID_PLAYING_ID, value, type);
                out = value;
                return result == AK_Success;
            };
            audio.SetBusVolume(audio::Bus::Music, 0.3f);
            audio.SetBusVolume(audio::Bus::Master, 0.6f);
            audio.SetBusMuted(audio::Bus::Ui, true);
            Settle(audio, 0.1f);
            float value = -1.0f;
            Check(globalValue("Bus_Music", value) && Near(value, 0.3f), "SetBusVolume(Music) sets Bus_Music");
            Check(globalValue("Bus_Master", value) && Near(value, 0.6f), "SetBusVolume(Master) sets Bus_Master");
            Check(globalValue("Bus_Ui", value) && Near(value, 0.0f), "a muted bus is 0");
            Check(Near(audio.GetBusVolume(audio::Bus::Music), 0.3f) && Near(audio.GetBusVolume(audio::Bus::Ui), 1.0f) && audio.IsBusMuted(audio::Bus::Ui) && !audio.IsBusMuted(audio::Bus::Sfx),
                  "GetBusVolume and IsBusMuted are what was set");
            audio.SetBusMuted(audio::Bus::Ui, false);
            audio.SetBusVolume(audio::Bus::Sfx, 2.5f);   // above 1: kept for the getter, the game parameter is 0..1
            Settle(audio, 0.1f);
            Check(globalValue("Bus_Ui", value) && Near(value, 1.0f), "unmuted: the volume is back");
            Check(globalValue("Bus_Sfx", value) && Near(value, 1.0f) && Near(audio.GetBusVolume(audio::Bus::Sfx), 2.5f), "above 1 the game parameter stops at 1");

            // a voice with a position: its game object is at that position
            audio::EventParams params;
            params.hasPosition = true;
            params.position = {10.0f, 2.0f, -5.0f};
            params.volumeScale = 0.5f;
            params.pitchScale = 1.5f;
            const audio::VoiceHandle voice = audio.PlayEvent("engine", params);
            Check(voice.IsValid(), "a looping event with a position plays");
            Settle(audio, 0.2f);
            const AkGameObjectID object = 100;   // the first game object of a voice
            const auto objectValue = [&](const char* name, float& out)
            {
                AK::SoundEngine::Query::RTPCValue_type type = AK::SoundEngine::Query::RTPCValue_GameObject;
                AkRtpcValue result = -1.0f;
                const AKRESULT status = AK::SoundEngine::Query::GetRTPCValue(AK::SoundEngine::GetIDFromString(name), object, AK_INVALID_PLAYING_ID, result, type);
                out = result;
                return status == AK_Success && type == AK::SoundEngine::Query::RTPCValue_GameObject;
            };
            const auto objectPosition = [&](float& x, float& y, float& z)
            {
                AkSoundPosition position;
                if (AK::SoundEngine::Query::GetPosition(object, position) != AK_Success)
                {
                    return false;
                }
                x = static_cast<float>(position.Position().X);
                y = static_cast<float>(position.Position().Y);
                z = static_cast<float>(position.Position().Z);
                return true;
            };
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            Check(objectPosition(x, y, z) && Near(x, 10.0f) && Near(y, 2.0f) && Near(z, -5.0f), "the game object of the voice is where the event was placed");
            Check(objectValue("Volume", value) && Near(value, 0.5f), "the volume scale is the game parameter Volume of the object");
            Check(objectValue("Pitch", value) && Near(value, 1.5f), "the pitch scale is the game parameter Pitch of the object");

            audio.SetVoicePosition(voice, {-3.0f, 1.0f, 8.0f}, {0.0f, 0.0f, 0.0f});
            audio.SetVoiceVolume(voice, 0.25f);
            audio.SetVoicePitch(voice, 2.0f);
            Settle(audio, 0.2f);
            Check(objectPosition(x, y, z) && Near(x, -3.0f) && Near(y, 1.0f) && Near(z, 8.0f), "SetVoicePosition moves the object");
            Check(objectValue("Volume", value) && Near(value, 0.5f), "SetVoiceVolume(engine, 0.25) is Volume 0.25 / 0.5 (the loudness of the event is 0.5)");
            Check(objectValue("Pitch", value) && Near(value, 2.0f), "SetVoicePitch sets Pitch");

            // the Doppler effect: a source that comes at the listener sounds higher
            audio.SetVoicePosition(voice, {0.0f, 0.0f, 50.0f}, {0.0f, 0.0f, -40.0f});
            Settle(audio, 0.1f);
            Check(objectValue("Pitch", value) && value > 2.0f * 1.05f, "a source that comes at the listener is higher than its pitch (Doppler)");
            audio.SetVoicePosition(voice, {0.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 40.0f});
            Settle(audio, 0.1f);
            Check(objectValue("Pitch", value) && value < 2.0f * 0.95f, "and lower when it goes away");
            audio.StopAll(0.0f);

            // a 3D event without a position sits at the listener and follows it
            audio::ListenerState listener;
            listener.position = {5.0f, 1.0f, 5.0f};
            audio.SetListener(listener);
            const audio::VoiceHandle own = audio.PlayEvent("engine");
            Settle(audio, 0.1f);
            const AkGameObjectID ownObject = 101;
            AkSoundPosition position;
            Check(AK::SoundEngine::Query::GetPosition(ownObject, position) == AK_Success && Near(static_cast<float>(position.Position().X), 5.0f), "an event without a position is at the listener");
            listener.position = {20.0f, 1.0f, 5.0f};
            audio.SetListener(listener);
            Settle(audio, 0.1f);
            Check(AK::SoundEngine::Query::GetPosition(ownObject, position) == AK_Success && Near(static_cast<float>(position.Position().X), 20.0f), "and follows it");
            AkSoundPosition listenerPosition;
            Check(AK::SoundEngine::Query::GetPosition(1, listenerPosition) == AK_Success && Near(static_cast<float>(listenerPosition.Position().X), 20.0f), "the listener of Wwise is the listener of the game");
            (void)own;
            audio.StopAll(0.0f);
        }
    }

    {
        // SetVoiceVolume is the volume of the voice, as in the XAudio2 build (where it replaces the volume of the event): the game parameter Volume is the asked
        // volume divided by the loudness that the project gives the event (the "volume" of the json: crowd_ambience 0.35, engine 0.5)
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            const auto objectVolume = [](const AkGameObjectID object, float& out)
            {
                AK::SoundEngine::Query::RTPCValue_type type = AK::SoundEngine::Query::RTPCValue_GameObject;
                AkRtpcValue value = -1.0f;
                const AKRESULT result = AK::SoundEngine::Query::GetRTPCValue(AK::SoundEngine::GetIDFromString("Volume"), object, AK_INVALID_PLAYING_ID, value, type);
                out = value;
                return result == AK_Success;
            };
            const audio::VoiceHandle crowd = audio.PlayEvent("crowd_ambience");   // the first voice: the game object 100
            const audio::VoiceHandle engine = audio.PlayEvent("engine");          // the second: 101
            Settle(audio, 0.1f);
            float value = -1.0f;
            Check(objectVolume(100, value) && Near(value, 1.0f), "a voice starts with the volume of its event (the game parameter 1)");
            audio.SetVoiceVolume(crowd, 1.0f);
            audio.SetVoiceVolume(engine, 0.7f);
            Settle(audio, 0.1f);
            Check(objectVolume(100, value) && Near(value, 1.0f / 0.35f, 0.02f), "SetVoiceVolume(crowd_ambience, 1) is Volume 1 / 0.35 (" + std::to_string(value) + ")");
            Check(objectVolume(101, value) && Near(value, 1.4f), "SetVoiceVolume(engine, 0.7) is Volume 1.4 (" + std::to_string(value) + ")");
            audio.SetVoiceVolume(crowd, 0.35f);
            audio.SetVoiceVolume(engine, 0.0f);
            Settle(audio, 0.1f);
            Check(objectVolume(100, value) && Near(value, 1.0f), "SetVoiceVolume(crowd_ambience, 0.35) is the volume of the project");
            Check(objectVolume(101, value) && Near(value, 0.0f), "and 0 is silence");
            audio.StopAll(0.0f);

            // The sound really is that much louder: the peak of the Sfx bus follows the volume that was asked for (the curve of the game parameter in the project goes to +12 dB, and
            // Wwise has to apply it, not to cut it at 0 dB)
            struct Meter
            {
                float peak = 0.0f;
            };
            static Meter meter;
            const AkBusMeteringCallbackFunc callback = [](AkBusMeteringCallbackInfo* info)
            {
                if (info == nullptr || info->pMetering == nullptr || info->pMetering->peak == nullptr)
                {
                    return;
                }
                for (AkUInt32 channel = 0; channel < info->channelConfig.uNumChannels; ++channel)
                {
                    meter.peak = std::max(meter.peak, info->pMetering->peak[channel]);
                }
            };
            const AkUniqueID busId = AK::SoundEngine::GetIDFromString("Sfx");
            const AKRESULT registered = AK::SoundEngine::RegisterBusMeteringCallback(busId, callback, AK_EnableBusMeter_Peak);
            Check(registered == AK_Success, "the peak of the Sfx bus can be metered (AKRESULT " + std::to_string(static_cast<int>(registered)) + ")");
            const audio::VoiceHandle loop = audio.PlayEvent("engine");
            const auto peakAt = [&](const float volume)
            {
                audio.SetVoiceVolume(loop, volume);
                Settle(audio, 0.2f);
                meter.peak = 0.0f;
                Settle(audio, 1.0f);
                return meter.peak;
            };
            // the engine loop is a steady sound (the noise of the stands has a different peak in every second): its peak follows the game parameter up to +12 dB
            const float quiet = peakAt(0.5f);   // the loudness of the event in the project: the parameter 1
            const float loud = peakAt(1.4f);    // the parameter 2.8: +8.9 dB
            const float full = peakAt(2.0f);    // the parameter 4: the end of the curve, +12 dB
            Check(quiet > 1e-3f, "the loop is heard on the Sfx bus (peak " + std::to_string(quiet) + ")");
            Check(quiet > 0.0f && loud / quiet > 2.3f && loud / quiet < 3.2f,
                  "a volume 2.8 times higher is that much louder, the positive dB are not cut (" + std::to_string(loud) + " / " + std::to_string(quiet) + " = " + std::to_string(quiet > 0.0f ? loud / quiet : 0.0f) + ")");
            Check(quiet > 0.0f && full / quiet > 3.3f && full / quiet < 4.7f, "and up to the end of the curve, 4 times (" + std::to_string(full / std::max(quiet, 1e-6f)) + ")");
            audio.StopAll(0.0f);
            Settle(audio, 0.1f);
            AK::SoundEngine::RegisterBusMeteringCallback(busId, nullptr, AK_EnableBusMeter_Peak);   // (a null callback unregisters)
        }
    }

    {
        // the pool: more sounds than voices; the ones of a higher priority are kept
        audio::AudioSystem audio;
        audio.SetLogger(&logger);
        audio::AudioConfig config;
        config.useDevice = false;
        config.monoVoices = 4;
        config.stereoVoices = 4;
        audio.Initialize(config);
        if (audio.LoadBank(source / "assets/data/audio_bank.json"))
        {
            for (int i = 0; i < 8; ++i)
            {
                Check(audio.PlayEvent("crowd_ambience").IsValid(), "a loop fills the pool");
            }
            Check(audio.GetStats().voicesActive == 8, "eight voices are active");
            const audio::VoiceHandle high = audio.PlayEvent("goal_horn");
            Check(high.IsValid() && audio.GetStats().voicesActive == 8 && audio.GetStats().stolen == 1, "a full pool gives the weakest voice to a sound of a higher priority");
            audio.StopAll(0.0f);
            Settle(audio, 0.2f);
            Check(audio.GetStats().voicesActive == 0, "the pool is empty again");

            // the engine loops (priority 6) are all stronger than the hum of the stands (priority 2): that one is dropped
            for (int i = 0; i < 8; ++i)
            {
                Check(audio.PlayEvent("engine").IsValid(), "an engine fills the pool");
            }
            const std::uint64_t dropped = audio.GetStats().dropped;
            const audio::VoiceHandle low = audio.PlayEvent("crowd_ambience");
            Check(!low.IsValid() && audio.GetStats().dropped == dropped + 1 && audio.GetStats().voicesActive == 8, "a sound weaker than all of them is dropped");
            audio.StopAll(0.0f);
        }
    }

    {
        // the limit of an event is the one of the Wwise project (ball_hit: 4 at once): the instances that Wwise ends early end their events too, so the slots come back
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            for (int i = 0; i < 7; ++i)
            {
                Check(audio.PlayEvent("ball_hit").IsValid(), "a hit of a series plays");
            }
            Settle(audio, 0.05f);
            const float elapsed = SecondsUntilFree(audio, {}, 4.0f);
            Check(audio.GetStats().voicesActive == 0, "every hit of the series is over, those that Wwise ended for its limit too (" + std::to_string(elapsed) + " s)");
        }
    }

    char* deviceFlag = nullptr;
    std::size_t deviceFlagLength = 0;
    const bool withDevice = _dupenv_s(&deviceFlag, &deviceFlagLength, "MYENGINE_WWISE_TEST_DEVICE") == 0 && deviceFlag != nullptr && deviceFlag[0] == '1';
    std::free(deviceFlag);
    if (withDevice)
    {
        // MYENGINE_WWISE_TEST_DEVICE=1: the real device of the machine and the real time (the engine thread of Wwise, the callbacks from it), silent: the master is 0
        audio::AudioSystem audio;
        audio.SetLogger(&logger);
        audio::AudioConfig config;
        config.useDevice = true;
        config.masterVolume = 0.0f;
        Check(audio.Initialize(config), "the audio system starts with the device");
        Check(audio.LoadBank(source / "assets/data/audio_bank.json"), "the banks load");
        audio.SetBusVolume(audio::Bus::Master, 0.0f);
        const audio::VoiceHandle loop = audio.PlayEvent("engine");
        const audio::VoiceHandle click = audio.PlayEvent("ui_click");
        const auto start = std::chrono::steady_clock::now();
        float seconds = 0.0f;
        while (seconds < 3.0f && audio.GetStats().voicesActive > 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            audio.Update(0.016f);
            seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        }
        std::printf("  device: %s, the click ended after %.2f s\n", audio.GetStats().deviceName.c_str(), seconds);
        Check(audio.IsPlaying(loop) && audio.GetStats().voicesActive == 1 && !audio.IsPlaying(click), "with the device the one-shot ends by the clock of Wwise, the loop goes on");
        audio.Stop(loop, 0.1f);
        for (int i = 0; i < 20; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            audio.Update(0.016f);
        }
        Check(audio.GetStats().voicesActive == 0, "and the loop stops");
    }

    // two starts and two stops in a row, with the banks
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        audio::AudioSystem audio;
        if (StartAudio(audio, logger, source))
        {
            Check(audio.PlayEvent("countdown_beep").IsValid(), "a cycle plays");
            Settle(audio, 0.3f);
        }
        audio.Shutdown();
        Check(!audio.PlayEvent("countdown_beep").IsValid() && !audio.HasEvent("countdown_beep"), "after Shutdown nothing plays");
    }

    std::printf(g_failures == 0 ? "audio_wwise: all passed\n" : "audio_wwise: FAILED\n");
    {
        // MYENGINE_WWISE_PROFILER=1: AudioSystem starts the link with the authoring (Debug and Profile libraries); Comm::Init and Comm::Term have run
        char* profiler = nullptr;
        std::size_t profilerLength = 0;
        const bool wanted = _dupenv_s(&profiler, &profilerLength, "MYENGINE_WWISE_PROFILER") == 0 && profiler != nullptr && profiler[0] == '1';
        std::free(profiler);
        if (wanted)
        {
            std::ifstream stream(logFile, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            const bool up = text.find("communication with the authoring started") != std::string::npos;
            std::printf("  profiler link with MYENGINE_WWISE_PROFILER=1: %s\n", up ? "up" : "not up (the Release libraries have none)");
        }
    }
    char* showLog = nullptr;
    std::size_t showLogLength = 0;
    const bool always = _dupenv_s(&showLog, &showLogLength, "MYENGINE_WWISE_TEST_LOG") == 0 && showLog != nullptr && showLog[0] == '1';
    std::free(showLog);
    if (g_failures != 0 || always)
    {
        std::ifstream stream(logFile, std::ios::binary);
        const std::string log((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::printf("--- log ---\n%s\n", log.c_str());
    }
    fs::remove_all(folder, error);
    return g_failures == 0 ? 0 : 1;
}
