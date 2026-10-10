#include <algorithm>
#include <cctype>
#include <cfloat>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/ui/ContentBrowser.h>

namespace myengine::ui
{
    namespace
    {
        constexpr int kMaxTreeDepth = 8;
        constexpr std::size_t kMaxVisibleEntries = 1000;
        constexpr float kMinTileSize = 56.0f;
        constexpr float kMaxTileSize = 160.0f;

        namespace fs = std::filesystem;

        std::string ToLower(std::string text)
        {
            std::transform(
                text.begin(),
                text.end(),
                text.begin(),
                [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return text;
        }

        bool EndsWith(const std::string& text, const char* suffix)
        {
            const std::string tail(suffix);
            return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
        }

        // Each type gets its own stripe colour, like Unreal's asset tiles
        ImU32 KindColor(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return IM_COL32(222, 170, 70, 255);
                case ContentKind::Scene: return IM_COL32(164, 108, 214, 255);
                case ContentKind::Script: return IM_COL32(86, 190, 110, 255);
                case ContentKind::Prefab: return IM_COL32(70, 190, 214, 255);
                case ContentKind::Material: return IM_COL32(226, 96, 84, 255);
                case ContentKind::Mesh: return IM_COL32(86, 140, 230, 255);
                case ContentKind::Texture: return IM_COL32(230, 150, 70, 255);
                case ContentKind::Shader: return IM_COL32(228, 112, 170, 255);
                case ContentKind::Other: break;
            }
            return IM_COL32(130, 138, 150, 255);
        }

        const char* KindTag(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return "";
                case ContentKind::Scene: return "SCENE";
                case ContentKind::Script: return "PY";
                case ContentKind::Prefab: return "PREFAB";
                case ContentKind::Material: return "MAT";
                case ContentKind::Mesh: return "MESH";
                case ContentKind::Texture: return "TEX";
                case ContentKind::Shader: return "SHADER";
                case ContentKind::Other: break;
            }
            return "FILE";
        }

        const char* KindName(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return "Folder";
                case ContentKind::Scene: return "Scene";
                case ContentKind::Script: return "Script";
                case ContentKind::Prefab: return "Prefab";
                case ContentKind::Material: return "Material";
                case ContentKind::Mesh: return "Mesh";
                case ContentKind::Texture: return "Texture";
                case ContentKind::Shader: return "Shader";
                case ContentKind::Other: break;
            }
            return "File";
        }

        std::string StemWithoutSuffix(const std::string& fileName, const char* suffix)
        {
            return fileName.substr(0, fileName.size() - std::string(suffix).size());
        }

