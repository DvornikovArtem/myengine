#include <myengine/core/AppPaths.h>

#include <string>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <myengine/core/ProjectContext.h>

namespace myengine::core::apppaths
{
    namespace fs = std::filesystem;

    namespace
    {
        std::wstring& ProductFolder()
        {
            static std::wstring name;
            return name;
        }

        fs::path EnvironmentDirectory(const wchar_t* name)
        {
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
            if (length > 0 && length < std::size(buffer))
            {
                return fs::path(buffer);
            }
            return {};
        }
    }

    void SetProductFolder(const wchar_t* name)
    {
        ProductFolder() = name != nullptr ? name : L"";
    }

    bool IsShipping()
    {
#ifdef MYENGINE_SHIPPING
        return true;
#else
        return false;
#endif
    }

    fs::path SourceRoot()
    {
#if defined(MYENGINE_SOURCE_DIR) && !defined(MYENGINE_SHIPPING)
        return fs::u8path(MYENGINE_SOURCE_DIR);
#else
        return {};
#endif
    }

    fs::path UserDataDirectory()
    {
        if (!ProductFolder().empty())
        {
            const fs::path base = EnvironmentDirectory(L"LOCALAPPDATA");
            return (base.empty() ? ProjectContext::ExecutableDirectory() : base) / ProductFolder();
        }
#ifdef MYENGINE_SHIPPING
        const fs::path base = EnvironmentDirectory(L"LOCALAPPDATA");
        return (base.empty() ? ProjectContext::ExecutableDirectory() : base) / L"RocketArena";
#else
        const fs::path base = EnvironmentDirectory(L"APPDATA");
        return (base.empty() ? ProjectContext::ExecutableDirectory() : base) / L"myengine";
#endif
    }

    fs::path LogDirectory()
    {
#ifdef MYENGINE_SHIPPING
        return UserDataDirectory() / L"logs";
#else
        return ProjectContext::ExecutableDirectory() / L"logs";
#endif
    }

    fs::path SavedDirectory(const fs::path& projectRoot)
    {
#ifdef MYENGINE_SHIPPING
        (void)projectRoot;
        return UserDataDirectory() / L"Saved";
#else
        return projectRoot / L"Saved";
#endif
    }

    fs::path AssetCacheDirectory()
    {
#ifdef MYENGINE_SHIPPING
        return UserDataDirectory() / L"Cache";
#else
        return {};
#endif
    }
}
