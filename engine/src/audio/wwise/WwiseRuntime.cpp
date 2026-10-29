// WwiseRuntime.cpp
//
// The order of the start and of the stop follows the integration guide and the IntegrationDemo of the SDK (samples/IntegrationDemo/Common/IntegrationDemo.cpp).

#include "WwiseRuntime.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <myengine/core/Logger.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <AK/SoundEngine/Common/AkMemoryMgr.h>
#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>
#include <AK/SoundEngine/Common/IAkPlugin.h>
#include <AK/Tools/Common/AkMonitorError.h>
#include <AK/Tools/Common/AkPlatformFuncs.h>
#ifndef AK_OPTIMIZED
#include <AK/Comm/AkCommunication.h>
#endif

// The codecs of the banks. Static linking: the inclusion of the factory (once, in one file) and the library are all that registers them.
#include <AK/Plugin/AkOpusDecoderFactory.h>
#include <AK/Plugin/AkVorbisDecoderFactory.h>

// The low-level IO and the job workers of the samples of the SDK (cmake/Wwise.cmake: myengine_wwise_io)
#include "AkDefaultIOHookDeferred.h"
#include "AkJobWorkerMgr.h"

namespace myengine::audio::wwise
{
    namespace
    {
        // Wwise has one engine for the process
        std::atomic<bool> g_running{false};

        // The messages of the SDK go to the logger of the runtime that runs (Debug and Profile only: the optimised SDK has no monitor)
        std::atomic<core::Logger*> g_monitorLogger{nullptr};