        bool ShellOpen(const wchar_t* verb, const std::wstring& file, const wchar_t* parameters = nullptr)
        {
            const auto result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, verb, file.c_str(), parameters, nullptr, SW_SHOWNORMAL));
            return result > 32;
        }

        void ShowInExplorer(const fs::path& absolute, const bool isFolder)
        {
            if (isFolder)
            {
                ShellOpen(L"open", absolute.wstring());
                return;
            }

            const std::wstring parameters = L"/select,\"" + absolute.wstring() + L"\"";
            ShellOpen(L"open", L"explorer.exe", parameters.c_str());
        }

        // A script goes to the editor the user has for .py files; without an "edit" verb the default action is used,
        // and without any association (nothing is registered for .py) Notepad opens it
        void OpenInExternalEditor(const fs::path& absolute)
        {
            if (ShellOpen(L"edit", absolute.wstring()) || ShellOpen(L"open", absolute.wstring()))
            {
                return;
            }

            const std::wstring parameters = L"\"" + absolute.wstring() + L"\"";
            ShellOpen(L"open", L"notepad.exe", parameters.c_str());
        }

        // Engine caches that sit next to the assets (cooked meshes) are not content
        bool IsHiddenFile(const std::string& lowerName)
        {
            return EndsWith(lowerName, ".myemesh");
        }
    }

    ContentBrowser::ContentBrowser() = default;

    void ContentBrowser::SetRoot(const fs::path& root)
    {
        std::string name = root.filename().u8string();
        if (name.empty())
        {
            name = root.parent_path().filename().u8string();
        }
        SetRoot(root, name, {});
    }

    void ContentBrowser::SetRoot(const fs::path& root, const std::string& keyPrefix, const std::vector<std::string>& visiblePaths)
    {
        root_ = root;
        rootName_ = root_.filename().u8string();
        if (rootName_.empty())
        {
            rootName_ = root_.parent_path().filename().u8string();
        }
        keyPrefix_ = keyPrefix;
        visiblePaths_.clear();
        for (const auto& visible : visiblePaths)
        {
            visiblePaths_.push_back(ToLower(fs::u8path(visible).generic_u8string()));
        }
        currentFolder_.clear();
        search_.clear();
        selectedPath_.clear();
        Refresh();
    }

    bool ContentBrowser::IsVisibleFolder(const std::string& relative) const
    {
        if (visiblePaths_.empty())
        {
            return true;
        }

        const std::string lower = ToLower(relative);
        for (const auto& visible : visiblePaths_)
        {
            const bool below = lower == visible || lower.rfind(visible + "/", 0) == 0;
            const bool leadsTo = visible.rfind(lower + "/", 0) == 0;
            if (below || leadsTo)
            {
                return true;
            }
        }
        return false;
    }

    bool ContentBrowser::ShowsFilesIn(const std::string& relative) const
    {
        if (visiblePaths_.empty())
        {
            return true;
        }

        const std::string lower = ToLower(relative);
        for (const auto& visible : visiblePaths_)
        {
            if (lower == visible || lower.rfind(visible + "/", 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    const fs::path& ContentBrowser::GetRoot() const
    {
        return root_;
    }

    void ContentBrowser::SetRootLabel(const std::string& label)
    {
        rootLabel_ = label.empty() ? "Content" : label;
        Refresh();
    }

    const std::string& ContentBrowser::GetRootName() const
    {
        return rootName_;
    }

    void ContentBrowser::Refresh()
    {
        tree_ = FolderNode{};
        tree_.name = rootLabel_;
        std::error_code error;
        if (!root_.empty() && fs::is_directory(root_, error))
        {
            BuildTree(tree_, 0);
            if (!currentFolder_.empty() && !fs::is_directory(ToAbsolute(root_, currentFolder_), error))
            {
                currentFolder_.clear();
            }
        }
        RebuildEntries();
    }

    bool ContentBrowser::OpenFolder(const std::string& relativePath)
    {
        std::string normalized = relativePath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        while (!normalized.empty() && normalized.front() == '/')
        {
            normalized.erase(normalized.begin());
        }
        while (!normalized.empty() && normalized.back() == '/')
        {
            normalized.pop_back();
        }

        for (const auto& part : fs::path(normalized))
        {
            if (part == "..")
            {
                return false;
            }
        }

        std::error_code error;
        if (root_.empty() || !fs::is_directory(ToAbsolute(root_, normalized), error))
        {
            return false;
        }

        currentFolder_ = normalized;
        search_.clear();
        RebuildEntries();
        return true;
    }

    const std::string& ContentBrowser::GetCurrentFolder() const
    {
        return currentFolder_;
    }

    std::vector<std::string> ContentBrowser::GetBreadcrumbs() const
    {
        std::vector<std::string> crumbs{rootLabel_};
        if (!currentFolder_.empty())
        {
            for (const auto& part : fs::path(currentFolder_))
            {
                crumbs.push_back(part.u8string());
            }
        }
        return crumbs;
    }

    void ContentBrowser::SetSearch(std::string text)
    {
        if (text == search_)
        {
            return;
        }

        search_ = std::move(text);
        RebuildEntries();
    }

    const std::string& ContentBrowser::GetSearch() const
    {
        return search_;
    }

    const std::vector<ContentEntry>& ContentBrowser::GetVisibleEntries() const
    {
        return entries_;
    }

    bool ContentBrowser::Activate(const ContentEntry& entry, const ContentBrowserHooks& hooks)
    {
        switch (entry.kind)
        {
            case ContentKind::Folder:
                return OpenFolder(entry.relative);

            case ContentKind::Scene:
                if (hooks.openScene)
                {
                    hooks.openScene(entry.path);
                    return true;
                }
                return false;

            case ContentKind::Prefab:
                if (hooks.openPrefab)
                {
                    hooks.openPrefab(StemWithoutSuffix(entry.name, ".prefab.json"));
                    return true;
                }
                return false;

            case ContentKind::Material:
                if (hooks.openMaterial)
                {
                    hooks.openMaterial(entry.path);
                    return true;
                }
                return false;

            case ContentKind::Script:
                OpenInExternalEditor(ToAbsolute(root_, entry.relative));
                return true;

            case ContentKind::Mesh:
            case ContentKind::Texture:
            case ContentKind::Shader:
            case ContentKind::Other:
                break;
        }

        return false;
    }

    ContentKind ContentBrowser::Classify(const std::string& relativePath, const bool isFolder)
    {
        if (isFolder)
        {
            return ContentKind::Folder;
        }

        const std::string lower = ToLower(relativePath);
        if (EndsWith(lower, ".prefab.json"))
        {
            return ContentKind::Prefab;
        }
        if (EndsWith(lower, ".material.json"))
        {
            return ContentKind::Material;
        }
        if (EndsWith(lower, ".shader.json") || EndsWith(lower, ".hlsl"))
        {
            return ContentKind::Shader;
        }
        if (EndsWith(lower, ".json"))
        {
            // A map is a .json inside a "scenes" or "Maps" folder, at any depth
            for (const auto& part : fs::u8path(lower).parent_path())
            {
                if (part == "scenes" || part == "maps")
                {
                    return ContentKind::Scene;
                }
            }
            return ContentKind::Other;
        }
        if (EndsWith(lower, ".py"))
        {
            return ContentKind::Script;
        }
        if (EndsWith(lower, ".obj"))
        {
            return ContentKind::Mesh;
        }
        for (const char* extension : {".bmp", ".png", ".jpg", ".jpeg", ".tga", ".dds"})
        {
            if (EndsWith(lower, extension))
            {
                return ContentKind::Texture;
            }
        }
        return ContentKind::Other;
    }

    fs::path ContentBrowser::ToAbsolute(const fs::path& root, const std::string& relativePath)
    {
        return relativePath.empty() ? root : root / fs::u8path(relativePath);
    }

    void ContentBrowser::BuildTree(FolderNode& node, const int depth) const
    {
        if (depth >= kMaxTreeDepth)
        {
            return;
        }

        std::error_code error;
        std::vector<fs::path> folders;
        for (fs::directory_iterator it(ToAbsolute(root_, node.relative), fs::directory_options::skip_permission_denied, error), end;
             !error && it != end;
             it.increment(error))
        {
            std::error_code statusError;
            if (it->is_directory(statusError))
            {
                folders.push_back(it->path());
            }
        }

        std::sort(
            folders.begin(),
            folders.end(),
            [](const fs::path& lhs, const fs::path& rhs)
            {
                return ToLower(lhs.filename().u8string()) < ToLower(rhs.filename().u8string());
            });

        for (const auto& folder : folders)
        {
            FolderNode child;
            child.name = folder.filename().u8string();
            child.relative = node.relative.empty() ? child.name : node.relative + "/" + child.name;
            if (!IsVisibleFolder(child.relative))
            {
                continue;
            }
            BuildTree(child, depth + 1);
            node.children.push_back(std::move(child));
        }
    }

    ContentEntry ContentBrowser::MakeEntry(const fs::path& absolute, const bool isFolder) const
    {
        ContentEntry entry;
        entry.name = absolute.filename().u8string();
        entry.relative = absolute.lexically_relative(root_).generic_u8string();
        entry.path = keyPrefix_.empty() ? entry.relative : keyPrefix_ + "/" + entry.relative;
        entry.kind = Classify(entry.relative, isFolder);
        return entry;
    }

    void ContentBrowser::RebuildEntries()
    {
        entries_.clear();
        std::error_code error;
        if (root_.empty() || !fs::is_directory(root_, error))
        {
            return;
        }

        const fs::path folder = ToAbsolute(root_, currentFolder_);
        const std::string needle = ToLower(search_);

        const auto consider = [&](const fs::directory_entry& item)
        {
            if (entries_.size() >= kMaxVisibleEntries)
            {
                return;
            }

            std::error_code statusError;
            const bool isFolder = item.is_directory(statusError);
            if (!isFolder && !item.is_regular_file(statusError))
            {
                return;
            }

            ContentEntry entry = MakeEntry(item.path(), isFolder);
            if (isFolder ? !IsVisibleFolder(entry.relative) : (IsHiddenFile(ToLower(entry.name)) ||
                    !ShowsFilesIn(fs::u8path(entry.relative).parent_path().generic_u8string())))
            {
                return;
            }
            if (!needle.empty() && ToLower(entry.name).find(needle) == std::string::npos)
            {
                return;
            }
            entries_.push_back(std::move(entry));
        };

        if (needle.empty())
        {
            for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end;
                 it.increment(error))
            {
                consider(*it);
            }
        }
        else
        {
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end;
                 it.increment(error))
            {
                consider(*it);
            }
        }

        std::sort(
            entries_.begin(),
            entries_.end(),
            [](const ContentEntry& lhs, const ContentEntry& rhs)
            {
                const bool lhsFolder = lhs.kind == ContentKind::Folder;
                const bool rhsFolder = rhs.kind == ContentKind::Folder;
                if (lhsFolder != rhsFolder)
                {
                    return lhsFolder;
                }

                const std::string lhsName = ToLower(lhs.name);
                const std::string rhsName = ToLower(rhs.name);
                return lhsName != rhsName ? lhsName < rhsName : lhs.relative < rhs.relative;
            });
    }

    void ContentBrowser::DrawTree(const FolderNode& node, const bool isRoot)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.children.empty())
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (node.relative == currentFolder_)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (isRoot)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }

        const std::string label = node.name + "##tree_" + node.relative;
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            OpenFolder(node.relative);
        }

        if (open && !node.children.empty())
        {
            for (const auto& child : node.children)
            {
                DrawTree(child, false);
            }
            ImGui::TreePop();
        }
    }

    void ContentBrowser::DrawTile(const ContentEntry& entry, const float tileSize, const ContentBrowserHooks& hooks)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float labelHeight = ImGui::GetTextLineHeight() * 2.0f + 6.0f;

        ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize + labelHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            selectedPath_ = entry.path;
        }

        const bool selected = selectedPath_ == entry.path;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImU32 background = selected
            ? ImGui::GetColorU32(ImGuiCol_Header)
            : (hovered ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg));
        drawList->AddRectFilled(min, max, background, 5.0f);
        if (selected)
        {
            drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_CheckMark), 5.0f, 0, 1.5f);
        }

        // Thumbnail area: a dark plate, the type stripe along its bottom edge and the type tag in the middle
        const float inset = 6.0f;
        const ImVec2 plateMin(min.x + inset, min.y + inset);
        const ImVec2 plateMax(max.x - inset, min.y + tileSize - inset);
        const ImU32 kindColor = KindColor(entry.kind);
        drawList->AddRectFilled(plateMin, plateMax, IM_COL32(22, 25, 31, 255), 4.0f);
        drawList->AddRectFilled(
            ImVec2(plateMin.x, plateMax.y - 5.0f),
            plateMax,
            kindColor,
            4.0f,
            ImDrawFlags_RoundCornersBottom);

        if (entry.kind == ContentKind::Folder)
        {
            const ImVec2 center((plateMin.x + plateMax.x) * 0.5f, (plateMin.y + plateMax.y) * 0.5f - 3.0f);
            const float half = (plateMax.x - plateMin.x) * 0.28f;
            drawList->AddRectFilled(
                ImVec2(center.x - half, center.y - half * 0.75f),
                ImVec2(center.x - half * 0.1f, center.y - half * 0.4f),
                kindColor,
                2.0f);
            drawList->AddRectFilled(
                ImVec2(center.x - half, center.y - half * 0.5f),
                ImVec2(center.x + half, center.y + half * 0.7f),
                kindColor,
                3.0f);
        }
        else
        {
            const char* tag = KindTag(entry.kind);
            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize() * 1.2f;
            const ImVec2 tagSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, tag);
            drawList->AddText(
                font,
                fontSize,
                ImVec2((plateMin.x + plateMax.x - tagSize.x) * 0.5f, (plateMin.y + plateMax.y - tagSize.y) * 0.5f - 3.0f),
                kindColor,
                tag);
        }

        // Name: up to two lines, clipped to the tile
        drawList->PushClipRect(ImVec2(min.x + 3.0f, min.y + tileSize), ImVec2(max.x - 3.0f, max.y), true);
        drawList->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            ImVec2(min.x + 6.0f, min.y + tileSize + 1.0f),
            ImGui::GetColorU32(ImGuiCol_Text),
            entry.name.c_str(),
            nullptr,
            tileSize - 12.0f);
        drawList->PopClipRect();

        if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImGui::SetTooltip("%s\n%s", entry.path.c_str(), KindName(entry.kind));
        }

        const char* payloadType = nullptr;
        switch (entry.kind)
        {
            case ContentKind::Mesh: payloadType = hooks.meshPayloadType; break;
            case ContentKind::Material: payloadType = hooks.materialPayloadType; break;
            case ContentKind::Texture: payloadType = hooks.texturePayloadType; break;
            default: break;
        }

        if (payloadType != nullptr && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(payloadType, entry.path.c_str(), entry.path.size() + 1u);
            ImGui::TextUnformatted(entry.path.c_str());
            ImGui::EndDragDropSource();
        }

    }

    void ContentBrowser::Draw(const ContentBrowserHooks& hooks)
    {
        if (root_.empty())
        {
            ImGui::TextDisabled("The content folder is not available.");
            return;
        }

        // The folder may have changed outside the editor while another window had the focus
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !wasFocused_)
        {
            Refresh();
        }
        wasFocused_ = focused;

        if (ImGui::Button("Refresh"))
        {
            Refresh();
        }

        const std::vector<std::string> crumbs = GetBreadcrumbs();
        std::string navigateTo;
        bool navigate = false;
        for (std::size_t index = 0; index < crumbs.size(); ++index)
        {
            ImGui::SameLine();
            if (index > 0)
            {
                ImGui::TextDisabled(">");
                ImGui::SameLine();
            }

            ImGui::PushID(static_cast<int>(index));
            if (ImGui::SmallButton(crumbs[index].c_str()))
            {
                navigate = true;
                navigateTo.clear();
                for (std::size_t part = 1; part <= index; ++part)
                {
                    navigateTo += (part > 1 ? "/" : "") + crumbs[part];
                }
            }
            ImGui::PopID();
        }
        if (navigate)
        {
            OpenFolder(navigateTo);
        }

        std::string searchText = search_;
        const float sliderWidth = 120.0f;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - sliderWidth - ImGui::GetStyle().ItemSpacing.x, 80.0f));
        if (ImGui::InputTextWithHint("##content_search", "Search by name", &searchText))
        {
            SetSearch(searchText);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##content_tile_size", &tileSize_, kMinTileSize, kMaxTileSize, "Size %.0f");

        const float treeWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.24f, 120.0f, 260.0f);
        ImGui::BeginChild("##content_tree", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_Borders);
        DrawTree(tree_, true);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content_tiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

        ContentEntry toActivate;
        bool activate = false;
        if (entries_.empty())
        {
            ImGui::TextDisabled(search_.empty() ? "This folder is empty." : "Nothing matches the search.");
        }
        else
        {
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float available = ImGui::GetContentRegionAvail().x;
            const int columns = std::max(1, static_cast<int>((available + spacing) / (tileSize_ + spacing)));

            for (std::size_t index = 0; index < entries_.size(); ++index)
            {
                if (index % static_cast<std::size_t>(columns) != 0)
                {
                    ImGui::SameLine();
                }

                const ContentEntry& entry = entries_[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::BeginGroup();
                DrawTile(entry, tileSize_, hooks);
                ImGui::EndGroup();

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    toActivate = entry;
                    activate = true;
                }

                if (ImGui::BeginPopupContextItem("##tile_menu"))
                {
                    selectedPath_ = entry.path;
                    if (ImGui::MenuItem("Open"))
                    {
                        toActivate = entry;
                        activate = true;
                    }
                    if (ImGui::MenuItem("Show in Explorer"))
                    {
                        ShowInExplorer(ToAbsolute(root_, entry.relative), entry.kind == ContentKind::Folder);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy path"))
                    {
                        ImGui::SetClipboardText(entry.path.c_str());
                    }
                    if (ImGui::MenuItem("Copy full path"))
                    {
                        ImGui::SetClipboardText(ToAbsolute(root_, entry.relative).u8string().c_str());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // After the loop: opening a folder rebuilds the list that the tiles above came from
        if (activate)
        {
            Activate(toActivate, hooks);
        }
    }
}
