// The audio module without a sound device (G7c): WAV reading, the 3D, the pool of voices and its stealing, the buses, fades,
// loops with pitch, the sound bank, and the sounds and the bank of the project. The simulated backend counts time only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/audio/AudioSpatial.h>
#include <myengine/audio/AudioSystem.h>
#include <myengine/audio/WavFile.h>

namespace
{
    using namespace myengine::audio;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const float a, const float b, const float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

    WavData Tone(const double seconds, const std::uint16_t channels = 1, const std::uint32_t rate = 44100)
    {
        WavData data;
        data.sampleRate = rate;
        data.channels = channels;
        const std::size_t frames = static_cast<std::size_t>(seconds * rate);
        data.samples.resize(frames * channels);
        for (std::size_t i = 0; i < frames; ++i)
        {
            const auto value = static_cast<std::int16_t>(12000.0 * std::sin(2.0 * 3.14159265 * 440.0 * static_cast<double>(i) / rate));
            for (std::uint16_t c = 0; c < channels; ++c)
            {
                data.samples[i * channels + c] = value;
            }
        }
        return data;
    }

#ifndef MYENGINE_WWISE   // the tests of the XAudio2 mixer: with Wwise the system is tested by audio_wwise (WwiseAudioTests.cpp)
    AudioConfig NoDevice(const std::uint32_t mono = 4, const std::uint32_t stereo = 2)
    {
        AudioConfig config;
        config.useDevice = false;
        config.monoVoices = mono;
        config.stereoVoices = stereo;
        return config;
    }
#endif

    // ---- WAV ----

