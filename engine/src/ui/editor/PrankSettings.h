// PrankSettings.h
//
// The switch of the easter egg tab (PrankTab, docs/editor/prank-tab.md): "prankTab" in %APPDATA%\myengine\editor_settings.json (on by default), the
// command line key --no-prank for one run, and the process-wide state that the editor, the font baking and the menu read.
// No ImGui here: the test (tests/ui/PrankTests.cpp) includes this file.

#pragma once

#include <filesystem>
#include <string>

namespace myengine::ui::prank
{
    // The file of the user settings: MYENGINE_ASSISTANT_SETTINGS (a test, a second profile) or %APPDATA%\myengine\editor_settings.json
    std::filesystem::path SettingsFile();

    // A missing, broken or silent file gives true (the tab is on). Only a boolean "prankTab": false turns it off
    bool LoadEnabled(const std::filesystem::path& file);
    // Writes "prankTab" and keeps every other key of the file. False with a reason in `error`
    bool SaveEnabled(const std::filesystem::path& file, bool enabled, std::string& error);

    // The process-wide state: the setting (loaded once from SettingsFile), and --no-prank on top of it
    void Initialize(bool noPrankOnCommandLine);
    bool IsInitialized();
    bool Enabled();                 // the tab, the menu button and the badge exist: the setting is on and --no-prank was not given
    bool Hidden();                  // --no-prank: even the View entry is gone
    bool SettingOn();               // the setting itself (the check mark)
    void SetEnabled(bool enabled);  // the check mark of the menu: applies at once and is saved
    bool Toggle();                  // Ctrl+Shift+H: flips the same setting; false (and nothing happens) with --no-prank
    // The easter egg fonts are baked into the atlas only when the tab is on at the start of the editor
    bool FontsWanted();
}