        std::string ToUtf8(const wchar_t* text)
        {
            if (text == nullptr || *text == L'\0')
            {
                return {};
            }
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (size <= 1)
            {
                return {};
            }
            std::string result(static_cast<std::size_t>(size - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
            return result;
        }

        std::wstring ToWide(const std::string& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            if (size <= 1)
            {
                return {};
            }
            std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
            return result;
        }

        const char* ResultName(const AKRESULT result)
        {
            switch (result)
            {
                case AK_NotImplemented: return "AK_NotImplemented";
                case AK_Success: return "AK_Success";
                case AK_Fail: return "AK_Fail";
                case AK_PartialSuccess: return "AK_PartialSuccess";
                case AK_NotCompatible: return "AK_NotCompatible";
                case AK_AlreadyConnected: return "AK_AlreadyConnected";
                case AK_InvalidFile: return "AK_InvalidFile";
                case AK_AudioFileHeaderTooLarge: return "AK_AudioFileHeaderTooLarge";
                case AK_MaxReached: return "AK_MaxReached";
                case AK_InvalidID: return "AK_InvalidID";
                case AK_IDNotFound: return "AK_IDNotFound";
                case AK_InvalidInstanceID: return "AK_InvalidInstanceID";
                case AK_NoMoreData: return "AK_NoMoreData";
                case AK_InvalidStateGroup: return "AK_InvalidStateGroup";
                case AK_ChildAlreadyHasAParent: return "AK_ChildAlreadyHasAParent";
                case AK_InvalidLanguage: return "AK_InvalidLanguage";
                case AK_InvalidParameter: return "AK_InvalidParameter";
                case AK_ElementAlreadyInList: return "AK_ElementAlreadyInList";
                case AK_PathNotFound: return "AK_PathNotFound";
                case AK_PathNoVertices: return "AK_PathNoVertices";
                case AK_PathNotRunning: return "AK_PathNotRunning";
                case AK_PathNotPaused: return "AK_PathNotPaused";
                case AK_PathNodeAlreadyInList: return "AK_PathNodeAlreadyInList";
                case AK_PathNodeNotInList: return "AK_PathNodeNotInList";
                case AK_DataNeeded: return "AK_DataNeeded";
                case AK_NoDataNeeded: return "AK_NoDataNeeded";
                case AK_DataReady: return "AK_DataReady";
                case AK_NoDataReady: return "AK_NoDataReady";
                case AK_InsufficientMemory: return "AK_InsufficientMemory";
                case AK_Cancelled: return "AK_Cancelled";
                case AK_UnknownBankID: return "AK_UnknownBankID";
                case AK_BankReadError: return "AK_BankReadError";
                case AK_InvalidSwitchType: return "AK_InvalidSwitchType";
                case AK_FormatNotReady: return "AK_FormatNotReady";
                case AK_WrongBankVersion: return "AK_WrongBankVersion";
                case AK_FileNotFound: return "AK_FileNotFound";
                case AK_DeviceNotReady: return "AK_DeviceNotReady";
                case AK_BankAlreadyLoaded: return "AK_BankAlreadyLoaded";
                case AK_RenderedFX: return "AK_RenderedFX";
                case AK_ProcessNeeded: return "AK_ProcessNeeded";
                case AK_ProcessDone: return "AK_ProcessDone";
                case AK_MemManagerNotInitialized: return "AK_MemManagerNotInitialized";
                case AK_StreamMgrNotInitialized: return "AK_StreamMgrNotInitialized";
                case AK_SSEInstructionsNotSupported: return "AK_SSEInstructionsNotSupported";
                case AK_Busy: return "AK_Busy";
                case AK_UnsupportedChannelConfig: return "AK_UnsupportedChannelConfig";
                case AK_PluginMediaNotAvailable: return "AK_PluginMediaNotAvailable";
                case AK_MustBeVirtualized: return "AK_MustBeVirtualized";
                case AK_CommandTooLarge: return "AK_CommandTooLarge";
                case AK_RejectedByFilter: return "AK_RejectedByFilter";
                case AK_InvalidCustomPlatformName: return "AK_InvalidCustomPlatformName";
                case AK_DLLCannotLoad: return "AK_DLLCannotLoad";
                case AK_DLLPathNotFound: return "AK_DLLPathNotFound";
                case AK_NoJavaVM: return "AK_NoJavaVM";
                case AK_OpenSLError: return "AK_OpenSLError";
                case AK_PluginNotRegistered: return "AK_PluginNotRegistered";
                case AK_DataAlignmentError: return "AK_DataAlignmentError";
                case AK_DeviceNotCompatible: return "AK_DeviceNotCompatible";
                case AK_DuplicateUniqueID: return "AK_DuplicateUniqueID";
                case AK_InitBankNotLoaded: return "AK_InitBankNotLoaded";
                case AK_DeviceNotFound: return "AK_DeviceNotFound";
                case AK_PlayingIDNotFound: return "AK_PlayingIDNotFound";
                case AK_InvalidFloatValue: return "AK_InvalidFloatValue";
                case AK_FileFormatMismatch: return "AK_FileFormatMismatch";
                case AK_NoDistinctListener: return "AK_NoDistinctListener";
                case AK_ResourceInUse: return "AK_ResourceInUse";
                case AK_InvalidBankType: return "AK_InvalidBankType";
                case AK_AlreadyInitialized: return "AK_AlreadyInitialized";
                case AK_NotInitialized: return "AK_NotInitialized";
                case AK_FilePermissionError: return "AK_FilePermissionError";
                case AK_UnknownFileError: return "AK_UnknownFileError";
                default: return "an unknown result";
            }
        }

#ifndef AK_OPTIMIZED
        void MonitorMessage(AK::Monitor::ErrorCode, const AkOSChar* text, AK::Monitor::ErrorLevel level, AkPlayingID, AkGameObjectID)
        {
            core::Logger* logger = g_monitorLogger.load();
            if (logger == nullptr)
            {
                return;
            }
            const std::string line = "Wwise: " + ToUtf8(text);
            if ((level & AK::Monitor::ErrorLevel_Error) != 0)
            {
                logger->Error(line);
            }
            else
            {
                logger->Debug(line);
            }
        }
#endif
    }

    struct WwiseRuntime::Impl
    {
        std::unique_ptr<CAkDefaultIOHookDeferred> io;
        AK::JobWorkerMgr::InitSettings jobWorkers;
        bool memory = false;
        bool streams = false;
        bool device = false;       // the streaming device of the IO hook
        bool workers = false;
        bool engine = false;
        bool communication = false;
    };

    WwiseRuntime::WwiseRuntime(core::Logger* logger) : logger_(logger) {}

    WwiseRuntime::~WwiseRuntime()
    {
        Shutdown();
    }

    bool WwiseRuntime::Initialize(const std::filesystem::path& bankDirectory, const bool withDevice, const bool withCommunication)
    {
        if (initialized_)
        {
            return true;
        }
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true))
        {
            if (logger_ != nullptr)
            {
                logger_->Error("Wwise: another WwiseRuntime is running, the sound engine of Wwise is one for the process");
            }
            return false;
        }

        impl_ = std::make_unique<Impl>();
        const auto fail = [this](const char* step, const AKRESULT result)
        {
            if (logger_ != nullptr)
            {
                logger_->Error(std::string("Wwise: ") + step + " failed: " + ResultName(result) + " (" + std::to_string(static_cast<int>(result)) + ")");
            }
            initialized_ = true;   // so that Shutdown stops what has started
            Shutdown();
            return false;
        };

