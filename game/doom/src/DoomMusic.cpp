// DoomMusic.cpp
//
// The music of DOOM: the MIDI made from a MUS lump goes to the OPL emulation (opl_render.c), which renders the whole song to PCM on a worker
// thread (a song takes a fraction of a second to a few seconds); Update() hands the finished song to the host, which loops it. A new song,
// or Stop, cancels the one that is still being rendered.

#include "DoomMusic.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "OplRender.h"

namespace doom
{
    namespace
    {
        constexpr unsigned int kOplRate = 49716;   // the native rate of the chip
        constexpr int kMaxSongSeconds = 600;
    }

    struct DoomMusic::Impl
    {
        struct Job
        {
            std::vector<unsigned char> midi;
            bool loop = false;
            std::shared_ptr<std::atomic<int>> cancel;
        };

        struct Result
        {
            std::vector<std::int16_t> pcm;   // stereo
            unsigned int rate = kOplRate;
            bool loop = false;
        };

        IDoomAudioHost& host;
        std::map<int, std::vector<unsigned char>> songs;
        int nextHandle = 1;
        bool instrumentsOk = false;
        bool playing = false;                // a song is asked for: it renders, or it plays

        // the worker
        std::thread worker;
        std::mutex mutex;
        std::condition_variable wake;
        bool quit = false;
        bool hasJob = false;
        Job job;
        std::shared_ptr<std::atomic<int>> currentCancel;   // of the job that runs or waits
        bool hasResult = false;
        Result result;
        std::uint64_t jobId = 0;
        std::uint64_t resultId = 0;
        std::uint64_t wantedId = 0;

        explicit Impl(IDoomAudioHost& h) : host(h) {}

        void Run()
        {
            for (;;)
            {
                Job mine;
                std::uint64_t id = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [this] { return quit || hasJob; });
                    if (quit)
                    {
                        return;
                    }
                    mine = std::move(job);
                    hasJob = false;
                    id = jobId;
                }
                short* pcm = nullptr;
                unsigned int rate = kOplRate;
                static_assert(sizeof(std::atomic<int>) == sizeof(int), "the renderer reads the flag as a plain int");
                const long frames = oplr_render(mine.midi.data(), static_cast<int>(mine.midi.size()), &pcm, kMaxSongSeconds, &rate,
                                                reinterpret_cast<const volatile int*>(mine.cancel.get()));
                Result done;
                if (frames > 0 && pcm != nullptr && mine.cancel->load() == 0)
                {
                    done.pcm.assign(pcm, pcm + static_cast<std::size_t>(frames) * 2);
                    done.rate = rate;
                    done.loop = mine.loop;
                }
                oplr_free(pcm);
                std::lock_guard<std::mutex> lock(mutex);
                if (!done.pcm.empty())
                {
                    result = std::move(done);
                    hasResult = true;
                    resultId = id;
                }
            }
        }
    };

    DoomMusic::DoomMusic(IDoomAudioHost& host) : impl_(std::make_unique<Impl>(host)) {}

    DoomMusic::~DoomMusic()
    {
        if (impl_->worker.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->quit = true;
                if (impl_->currentCancel)
                {
                    impl_->currentCancel->store(1);
                }
            }
            impl_->wake.notify_all();
            impl_->worker.join();
        }
    }

    void DoomMusic::SetInstruments(const unsigned char* genmidi, const int length)
    {
        if (!impl_->instrumentsOk)
        {
            impl_->instrumentsOk = oplr_start(genmidi, length, kOplRate) != 0;
        }
    }

    int DoomMusic::Register(const unsigned char* midi, const std::size_t length)
    {
        const int handle = impl_->nextHandle++;
        impl_->songs[handle].assign(midi, midi + length);
        return handle;
    }

    void DoomMusic::Unregister(const int handle)
    {
        impl_->songs.erase(handle);
    }

    void DoomMusic::Play(const int handle, const bool loop)
    {
        Stop();
        const auto it = impl_->songs.find(handle);
        if (it == impl_->songs.end() || !impl_->instrumentsOk)
        {
            return;
        }
        if (!impl_->worker.joinable())
        {
            impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->job.midi = it->second;
            impl_->job.loop = loop;
            impl_->job.cancel = std::make_shared<std::atomic<int>>(0);
            impl_->currentCancel = impl_->job.cancel;
            impl_->hasJob = true;
            impl_->hasResult = false;
            impl_->wantedId = ++impl_->jobId;
        }
        impl_->wake.notify_all();
        impl_->playing = true;
    }

    void DoomMusic::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->currentCancel)
            {
                impl_->currentCancel->store(1);   // a song that is still rendering is dropped
            }
            impl_->hasJob = false;
            impl_->hasResult = false;
            impl_->wantedId = 0;
        }
        impl_->host.StopMusic();
        impl_->playing = false;
    }

    void DoomMusic::Pause()
    {
        impl_->host.PauseMusic();
    }

    void DoomMusic::Resume()
    {
        impl_->host.ResumeMusic();
    }

    void DoomMusic::SetVolume(const float volume)
    {
        impl_->host.SetMusicVolume(volume);
    }

    bool DoomMusic::IsPlaying() const
    {
        return impl_->playing;
    }

    void DoomMusic::Update()
    {
        Impl::Result ready;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->hasResult || impl_->resultId != impl_->wantedId)
            {
                return;
            }
            ready = std::move(impl_->result);
            impl_->hasResult = false;
        }
        impl_->host.PlayMusic(ready.pcm.data(), ready.pcm.size() / 2, static_cast<int>(ready.rate), ready.loop);
    }
}
