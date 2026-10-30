// DoomWad.cpp
//
// Where the game data comes from (docs/game/doom.md): the WAD the player names, the original DOOM they own (next to the exe or in Steam),
// or Freedoom. The originals are never copied or shipped.

#include <doom/DoomGame.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace doom
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool IsFreedoomName(const fs::path& path)
        {
            return Lower(path.filename().string()).find("freedoom") != std::string::npos;
        }

        bool IsFile(const fs::path& path)
        {
            std::error_code ec;
            return fs::is_regular_file(path, ec);
        }

        // The Steam folder of the player: HKCU\Software\Valve\Steam\SteamPath
        fs::path SteamRoot()
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return {};
            }
            wchar_t buffer[MAX_PATH * 2] = {};
            DWORD size = sizeof(buffer) - sizeof(wchar_t);
            DWORD type = 0;
            fs::path result;
            if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ)
            {
                result = fs::path(std::wstring(buffer));
            }
            RegCloseKey(key);
            return result;
        }

        // The libraries of Steam: the main folder and every "path" of steamapps/libraryfolders.vdf
        std::vector<fs::path> SteamLibraries()
        {
            std::vector<fs::path> libraries;
            const fs::path root = SteamRoot();
            if (root.empty())
            {
                return libraries;
            }
            libraries.push_back(root);
            std::ifstream vdf(root / "steamapps" / "libraryfolders.vdf");
            std::string line;
            while (std::getline(vdf, line))
            {
                const std::size_t key = line.find("\"path\"");
                if (key == std::string::npos)
                {
                    continue;
                }
                const std::size_t open = line.find('"', key + 6);
                const std::size_t close = open == std::string::npos ? open : line.find('"', open + 1);
                if (open == std::string::npos || close == std::string::npos)
                {
                    continue;
                }
                const std::string value = line.substr(open + 1, close - open - 1);
                std::string unescaped;
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
                    {
                        ++i;
                    }
                    unescaped += value[i];
                }
                const fs::path library(unescaped);
                if (std::find(libraries.begin(), libraries.end(), library) == libraries.end())
                {
                    libraries.push_back(library);
                }
            }
            return libraries;
        }

        // The game folders of Steam that hold the classic WAD files, and the folders inside them (the 2019 re-release has "rerelease")
        std::vector<fs::path> SteamCandidates()
        {
            static const char* const kGames[] = {"Ultimate Doom", "DOOM + DOOM II", "Doom 2", "DOOM II", "Final Doom", "Doom 3 BFG Edition"};
            static const char* const kFolders[] = {"rerelease", "base", "base/wads", "."};
            static const char* const kFiles[] = {"DOOM.WAD", "DOOM2.WAD", "PLUTONIA.WAD", "TNT.WAD"};
            std::vector<fs::path> out;
            for (const fs::path& library : SteamLibraries())
            {
                for (const char* game : kGames)
                {
                    for (const char* folder : kFolders)
                    {
                        for (const char* file : kFiles)
                        {
                            out.push_back(library / "steamapps" / "common" / game / folder / file);
                        }
                    }
                }
            }
            return out;
        }

        void Consider(const fs::path& path, std::vector<fs::path>& found, std::vector<std::string>& tried)
        {
            tried.push_back(path.string());
            if (IsFile(path) && std::find(found.begin(), found.end(), path) == found.end())
            {
                found.push_back(path);
            }
        }
    }

    namespace
    {
        // The game the file is: a full game (DOOM.WAD, DOOM2.WAD ...) is chosen before the shareware episode, and that before Freedoom
        int WadRank(const std::filesystem::path& path)
        {
            const std::string name = Lower(path.filename().string());
            if (name.find("freedoom") != std::string::npos)
            {
                return 2;
            }
            if (name == "doom1.wad")
            {
                return 1;
            }
            return 0;
        }
    }

    IwadSearchResult FindIwad(const std::filesystem::path& argument, const std::filesystem::path& exeDirectory,
                              const std::vector<std::filesystem::path>& userDirectories)
    {
        IwadSearchResult result;

        struct Candidate
        {
            fs::path path;
            IwadSource source;
        };
        std::vector<Candidate> candidates;   // in the order of the places; the stable sort by rank keeps it among the equal ones

        if (!argument.empty())
        {
            result.tried.push_back(argument.string());
            if (IsFile(argument))
            {
                // what the player named is the choice, whatever it is
                result.path = argument;
                result.source = IwadSource::Argument;
                result.isFreedoom = IsFreedoomName(argument);
            }
        }

        static const char* const kNames[] = {"DOOM.WAD", "DOOM2.WAD", "doom.wad", "doom2.wad", "plutonia.wad", "tnt.wad", "doom1.wad", "freedoom1.wad", "freedoom2.wad"};
        auto addFolder = [&](const fs::path& folder, const IwadSource source)
        {
            std::vector<fs::path> found;
            for (const char* name : kNames)
            {
                Consider(folder / name, found, result.tried);
            }
            for (const fs::path& path : found)
            {
                candidates.push_back({path, source});
            }
        };

        if (!exeDirectory.empty())
        {
            addFolder(exeDirectory / "assets" / "doom", IwadSource::NextToExe);
            addFolder(exeDirectory / "doom", IwadSource::NextToExe);
            addFolder(exeDirectory, IwadSource::NextToExe);
        }
        for (const fs::path& directory : userDirectories)
        {
            if (!directory.empty())
            {
                addFolder(directory, IwadSource::UserFolder);
            }
        }

        const std::vector<fs::path> steamCandidates = SteamCandidates();
        for (const fs::path& path : steamCandidates)
        {
            // Steam has many candidate paths: only a summary goes into the message
            if (IsFile(path))
            {
                candidates.push_back({path, IwadSource::Steam});
            }
        }
        result.tried.push_back(steamCandidates.empty() ? "Steam: not installed"
                                                        : "Steam: steamapps/common/Ultimate Doom/rerelease/DOOM.WAD and the other DOOM folders of the libraries");

        for (const wchar_t* variable : {L"DOOMWADDIR", L"DOOMWADPATH"})
        {
            wchar_t value[MAX_PATH * 2] = {};
            if (GetEnvironmentVariableW(variable, value, static_cast<DWORD>(sizeof(value) / sizeof(value[0]))) > 0)
            {
                addFolder(fs::path(value), IwadSource::Environment);
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return WadRank(a.path) < WadRank(b.path); });

        // The first one is the choice, the rest are what a menu can offer instead
        for (const Candidate& candidate : candidates)
        {
            if (result.path.empty())
            {
                result.path = candidate.path;
                result.source = candidate.source;
                result.isFreedoom = IsFreedoomName(candidate.path);
            }
            else if (candidate.path != result.path && std::find(result.alternatives.begin(), result.alternatives.end(), candidate.path) == result.alternatives.end())
            {
                result.alternatives.push_back(candidate.path);
            }
        }
        return result;
    }

    const char* IwadSourceName(const IwadSource source)
    {
        switch (source)
        {
            case IwadSource::Argument: return "the --iwad argument";
            case IwadSource::NextToExe: return "next to the exe";
            case IwadSource::UserFolder: return "the WAD folder of the user";
            case IwadSource::Steam: return "the Steam library";
            case IwadSource::Environment: return "DOOMWADDIR";
            case IwadSource::None: break;
        }
        return "nowhere";
    }

    std::string NoIwadMessage(const IwadSearchResult& result)
    {
        std::string message =
            "FlyDoom needs the game data (a WAD file) and found none.\n"
            "  - the free one: run tools\\doom\\fetch_freedoom.ps1 (it puts Freedoom into assets\\doom);\n"
            "  - the original DOOM, if you own it: put DOOM.WAD into assets\\doom next to the exe, or start FlyDoom.exe --iwad <path>\n"
            "    (the Steam copy is found by itself).\n"
            "Looked in:\n";
        for (const std::string& place : result.tried)
        {
            message += "  " + place + "\n";
        }
        return message;
    }
}