        // 1. the Memory Manager
        AkMemSettings memorySettings;
        AK::MemoryMgr::GetDefaultSettings(memorySettings);
        AKRESULT result = AK::MemoryMgr::Init(&memorySettings);
        if (result != AK_Success)
        {
            return fail("the Memory Manager", result);
        }
        impl_->memory = true;

        // 2. the Stream Manager and its streaming device with the low-level IO that reads the files of the banks
        AkStreamMgrSettings streamSettings;
        AK::StreamMgr::GetDefaultSettings(streamSettings);
        if (AK::StreamMgr::Create(streamSettings) == nullptr)
        {
            return fail("the Stream Manager", AK_Fail);
        }
        impl_->streams = true;

        impl_->io = std::make_unique<CAkDefaultIOHookDeferred>();
        AkDeviceSettings deviceSettings;
        AK::StreamMgr::GetDefaultDeviceSettings(deviceSettings);
        deviceSettings.bUseStreamCache = true;
        result = impl_->io->Init(deviceSettings);
        if (result != AK_Success)
        {
            return fail("the low-level IO", result);
        }
        impl_->device = true;
        if (!bankDirectory.empty())
        {
            std::error_code error;
            if (!std::filesystem::is_directory(bankDirectory, error) && logger_ != nullptr)
            {
                logger_->Warning("Wwise: the folder of the banks does not exist: " + bankDirectory.generic_u8string());
            }
            impl_->io->SetBasePath(bankDirectory.c_str());
        }

        // 3. the job workers (two threads: the machine of the developer does not like more)
        AK::JobWorkerMgr::GetDefaultInitSettings(impl_->jobWorkers);
        impl_->jobWorkers.uNumWorkerThreads = 2;
        result = AK::JobWorkerMgr::InitWorkers(impl_->jobWorkers);
        if (result != AK_Success)
        {
            return fail("the job workers", result);
        }
        impl_->workers = true;

        // 4. the Sound Engine (the music engine is part of it, Spatial Audio is not used)
        AkInitSettings initSettings;
        AK::SoundEngine::GetDefaultInitSettings(initSettings);
        initSettings.settingsJobManager = impl_->jobWorkers.GetJobMgrSettings();
        for (AkUInt32 jobType = 0; jobType < AK_NUM_JOB_TYPES; ++jobType)
        {
            initSettings.settingsJobManager.uMaxActiveWorkers[jobType] = 2;
        }
        offline_ = !withDevice;
        if (offline_)
        {
            // Nothing is played and nothing runs by itself: the time of the engine is what RenderAudio(seconds) renders, on the thread that calls it (so the
            // end of an event comes inside that call, and the tests do not wait for the clock). The main output of the project is mixed but not sent anywhere.
            initSettings.bOfflineRendering = true;
            initSettings.bUseLEngineThread = false;
        }
        AkPlatformInitSettings platformSettings;
        AK::SoundEngine::GetDefaultPlatformInitSettings(platformSettings);
        result = AK::SoundEngine::Init(&initSettings, &platformSettings);
        if (result != AK_Success)
        {
            return fail("the Sound Engine", result);
        }
        impl_->engine = true;

#ifndef AK_OPTIMIZED
        g_monitorLogger.store(logger_);
        AK::Monitor::SetLocalOutput(AK::Monitor::ErrorLevel_All, MonitorMessage);

        // 5. the communication with the authoring (Profiler, Remote Connect): without it the game still sounds
        if (withCommunication)
        {
            AkCommSettings commSettings;
            AK::Comm::GetDefaultInitSettings(commSettings);
            AKPLATFORM::SafeStrCpy(commSettings.szAppNetworkName, "myengine", AK_COMM_SETTINGS_MAX_STRING_SIZE);
            result = AK::Comm::Init(commSettings);
            if (result == AK_Success)
            {
                impl_->communication = true;
                if (logger_ != nullptr)
                {
                    logger_->Info("Wwise: the communication with the authoring started (Profiler, Remote Connect)");
                }
            }
            else if (logger_ != nullptr)
            {
                logger_->Warning(std::string("Wwise: the communication with the authoring did not start (") + ResultName(result) + "), the profiler and Remote Connect are not available");
            }
        }
#else
        (void)withCommunication;
#endif

