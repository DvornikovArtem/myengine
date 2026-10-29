// AppPaths.h
// Where the application reads the sources and writes its own files. A development build reads the repository the
// executable was built from and keeps its files next to the executable and in %APPDATA%\myengine; a shipping build
// (MYENGINE_SHIPPING, docs/game/packaging.md) knows no repository and writes only to %LOCALAPPDATA%\RocketArena.

#pragma once

#include <filesystem>

namespace myengine::core::apppaths
{
    // True in a shipping build
    bool IsShipping();

    // The sources the executable was built from (MYENGINE_SOURCE_DIR); empty in a shipping build
    std::filesystem::path SourceRoot();

    // Another product of the same code (FlyDoom.exe, docs/game/doom.md) keeps its files in %LOCALAPPDATA%\<name>, shipping or not. Call it first thing in
    // main, before anything asks for a folder.
    void SetProductFolder(const wchar_t* name);

    // Per-user folder of settings: %LOCALAPPDATA%\RocketArena (shipping) or %APPDATA%\myengine (development), or %LOCALAPPDATA%\<name> after SetProductFolder.
    // The folder is not created here. Falls back to the executable directory when the variable is not set.
    std::filesystem::path UserDataDirectory();

    // Log folder: <UserData>\logs (shipping) or <exe>\logs (development)
    std::filesystem::path LogDirectory();

    // Generated files of a run (thumbnails, GPU captures, frame statistics): <UserData>\Saved (shipping) or
    // <project>\Saved (development)
    std::filesystem::path SavedDirectory(const std::filesystem::path& projectRoot);

    // Binary caches of the models and textures (.myemesh / .myetex): <UserData>\Cache (shipping); empty in a
    // development build, where the cache lies next to the source file
    std::filesystem::path AssetCacheDirectory();
}
