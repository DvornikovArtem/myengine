// GpuCapture.cpp

#include <myengine/render/GpuCapture.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <vector>

#include <renderdoc_app.h>

namespace myengine::render
{
    namespace
    {
        namespace fs = std::filesystem;

        RENDERDOC_API_1_6_0* Api(void* api)
        {
            return static_cast<RENDERDOC_API_1_6_0*>(api);
        }

        std::string ToUtf8(const std::wstring& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        std::wstring EnvironmentValue(const wchar_t* name)
        {
            wchar_t buffer[MAX_PATH * 2]{};
            const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
            if (length == 0 || length >= std::size(buffer))
            {
                return {};
            }
            return std::wstring(buffer, length);
        }

        // Where renderdoc.dll may be, most likely first
        std::vector<fs::path> RenderDocCandidates()
        {
            std::vector<fs::path> candidates;
            if (const std::wstring explicitPath = EnvironmentValue(L"RENDERDOC_DLL"); !explicitPath.empty())
            {
                fs::path path(explicitPath);
                candidates.push_back(fs::is_directory(path) ? path / L"renderdoc.dll" : path);
            }
            for (const wchar_t* variable : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"})
            {
                if (const std::wstring base = EnvironmentValue(variable); !base.empty())
                {
                    candidates.push_back(fs::path(base) / L"RenderDoc" / L"renderdoc.dll");
                }
            }
            wchar_t found[MAX_PATH]{};
            if (SearchPathW(nullptr, L"renderdoc.dll", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr) > 0)
            {
                candidates.push_back(fs::path(found));
            }
            return candidates;
        }
    }

    GpuCapture::GpuCapture() = default;
    GpuCapture::~GpuCapture() = default;

    const char* GpuCapture::InstallHint()
    {
        return "RenderDoc is not installed or not found. Install it with `winget install BaldurKarlsson.RenderDoc` "
               "(or set RENDERDOC_DLL to the path of renderdoc.dll) and start the engine again.";
    }

    bool GpuCapture::LoadRenderDoc(std::string& message)
    {
        if (api_ != nullptr)
        {
            message = "RenderDoc is loaded";
            return true;
        }

        HMODULE module = GetModuleHandleW(L"renderdoc.dll"); // started from RenderDoc: it is injected already
        std::string source = "injected by RenderDoc";
        if (module == nullptr)
        {
            for (const fs::path& candidate : RenderDocCandidates())
            {
                std::error_code error;
                if (!fs::exists(candidate, error))
                {
                    continue;
                }
                module = LoadLibraryW(candidate.c_str());
                if (module != nullptr)
                {
                    source = ToUtf8(candidate.wstring());
                    break;
                }
            }
        }
        if (module == nullptr)
        {
            message = InstallHint();
            return false;
        }

        const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        void* api = nullptr;
        if (getApi == nullptr || getApi(eRENDERDOC_API_Version_1_6_0, &api) != 1 || api == nullptr)
        {
            message = "renderdoc.dll was found (" + source + ") but it does not provide the in-application API 1.6; update RenderDoc";
            return false;
        }

        api_ = api;
        int major = 0;
        int minor = 0;
        int patch = 0;
        Api(api_)->GetAPIVersion(&major, &minor, &patch);
        message = "RenderDoc " + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + " (" + source + ")";
        return true;
    }

    bool GpuCapture::LoadPix(std::string& message)
    {
        if (pixLoaded_)
        {
            message = "PIX capturer is loaded";
            return true;
        }
        if (GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr)
        {
            pixLoaded_ = true;
            message = "PIX capturer is loaded (started from PIX)";
            return true;
        }

        std::wstring base = EnvironmentValue(L"ProgramFiles");
        if (base.empty())
        {
            base = L"C:\\Program Files";
        }
        const fs::path root = fs::path(base) / L"Microsoft PIX";
        std::error_code error;
        if (!fs::is_directory(root, error))
        {
            message = "PIX is not installed (winget install Microsoft.PIX)";
            return false;
        }

        // The newest version folder, as PIXLoadLatestWinPixGpuCapturerLibrary does
        fs::path newest;
        for (const auto& entry : fs::directory_iterator(root, error))
        {
            if (entry.is_directory() && (newest.empty() || entry.path().filename() > newest.filename()))
            {
                newest = entry.path();
            }
        }
        const fs::path dll = newest / L"WinPixGpuCapturer.dll";
        if (newest.empty() || !fs::exists(dll, error) || LoadLibraryW(dll.c_str()) == nullptr)
        {
            message = "PIX is installed but WinPixGpuCapturer.dll could not be loaded";
            return false;
        }

        pixLoaded_ = true;
        message = "PIX capturer loaded (" + ToUtf8(dll.wstring()) + "): attach PIX to this process to capture";
        return true;
    }

    void GpuCapture::SetOutput(const fs::path& directory, const std::string& name)
    {
        directory_ = directory;
        if (api_ == nullptr)
        {
            return;
        }

        std::error_code error;
        fs::create_directories(directory, error);

        // RenderDoc appends _frameN.rdc to the template
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        const fs::path captureTemplate = directory / (name + "_" + stamp);
        Api(api_)->SetCaptureFilePathTemplate(captureTemplate.u8string().c_str());
    }

    bool GpuCapture::TriggerNextFrame(std::string& message)
    {
        if (api_ == nullptr)
        {
            message = InstallHint();
            return false;
        }
        Api(api_)->TriggerCapture();
        message = "GPU capture of the next frame requested";
        return true;
    }

    bool GpuCapture::IsCapturing() const
    {
        return api_ != nullptr && Api(api_)->IsFrameCapturing() != 0;
    }

    std::uint32_t GpuCapture::CaptureCount() const
    {
        return api_ != nullptr ? Api(api_)->GetNumCaptures() : 0;
    }

    std::string GpuCapture::LastCapturePath() const
    {
        if (api_ == nullptr)
        {
            return {};
        }
        const std::uint32_t count = Api(api_)->GetNumCaptures();
        if (count == 0)
        {
            return {};
        }

        std::uint32_t length = 0;
        if (Api(api_)->GetCapture(count - 1, nullptr, &length, nullptr) == 0 || length == 0)
        {
            return {};
        }
        std::string path(length, '\0');
        if (Api(api_)->GetCapture(count - 1, path.data(), &length, nullptr) == 0)
        {
            return {};
        }
        while (!path.empty() && path.back() == '\0')
        {
            path.pop_back();
        }
        return path;
    }
}