        initialized_ = true;
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("Wwise: started (") + (withDevice ? "audio device" : "offline") + ", banks: " + bankDirectory.generic_u8string() + ")");
        }
        return true;
    }

    void WwiseRuntime::Shutdown()
    {
        if (!initialized_ || impl_ == nullptr)
        {
            return;
        }
        if (logger_ != nullptr)
        {
            logger_->Info("Wwise: memory in use at the stop: " + std::to_string(MemoryUsed()) + " bytes (0: the Release libraries do not count)");
        }
        initialized_ = false;
        offline_ = false;

        // the other way round
#ifndef AK_OPTIMIZED
        if (impl_->communication)
        {
            AK::Comm::Term();
        }
        AK::Monitor::SetLocalOutput(0, nullptr);
        g_monitorLogger.store(nullptr);
#endif
        if (impl_->engine && AK::SoundEngine::IsInitialized())
        {
            AK::SoundEngine::Term();
        }
        if (impl_->workers)
        {
            AK::JobWorkerMgr::TermWorkers();
        }
        if (impl_->device && impl_->io != nullptr)
        {
            impl_->io->Term();   // destroys its streaming device and stops being the file location resolver
        }
        impl_->io.reset();
        if (impl_->streams && AK::IAkStreamMgr::Get() != nullptr)
        {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        if (impl_->memory && AK::MemoryMgr::IsInitialized())
        {
            AK::MemoryMgr::Term();
        }
        impl_.reset();
        g_running.store(false);
        if (logger_ != nullptr)
        {
            logger_->Info("Wwise: stopped");
        }
    }

    std::uint64_t WwiseRuntime::MemoryUsed() const
    {
#ifndef AK_OPTIMIZED
        if (initialized_ && AK::MemoryMgr::IsInitialized())
        {
            AK::MemoryMgr::GlobalStats stats{};
            AK::MemoryMgr::GetGlobalStats(stats);
            return stats.uUsed;
        }
#endif
        return 0;
    }

    bool WwiseRuntime::CommunicationRunning() const
    {
        return initialized_ && impl_ != nullptr && impl_->communication;
    }

    void WwiseRuntime::RenderAudio(const float seconds)
    {
        if (!initialized_)
        {
            return;
        }
        if (offline_)
        {
            AK::SoundEngine::SetOfflineRenderingFrameTime(seconds > 0.0f ? seconds : 0.0f);
        }
        AK::SoundEngine::RenderAudio();
    }

    bool WwiseRuntime::SetBankDirectory(const std::filesystem::path& bankDirectory)
    {
        if (!initialized_ || impl_ == nullptr || impl_->io == nullptr)
        {
            return false;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(bankDirectory, error) && logger_ != nullptr)
        {
            logger_->Warning("Wwise: the folder of the banks does not exist: " + bankDirectory.generic_u8string());
        }
        return impl_->io->SetBasePath(bankDirectory.c_str()) == AK_Success;
    }

    namespace
    {
        // the answer of the bank manager to an asynchronous load
        struct BankLoadResult
        {
            std::atomic<bool> done{false};
            AKRESULT result = AK_Fail;
        };

        void BankLoaded(AkUInt32, const void*, const AKRESULT result, void* cookie)
        {
            auto* answer = static_cast<BankLoadResult*>(cookie);
            answer->result = result;
            answer->done.store(true);
        }
    }

    bool WwiseRuntime::LoadBank(const std::string& name)
    {
        if (!initialized_)
        {
            return false;
        }
        const std::wstring wide = ToWide(name);
        // Not the synchronous LoadBank: a bank that cannot be read to its end (a file that is cut) made it wait for the sound engine, which without its own thread
        // (offline) works only inside RenderAudio of this very thread, so the call never returned. The asynchronous one is asked and waited for while the sound
        // engine is rendered, so the answer always comes (or the time is out: an error, not a hang).
        // (on the heap: when the time is out the callback may still come, and it must find its answer alive; that one is then not freed, a few bytes)
        auto answer = std::make_unique<BankLoadResult>();
        AkBankID bankId = 0;
        AKRESULT result = AK::SoundEngine::LoadBank(wide.c_str(), &BankLoaded, answer.get(), bankId);
        if (result == AK_Success)
        {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!answer->done.load())
            {
                if (std::chrono::steady_clock::now() > limit)
                {
                    if (logger_ != nullptr)
                    {
                        logger_->Error("Wwise: the bank '" + name + "' did not answer in 20 s: given up");
                    }
                    answer.release();
                    return false;
                }
                AK::SoundEngine::ProcessBanks();
                RenderAudio(0.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            result = answer->result;
        }
        if (result == AK_BankAlreadyLoaded)
        {
            return true;   // (the same bank loaded again: Wwise counts the loads)
        }
        if (result != AK_Success)
        {
            if (logger_ != nullptr)
            {
                logger_->Error("Wwise: the bank '" + name + "' did not load: " + ResultName(result) + " (" + std::to_string(static_cast<int>(result)) + ")");
            }
            return false;
        }
        return true;
    }
}
