// AssistantSettings.h

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace myengine::assistant
{
    // Which coding agent the editor's Assistant tab talks to. None is the default: no process, no MCP bridge, no pipe
    enum class AssistantBackendKind : std::uint8_t
    {
        None,
        Claude, // Claude Code CLI
        Codex,  // OpenAI Codex CLI
    };

    const char* BackendKindId(AssistantBackendKind kind);    // "none", "claude", "codex": the value in the file
    const char* BackendKindLabel(AssistantBackendKind kind); // "Off", "Claude Code", "Codex"
    // Unknown text gives None
    AssistantBackendKind ParseBackendKind(const std::string& text);

    // Per-user settings of the assistant: %APPDATA%\myengine\editor_settings.json, key "assistant". They belong to the
    // machine, not to the project (two people have two different CLIs), so nothing of this goes into the repository.
    // {"assistant": {"backend": "none" | "claude" | "codex", "claudePath": "", "codexPath": "", "codexModel": "", "allowExternalMcp": false}}
    struct AssistantSettings
    {
        AssistantBackendKind backend = AssistantBackendKind::None;
        std::filesystem::path claudePath; // explicit path to claude(.exe/.cmd); empty: search
        std::filesystem::path codexPath;  // explicit path to codex(.exe/.cmd); empty: search
        std::string codexModel;           // model of new Codex chats; empty: the CLI's own default
        // The MCP bridge runs even while the assistant is off (or whatever its backend is), so that Claude Code,
        // Codex or another MCP client started by the user can use the engine tools. Off by default
        bool allowExternalMcp = false;

        // %APPDATA%\myengine\editor_settings.json; empty when APPDATA is unknown
        static std::filesystem::path DefaultFile();

        // A missing, unreadable or broken file gives the defaults (backend None); `problem` (optional) tells why
        static AssistantSettings Load(const std::filesystem::path& file, std::string* problem = nullptr);
        // Writes the "assistant" key and keeps every other key of the file. False with a reason in `error`
        bool Save(const std::filesystem::path& file, std::string& error) const;
    };
}
