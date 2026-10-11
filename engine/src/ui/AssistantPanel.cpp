// AssistantPanel.cpp
// AssistantPanel.cpp

#include <myengine/ui/AssistantPanel.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/assistant/AssistantBridge.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/assistant/ClaudeCliBackend.h>
#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>

#include "AssistantImages.h"
#include "AssistantMarkdownView.h"
#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        using Kind = assistant::AssistantMessage::Kind;

        constexpr ImU32 kDiffAdded = IM_COL32(0x7F, 0xD6, 0x84, 255);
        constexpr std::size_t kMaxShownBytes = 64 * 1024;

        void WrappedText(const std::string& text)
        {
            const std::size_t shown = std::min(text.size(), kMaxShownBytes);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str(), text.c_str() + shown);
            if (shown < text.size())
            {
                ImGui::TextDisabled("... (%zu more bytes)", text.size() - shown);
            }
            ImGui::PopTextWrapPos();
        }

        // Natural heights of a confirmation card: the title, the detail block and the whole card
        struct ApprovalMetrics
        {
            float title = 0.0f;
            float detail = 0.0f;
            float card = 0.0f;
        };

        ApprovalMetrics MeasureApproval(const assistant::AssistantApproval& approval, const float width)
        {
            ApprovalMetrics metrics;
            const std::string title = "Confirm: " + approval.title;
            PushFontRole(FontRole::Strong);
            metrics.title = std::max(ImGui::CalcTextSize(title.c_str(), nullptr, false, std::max(width - 14.0f - 24.0f - 12.0f, 40.0f)).y, 18.0f);
            PopFontRole();
            if (!approval.detail.empty())
            {
                const std::size_t shown = std::min(approval.detail.size(), kMaxShownBytes);
                PushFontRole(FontRole::Mono);
                metrics.detail = ImGui::CalcTextSize(approval.detail.c_str(), approval.detail.c_str() + shown, false,
                                                     std::max(width - 14.0f - 12.0f - 16.0f, 40.0f)).y + 10.0f;
                PopFontRole();
            }
            // top 4 + title + gap 4 + detail + gap 6 + buttons + bottom 6
            metrics.card = 4.0f + metrics.title + (metrics.detail > 0.0f ? 4.0f + metrics.detail : 0.0f) + 6.0f + style::kFrameHeight + 6.0f;
            return metrics;
        }

        void DrawDiff(const std::string& diff)
        {
            std::size_t position = 0;
            while (position < diff.size())
            {
                auto end = diff.find('\n', position);
                if (end == std::string::npos)
                {
                    end = diff.size();
                }
                const char* begin = diff.c_str() + position;
                const char* stop = diff.c_str() + end;
                const bool added = begin < stop && *begin == '+';
                const bool removed = begin < stop && *begin == '-';
                const ImU32 color = added ? kDiffAdded : (removed ? style::kErrorText : style::kTextDim);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(begin, stop);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                position = end + 1;
            }
        }

        // ---- AS3: formatting and small widgets ----
        constexpr float kWideWidth = 720.0f; // panel width from which the list of chats sits at the left
        constexpr float kListWidth = 250.0f;
        constexpr float kHeaderHeight = 40.0f;

        struct ModelOption
        {
            const char* alias;
            const char* label;
            const char* hint;
        };
        constexpr ModelOption kModels[] = {
            {"opus", "Opus 5.5", "best quality"},
            {"sonnet", "Sonnet 5.5", "balanced"},
            {"haiku", "Haiku 5.5", "fastest"},
        };

        struct EffortOption
        {
            const char* value;
            const char* label;
        };
        constexpr EffortOption kEfforts[] = {
            {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"xhigh", "Extra high"}, {"max", "Max"},
        };

        constexpr ImU32 kSparkles = IM_COL32(0xB5, 0x7E, 0xDC, 255);

        std::string FriendlyAliasLabel(const std::string& model);

        // The text cut to `maxWidth` in the current font with "..." (U+2026) at the end; whole characters only
        std::string EllipsizeToWidth(const std::string& text, const float maxWidth, bool* truncated = nullptr)
        {
            if (truncated != nullptr)
            {
                *truncated = false;
            }
            if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
            {
                return text;
            }
            if (truncated != nullptr)
            {
                *truncated = true;
            }
            static const std::string kDots = "\xE2\x80\xA6";
            std::size_t end = text.size();
            while (end > 0)
            {
                do
                {
                    --end;
                }
                while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80);
                std::string candidate = text.substr(0, end);
                while (!candidate.empty() && candidate.back() == ' ')
                {
                    candidate.pop_back();
                }
                candidate += kDots;
                if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
                {
                    return candidate;
                }
            }
            return kDots;
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // "claude-opus-5-5" -> "opus", an alias stays as it is
        std::string ModelFamily(const std::string& id)
        {
            std::string text = Lower(id);
            if (text.rfind("claude-", 0) == 0)
            {
                text = text.substr(7);
            }
            const auto dash = text.find('-');
            return dash == std::string::npos ? text : text.substr(0, dash);
        }

        bool LocalTime(const std::int64_t unix, std::tm& out)
        {
            const auto value = static_cast<std::time_t>(unix);
            return unix > 0 && localtime_s(&out, &value) == 0;
        }

        std::string FormatClock(const std::int64_t unix)
        {
            std::tm local{};
            if (!LocalTime(unix, local))
            {
                return {};
            }
            char buffer[16];
            std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
            return buffer;
        }

        std::string FormatDate(const std::int64_t unix)
        {
            std::tm local{};
            if (!LocalTime(unix, local))
            {
                return {};
            }
            char buffer[24];
            std::strftime(buffer, sizeof(buffer), "%d %b", &local);
            return buffer;
        }

        // The number of days of the local calendar: only the differences are used
        int LocalDay(const std::int64_t unix)
        {
            std::tm local{};
            if (!LocalTime(unix, local))
            {
                return 0;
            }
            return (local.tm_year - 100) * 366 + local.tm_yday;
        }

        std::string FormatTokens(const std::uint64_t count)
        {
            char buffer[32];
            if (count < 1000)
            {
                std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(count));
            }
            else if (count < 1000000)
            {
                std::snprintf(buffer, sizeof(buffer), "%.1fk", static_cast<double>(count) / 1000.0);
            }
            else
            {
                std::snprintf(buffer, sizeof(buffer), "%.1fM", static_cast<double>(count) / 1000000.0);
            }
            return buffer;
        }

        // A drop-down button of the header: [icon text v]. Returns true when it was pressed
        bool PopupButton(const char* id, const char* icon, const ImU32 iconColor, const std::string& text, const float width)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 size(width, style::kFrameHeight);
            const bool pressed = ImGui::InvisibleButton(id, size);
            const bool hovered = ImGui::IsItemHovered();
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + size.x, min.y + size.y);
            drawList->AddRectFilled(min, max, hovered ? style::kControlHover : style::kControl, style::kRounding);
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(min.x + 14.0f, min.y + size.y * 0.5f), iconColor);
            PushFontRole(FontRole::Secondary);
            const float fontSize = ImGui::GetFontSize();
            drawList->PushClipRect(ImVec2(min.x + 28.0f, min.y), ImVec2(max.x - 22.0f, max.y), true);
            drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(min.x + 28.0f, min.y + (size.y - fontSize) * 0.5f), style::kText, text.c_str());
            drawList->PopClipRect();
            PopFontRole();
            DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(max.x - 12.0f, min.y + size.y * 0.5f), style::kTextDim);
            return pressed;
        }

        float PopupButtonWidth(const std::string& text)
        {
            PushFontRole(FontRole::Secondary);
            const float width = ImGui::CalcTextSize(text.c_str()).x;
            PopFontRole();
            return std::clamp(width + 28.0f + 24.0f, 96.0f, 220.0f);
        }

        std::string FriendlyAliasLabel(const std::string& model)
        {
            for (const auto& option : kModels)
            {
                if (model == option.alias)
                {
                    return option.label;
                }
            }
            return assistant::FriendlyModelName(model);
        }

        void OpenUrl(const std::string& url)
        {
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            {
                return;
            }
            const std::wstring wide = std::filesystem::u8path(url).wstring();
            ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    AssistantPanel::AssistantPanel(const AssistantPanelConfig& config) : config_(config)
    {
        assistant::ClaudeCliConfig cli;
        std::error_code error;
        cli.workingDirectory = std::filesystem::is_directory(config.repositoryRoot, error) ? config.repositoryRoot : std::filesystem::current_path(error);
        cli.systemPromptFile = cli.workingDirectory / "assets" / "assistant" / "system_prompt.md";
        cli.logger = config.logger;

        // Engine tools and confirmations go through myengine_mcp.exe (a separate process the CLI starts)
        if (config.tools != nullptr)
        {
            auto bridgeExecutable = config.bridgeExecutable;
            if (bridgeExecutable.empty())
            {
                wchar_t module[32768]{};
                const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
                if (length > 0 && length < std::size(module))
                {
                    bridgeExecutable = std::filesystem::path(module).parent_path() / "myengine_mcp.exe";
                }
            }
            if (!bridgeExecutable.empty() && std::filesystem::is_regular_file(bridgeExecutable, error))
            {
                assistant::AssistantBridgeConfig bridgeConfig;
                bridgeConfig.repositoryRoot = cli.workingDirectory;
                auto bridge = std::make_unique<assistant::AssistantBridge>(*config.tools, std::move(bridgeConfig));
                std::string bridgeError;
                if (bridge->Start(bridgeError))
                {
                    cli.bridge.executable = bridgeExecutable;
                    cli.bridge.pipeName = bridge->GetPipeName();
                    cli.bridge.token = bridge->GetToken();
                    cli.bridge.allowedTools = bridge->ReadOnlyToolNames();
                    cli.bridge.approveTool = assistant::AssistantBridge::ApproveToolFullName();
                    bridge_ = std::move(bridge);
                }
                else if (config.logger != nullptr)
                {
                    config.logger->Warning("Assistant: the engine tool bridge is off: " + bridgeError);
                }
            }
            else if (config.logger != nullptr)
            {
                config.logger->Warning("Assistant: myengine_mcp.exe was not found next to the editor; the assistant works with files only");
            }
        }

        const auto root = cli.workingDirectory;
        auto backend = std::make_unique<assistant::ClaudeCliBackend>(std::move(cli));
        cli_ = backend.get();
        service_ = std::make_unique<assistant::AssistantService>(std::move(backend));
        service_->SetRootDirectory(root.u8string());
        // History: the CLI's own session files of this folder plus the small index of ours under Saved/
        service_->SetChatStore(std::make_unique<assistant::ChatStore>(
            root / "Saved" / "assistant" / "chats.json", assistant::ChatStore::DefaultSessionsDirectory(root)));
        if (bridge_ != nullptr)
        {
            bridge_->onNotice = [this](const std::string& text) { service_->AddNotice(text); };
        }
    }

    AssistantPanel::AssistantPanel(std::unique_ptr<assistant::IAssistantBackend> backend)
        : service_(std::make_unique<assistant::AssistantService>(std::move(backend)))
    {
    }

    AssistantPanel::~AssistantPanel()
    {
        ClearAttachments();
        for (const auto& [path, texture] : thumbnails_)
        {
            if (texture != 0 && config_.destroyTexture)
            {
                config_.destroyTexture(texture);
            }
        }
        thumbnails_.clear();
    }

    void AssistantPanel::Update()
    {
        if (bridge_ != nullptr)
        {
            bridge_->Update();
            if (!service_->IsBusy() && !bridge_->GetPending().empty())
            {
                bridge_->RejectAll("The turn ended before the action was confirmed.");
            }
        }
        service_->Update();

        // A drop that no panel took within a few frames is stale; the next visible panel must not pick it up
        auto& drag = core::ServiceLocator::GetEditorRuntimeState().fileDrag;
        if (!drag.dropped.empty())
        {
            if (++dropAgeFrames_ > 3)
            {
                drag.dropped.clear();
                dropAgeFrames_ = 0;
            }
        }
        else
        {
            dropAgeFrames_ = 0;
        }
    }

    void AssistantPanel::Shutdown()
    {
        service_->Shutdown();
        if (bridge_ != nullptr)
        {
            bridge_->Stop();
        }
    }

    std::filesystem::path AssistantPanel::AttachmentsDirectory() const
    {
        return config_.repositoryRoot / "Saved" / "assistant" / "attachments";
    }

    std::filesystem::path AssistantPanel::ResolveProjectPath(const std::string& projectPath) const
    {
        auto path = std::filesystem::u8path(projectPath);
        std::error_code error;
        if (path.is_absolute())
        {
            return path;
        }
        for (const auto& base : {config_.repositoryRoot, config_.repositoryRoot / "assets"})
        {
            const auto candidate = base / path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
        return config_.repositoryRoot / path;
    }

    void AssistantPanel::ClearAttachments()
    {
        attachments_.clear(); // textures are cached per path in thumbnails_ and released with the panel
    }

    void AssistantPanel::RemoveAttachment(const std::size_t index)
    {
        if (index < attachments_.size())
        {
            attachments_.erase(attachments_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    std::size_t AssistantPanel::AttachFiles(const std::vector<std::filesystem::path>& files)
    {
        constexpr std::uint64_t kMaxImageBytes = 5ull * 1024 * 1024;
        std::size_t added = 0;
        for (const auto& file : files)
        {
            std::error_code error;
            if (file.empty() || std::filesystem::is_directory(file, error))
            {
                continue;
            }
            const auto absolute = file.is_absolute() ? file : std::filesystem::absolute(file, error);
            const auto text = absolute.u8string();
            const bool duplicate = std::any_of(attachments_.begin(), attachments_.end(),
                                               [&](const PendingAttachment& item) { return item.info.path == text; });
            if (duplicate || attachments_.size() >= 12)
            {
                continue;
            }
            PendingAttachment attachment;
            attachment.info.path = text;
            attachment.info.name = absolute.filename().u8string();
            attachment.info.mediaType = ui::assistant_images::ImageMediaType(absolute);
            const auto size = std::filesystem::file_size(absolute, error);
            if (error)
            {
                attachment.problem = "The file could not be read.";
            }
            else if (attachment.info.IsImage() && size > kMaxImageBytes)
            {
                attachment.problem = "The picture is larger than 5 MB.";
            }
            else if (!attachment.info.IsImage() && ui::assistant_images::IsUnsupportedPicture(absolute))
            {
                attachment.problem = "Only png, jpg, webp and gif pictures can be sent.";
            }
            if (attachment.problem.empty() && attachment.info.IsImage() && config_.createTexture)
            {
                auto found = thumbnails_.find(text);
                if (found == thumbnails_.end())
                {
                    std::vector<unsigned char> pixels;
                    int width = 0;
                    int height = 0;
                    std::uint64_t texture = 0;
                    if (ui::assistant_images::LoadThumbnail(absolute, 64, pixels, width, height))
                    {
                        texture = config_.createTexture(pixels.data(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
                        thumbnailAspect_[text] = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
                    }
                    found = thumbnails_.emplace(text, texture).first;
                }
                attachment.texture = found->second;
                attachment.aspect = thumbnailAspect_.count(text) != 0 ? thumbnailAspect_[text] : 1.0f;
            }
            attachments_.push_back(std::move(attachment));
            ++added;
        }
        return added;
    }

    void AssistantPanel::PasteFromClipboard()
    {
        std::string pasteError;
        const auto files = assistant_images::PasteFromClipboard(AttachmentsDirectory(), pasteError);
        if (!files.empty())
        {
            AttachFiles(files);
        }
        else if (!pasteError.empty())
        {
            service_->AddNotice(pasteError);
        }
    }

    void AssistantPanel::ChooseFiles(const bool imagesOnly)
    {
        // The standard Open dialog, several files at once. It blocks the editor for as long as it is open.
        std::vector<wchar_t> buffer(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        static const wchar_t kFilterAll[] = L"Pictures and files\0*.png;*.jpg;*.jpeg;*.webp;*.gif;*.*\0Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.gif\0All files\0*.*\0\0";
        static const wchar_t kFilterImages[] = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.gif\0\0";
        dialog.lpstrFilter = imagesOnly ? kFilterImages : kFilterAll;
        dialog.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        const auto repo = config_.repositoryRoot.wstring();
        dialog.lpstrInitialDir = repo.c_str();
        if (!GetOpenFileNameW(&dialog))
        {
            return;
        }
        // "dir\0name1\0name2\0\0" for several files, one full path for a single file
        std::vector<std::filesystem::path> files;
        const std::wstring first(buffer.data());
        const wchar_t* next = buffer.data() + first.size() + 1;
        if (*next == L'\0')
        {
            files.emplace_back(first);
        }
        else
        {
            while (*next != L'\0')
            {
                const std::wstring name(next);
                files.emplace_back(std::filesystem::path(first) / name);
                next += name.size() + 1;
            }
        }
        AttachFiles(files);
    }

    const std::vector<assistant::MarkdownBlock>& AssistantPanel::Markdown(const std::size_t index, const std::string& text)
    {
        // Parsed once per message and again only when the text grew (a streamed answer)
        auto& entry = markdown_[static_cast<std::uint64_t>(index)];
        const std::size_t hash = std::hash<std::string_view>{}(text);
        if (entry.size != text.size() || entry.hash != hash || entry.blocks.empty())
        {
            entry.blocks = assistant::ParseMarkdown(text);
            entry.size = text.size();
            entry.hash = hash;
        }
        return entry.blocks;
    }

    void AssistantPanel::RefreshChats(const bool force)
    {
        const double now = ImGui::GetTime();
        if (!force && !chatsDirty_ && now - chatsRefreshedAt_ < 5.0)
        {
            return;
        }
        if (auto* store = service_->GetChatStore(); store != nullptr)
        {
            store->Refresh();
            chats_ = store->Chats();
        }
        chatsRefreshedAt_ = now;
        chatsDirty_ = false;
    }

    void AssistantPanel::Submit()
    {
        if (service_->IsBusy())
        {
            return;
        }
        for (const auto& attachment : attachments_)
        {
            if (!attachment.problem.empty())
            {
                return; // a chip with a problem blocks sending until it is removed
            }
        }
        std::vector<assistant::AssistantAttachment> list;
        for (const auto& attachment : attachments_)
        {
            list.push_back(attachment.info);
        }
        if (service_->Send(input_, std::move(list)))
        {
            input_.clear();
            attachments_.clear();
            scrollToBottom_ = true;
            chatsDirty_ = true;
        }
        reclaimFocus_ = true;
    }

    // ------------------------------------------------------------------ layout
    void AssistantPanel::Draw()
    {
        RefreshChats(false);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 region = ImGui::GetContentRegionAvail();
        panelMin_[0] = origin.x;
        panelMin_[1] = origin.y;
        panelMax_[0] = origin.x + region.x;
        panelMax_[1] = origin.y + region.y;

        const bool wide = region.x >= kWideWidth;
        wideLayout_ = wide;
        if (wide)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
            ImGui::BeginChild("##assistant_chats", ImVec2(kListWidth, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            DrawChatList(kListWidth - 16.0f, false);
            ImGui::EndChild();
            ImGui::SameLine(0.0f, 2.0f); // the 2 px divider is the gap of the docked window background

            ImGui::BeginChild("##assistant_chat", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawChat(true);
            ImGui::EndChild();
        }
        else
        {
            DrawChat(false);
        }

        TakeDroppedFiles(panelMin_, panelMax_);
        DrawDropOverlay(panelMin_, panelMax_);
    }

    // ------------------------------------------------------------------ chat list
    void AssistantPanel::DrawChatList(const float width, const bool popup)
    {
        const bool available = service_->GetAvailabilityMessage().empty();
        if (Button("New Chat", ICON_PLUS, available && !service_->IsBusy(), width))
        {
            service_->NewConversation();
            input_.clear();
            ClearAttachments();
            markdown_.clear();
            drawnRevision_ = 0;
            reclaimFocus_ = true;
            if (popup)
            {
                ImGui::CloseCurrentPopup();
            }
        }
        SearchField("##chat_search", &search_, "Search chats", width);

        const float listHeight = std::max(ImGui::GetContentRegionAvail().y, 40.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
        if (ImGui::BeginChild("##chat_rows", ImVec2(0.0f, listHeight), ImGuiChildFlags_None))
        {
            const std::string needle = Lower(search_);
            std::vector<const assistant::ChatInfo*> visible;
            for (const auto& chat : chats_)
            {
                if (needle.empty() || Lower(chat.title).find(needle) != std::string::npos)
                {
                    visible.push_back(&chat);
                }
            }

            // the chat that is open but has no session yet (before its first answer)
            if (service_->GetSessionId().empty() && !service_->GetMessages().empty() && needle.empty())
            {
                assistant::ChatInfo fresh;
                fresh.title = service_->GetChatTitle().empty() ? std::string("New chat") : service_->GetChatTitle();
                fresh.model = service_->GetModel();
                fresh.effort = service_->GetEffort();
                fresh.updatedUnix = std::time(nullptr);
                DrawChatRow(fresh, width, popup);
            }

            if (visible.empty())
            {
                PushFontRole(FontRole::Secondary);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                if (needle.empty())
                {
                    ImGui::TextUnformatted("No chats yet");
                }
                else
                {
                    ImGui::TextWrapped("No chats match \"%s\"", search_.c_str());
                }
                ImGui::PopStyleColor();
                PopFontRole();
            }

            const int today = LocalDay(std::time(nullptr));
            int lastGroup = -1;
            for (const auto* chat : visible)
            {
                const int age = today - LocalDay(chat->updatedUnix);
                const int group = age <= 0 ? 0 : (age == 1 ? 1 : (age <= 7 ? 2 : 3));
                if (group != lastGroup)
                {
                    static const char* const kGroups[] = {"TODAY", "YESTERDAY", "LAST 7 DAYS", "OLDER"};
                    PushFontRole(FontRole::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
                    ImGui::TextUnformatted(kGroups[group]);
                    ImGui::PopStyleColor();
                    PopFontRole();
                    lastGroup = group;
                }
                DrawChatRow(*chat, width, popup);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void AssistantPanel::DrawChatRow(const assistant::ChatInfo& chat, const float width, const bool popup)
    {
        constexpr float kRowHeight = 44.0f;
        const bool known = !chat.id.empty();
        const bool selected = known ? chat.id == service_->GetSessionId() : service_->GetSessionId().empty();
        const bool working = selected && service_->IsBusy();
        const bool renaming = known && renameId_ == chat.id;

        ImGui::PushID(chat.id.empty() ? "##fresh" : chat.id.c_str());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + kRowHeight);
        auto* drawList = ImGui::GetWindowDrawList();

        // the right 30 px belong to the menu button: the row's own button stops before it, nothing overlaps
        const bool pressed = ImGui::InvisibleButton("##row", ImVec2(known ? width - 30.0f : width, kRowHeight));
        // AllowWhenBlockedByActiveItem: while the button of the row is held the window is "blocked" by it,
        // and the button must stay submitted to be released
        const bool hovered = ImGui::IsMouseHoveringRect(min, max) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (selected || hovered)
        {
            drawList->AddRectFilled(min, max, selected ? style::kControl : style::kHeader, 4.0f);
        }

        const float iconX = min.x + 14.0f;
        if (working)
        {
            drawList->AddCircleFilled(ImVec2(iconX, min.y + 14.0f), 4.0f, style::kWarning);
        }
        else
        {
            DrawIcon(drawList, IconSize::Row14, ICON_MESSAGE_SQUARE, ImVec2(iconX, min.y + 14.0f), style::kTextDim);
        }

        const float textX = min.x + 30.0f;
        const float textRight = max.x - (hovered || renaming ? 30.0f : 8.0f);
        if (renaming)
        {
            // as high as the title line: the meta line under it stays visible
            ImGui::SetCursorScreenPos(ImVec2(textX - 4.0f, min.y + 3.0f));
            ImGui::SetNextItemWidth(std::max(textRight - textX + 4.0f, 40.0f));
            if (renameFocus_)
            {
                ImGui::SetKeyboardFocusHere();
                renameFocus_ = false;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));
            const bool enter = ImGui::InputText("##rename", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleVar();
            FocusOutline();
            if (enter)
            {
                service_->RenameChat(renameId_, renameText_);
                renameId_.clear();
                chatsDirty_ = true;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && !ImGui::IsItemActivated() && !renameFocus_ &&
                                                              ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered))
            {
                renameId_.clear();
            }
        }
        else
        {
            PushFontRole(FontRole::Body);
            const std::string fullTitle = chat.title.empty() ? std::string("New chat") : chat.title;
            bool cut = false;
            const std::string shownTitle = EllipsizeToWidth(fullTitle, textRight - textX, &cut);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, min.y + 4.0f),
                              selected ? style::kTextStrong : style::kText, shownTitle.c_str());
            PopFontRole();
            if (cut && hovered && ImGui::GetIO().MousePos.x < textRight)
            {
                Tooltip(fullTitle.c_str());
            }
        }

        // second line: model, effort, time or date
        {
            std::string meta;
            const auto family = assistant::FriendlyModelName(!chat.model.empty() ? chat.model : chat.lastModel);
            meta += family.empty() ? std::string("Default") : family;
            for (const auto& effort : kEfforts)
            {
                if (chat.effort == effort.value)
                {
                    meta += std::string(" \xC2\xB7 ") + effort.label;
                }
            }
            const int age = LocalDay(std::time(nullptr)) - LocalDay(chat.updatedUnix);
            meta += " \xC2\xB7 ";
            meta += working ? std::string("working...") : (age <= 0 ? FormatClock(chat.updatedUnix) : FormatDate(chat.updatedUnix));
            PushFontRole(FontRole::Tiny);
            drawList->PushClipRect(ImVec2(textX, min.y), ImVec2(max.x - 6.0f, max.y), true);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, min.y + 24.0f), style::kTextDim, meta.c_str());
            drawList->PopClipRect();
            PopFontRole();
        }

        // the menu button appears under the mouse
        if ((hovered || ImGui::IsPopupOpen("##chat_menu")) && known && !renaming)
        {
            ImGui::SetCursorScreenPos(ImVec2(max.x - 28.0f, min.y + 8.0f));
            if (IconButton("##more", ICON_ELLIPSIS, "More", false, true, 0, 24.0f, nullptr, IconSize::Row14))
            {
                ImGui::OpenPopup("##chat_menu");
            }
        }
        if (BeginMenuPopup("##chat_menu"))
        {
            if (MenuItemIcon(ICON_PENCIL, "Rename", nullptr, false, true, false, 180.0f))
            {
                renameId_ = chat.id;
                renameText_ = chat.title;
                renameFocus_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (MenuItemIcon(ICON_COPY, "Copy Session ID", nullptr, false, true, false, 180.0f))
            {
                ImGui::SetClipboardText(chat.id.c_str());
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (MenuItemIcon(ICON_EYE_OFF, "Hide from list", nullptr, false, true, false, 180.0f))
            {
                service_->HideChat(chat.id);
                chatsDirty_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (pressed && known && !renaming && !selected)
        {
            std::string error;
            if (service_->OpenChat(chat.id, error))
            {
                drawnRevision_ = 0;
                scrollToBottom_ = true;
                markdown_.clear();
                input_.clear();
                ClearAttachments();
                if (popup)
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                service_->AddNotice(error);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
        ImGui::PopID();
    }

    // ------------------------------------------------------------------ one chat
    void AssistantPanel::DrawChat(const bool wide)
    {
        DrawChatHeader(wide);
        DrawCliSettings();

        // CLI not found: warning with a link to the settings
        const auto& unavailable = service_->GetAvailabilityMessage();
        if (!unavailable.empty())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
            ImGui::BeginChild("##assistant_banner", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            Banner(BannerKind::Warning, unavailable.c_str());
            PushFontRole(FontRole::Secondary);
            const char* linkLabel = "Set path...";
            const float linkWidth = ImGui::CalcTextSize(linkLabel).x;
            const ImVec2 linkPosition = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##set_path", ImVec2(linkWidth, 18.0f));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddText(linkPosition, hovered ? style::kPrimaryHover : style::kLink, linkLabel);
            PopFontRole();
            if (clicked)
            {
                openSettingsRequested_ = true;
            }
            ImGui::EndChild();
        }

        // composer height: chips, 2..8 lines of text, the row of buttons
        PushFontRole(FontRole::Body);
        const float lineHeight = ImGui::GetTextLineHeight();
        const float inputWidth = std::max(ImGui::GetContentRegionAvail().x - 40.0f, 80.0f);
        const float textHeight = input_.empty() ? lineHeight : ImGui::CalcTextSize(input_.c_str(), nullptr, false, inputWidth).y;
        PopFontRole();
        const float fieldHeight = std::clamp(textHeight + 4.0f, lineHeight * 2.0f, lineHeight * 8.0f);
        float chipsHeight = 0.0f;
        if (!attachments_.empty())
        {
            chipsHeight = 34.0f * std::ceil(static_cast<float>(attachments_.size()) / std::max(1.0f, std::floor(inputWidth / 190.0f)));
        }
        const float composerHeight = chipsHeight + fieldHeight + 34.0f + 22.0f;
        const float footerHeight = composerHeight + 10.0f;

        // While the model waits for a decision the card takes the composer's place
        std::vector<assistant::AssistantApproval> pending;
        if (bridge_ != nullptr)
        {
            pending = bridge_->GetPending();
        }
        if (!pending.empty())
        {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ApprovalMetrics metrics = MeasureApproval(pending.front(), std::max(available.x - 20.0f, 80.0f));
            const float preferred = metrics.card + 16.0f + (pending.size() > 1 ? 20.0f : 0.0f);
            const float cardsHeight = std::min(preferred, std::max(available.y - 44.0f, 110.0f));
            DrawTranscript(cardsHeight + ImGui::GetStyle().ItemSpacing.y);
            DrawApprovals(pending, cardsHeight);
        }
        else
        {
            DrawTranscript(footerHeight);
            DrawComposer(composerHeight);
        }
    }

    void AssistantPanel::DrawChatHeader(const bool wide)
    {
        const bool busy = service_->IsBusy();
        const float totalWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
        ImGui::BeginChild("##assistant_header", ImVec2(0.0f, kHeaderHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 windowMin = ImGui::GetWindowPos();
        drawList->AddLine(ImVec2(windowMin.x, windowMin.y + kHeaderHeight - 1.0f), ImVec2(windowMin.x + totalWidth, windowMin.y + kHeaderHeight - 1.0f),
                          style::kBorderLight);

        // The right group is laid out first so the title knows how much room is left
        std::string modelText;
        {
            const auto& model = service_->GetModel();
            const auto& actual = service_->GetActualModel();
            const std::string selected = model.empty() ? std::string() : FriendlyAliasLabel(model);
            if (!actual.empty() && (model.empty() || ModelFamily(actual) != ModelFamily(model)))
            {
                modelText = assistant::FriendlyModelName(actual);
            }
            else
            {
                modelText = selected.empty() ? std::string("Default model") : selected;
            }
        }
        std::string effortText = "Default effort";
        for (const auto& effort : kEfforts)
        {
            if (service_->GetEffort() == effort.value)
            {
                effortText = effort.label;
            }
        }
        std::string usage;
        {
            const auto tokens = service_->GetTotalInputTokens() + service_->GetTotalOutputTokens();
            if (tokens > 0 || service_->GetTotalCostUsd() > 0.0)
            {
                char cost[32];
                std::snprintf(cost, sizeof(cost), "$%.3f", service_->GetTotalCostUsd());
                usage = FormatTokens(tokens) + " tokens \xC2\xB7 " + cost;
            }
        }
        const float modelWidth = PopupButtonWidth(modelText);
        const float effortWidth = PopupButtonWidth(effortText);
        float usageWidth = 0.0f;
        if (!usage.empty() && wide)
        {
            PushFontRole(FontRole::Tiny);
            usageWidth = ImGui::CalcTextSize(usage.c_str()).x + 10.0f;
            PopFontRole();
        }
        const float moreWidth = style::kPanelIconButton + 6.0f;
        const float groupWidth = modelWidth + 6.0f + effortWidth + 6.0f + usageWidth + moreWidth;
        const float groupLeft = totalWidth - groupWidth - 10.0f;

        // left: history (narrow), title
        float x = 0.0f;
        if (!wide)
        {
            if (IconButton("##history", ICON_HISTORY, "Chats", false, true, 0, style::kPanelIconButton, nullptr, IconSize::Row14))
            {
                ImGui::OpenPopup("##history_popup");
            }
            x = style::kPanelIconButton + 6.0f;
            ImGui::SameLine(0.0f, 6.0f);
        }
        {
            const auto title = service_->GetChatTitle();
            const std::string shown = title.empty() ? std::string("New chat") : title;
            const float titleWidth = std::max(groupLeft - x - 20.0f, 40.0f);
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const bool canRename = !service_->GetSessionId().empty();
            if (titleRenameRequested_ && canRename)
            {
                ImGui::SetNextItemWidth(titleWidth);
                if (renameFocus_)
                {
                    ImGui::SetKeyboardFocusHere();
                    renameFocus_ = false;
                }
                const bool enter = ImGui::InputText("##title_rename", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                FocusOutline();
                if (enter)
                {
                    service_->RenameChat(service_->GetSessionId(), renameText_);
                    titleRenameRequested_ = false;
                    chatsDirty_ = true;
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered() && !renameFocus_))
                {
                    titleRenameRequested_ = false;
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + 2.0f));
                const bool pressed = ImGui::InvisibleButton("##title", ImVec2(titleWidth, 24.0f));
                if (pressed && canRename && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    titleRenameRequested_ = true;
                    renameText_ = shown;
                    renameFocus_ = true;
                }
                PushFontRole(FontRole::Strong);
                bool titleCut = false;
                const std::string shownTitle = EllipsizeToWidth(shown, titleWidth, &titleCut);
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cursor.x, cursor.y + 4.0f), style::kTextStrong, shownTitle.c_str());
                PopFontRole();
                if (titleCut && ImGui::IsMouseHoveringRect(ImVec2(cursor.x, cursor.y), ImVec2(cursor.x + titleWidth, cursor.y + 28.0f)))
                {
                    Tooltip(shown.c_str());
                }
                if (busy)
                {
                    // nothing next to the title: the working state is in the list and in the Stop button
                }
            }
        }

        // right group
        ImGui::SetCursorPos(ImVec2(std::max(groupLeft, x + 40.0f), 7.0f));
        if (PopupButton("##model_button", ICON_SPARKLES, kSparkles, modelText, modelWidth))
        {
            ImGui::OpenPopup("##model_menu");
        }
        if (ImGui::IsItemHovered())
        {
            char tip[160];
            std::snprintf(tip, sizeof(tip), "%s tokens in / %s out \xC2\xB7 $%.3f",
                          FormatTokens(service_->GetTotalInputTokens()).c_str(), FormatTokens(service_->GetTotalOutputTokens()).c_str(),
                          service_->GetTotalCostUsd());
            Tooltip("Model", nullptr, tip);
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (PopupButton("##effort_button", ICON_BRAIN, style::kTextDim, effortText, effortWidth))
        {
            ImGui::OpenPopup("##effort_menu");
        }
        if (usageWidth > 0.0f)
        {
            ImGui::SameLine(0.0f, 10.0f);
            PushFontRole(FontRole::Tiny);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::TextUnformatted(usage.c_str());
            ImGui::PopStyleColor();
            PopFontRole();
        }
        ImGui::SameLine(0.0f, 6.0f);
        const ImVec2 menuAnchor = ImGui::GetCursorScreenPos();
        if (IconButton("##chat_more", ICON_ELLIPSIS, "More", false, true, 0, style::kPanelIconButton, nullptr, IconSize::Row14))
        {
            ImGui::OpenPopup("##header_menu");
        }
        settingsAnchor_[0] = menuAnchor.x + style::kPanelIconButton;
        settingsAnchor_[1] = menuAnchor.y + style::kPanelIconButton + 2.0f;

        // The popups are drawn here, inside the child: OpenPopup above used its ID scope, a popup of the parent window
        // would not be found
        DrawModelMenus();
        if (BeginMenuPopup("##header_menu"))
        {
            const bool has = !service_->GetSessionId().empty();
            if (MenuItemIcon(ICON_PENCIL, "Rename", nullptr, false, has, false, 190.0f))
            {
                titleRenameRequested_ = true;
                renameText_ = service_->GetChatTitle();
                renameFocus_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (MenuItemIcon(ICON_COPY, "Copy Session ID", nullptr, false, has, false, 190.0f))
            {
                ImGui::SetClipboardText(service_->GetSessionId().c_str());
                ImGui::CloseCurrentPopup();
            }
            if (MenuItemIcon(ICON_SETTINGS, "Assistant Settings...", nullptr, false, cli_ != nullptr, false, 190.0f))
            {
                openSettingsRequested_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (MenuItemIcon(ICON_EYE_OFF, "Hide from list", nullptr, false, has, false, 190.0f))
            {
                service_->HideChat(service_->GetSessionId());
                chatsDirty_ = true;
                drawnRevision_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!wide)
        {
            ImGui::SetNextWindowSize(ImVec2(300.0f, 420.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            if (ImGui::BeginPopup("##history_popup"))
            {
                DrawChatList(284.0f, true);
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
    }

    void AssistantPanel::DrawModelMenus()
    {
        if (BeginMenuPopup("##model_menu"))
        {
            MenuSection("MODEL");
            for (const auto& model : kModels)
            {
                if (MenuItemIcon(ICON_SPARKLES, model.label, model.hint, service_->GetModel() == model.alias, true, true, 240.0f))
                {
                    service_->SetModel(model.alias);
                    chatsDirty_ = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            if (MenuItemIcon(ICON_SETTINGS, "Default (CLI settings)", nullptr, service_->GetModel().empty(), true, true, 240.0f))
            {
                service_->SetModel({});
                chatsDirty_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (BeginMenuPopup("##effort_menu"))
        {
            MenuSection("THINKING EFFORT");
            for (const auto& effort : kEfforts)
            {
                if (MenuItemIcon(ICON_BRAIN, effort.label, nullptr, service_->GetEffort() == effort.value, true, true, 220.0f))
                {
                    service_->SetEffort(effort.value);
                    chatsDirty_ = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            if (MenuItemIcon(ICON_SETTINGS, "Default", nullptr, service_->GetEffort().empty(), true, true, 220.0f))
            {
                service_->SetEffort({});
                chatsDirty_ = true;
                ImGui::CloseCurrentPopup();
            }
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
            ImGui::TextUnformatted("Applies to the next message.");
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::EndPopup();
        }
    }

    // Assistant Settings popup: the CLI path (what used to be the blue CollapsingHeader)
    void AssistantPanel::DrawCliSettings()
    {
        constexpr char kPopup[] = "##assistant_settings_popup";
        if (cli_ == nullptr)
        {
            openSettingsRequested_ = false;
            return;
        }
        if (openSettingsRequested_)
        {
            ImGui::OpenPopup(kPopup);
            openSettingsRequested_ = false;
        }

        ImGui::SetNextWindowPos(ImVec2(settingsAnchor_[0], settingsAnchor_[1]), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        const bool open = ImGui::BeginPopup(kPopup);
        ImGui::PopStyleVar();
        if (!open)
        {
            return;
        }

        PushFontRole(FontRole::Strong);
        ImGui::TextUnformatted("Assistant Settings");
        PopFontRole();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        PushFontRole(FontRole::Tiny);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
        ImGui::TextUnformatted("CLAUDE CODE CLI PATH");
        ImGui::PopStyleColor();
        PopFontRole();

        const auto& resolved = cli_->GetResolvedExecutable();
        if (pathInput_.empty() && !cli_->GetExecutableOverride().empty())
        {
            pathInput_ = cli_->GetExecutableOverride().u8string();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##claude_path", "empty: PATH or MYENGINE_CLAUDE_PATH", &pathInput_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        FocusOutline();
        if (!resolved.empty())
        {
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Found: %s", resolved.u8string().c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            PopFontRole();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        const bool apply = PrimaryButton("Apply", ICON_CHECK) || enter;
        if (apply)
        {
            cli_->SetExecutableOverride(std::filesystem::u8path(pathInput_));
            service_->RefreshAvailability();
        }
        ImGui::EndPopup();
    }

    // ------------------------------------------------------------------ transcript
    void AssistantPanel::DrawTranscript(const float footerHeight)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 12.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool shown = ImGui::BeginChild("assistant_transcript", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!shown)
        {
            ImGui::EndChild();
            return;
        }
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        const auto& messages = service_->GetMessages();
        if (messages.empty())
        {
            if (ImGui::GetContentRegionAvail().y < 140.0f)
            {
                // a low panel: only the title, the icon and the hint would be covered by the composer
                PushFontRole(FontRole::Secondary);
                const char* title = "Ask the assistant";
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::SetCursorPosX(std::max((ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) * 0.5f, 8.0f));
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                PopFontRole();
            }
            else
            {
                EmptyState(ICON_SPARKLES, "Ask the assistant",
                           "Where something lives in the project, or a change to a script or a prefab. Attach screenshots or files with the paperclip.");
            }
        }
        std::size_t index = 0;
        for (const auto& message : messages)
        {
            ImGui::PushID(static_cast<int>(index));
            DrawMessage(message, index, index + 1 == messages.size() || messages[index + 1].kind == Kind::User);
            ImGui::PopID();
            ++index;
        }

        const auto revision = service_->GetRevision();
        if (revision != drawnRevision_)
        {
            if (atBottom || scrollToBottom_)
            {
                ImGui::SetScrollHereY(1.0f);
                newBelow_ = false;
            }
            else
            {
                newBelow_ = true; // the user is reading above: do not drag the view down, offer a button
            }
            drawnRevision_ = revision;
        }
        if (atBottom)
        {
            newBelow_ = false;
        }
        scrollToBottom_ = false;

        if (newBelow_)
        {
            // "New messages" pill, pinned to the bottom of the view
            PushFontRole(FontRole::Secondary);
            const char* label = "New messages";
            const float textWidth = ImGui::CalcTextSize(label).x;
            const float pillWidth = textWidth + 40.0f;
            const ImVec2 windowPosition = ImGui::GetWindowPos();
            const float scroll = ImGui::GetScrollY();
            const ImVec2 min(windowPosition.x + (ImGui::GetWindowWidth() - pillWidth) * 0.5f,
                             windowPosition.y + ImGui::GetWindowHeight() - 40.0f);
            const ImVec2 max(min.x + pillWidth, min.y + 26.0f);
            (void)scroll;
            auto* drawList = ImGui::GetForegroundDrawList();
            const bool hovered = ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            drawList->AddRectFilled(min, max, hovered ? style::kPrimaryHover : style::kPrimary, 13.0f);
            DrawIcon(drawList, IconSize::Chevron12, ICON_ARROW_DOWN_TO_LINE, ImVec2(min.x + 16.0f, min.y + 13.0f), style::kTextStrong);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 28.0f, min.y + (26.0f - ImGui::GetFontSize()) * 0.5f),
                              style::kTextStrong, label);
            PopFontRole();
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                scrollToBottom_ = true;
                drawnRevision_ = 0;
                newBelow_ = false;
            }
        }
        ImGui::EndChild();
    }

    void AssistantPanel::DrawAttachmentChips(const std::vector<assistant::AssistantAttachment>& attachments)
    {
        const float available = ImGui::GetContentRegionAvail().x;
        auto* drawList = ImGui::GetWindowDrawList();
        float x = 0.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float y = 0.0f;
        for (const auto& attachment : attachments)
        {
            PushFontRole(FontRole::Tiny);
            bool nameCut = false;
            const std::string shownName = EllipsizeToWidth(attachment.name, 180.0f, &nameCut);
            const float nameWidth = ImGui::CalcTextSize(shownName.c_str()).x;
            PopFontRole();
            const float chipWidth = 8.0f + 24.0f + 6.0f + nameWidth + 10.0f;
            if (x > 0.0f && x + chipWidth > available)
            {
                x = 0.0f;
                y += 34.0f;
            }
            const ImVec2 min(origin.x + x, origin.y + y);
            const ImVec2 max(min.x + chipWidth, min.y + 30.0f);
            drawList->AddRectFilled(min, max, IM_COL32(0x26, 0x26, 0x26, 255), 4.0f);
            drawList->AddRect(min, max, IM_COL32(0x33, 0x33, 0x33, 255), 4.0f);
            DrawIcon(drawList, IconSize::Row14, attachment.IsImage() ? ICON_IMAGE : ICON_FILE, ImVec2(min.x + 20.0f, min.y + 15.0f), style::kTextDim);
            PushFontRole(FontRole::Tiny);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 38.0f, min.y + (30.0f - ImGui::GetFontSize()) * 0.5f),
                              style::kText, shownName.c_str());
            PopFontRole();
            if (nameCut && ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
            {
                Tooltip(attachment.name.c_str());
            }
            x += chipWidth + 6.0f;
        }
        ImGui::Dummy(ImVec2(available, y + 30.0f));
    }

    void AssistantPanel::DrawMessage(const assistant::AssistantMessage& message, const std::size_t index, const bool lastOfRun)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const float available = std::max(ImGui::GetContentRegionAvail().x, 60.0f);

        switch (message.kind)
        {
        case Kind::User:
        {
            // a card on the whole width: the text, then the attachments under it
            const std::size_t shown = std::min(message.text.size(), kMaxShownBytes);
            PushFontRole(FontRole::Body);
            const ImVec2 textSize = message.text.empty()
                ? ImVec2(0.0f, 0.0f)
                : ImGui::CalcTextSize(message.text.c_str(), message.text.c_str() + shown, false, available - 24.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float chipsHeight = message.attachments.empty() ? 0.0f : 30.0f + 6.0f + (message.attachments.size() > 3 ? 34.0f : 0.0f);
            const float cardHeight = 18.0f + textSize.y + (chipsHeight > 0.0f ? chipsHeight + (textSize.y > 0.0f ? 6.0f : 0.0f) : 0.0f);
            drawList->AddRectFilled(min, ImVec2(min.x + available, min.y + cardHeight), style::kHeader, 6.0f);
            if (textSize.y > 0.0f)
            {
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 12.0f, min.y + 9.0f), style::kTextStrong,
                                  message.text.c_str(), message.text.c_str() + shown, available - 24.0f);
            }
            PopFontRole();
            if (!message.attachments.empty())
            {
                ImGui::SetCursorScreenPos(ImVec2(min.x + 12.0f, min.y + 9.0f + textSize.y + (textSize.y > 0.0f ? 6.0f : 0.0f)));
                ImGui::PushItemWidth(available - 24.0f);
                ImGui::BeginGroup();
                DrawAttachmentChips(message.attachments);
                ImGui::EndGroup();
                ImGui::PopItemWidth();
            }
            ImGui::SetCursorScreenPos(min);
            ImGui::Dummy(ImVec2(available, cardHeight));
            break;
        }

        case Kind::Assistant:
        {
            const auto& blocks = Markdown(index, message.text);
            const auto result = DrawMarkdown(blocks, available, static_cast<int>(index));
            if (!result.clickedLink.empty())
            {
                OpenUrl(result.clickedLink);
            }
            if (result.codeCopied)
            {
                ImGui::SetClipboardText(result.copiedCode.c_str());
            }
            break;
        }

        case Kind::Thinking:
        {
            // a folded row: chevron, brain, "Thought for N s"; a click opens the text
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID openId = ImGui::GetID("##thinking_open");
            bool open = storage->GetBool(openId, false);
            const bool ongoing = service_->IsBusy() && message.endTimeUnix == 0 && index + 1 == service_->GetMessages().size();
            std::string label;
            if (ongoing)
            {
                label = "Thinking...";
            }
            else
            {
                const std::int64_t seconds = message.endTimeUnix > message.timeUnix ? message.endTimeUnix - message.timeUnix : 0;
                label = seconds > 0 ? "Thought for " + std::to_string(seconds) + " s" : std::string("Thought");
            }
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float rowWidth = available;
            const bool pressed = ImGui::InvisibleButton("##thinking", ImVec2(rowWidth, 22.0f));
            if (pressed)
            {
                open = !open;
                storage->SetBool(openId, open);
            }
            DrawIcon(drawList, IconSize::Chevron12, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(min.x + 6.0f, min.y + 11.0f), style::kTextDim);
            DrawIcon(drawList, IconSize::Row14, ongoing ? ICON_LOADER_CIRCLE : ICON_BRAIN, ImVec2(min.x + 26.0f, min.y + 11.0f), style::kTextDim);
            PushFontRole(FontRole::Secondary);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 40.0f, min.y + (22.0f - ImGui::GetFontSize()) * 0.5f),
                              style::kTextDim, label.c_str());
            PopFontRole();
            if (open && !message.text.empty())
            {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                ImGui::Indent(20.0f);
                PushFontRole(FontRole::Secondary);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                WrappedText(message.text);
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::Unindent(20.0f);
                const float bottom = ImGui::GetCursorScreenPos().y;
                drawList->AddRectFilled(ImVec2(start.x + 8.0f, start.y), ImVec2(start.x + 10.0f, bottom), IM_COL32(0x33, 0x33, 0x33, 255));
            }
            break;
        }

        case Kind::Tool:
        {
            std::string toolName = message.toolName;
            if (toolName.rfind("mcp__myengine__", 0) == 0)
            {
                toolName = "engine." + toolName.substr(15); // an engine tool through the bridge
            }
            std::string header = toolName;
            if (!message.text.empty())
            {
                header += "  " + message.text;
            }

            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID openId = ImGui::GetID("##tool_open");
            bool open = storage->GetBool(openId, false);
            const bool expandable = !message.detail.empty();

            const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float rowHeight = 24.0f;
            const ImVec2 max(min.x + rowWidth, min.y + rowHeight);
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(min, max);
            if (hovered && expandable)
            {
                drawList->AddRectFilled(min, max, style::kHeader, style::kRounding);
            }
            const float centerY = min.y + rowHeight * 0.5f;
            // The user's own "no" is a decision, not a failure: neutral icon, text and chip
            const bool rejected = message.toolFailed && message.toolOutput.rfind(assistant::kUserRejectedText, 0) == 0;
            const bool failed = message.toolFailed && !rejected;
            DrawIcon(drawList, IconSize::Row14, rejected ? ICON_CIRCLE_X : ICON_WRENCH, ImVec2(min.x + 12.0f, centerY),
                     failed ? style::kError : style::kTextDim);

            // text, then the status icon right after it: check / loader-circle / x (the user's "no" is neutral)
            PushFontRole(FontRole::Mono);
            const float textLeft = min.x + 26.0f;
            const float textRoom = std::max(max.x - textLeft - (expandable ? 48.0f : 30.0f), 20.0f);
            bool headerCut = false;
            const std::string shownHeader = EllipsizeToWidth(header, textRoom, &headerCut);
            const float shownWidth = ImGui::CalcTextSize(shownHeader.c_str()).x;
            drawList->AddText(ImVec2(textLeft, std::floor(centerY - FontRoleSize(FontRole::Mono) * 0.5f - 0.5f)),
                              failed ? style::kErrorText : style::kTextDim, shownHeader.c_str());
            PopFontRole();
            const char* statusIcon = rejected ? ICON_CIRCLE_X : (failed ? ICON_X : (message.toolDone ? ICON_CHECK : ICON_LOADER_CIRCLE));
            const ImU32 statusColor = rejected ? style::kTextDim : (failed ? style::kError : (message.toolDone ? style::kPlay : style::kTextDim));
            DrawIcon(drawList, IconSize::Row14, statusIcon, ImVec2(textLeft + shownWidth + 14.0f, centerY), statusColor);
            if (expandable)
            {
                DrawIcon(drawList, IconSize::Chevron12, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(max.x - 8.0f, centerY), style::kTextDim);
            }

            // The row's own button last: it ends the row with an item and keeps the cursor spacing
            ImGui::SetCursorScreenPos(min);
            const bool pressed = ImGui::InvisibleButton("##tool", ImVec2(rowWidth, rowHeight));
            if (ImGui::IsItemHovered() && headerCut)
            {
                Tooltip(header.c_str());
            }
            if (pressed && expandable)
            {
                open = !open;
                storage->SetBool(openId, open);
            }

            if (expandable && open)
            {
                ImGui::Indent(24.0f);
                PushFontRole(FontRole::Mono);
                DrawDiff(message.detail);
                PopFontRole();
                ImGui::Unindent(24.0f);
            }
            if (failed && !message.toolOutput.empty())
            {
                ImGui::Indent(24.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kErrorText));
                PushFontRole(FontRole::Mono);
                WrappedText(message.toolOutput);
                PopFontRole();
                ImGui::PopStyleColor();
                ImGui::Unindent(24.0f);
            }
            break;
        }

        case Kind::Notice:
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            WrappedText(message.text);
            ImGui::PopStyleColor();
            PopFontRole();
            break;

        case Kind::Error:
        {
            Banner(BannerKind::Error, message.text.c_str());
            if (lastOfRun && !service_->IsBusy() && Button("Retry", ICON_REFRESH_CW))
            {
                if (service_->Retry())
                {
                    scrollToBottom_ = true;
                }
            }
            break;
        }

        case Kind::Summary:
        {
            // footer of an answer: time, usage, the model if it is not the current one, Copy and Retry
            const auto& messages = service_->GetMessages();
            std::string answer;
            std::string answeredBy;
            for (std::size_t back = index; back > 0; --back)
            {
                const auto& earlier = messages[back - 1];
                if (earlier.kind == Kind::User)
                {
                    break;
                }
                if (earlier.kind == Kind::Assistant)
                {
                    answer = earlier.text + (answer.empty() ? "" : "\n\n") + answer;
                    if (answeredBy.empty())
                    {
                        answeredBy = earlier.model;
                    }
                }
            }
            std::string line = FormatClock(message.timeUnix);
            if (!line.empty())
            {
                line += " \xC2\xB7 ";
            }
            line += message.text;
            const auto& selected = service_->GetModel();
            if (!answeredBy.empty() && !selected.empty() && ModelFamily(answeredBy) != ModelFamily(selected))
            {
                line += " \xC2\xB7 " + assistant::FriendlyModelName(answeredBy);
            }

            const ImVec2 min = ImGui::GetCursorScreenPos();
            PushFontRole(FontRole::Tiny);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::SameLine(0.0f, 8.0f);
            if (!answer.empty() && IconButton("##copy_answer", ICON_COPY, "Copy answer", false, true, 0, 22.0f, nullptr, IconSize::Chevron12))
            {
                ImGui::SetClipboardText(answer.c_str());
            }
            ImGui::SameLine(0.0f, 2.0f);
            const bool last = index + 1 == messages.size();
            if (last && IconButton("##retry", ICON_REFRESH_CW, "Retry the last request", false, !service_->IsBusy(), 0, 22.0f, nullptr,
                                   IconSize::Chevron12))
            {
                if (service_->Retry())
                {
                    scrollToBottom_ = true;
                }
            }
            (void)min;
            break;
        }

        case Kind::Files:
        {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            DrawIcon(drawList, IconSize::Row14, ICON_FILE_DIFF, ImVec2(position.x + 7.0f, position.y + 8.0f), style::kWarning);
            ImGui::SetCursorScreenPos(ImVec2(position.x + 24.0f, position.y));
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kText));
            ImGui::TextUnformatted("Changed files (hot reload picks them up)");
            ImGui::PopStyleColor();
            PopFontRole();
            PushFontRole(FontRole::Mono);
            std::size_t position2 = 0;
            while (position2 < message.text.size())
            {
                auto end = message.text.find('\n', position2);
                if (end == std::string::npos)
                {
                    end = message.text.size();
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::Text("%.*s", static_cast<int>(end - position2), message.text.c_str() + position2);
                ImGui::PopStyleColor();
                position2 = end + 1;
            }
            PopFontRole();
            break;
        }
        }
    }

    // ------------------------------------------------------------------ composer
    void AssistantPanel::DrawComposer(const float height)
    {
        const bool busy = service_->IsBusy();
        const bool available = service_->GetAvailabilityMessage().empty();
        const float width = ImGui::GetContentRegionAvail().x;
        const bool narrow = !wideLayout_ || width < 420.0f; // the hint is for the wide layout (spec 2.5)
        auto* drawList = ImGui::GetWindowDrawList();

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float boxLeft = min.x + 8.0f;
        const float boxWidth = std::max(width - 16.0f, 80.0f);
        const ImVec2 boxMin(boxLeft, min.y);
        const ImVec2 boxMax(boxLeft + boxWidth, min.y + height);
        const bool focusedBefore = composerFocused_;
        drawList->AddRectFilled(boxMin, boxMax, style::kInput, 6.0f);
        drawList->AddRect(boxMin, boxMax, focusedBefore ? style::kPrimary : IM_COL32(0x2A, 0x2A, 0x2A, 255), 6.0f);

        float y = boxMin.y + 6.0f;

        // chips of the attachments (with a preview of a picture)
        if (!attachments_.empty())
        {
            float x = boxMin.x + 8.0f;
            const float right = boxMax.x - 8.0f;
            int removeIndex = -1;
            for (std::size_t i = 0; i < attachments_.size(); ++i)
            {
                const auto& attachment = attachments_[i];
                PushFontRole(FontRole::Tiny);
                bool nameCut = false;
                const std::string shownName = EllipsizeToWidth(attachment.info.name, 150.0f, &nameCut);
                const float nameWidth = ImGui::CalcTextSize(shownName.c_str()).x;
                PopFontRole();
                const float chipWidth = 6.0f + 24.0f + 6.0f + nameWidth + 6.0f + 18.0f + 4.0f;
                if (x > boxMin.x + 8.0f && x + chipWidth > right)
                {
                    x = boxMin.x + 8.0f;
                    y += 34.0f;
                }
                const ImVec2 chipMin(x, y);
                const ImVec2 chipMax(x + chipWidth, y + 30.0f);
                const bool problem = !attachment.problem.empty();
                drawList->AddRectFilled(chipMin, chipMax, IM_COL32(0x26, 0x26, 0x26, 255), 4.0f);
                drawList->AddRect(chipMin, chipMax, problem ? style::kError : IM_COL32(0x33, 0x33, 0x33, 255), 4.0f);
                const ImVec2 thumbMin(chipMin.x + 6.0f, chipMin.y + 3.0f);
                if (attachment.texture != 0)
                {
                    drawList->AddImage(static_cast<ImTextureID>(attachment.texture), thumbMin, ImVec2(thumbMin.x + 24.0f, thumbMin.y + 24.0f));
                }
                else
                {
                    DrawIcon(drawList, IconSize::Row14, attachment.info.IsImage() ? ICON_IMAGE : ICON_FILE,
                             ImVec2(thumbMin.x + 12.0f, thumbMin.y + 12.0f), problem ? style::kErrorText : style::kTextDim);
                }
                PushFontRole(FontRole::Tiny);
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(chipMin.x + 36.0f, chipMin.y + (30.0f - ImGui::GetFontSize()) * 0.5f),
                                  problem ? style::kErrorText : style::kText, shownName.c_str());
                PopFontRole();
                if (nameCut && !problem && ImGui::IsMouseHoveringRect(chipMin, ImVec2(chipMax.x - 26.0f, chipMax.y)) &&
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
                {
                    Tooltip(attachment.info.name.c_str(), nullptr, attachment.info.path.c_str());
                }
                ImGui::SetCursorScreenPos(ImVec2(chipMax.x - 24.0f, chipMin.y + 3.0f));
                ImGui::PushID(static_cast<int>(i));
                if (IconButton("##remove", ICON_X, problem ? attachment.problem.c_str() : "Remove", false, true, 0, 24.0f, nullptr, IconSize::Chevron12))
                {
                    removeIndex = static_cast<int>(i);
                }
                ImGui::PopID();
                if (problem)
                {
                    ImGui::SetCursorScreenPos(chipMin);
                    ImGui::InvisibleButton("##problem", ImVec2(chipWidth - 28.0f, 30.0f));
                    if (ImGui::IsItemHovered())
                    {
                        Tooltip(attachment.problem.c_str());
                    }
                }
                x += chipWidth + 6.0f;
            }
            if (removeIndex >= 0)
            {
                RemoveAttachment(static_cast<std::size_t>(removeIndex));
            }
            y += 34.0f;
        }

        // the text field: transparent, inside the box
        PushFontRole(FontRole::Body);
        const float lineHeight = ImGui::GetTextLineHeight();
        const float bottomRow = 34.0f;
        const float fieldHeight = std::max(boxMax.y - y - bottomRow - 2.0f, lineHeight * 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(boxMin.x + 4.0f, y));
        if (reclaimFocus_)
        {
            ImGui::SetKeyboardFocusHere();
            reclaimFocus_ = false;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap;
        const bool submit = ImGui::InputTextMultiline("##assistant_input", &input_, ImVec2(boxWidth - 8.0f, fieldHeight), flags);
        composerFocused_ = ImGui::IsItemActive() || ImGui::IsItemFocused();
        const bool inputHovered = ImGui::IsItemHovered();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (input_.empty() && !composerFocused_)
        {
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(boxMin.x + 12.0f, y + 4.0f), style::kTextDim,
                              "Ask about the project or request a change...");
        }
        PopFontRole();
        (void)inputHovered;

        // Ctrl+V with a picture or files on the clipboard (text is pasted by the field itself)
        if (composerFocused_ && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) &&
            assistant_images::ClipboardHasContent() && !IsClipboardFormatAvailable(CF_UNICODETEXT))
        {
            PasteFromClipboard();
        }

        // bottom row: paperclip, @, hint, Send / Stop
        const float rowY = boxMax.y - bottomRow + 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(boxMin.x + 6.0f, rowY));
        if (IconButton("##attach", ICON_PAPERCLIP, "Attach pictures or files", false, available, 0, 28.0f, nullptr, IconSize::Row14))
        {
            ImGui::OpenPopup("##attach_menu");
        }
        if (BeginMenuPopup("##attach_menu"))
        {
            if (MenuItemIcon(ICON_FILE, "Pictures and files...", nullptr, false, true, false, 230.0f))
            {
                ImGui::CloseCurrentPopup();
                ChooseFiles(false);
            }
            if (MenuItemIcon(ICON_COPY, "Paste from clipboard", "Ctrl+V", false, assistant_images::ClipboardHasContent(), false, 230.0f))
            {
                ImGui::CloseCurrentPopup();
                PasteFromClipboard();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (IconButton("##mention", ICON_AT_SIGN, "Mention a project file", false, available, 0, 28.0f, nullptr, IconSize::Row14))
        {
            openMentionRequested_ = true;
        }
        if (!narrow)
        {
            ImGui::SameLine(0.0f, 10.0f);
            PushFontRole(FontRole::Tiny);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::TextUnformatted("Enter: send \xC2\xB7 Ctrl+Enter: new line \xC2\xB7 drop or paste images");
            ImGui::PopStyleColor();
            PopFontRole();
        }

        bool blocked = false;
        for (const auto& attachment : attachments_)
        {
            blocked = blocked || !attachment.problem.empty();
        }
        const float buttonWidth = 84.0f;
        ImGui::SetCursorScreenPos(ImVec2(boxMax.x - buttonWidth - 6.0f, rowY + 1.0f));
        if (busy)
        {
            if (Button("Stop", ICON_SQUARE, true, buttonWidth))
            {
                service_->Cancel();
            }
        }
        else
        {
            const bool canSend = available && !blocked && (!input_.empty() || !attachments_.empty());
            const bool pressed = PrimaryButton("Send", ICON_SEND, canSend, buttonWidth);
            if ((pressed || submit) && canSend)
            {
                Submit();
            }
        }

        // the popup of the @ button: a project file by name
        if (openMentionRequested_)
        {
            ImGui::OpenPopup("##mention_popup");
            openMentionRequested_ = false;
            mentionSearch_.clear();
            if (projectFiles_.empty())
            {
                std::error_code scanError;
                static const char* const kSkipped[] = {".git", "build", "Saved", "external", ".team", ".claude", "node_modules", ".vs", "logs"};
                for (auto it = std::filesystem::recursive_directory_iterator(
                         config_.repositoryRoot, std::filesystem::directory_options::skip_permission_denied, scanError);
                     it != std::filesystem::recursive_directory_iterator() && projectFiles_.size() < 8000; it.increment(scanError))
                {
                    if (scanError)
                    {
                        break;
                    }
                    const auto name = it->path().filename().string();
                    if (it->is_directory(scanError))
                    {
                        if (std::any_of(std::begin(kSkipped), std::end(kSkipped), [&](const char* skipped) { return name == skipped; }) ||
                            name.rfind("build-", 0) == 0)
                        {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    const auto extension = Lower(it->path().extension().string());
                    if (extension == ".myetex" || extension == ".myemesh" || extension == ".pdb" || extension == ".obj" && name.find("rl_") == 0)
                    {
                        continue;
                    }
                    projectFiles_.push_back(std::filesystem::relative(it->path(), config_.repositoryRoot, scanError).generic_u8string());
                }
                std::sort(projectFiles_.begin(), projectFiles_.end());
            }
        }
        ImGui::SetNextWindowPos(ImVec2(boxMin.x + 36.0f, boxMin.y), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(std::min(boxWidth - 40.0f, 420.0f), 300.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        if (ImGui::BeginPopup("##mention_popup"))
        {
            ImGui::PopStyleVar();
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            SearchField("##mention_search", &mentionSearch_, "Project file name", -1.0f);
            const std::string needle = Lower(mentionSearch_);
            ImGui::BeginChild("##mention_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            int shown = 0;
            for (const auto& file : projectFiles_)
            {
                if (!needle.empty() && Lower(file).find(needle) == std::string::npos)
                {
                    continue;
                }
                if (++shown > 200)
                {
                    break;
                }
                ImGui::PushID(shown);
                if (ImGui::Selectable(file.c_str(), false))
                {
                    AttachFiles({config_.repositoryRoot / std::filesystem::u8path(file)});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            if (shown == 0)
            {
                ImGui::TextDisabled("No files match");
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        else
        {
            ImGui::PopStyleVar();
        }

        // an asset dragged from the Content Browser onto the composer
        const ImRect dropArea(boxMin, boxMax);
        (void)dropArea;
        if (ImGui::BeginDragDropTargetCustom(ImRect(ImVec2(panelMin_[0], panelMin_[1]), ImVec2(panelMax_[0], panelMax_[1])),
                                             ImGui::GetID("##assistant_drop")))
        {
            for (const char* type : {"MYENGINE_ASSET_TEXTURE", "MYENGINE_ASSET_MESH", "MYENGINE_ASSET_MATERIAL", "MYENGINE_ASSET_FILE",
                                     "MYENGINE_ASSET_PREFAB", "MYENGINE_ASSET_SCRIPT"})
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type))
                {
                    const std::string projectPath(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1u : 0u);
                    AttachFiles({ResolveProjectPath(projectPath)});
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, boxMax.y));
        ImGui::Dummy(ImVec2(width, 4.0f));
    }

    // ------------------------------------------------------------------ files from Explorer
    void AssistantPanel::TakeDroppedFiles(const float min[2], const float max[2])
    {
        auto& drag = core::ServiceLocator::GetEditorRuntimeState().fileDrag;
        if (drag.dropped.empty())
        {
            return;
        }
        if (drag.dropX < min[0] || drag.dropX > max[0] || drag.dropY < min[1] || drag.dropY > max[1])
        {
            return;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& path : drag.dropped)
        {
            files.push_back(std::filesystem::u8path(path));
        }
        drag.dropped.clear();
        AttachFiles(files);
    }

    void AssistantPanel::DrawDropOverlay(const float min[2], const float max[2])
    {
        const auto& drag = core::ServiceLocator::GetEditorRuntimeState().fileDrag;
        bool active = drag.dragging && drag.x >= min[0] && drag.x <= max[0] && drag.y >= min[1] && drag.y <= max[1];
        if (!active)
        {
            if (const ImGuiPayload* payload = ImGui::GetDragDropPayload();
                payload != nullptr && payload->DataType != nullptr && std::strncmp(payload->DataType, "MYENGINE_ASSET", 14) == 0)
            {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                active = mouse.x >= min[0] && mouse.x <= max[0] && mouse.y >= min[1] && mouse.y <= max[1];
            }
        }
        if (!active)
        {
            return;
        }
        auto* drawList = ImGui::GetForegroundDrawList();
        const ImVec2 a(min[0], min[1]);
        const ImVec2 b(max[0], max[1]);
        drawList->AddRectFilled(a, b, IM_COL32(0x00, 0x70, 0xE0, 26));
        drawList->AddRect(ImVec2(a.x + 4.0f, a.y + 4.0f), ImVec2(b.x - 4.0f, b.y - 4.0f), style::kPrimary, 6.0f, 0, 2.0f);
        const char* text = "Drop images or project files to attach";
        PushFontRole(FontRole::Body);
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 center((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImU32 color = IM_COL32(0x8C, 0xC2, 0xFF, 255);
        DrawIcon(drawList, IconSize::Toolbar18, ICON_PAPERCLIP, ImVec2(center.x, center.y - 18.0f), color);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(center.x - size.x * 0.5f, center.y + 2.0f), color, text);
        PopFontRole();
    }

    // Confirmation card, pinned under the transcript while the model waits for the decision: light border,
    // panel fill, amber stripe on the left, a scrollable detail block and Reject / Apply that never scroll away
    void AssistantPanel::DrawApprovals(const std::vector<assistant::AssistantApproval>& pending, const float height)
    {
        constexpr float kRegionPad = 8.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, kRegionPad));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool shown = ImGui::BeginChild("assistant_approvals", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (!shown || pending.empty())
        {
            ImGui::EndChild();
            return;
        }

        const auto& approval = pending.front();
        const bool more = pending.size() > 1;
        auto* drawList = ImGui::GetWindowDrawList();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 80.0f);
        const ApprovalMetrics metrics = MeasureApproval(approval, width);
        const float moreHeight = more ? 20.0f : 0.0f;
        const float cardHeight = std::min(metrics.card, std::max(height - 2.0f * kRegionPad - moreHeight, 60.0f));
        const float detailHeight = metrics.detail > 0.0f
            ? std::clamp(metrics.detail, 24.0f, std::max(cardHeight - (metrics.card - metrics.detail), 24.0f))
            : 0.0f;
        const float cardBottomPad = 6.0f;

        ImGui::PushID(static_cast<int>(approval.id));
        const ImVec2 cardMin = ImGui::GetCursorScreenPos();
        const ImVec2 cardMax(cardMin.x + width, cardMin.y + cardHeight);
        drawList->AddRectFilled(cardMin, cardMax, style::kPanel, style::kRoundingDialog);
        drawList->AddRectFilled(cardMin, ImVec2(cardMin.x + 3.0f, cardMax.y), style::kWarning, style::kRoundingDialog, ImDrawFlags_RoundCornersLeft);
        drawList->AddRect(cardMin, cardMax, style::kBorderLight, style::kRoundingDialog, 0, 1.0f);

        // Title row
        float y = cardMin.y + 4.0f;
        DrawIcon(drawList, IconSize::Row14, ICON_TRIANGLE_ALERT, ImVec2(cardMin.x + 14.0f + 7.0f, y + 9.0f), style::kWarning);
        ImGui::SetCursorScreenPos(ImVec2(cardMin.x + 14.0f + 24.0f, y));
        PushFontRole(FontRole::Strong);
        ImGui::PushTextWrapPos(cardMin.x + width - 12.0f);
        ImGui::Text("Confirm: %s", approval.title.c_str());
        ImGui::PopTextWrapPos();
        PopFontRole();
        y += metrics.title + 4.0f;

        // Detail: its own scroll area, so that long JSON or a diff never pushes the buttons out of view
        if (detailHeight > 0.0f)
        {
            ImGui::SetCursorScreenPos(ImVec2(cardMin.x + 14.0f, y));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style::kRounding);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kInput));
            if (ImGui::BeginChild("##approval_detail", ImVec2(width - 14.0f - 12.0f, detailHeight), ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                PushFontRole(FontRole::Mono);
                if (approval.detail.front() == '+' || approval.detail.front() == '-')
                {
                    DrawDiff(approval.detail);
                }
                else
                {
                    WrappedText(approval.detail);
                }
                PopFontRole();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // Buttons on the right, on the bottom edge of the card
        const float buttonsY = cardMax.y - cardBottomPad - style::kFrameHeight;
        ImGui::SetCursorScreenPos(ImVec2(cardMax.x - 12.0f - 2.0f * 88.0f - 8.0f, buttonsY));
        const bool reject = Button("Reject", ICON_X, true, 88.0f);
        ImGui::SameLine();
        const bool apply = PrimaryButton("Apply", ICON_CHECK, true, 88.0f);

        ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // ends the positioned drawing with an item
        if (more)
        {
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::Text("+%zu more waiting for your decision", pending.size() - 1);
            ImGui::PopStyleColor();
            PopFontRole();
        }
        ImGui::PopID();
        ImGui::EndChild();

        if (apply)
        {
            bridge_->Resolve(approval.id, true);
        }
        else if (reject)
        {
            bridge_->Resolve(approval.id, false);
        }
    }
}