    void TestWav()
    {
        const WavData original = Tone(0.01, 2, 22050);
        const std::vector<std::uint8_t> bytes = BuildWav(original);
        WavData parsed;
        std::string error;
        Check(ParseWav(bytes.data(), bytes.size(), parsed, error), "a 16 bit stereo file parses: " + error);
        Check(parsed.channels == 2 && parsed.sampleRate == 22050 && parsed.samples == original.samples, "the samples round-trip");
        Check(Near(static_cast<float>(parsed.Seconds()), 0.01f, 1e-4f), "the length in seconds");

        // 8 bit unsigned, float 32 and 24 bit are converted to 16 bit
        std::vector<std::uint8_t> eight = bytes;
        eight[20] = 1; eight[34] = 8;                      // PCM, 8 bits per sample
        eight[32] = 2;                                     // block align
        const std::size_t dataStart = 44;
        eight.resize(dataStart + 4);
        eight[40] = 4; eight[41] = 0; eight[42] = 0; eight[43] = 0;
        eight[dataStart] = 128; eight[dataStart + 1] = 255; eight[dataStart + 2] = 0; eight[dataStart + 3] = 192;
        Check(ParseWav(eight.data(), eight.size(), parsed, error) && parsed.samples.size() == 4 && parsed.samples[0] == 0 && parsed.samples[1] == 127 * 256 && parsed.samples[2] == -128 * 256,
            "8 bit: " + error);

        std::vector<std::uint8_t> floating = bytes;
        floating[20] = 3; floating[34] = 32;
        floating.resize(dataStart + 8);
        floating[40] = 8; floating[41] = 0; floating[42] = 0; floating[43] = 0;
        const float values[2] = {0.5f, -1.0f};
        std::memcpy(floating.data() + dataStart, values, 8);
        Check(ParseWav(floating.data(), floating.size(), parsed, error) && parsed.samples.size() == 2 && std::abs(parsed.samples[0] - 16383) <= 1 && parsed.samples[1] == -32767,
            "float 32: " + error);

        // what is refused says why
        Check(!ParseWav(reinterpret_cast<const std::uint8_t*>("NOTAWAVEFILE"), 12, parsed, error) && error.find("RIFF") != std::string::npos, "not RIFF");
        std::vector<std::uint8_t> three = bytes;
        three[22] = 3;
        Check(!ParseWav(three.data(), three.size(), parsed, error) && error.find("channels") != std::string::npos, "three channels are refused: " + error);
        std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + 30);
        Check(!ParseWav(cut.data(), cut.size(), parsed, error), "a truncated chunk is refused");
        std::vector<std::uint8_t> noData(bytes.begin(), bytes.begin() + 36);
        Check(!ParseWav(noData.data(), noData.size(), parsed, error) && error.find("data") != std::string::npos, "no data chunk: " + error);
    }

    // ---- 3D ----

    void TestSpatial()
    {
        ListenerState listener;                      // at the origin, looking along +z, up +y: right is +x
        PlayParams source;
        source.spatial = true;
        source.minDistance = 2.0f;
        source.maxDistance = 50.0f;

        source.position = {0.0f, 0.0f, 1.0f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 1.0f).gain, 1.0f), "inside the minimum distance: full volume");
        source.position = {0.0f, 0.0f, 4.0f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 1.0f).gain, 0.5f), "twice the minimum distance: half (rolloff 1)");
        source.position = {0.0f, 0.0f, 50.0f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 1.0f).gain, 0.0f), "at the maximum distance: silence");
        source.position = {0.0f, 0.0f, 60.0f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 1.0f).gain, 0.0f), "beyond it: silence");
        float previous = 2.0f;
        for (float d = 2.0f; d <= 50.0f; d += 1.0f)
        {
            source.position = {0.0f, 0.0f, d};
            const float gain = ComputeSpatial(listener, source, 343.0f, 1.0f).gain;
            Check(gain <= previous + 1e-5f, "the volume never grows with the distance (" + std::to_string(d) + " m)");
            previous = gain;
        }

        // pan: the side of the listener
        source.position = {10.0f, 0.0f, 0.0f};
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).pan > 0.95f, "a source on the right pans right");
        source.position = {-10.0f, 0.0f, 0.0f};
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).pan < -0.95f, "a source on the left pans left");
        source.position = {0.0f, 0.0f, 10.0f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 1.0f).pan, 0.0f), "a source ahead is in the middle");
        listener.forward = {1.0f, 0.0f, 0.0f};       // the listener turns right: the source that was ahead is on the left now
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).pan < -0.95f, "the pan follows the orientation of the listener");
        float left = 0.0f;
        float right = 0.0f;
        PanGains(0.0f, left, right);
        Check(Near(left, right) && Near(left * left + right * right, 1.0f), "equal power in the middle");
        PanGains(-1.0f, left, right);
        Check(Near(left, 1.0f) && Near(right, 0.0f), "all left");

        // Doppler
        listener = {};
        source.position = {0.0f, 0.0f, 30.0f};
        source.velocity = {0.0f, 0.0f, -34.3f};      // comes closer at a tenth of the speed of sound
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).doppler > 1.05f, "a source that comes closer sounds higher");
        source.velocity = {0.0f, 0.0f, 34.3f};
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).doppler < 0.95f, "a source that goes away sounds lower");
        source.velocity = {};
        listener.velocity = {0.0f, 0.0f, 34.3f};
        Check(ComputeSpatial(listener, source, 343.0f, 1.0f).doppler > 1.05f, "a listener that comes closer hears it higher");
        source.velocity = {0.0f, 0.0f, -34.3f};
        Check(Near(ComputeSpatial(listener, source, 343.0f, 0.0f).doppler, 1.0f), "no Doppler when the factor is 0");
    }

    // ---- the system ----

