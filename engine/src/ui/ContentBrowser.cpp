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

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        constexpr int kMaxTreeDepth = 8;
        constexpr std::size_t kMaxVisibleEntries = 1000;
        constexpr float kMinTileSize = 72.0f;
        constexpr float kMaxTileSize = 160.0f;
        constexpr float kToolsHeight = 36.0f; // spec 5.4: the tools row
        constexpr float kFooterHeight = 24.0f; // "N items (1 selected)"

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

        // Each type has its own colour and icon (spec 2.1), like Unreal's asset tiles
        ImU32 KindColor(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return style::kTypeFolder;
                case ContentKind::Scene: return style::kTypeScene;
                case ContentKind::Script: return style::kTypeScript;
                case ContentKind::Prefab: return style::kTypePrefab;
                case ContentKind::Material: return style::kTypeMaterial;
                case ContentKind::Mesh: return style::kTypeMesh;
                case ContentKind::Texture: return style::kTypeTexture;
                case ContentKind::Shader: return style::kTypeShader;
                case ContentKind::Other: break;
            }
            return style::kTypeOther;
        }

        const char* KindIcon(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return ICON_FOLDER;
                case ContentKind::Scene: return ICON_MAP;
                case ContentKind::Script: return ICON_FILE_CODE;
                case ContentKind::Prefab: return ICON_PACKAGE;
                case ContentKind::Material: return ICON_PALETTE;
                case ContentKind::Mesh: return ICON_BOX;
                case ContentKind::Texture: return ICON_IMAGE;
                case ContentKind::Shader: return ICON_CODE_XML;
                case ContentKind::Other: break;
            }
            return ICON_FILE;
        }

        const char* KindName(const ContentKind kind)
        {
            switch (kind)
            {
                case ContentKind::Folder: return "Folder";
                case ContentKind::Scene: return "Map";
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

        // The font and size of a role, for ImDrawList::AddText (which takes both explicitly)
        void RoleFontOf(const FontRole role, ImFont*& font, float& size)
        {
            PushFontRole(role);
            font = ImGui::GetFont();
            size = ImGui::GetFontSize();
            PopFontRole();
        }

        float RoleTextWidth(const FontRole role, const char* text)
        {
            PushFontRole(role);
            const float width = ImGui::CalcTextSize(text).x;
            PopFontRole();
            return width;
        }

        // Next UTF-8 character boundary after `index`
        std::size_t NextCharacter(const std::string& text, std::size_t index)
        {
            ++index;
            while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80)
            {
                ++index;
            }
            return index;
        }

        // Splits a file name into at most two lines by characters (names have no spaces to wrap at);
        // the end of the second line gets "..." when it does not fit
        void WrapName(ImFont* font, const float size, const std::string& name, const float maxWidth, std::string& line1, std::string& line2)
        {
            line1.clear();
            line2.clear();
            const auto width = [&](const std::string& text)
            {
                return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
            };

            if (width(name) <= maxWidth)
            {
                line1 = name;
                return;
            }

            std::size_t position = 0;
            while (position < name.size())
            {
                const std::size_t next = NextCharacter(name, position);
                if (width(name.substr(0, next)) > maxWidth)
                {
                    break;
                }
                position = next;
            }
            if (position == 0)
            {
                position = NextCharacter(name, 0);
            }
            line1 = name.substr(0, position);

            const std::string rest = name.substr(position);
            if (width(rest) <= maxWidth)
            {
                line2 = rest;
                return;
            }

            std::size_t end = 0;
            while (end < rest.size())
            {
                const std::size_t next = NextCharacter(rest, end);
                if (width(rest.substr(0, next) + "...") > maxWidth)
                {
                    break;
                }
                end = next;
            }
            line2 = rest.substr(0, end) + "...";
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

    void ContentBrowser::DrawTree(const FolderNode& node, const bool isRoot, const bool panelFocused)
    {
        const bool hasChildren = !node.children.empty();
        const bool selected = node.relative == currentFolder_;

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanFullWidth |
            ImGuiTreeNodeFlags_FramePadding |
            (selected ? ImGuiTreeNodeFlags_Selected : 0) |
            (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);
        if (isRoot)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }

        // The node gives the row (hit area, hover, selection); its arrow and text are drawn below
        const float nodeX = ImGui::GetCursorScreenPos().x;
        const std::string id = "##tree_" + node.relative;
        PushSelectionColors(panelFocused);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, (style::kRowHeight - ImGui::GetFontSize()) * 0.5f));
        const bool open = ImGui::TreeNodeEx(id.c_str(), flags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        PopSelectionColors();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            OpenFolder(node.relative);
        }

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float centerY = std::floor((rowMin.y + rowMax.y) * 0.5f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (hasChildren)
        {
            DrawIcon(drawList, IconSize::Chevron12, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(nodeX + 9.0f, centerY), style::kTextDim);
        }

        // folder-open only for the root and the current folder; every folder keeps the folder type colour
        DrawIcon(
            drawList,
            IconSize::Row14,
            (isRoot || selected) ? ICON_FOLDER_OPEN : ICON_FOLDER,
            ImVec2(nodeX + 29.0f, centerY),
            style::kTypeFolder);

        ImFont* font = nullptr;
        float size = 0.0f;
        RoleFontOf(isRoot ? FontRole::Strong : FontRole::Body, font, size);
        drawList->PushClipRect(ImVec2(nodeX, rowMin.y), ImVec2(rowMax.x - 4.0f, rowMax.y), true);
        drawList->AddText(
            font,
            size,
            ImVec2(nodeX + 42.0f, std::floor(centerY - size * 0.5f)),
            (selected || isRoot) ? style::kTextStrong : style::kText,
            node.name.c_str());
        drawList->PopClipRect();

        if (open)
        {
            for (const auto& child : node.children)
            {
                DrawTree(child, false, panelFocused);
            }
            ImGui::TreePop();
        }
    }

    void ContentBrowser::DrawTile(const ContentEntry& entry, const float tileSize, const ContentBrowserHooks& hooks)
    {
        const float previewSize = tileSize - 12.0f;
        const float tileHeight = previewSize + 55.0f;
        const bool isFolder = entry.kind == ContentKind::Folder;

        ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            selectedPath_ = entry.path;
        }
        Tooltip(entry.path.c_str(), nullptr, KindName(entry.kind));

        // The source tile is dimmed while its own drag is in progress
        float alpha = 1.0f;
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload();
            payload != nullptr && payload->Data != nullptr && payload->DataSize > 0 &&
            entry.path == static_cast<const char*>(payload->Data))
        {
            alpha = 0.45f;
        }
        const auto faded = [alpha](const ImU32 color) { return alpha < 1.0f ? style::WithAlpha(color, alpha) : color; };

        const bool selected = selectedPath_ == entry.path;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImU32 typeColor = KindColor(entry.kind);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImFont* nameFont = nullptr;
        float nameSize = 0.0f;
        RoleFontOf(FontRole::Secondary, nameFont, nameSize);
        ImFont* typeFont = nullptr;
        float typeSize = 0.0f;
        RoleFontOf(FontRole::Tiny, typeFont, typeSize);

        if (isFolder)
        {
            // No card: a big folder icon, the name and "Folder" under it; the highlight is a 100 px high zone
            const ImVec2 zoneMax(max.x, min.y + std::min(100.0f, tileHeight));
            if (selected)
            {
                drawList->AddRectFilled(min, zoneMax, style::kSelectTileFill, style::kRounding);
                drawList->AddRect(min, zoneMax, style::kPrimary, style::kRounding, 0, 2.0f);
            }
            else if (hovered)
            {
                drawList->AddRectFilled(min, zoneMax, style::kControl, style::kRounding);
            }

            const float centerX = (min.x + max.x) * 0.5f;
            DrawIcon(drawList, IconSize::Folder58, ICON_FOLDER, ImVec2(centerX, min.y + 34.0f), faded(typeColor));

            // One line, centred: cut at the tile width and end with "..."
            std::string oneLine = entry.name;
            const float maxNameWidth = tileSize - 12.0f;
            if (nameFont->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, oneLine.c_str()).x > maxNameWidth)
            {
                while (!oneLine.empty() &&
                       nameFont->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, (oneLine + "...").c_str()).x > maxNameWidth)
                {
                    oneLine.pop_back();
                }
                oneLine += "...";
            }
            const float nameWidth = nameFont->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, oneLine.c_str()).x;
            drawList->AddText(nameFont, nameSize, ImVec2(std::floor(centerX - nameWidth * 0.5f), min.y + 66.0f), faded(style::kTextStrong), oneLine.c_str());
            const char* typeText = "Folder";
            const float typeWidth = typeFont->CalcTextSizeA(typeSize, FLT_MAX, 0.0f, typeText).x;
            drawList->AddText(typeFont, typeSize, ImVec2(std::floor(centerX - typeWidth * 0.5f), min.y + 83.0f), faded(style::kTextDim), typeText);
        }
        else
        {
            const ImU32 cardFill = selected ? style::kSelectTileFill : (hovered ? style::kControl : style::kPanel);
            drawList->AddRectFilled(min, max, faded(cardFill), style::kRounding);
            if (selected)
            {
                drawList->AddRect(min, max, faded(style::kPrimary), style::kRounding, 0, 2.0f);
            }

            // Preview: a dark plate with the type icon, and the 3 px type stripe under it
            const ImVec2 previewMin(min.x + 6.0f, min.y + 6.0f);
            const ImVec2 previewMax(max.x - 6.0f, min.y + 6.0f + previewSize);
            drawList->AddRectFilled(previewMin, previewMax, faded(style::kViewportBackdrop), 3.0f, ImDrawFlags_RoundCornersTop);
            drawList->AddRectFilled(ImVec2(previewMin.x, previewMax.y), ImVec2(previewMax.x, previewMax.y + style::kTileStripe), faded(typeColor));
            DrawIcon(
                drawList,
                IconSize::Tile40,
                KindIcon(entry.kind),
                ImVec2((previewMin.x + previewMax.x) * 0.5f, (previewMin.y + previewMax.y) * 0.5f),
                faded(typeColor));

            // Name: up to two lines wrapped by characters, then the type, dim, pinned to the bottom
            std::string line1;
            std::string line2;
            WrapName(nameFont, nameSize, entry.name, tileSize - 16.0f, line1, line2);
            const float nameTop = previewMax.y + style::kTileStripe + 3.0f;
            drawList->AddText(nameFont, nameSize, ImVec2(min.x + 8.0f, nameTop), faded(style::kTextStrong), line1.c_str());
            if (!line2.empty())
            {
                drawList->AddText(nameFont, nameSize, ImVec2(min.x + 8.0f, nameTop + 14.0f), faded(style::kTextStrong), line2.c_str());
            }
            drawList->AddText(typeFont, typeSize, ImVec2(min.x + 8.0f, max.y - typeSize - 2.0f), faded(style::kTextDim), KindName(entry.kind));
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
            // The drag preview: the type icon and the name
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(typeColor));
            ImGui::TextUnformatted(KindIcon(entry.kind));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.name.c_str());
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

        // Edge to edge: the tools row, the two wells and the footer lay themselves out
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 region = ImGui::GetContentRegionAvail();
        const float width = std::max(region.x, 160.0f);
        const float height = std::max(region.y, kToolsHeight + kFooterHeight + 40.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // ---- 1. Tools row: Refresh | breadcrumbs ...... search, View Options
        {
            const float centerY = origin.y + kToolsHeight * 0.5f;
            const float buttonY = origin.y + (kToolsHeight - style::kPanelIconButton) * 0.5f;
            float x = origin.x + 8.0f;

            ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
            if (IconButton("##content_refresh", ICON_REFRESH_CW, "Refresh"))
            {
                Refresh();
            }
            x += style::kPanelIconButton + 8.0f;
            drawList->AddLine(ImVec2(x, origin.y + 10.0f), ImVec2(x, origin.y + kToolsHeight - 10.0f), style::kBorderLight, 1.0f);
            x += 10.0f;

            const float searchWidth = std::min(260.0f, std::max(width * 0.3f, 120.0f));
            const float settingsX = origin.x + width - 8.0f - style::kPanelIconButton;
            const float searchX = settingsX - 6.0f - searchWidth;
            const float crumbsRight = searchX - 12.0f;

            ImFont* bodyFont = nullptr;
            float bodySize = 0.0f;
            RoleFontOf(FontRole::Body, bodyFont, bodySize);

            const std::vector<std::string> crumbs = GetBreadcrumbs();
            std::string navigateTo;
            bool navigate = false;
            for (std::size_t index = 0; index < crumbs.size(); ++index)
            {
                const bool last = index + 1 == crumbs.size();
                if (index > 0)
                {
                    DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_RIGHT, ImVec2(x + 6.0f, centerY), style::kTextDim);
                    x += 14.0f;
                }

                const float iconWidth = index == 0 ? 20.0f : 0.0f;
                const float partWidth = iconWidth + RoleTextWidth(FontRole::Body, crumbs[index].c_str()) + 12.0f;
                if (x + partWidth > crumbsRight)
                {
                    break; // too narrow: the rest of the path is cut off
                }

                ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
                ImGui::PushID(static_cast<int>(index));
                const bool pressed = ImGui::InvisibleButton("##crumb", ImVec2(partWidth, style::kPanelIconButton));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered)
                {
                    drawList->AddRectFilled(ImVec2(x, buttonY), ImVec2(x + partWidth, buttonY + style::kPanelIconButton), style::kControl, style::kRounding);
                }
                if (index == 0)
                {
                    DrawIcon(drawList, IconSize::Row14, last ? ICON_FOLDER_OPEN : ICON_FOLDER, ImVec2(x + 13.0f, centerY), style::kTypeFolder);
                }
                drawList->AddText(
                    bodyFont,
                    bodySize,
                    ImVec2(x + 6.0f + iconWidth, std::floor(centerY - bodySize * 0.5f)),
                    last ? style::kTextStrong : style::kText,
                    crumbs[index].c_str());

                if (pressed)
                {
                    navigate = true;
                    navigateTo.clear();
                    for (std::size_t part = 1; part <= index; ++part)
                    {
                        navigateTo += (part > 1 ? "/" : "") + crumbs[part];
                    }
                }
                x += partWidth + 2.0f;
            }
            if (navigate)
            {
                OpenFolder(navigateTo);
            }

            ImGui::SetCursorScreenPos(ImVec2(searchX, origin.y + (kToolsHeight - style::kFrameHeight) * 0.5f));
            std::string searchText = search_;
            const std::string hint = "Search " + (currentFolder_.empty() ? rootLabel_ : fs::u8path(currentFolder_).filename().u8string());
            if (SearchField("##content_search", &searchText, hint.c_str(), searchWidth))
            {
                SetSearch(searchText);
            }

            ImGui::SetCursorScreenPos(ImVec2(settingsX, buttonY));
            if (IconButton("##content_view_options", ICON_SETTINGS, "View Options"))
            {
                ImGui::OpenPopup("##content_view_options_popup");
            }
            if (ImGui::BeginPopup("##content_view_options_popup"))
            {
                ImGui::TextUnformatted("Tile Size");
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("##tile_size", &tileSize_, kMinTileSize, kMaxTileSize, "%.0f");
                ImGui::EndPopup();
            }
        }

        // ---- 2. Sources tree and the tiles
        const float bodyTop = origin.y + kToolsHeight;
        const float bodyHeight = std::max(height - kToolsHeight - kFooterHeight, 40.0f);
        const float treeWidth = std::clamp(width * 0.2f, 140.0f, 230.0f);

        ImGui::SetCursorScreenPos(ImVec2(origin.x, bodyTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f));
        const bool treeShown = ImGui::BeginChild("##content_sources", ImVec2(treeWidth, bodyHeight), ImGuiChildFlags_None);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (treeShown)
        {
            DrawTree(tree_, true, ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
        }
        ImGui::EndChild();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + treeWidth + style::kSplitter, bodyTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
        const bool tilesShown = ImGui::BeginChild(
            "##content_tiles",
            ImVec2(std::max(width - treeWidth - style::kSplitter, 40.0f), bodyHeight),
            ImGuiChildFlags_None);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ContentEntry toActivate;
        bool activate = false;
        bool clearSearch = false;
        if (tilesShown)
        {
            if (entries_.empty())
            {
                if (search_.empty())
                {
                    EmptyState(ICON_FOLDER_OPEN, nullptr, "This folder is empty.");
                }
                else
                {
                    const std::string message = "No items match \"" + search_ + "\".";
                    clearSearch = EmptyState(ICON_SEARCH, nullptr, message.c_str(), "Clear search");
                }
            }
            else
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
                const float available = ImGui::GetContentRegionAvail().x;
                const int columns = std::max(1, static_cast<int>((available + 8.0f) / (tileSize_ + 8.0f)));

                for (std::size_t index = 0; index < entries_.size(); ++index)
                {
                    if (index % static_cast<std::size_t>(columns) != 0)
                    {
                        ImGui::SameLine();
                    }

                    const ContentEntry& entry = entries_[index];
                    ImGui::PushID(static_cast<int>(index));
                    DrawTile(entry, tileSize_, hooks);

                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        toActivate = entry;
                        activate = true;
                    }

                    if (ImGui::BeginPopupContextItem("##tile_menu"))
                    {
                        selectedPath_ = entry.path;
                        if (ImGui::MenuItem(ICON_FOLDER_OPEN "  Open"))
                        {
                            toActivate = entry;
                            activate = true;
                        }
                        if (ImGui::MenuItem(ICON_FOLDER "  Show in Explorer"))
                        {
                            ShowInExplorer(ToAbsolute(root_, entry.relative), entry.kind == ContentKind::Folder);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Copy Path"))
                        {
                            ImGui::SetClipboardText(entry.path.c_str());
                        }
                        if (ImGui::MenuItem("Copy Full Path"))
                        {
                            ImGui::SetClipboardText(ToAbsolute(root_, entry.relative).u8string().c_str());
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::PopStyleVar();
            }
        }
        ImGui::EndChild();

        // ---- 3. Footer: "N items (1 selected)"
        {
            const float footerTop = origin.y + height - kFooterHeight;
            drawList->AddRectFilled(ImVec2(origin.x, footerTop), ImVec2(origin.x + width, footerTop + kFooterHeight), style::kPanel);
            drawList->AddLine(ImVec2(origin.x, footerTop), ImVec2(origin.x + width, footerTop), style::kInput, 1.0f);

            const bool anySelected = std::any_of(
                entries_.begin(),
                entries_.end(),
                [this](const ContentEntry& entry) { return entry.path == selectedPath_; });
            std::string text = std::to_string(entries_.size()) + (entries_.size() == 1 ? " item" : " items");
            if (anySelected)
            {
                text += " (1 selected)";
            }

            ImFont* font = nullptr;
            float size = 0.0f;
            RoleFontOf(FontRole::Secondary, font, size);
            drawList->AddText(font, size, ImVec2(origin.x + 12.0f, std::floor(footerTop + (kFooterHeight - size) * 0.5f)), style::kTextDim, text.c_str());
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, height));

        // After the loops: these rebuild the list that the tiles above came from
        if (clearSearch)
        {
            SetSearch(std::string());
        }
        if (activate)
        {
            Activate(toActivate, hooks);
        }
    }
}