#ifndef MYENGINE_WWISE
    void TestVoicesAndLoops()
    {
        AudioSystem audio;
        Check(audio.Initialize(NoDevice()), "initializes without a device");
        Check(!audio.HasDevice() && audio.IsInitialized() && audio.VoiceCount() == 6, "no device, 4 mono + 2 stereo voices");

        const SoundHandle one = audio.CreateSound("one-second", Tone(1.0));
        Check(one.IsValid() && audio.CreateSound("one-second", Tone(1.0)) == one, "a sound is cached by its name");
        SoundInfo info;
        Check(audio.GetSoundInfo(one, info) && info.channels == 1 && Near(static_cast<float>(info.seconds), 1.0f) && info.bytes == 44100 * 2, "the sound info");
        Check(!audio.CreateSound("empty", WavData{}).IsValid(), "an empty sound is refused");

        const VoiceHandle voice = audio.Play(one);
        Check(voice.IsValid() && audio.IsPlaying(voice) && audio.ActiveVoiceCount() == 1, "a sound plays");
        audio.Update(0.5f);
        Check(audio.IsPlaying(voice), "half a second in: still playing");
        audio.Update(0.6f);
        Check(!audio.IsPlaying(voice) && audio.ActiveVoiceCount() == 0, "after its length the voice is free again");
        Check(audio.GetVoiceGain(voice) == 0.0f, "a handle of a finished voice is dead");
        audio.SetVoiceVolume(voice, 1.0f);
        audio.Stop(voice);

        // pitch changes the length: twice the pitch, half the time
        PlayParams fast;
        fast.pitch = 2.0f;
        const VoiceHandle quick = audio.Play(one, fast);
        audio.Update(0.55f);
        Check(!audio.IsPlaying(quick), "pitch 2 plays a one second sound in half a second");

        // a loop goes on, and its pitch follows on the fly
        PlayParams loop;
        loop.loop = true;
        const VoiceHandle looping = audio.Play(one, loop);
        for (int i = 0; i < 40; ++i)
        {
            audio.Update(0.25f);
        }
        Check(audio.IsPlaying(looping), "a loop plays for ever");
        audio.SetVoicePitch(looping, 1.5f);
        audio.SetVoiceVolume(looping, 0.25f);
        Check(Near(audio.GetVoiceGain(looping), 0.25f), "the volume is changed on the fly");
        audio.Stop(looping);
        Check(!audio.IsPlaying(looping) && audio.ActiveVoiceCount() == 0, "Stop frees the voice");

        // a fade out
        const VoiceHandle fading = audio.Play(one, loop);
        audio.Stop(fading, 0.5f);
        audio.Update(0.25f);
        Check(Near(audio.GetVoiceGain(fading), 0.5f, 0.02f), "halfway through a fade the volume is half");
        audio.Update(0.3f);
        Check(!audio.IsPlaying(fading), "the fade ends the voice");

        // stereo sounds use the stereo voices
        const SoundHandle stereo = audio.CreateSound("stereo", Tone(1.0, 2));
        PlayParams strongLoop = loop;
        strongLoop.priority = 5;
        PlayParams weakLoop = loop;
        weakLoop.priority = -1;
        Check(audio.Play(stereo, strongLoop).IsValid() && audio.Play(stereo, strongLoop).IsValid() && !audio.Play(stereo, weakLoop).IsValid(),
            "2 stereo voices: a third sound that is weaker than both is refused");
        audio.StopAll();
        Check(audio.ActiveVoiceCount() == 0, "StopAll");

        audio.UnloadSound(one);
        Check(!audio.Play(one).IsValid(), "an unloaded sound does not play");
        audio.Shutdown();
        Check(!audio.IsInitialized() && !audio.Play(one).IsValid(), "after Shutdown nothing plays");
    }

    void TestPoolStealing()
    {
        AudioSystem audio;
        audio.Initialize(NoDevice(4, 0));
        const SoundHandle sound = audio.CreateSound("loop", Tone(1.0));
        PlayParams params;
        params.loop = true;

        std::vector<VoiceHandle> voices;
        for (int i = 0; i < 4; ++i)
        {
            params.priority = i;                   // priorities 0, 1, 2, 3
            voices.push_back(audio.Play(sound, params));
            Check(voices.back().IsValid(), "the pool has room for four");
        }
        Check(audio.ActiveVoiceCount() == 4 && audio.GetStats().voicesPeak == 4, "four active");

        params.priority = -1;                      // weaker than all of them: nothing it may take
        Check(!audio.Play(sound, params).IsValid() && audio.GetStats().dropped == 1, "a sound that is weaker than every voice is dropped");
        params.priority = 2;
        const VoiceHandle strong = audio.Play(sound, params);
        Check(strong.IsValid() && audio.GetStats().stolen == 1, "a stronger sound takes the weakest voice");
        Check(!audio.IsPlaying(voices[0]) && audio.IsPlaying(voices[1]) && audio.IsPlaying(voices[3]), "the priority 0 voice went");
        Check(!audio.IsPlaying(voices[0]), "the stolen handle is dead for good (the generation changed)");

        // among the equal ones the quietest, then the oldest
        audio.StopAll();
        params.priority = 1;
        params.volume = 1.0f;
        const VoiceHandle loud = audio.Play(sound, params);
        params.volume = 0.2f;
        const VoiceHandle quiet = audio.Play(sound, params);
        params.volume = 1.0f;
        audio.Play(sound, params);
        audio.Play(sound, params);
        audio.Play(sound, params);
        Check(audio.IsPlaying(loud) && !audio.IsPlaying(quiet), "the quietest of the equal voices is the one that goes");
    }

    void TestBuses()
    {
        AudioSystem audio;
        audio.Initialize(NoDevice());
        const SoundHandle sound = audio.CreateSound("tone", Tone(1.0));
        PlayParams sfx;
        sfx.bus = Bus::Sfx;
        sfx.volume = 0.8f;
        PlayParams music;
        music.bus = Bus::Music;
        PlayParams ui;
        ui.bus = Bus::Ui;
        const VoiceHandle a = audio.Play(sound, sfx);
        const VoiceHandle b = audio.Play(sound, music);
        const VoiceHandle c = audio.Play(sound, ui);
        Check(Near(audio.GetVoiceGain(a), 0.8f) && Near(audio.GetVoiceGain(b), 1.0f) && Near(audio.GetVoiceGain(c), 1.0f), "the default volumes");

        audio.SetBusVolume(Bus::Sfx, 0.5f);
        audio.SetBusVolume(Bus::Master, 0.5f);
        Check(Near(audio.GetVoiceGain(a), 0.8f * 0.5f * 0.5f), "voice x bus x master");
        Check(Near(audio.GetVoiceGain(b), 0.5f) && Near(audio.GetVoiceGain(c), 0.5f), "the master reaches every bus");
        Check(Near(audio.GetBusVolume(Bus::Sfx), 0.5f) && Near(audio.GetBusVolume(Bus::Master), 0.5f), "the bus volumes read back");
        audio.SetBusMuted(Bus::Music, true);
        Check(audio.GetVoiceGain(b) == 0.0f && audio.IsBusMuted(Bus::Music) && audio.GetVoiceGain(c) > 0.0f, "a muted bus is silent, the others are not");
        audio.SetBusMuted(Bus::Music, false);
        audio.SetBusMuted(Bus::Master, true);
        Check(audio.GetVoiceGain(a) == 0.0f && audio.GetVoiceGain(c) == 0.0f, "a muted master silences all");
        audio.SetBusMuted(Bus::Master, false);
        audio.SetBusVolume(Bus::Sfx, 9.0f);
        Check(audio.GetBusVolume(Bus::Sfx) <= 4.0f, "a bus volume is limited");

        audio.StopBus(Bus::Ui);
        Check(!audio.IsPlaying(c) && audio.IsPlaying(a), "StopBus stops one bus");
    }

    void TestSpatialVoices()
    {
        AudioSystem audio;
        audio.Initialize(NoDevice());
        const SoundHandle sound = audio.CreateSound("tone", Tone(2.0));
        ListenerState listener;
        audio.SetListener(listener);
        PlayParams params;
        params.loop = true;
        params.spatial = true;
        params.minDistance = 2.0f;
        params.maxDistance = 40.0f;
        params.position = {0.0f, 0.0f, 4.0f};
        const VoiceHandle voice = audio.Play(sound, params);
        Check(Near(audio.GetVoiceGain(voice), 0.5f), "3D: twice the minimum distance, half the volume");
        audio.SetVoicePosition(voice, {0.0f, 0.0f, 40.0f});
        Check(Near(audio.GetVoiceGain(voice), 0.0f), "3D: the source moved to the limit: silent");
        audio.SetVoicePosition(voice, {0.0f, 0.0f, 1.0f});
        Check(Near(audio.GetVoiceGain(voice), 1.0f), "3D: close: full");
        listener.position = {0.0f, 0.0f, 20.0f};
        audio.SetListener(listener);
        Check(Near(audio.GetVoiceGain(voice), 2.0f / 19.0f, 2e-3f), "3D: the listener moved away (19 m)");
        audio.Update(0.016f);
        Check(audio.IsPlaying(voice), "the voice goes on after the update");
    }

    // ---- the bank ----

    void WriteFile(const fs::path& path, const std::vector<std::uint8_t>& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void TestBank(const fs::path& directory)
    {
        WriteFile(directory / "sounds/hit_a.wav", BuildWav(Tone(0.3)));
        WriteFile(directory / "sounds/hit_b.wav", BuildWav(Tone(0.3)));
        WriteFile(directory / "sounds/amb.wav", BuildWav(Tone(1.0, 2)));
        const nlohmann::json bank = {
            {"directory", "sounds"},
            {"events", {
                {"hit", {{"files", {"hit_a.wav", "hit_b.wav"}}, {"bus", "sfx"}, {"volume", 0.8}, {"pitchMin", 0.9}, {"pitchMax", 1.1}, {"spatial", true},
                         {"minDistance", 2}, {"maxDistance", 40}, {"maxInstances", 2}}},
                {"ambience", {{"file", "amb.wav"}, {"bus", "music"}, {"loop", true}, {"volume", 0.5}}},
                {"broken", {{"files", {"missing.wav"}}}}}}};
        const fs::path bankFile = directory / "data/bank.json";
        fs::create_directories(bankFile.parent_path());
        std::ofstream(bankFile) << bank.dump(2);

        AudioSystem audio;
        audio.Initialize(NoDevice(8, 2));
        Check(audio.LoadBank(bankFile), "the bank loads");
        Check(audio.HasEvent("hit") && audio.HasEvent("ambience") && !audio.HasEvent("broken"), "events with sounds that load stay; the broken one is skipped");
        Check(!audio.PlayEvent("nothing").IsValid(), "an unknown event plays nothing");

        EventParams at;
        at.hasPosition = true;
        at.position = {0.0f, 0.0f, 4.0f};
        const VoiceHandle first = audio.PlayEvent("hit", at);
        Check(first.IsValid(), "an event plays");
        Check(audio.GetVoiceGain(first) > 0.35f && audio.GetVoiceGain(first) < 0.45f, "event: volume 0.8 at twice the minimum distance = 0.4: " + std::to_string(audio.GetVoiceGain(first)));
        EventParams strong = at;
        strong.volumeScale = 0.5f;
        const VoiceHandle second = audio.PlayEvent("hit", strong);
        Check(audio.GetVoiceGain(second) < audio.GetVoiceGain(first) * 0.7f, "volumeScale scales the event");
        const VoiceHandle third = audio.PlayEvent("hit", at);
        Check(third.IsValid() && !audio.IsPlaying(first) && audio.IsPlaying(second), "maxInstances 2: the third hit takes the oldest voice");

        // not spatial without a position
        const VoiceHandle plain = audio.PlayEvent("hit");
        Check(plain.IsValid() && audio.GetVoiceGain(plain) > 0.7f && audio.GetVoiceGain(plain) < 0.9f, "no position: the event is not spatial");

        // the music: its bus, its loop
        const VoiceHandle music = audio.PlayEvent("ambience");
        audio.SetBusVolume(Bus::Music, 0.5f);
        Check(Near(audio.GetVoiceGain(music), 0.25f), "the event's bus decides its volume");
        for (int i = 0; i < 20; ++i)
        {
            audio.Update(0.25f);
        }
        Check(audio.IsPlaying(music), "the event loops");

        const SoundHandle a = audio.LoadSound(directory / "sounds/hit_a.wav");
        Check(a.IsValid(), "the sound of the bank is cached under its path");
        Check(audio.GetStats().soundsLoaded == 3, "three sounds loaded: " + std::to_string(audio.GetStats().soundsLoaded));
    }

    // ---- the sounds of the project ----

    void TestProjectSounds(const fs::path& source)
    {
        const fs::path manifestFile = source / "assets/audio/manifest.json";
        Check(fs::exists(manifestFile), "assets/audio/manifest.json exists (python tools/gen_rl_audio.py)");
        std::ifstream manifestStream(manifestFile);
        nlohmann::json manifest;
        manifestStream >> manifest;
        std::uint64_t total = 0;
        std::size_t count = 0;
        for (const auto& entry : manifest["sounds"])
        {
            const fs::path file = source / "assets/audio" / entry["file"].get<std::string>();
            WavData data;
            std::string error;
            Check(LoadWavFile(file, data, error), file.filename().string() + ": " + error);
            Check(data.sampleRate == 44100 && data.channels == entry["channels"].get<int>(), file.filename().string() + ": the format of the manifest");
            Check(std::fabs(data.Seconds() - entry["seconds"].get<double>()) < 0.002, file.filename().string() + ": the length of the manifest");
            std::int16_t peak = 0;
            for (const std::int16_t sample : data.samples)
            {
                peak = std::max<std::int16_t>(peak, static_cast<std::int16_t>(std::abs(static_cast<int>(sample))));
            }
            Check(peak > 3000 && peak < 32700, file.filename().string() + ": audible and not clipped (peak " + std::to_string(peak) + ")");
            if (entry["loop"].get<bool>())
            {
                // a loop is seamless: the jump from the last sample to the first is of the size of a step inside the sound
                double step = 0.0;
                for (std::size_t i = data.channels; i < std::min<std::size_t>(data.samples.size(), 20000); ++i)
                {
                    step += std::abs(static_cast<int>(data.samples[i]) - static_cast<int>(data.samples[i - data.channels]));
                }
                step /= static_cast<double>(std::min<std::size_t>(data.samples.size(), 20000) - data.channels);
                const double jump = std::abs(static_cast<int>(data.samples[0]) - static_cast<int>(data.samples[data.samples.size() - data.channels]));
                Check(jump < step * 6.0 + 200.0, file.filename().string() + ": the loop is seamless (jump " + std::to_string(jump) + ", step " + std::to_string(step) + ")");
            }
            total += fs::file_size(file);
            ++count;
        }
        Check(count >= 20, "the project has its sounds (" + std::to_string(count) + ")");
        Check(total <= 15ull * 1024 * 1024, "the sounds are within 15 MB");

        // the bank of the project: every event loads
        AudioSystem audio;
        audio.Initialize(NoDevice());
        Check(audio.LoadBank(source / "assets/data/audio_bank.json"), "audio_bank.json loads");
        std::ifstream bankStream(source / "assets/data/audio_bank.json");
        nlohmann::json bank;
        bankStream >> bank;
        for (const auto& entry : bank["events"].items())
        {
            Check(audio.HasEvent(entry.key()), "the event '" + entry.key() + "' of the bank loads (its sounds exist)");
        }
    }

    // ---- the real device, when there is one ----

    void TestDeviceSmoke()
    {
        AudioSystem audio;
        AudioConfig config;
        config.useDevice = true;
        Check(audio.Initialize(config), "initializes with the device wanted (or without one)");
        const SoundHandle sound = audio.CreateSound("silent-tone", Tone(0.2));
        PlayParams params;
        params.volume = 0.0f; // the test makes no noise
        const VoiceHandle voice = audio.Play(sound, params);
        Check(voice.IsValid(), "plays on " + audio.GetStats().deviceName);
        audio.Update(0.016f);
        audio.StopAll();
        audio.Shutdown();
        std::cout << "device smoke: " << (audio.HasDevice() ? "device" : "no device (simulated)") << "\n";
    }
#endif   // MYENGINE_WWISE
}

int main()
{
    try
    {
        [[maybe_unused]] const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        const fs::path scratch = fs::temp_directory_path() / "myengine_audio_tests";
        std::error_code ec;
        fs::remove_all(scratch, ec);

        TestWav();
        TestSpatial();
#ifndef MYENGINE_WWISE
        TestVoicesAndLoops();
        TestPoolStealing();
        TestBuses();
        TestSpatialVoices();
        TestBank(scratch);
        TestProjectSounds(source);
        TestDeviceSmoke();
#endif
        fs::remove_all(scratch, ec);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "audio tests passed\n";
    return 0;
}
