#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <ctime>
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
        constexpr std::size_t kMaxVisibleEntries = 5000;
        constexpr int kThumbnailsPerFrame = 3; // pictures that the hook may create in one frame
        constexpr double kPollSeconds = 1.0; // how often the current folder is checked for changes
        constexpr float kListRowHeight = 26.0f; // spec p2 1.4
        constexpr float kListHeaderHeight = 24.0f;
        constexpr float kTileGap = 10.0f; // between tiles, and the padding of the tile area
        constexpr float kTileTextHeight = 64.0f; // stripe, two lines of name and the type under the square thumbnail
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

        // The icon of an item: its type, and for plain text files the text icon
        const char* EntryIcon(const ContentEntry& entry)
        {
            if (entry.kind == ContentKind::Other)
            {
                const std::string lower = ToLower(entry.name);
                if (EndsWith(lower, ".txt") || EndsWith(lower, ".md"))
                {
                    return ICON_FILE_TEXT;
                }
            }
            return KindIcon(entry.kind);
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
                if (width(rest.substr(0, next) + "\xE2\x80\xA6") > maxWidth)
                {
                    break;
                }
                end = next;
            }
            line2 = rest.substr(0, end) + "\xE2\x80\xA6";
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

        // File time as seconds since the Unix epoch (C++17 has no clock_cast)
        std::int64_t ToUnixSeconds(const fs::file_time_type time)
        {
            using namespace std::chrono;
            const auto converted = time_point_cast<system_clock::duration>(
                time - fs::file_time_type::clock::now() + system_clock::now());
            return static_cast<std::int64_t>(duration_cast<seconds>(converted.time_since_epoch()).count());
        }

        // A changed folder is noticed by its own write time (it changes when an entry is added, removed or renamed)
        std::int64_t FolderStamp(const fs::path& folder)
        {
            std::error_code error;
            const auto time = fs::last_write_time(folder, error);
            return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
        }

        // Columns of the List view, from the width of the list
        struct ListColumns
        {
            float typeX = 0.0f;
            float sizeX = 0.0f;
            float modifiedX = 0.0f;
            bool showType = false;
            bool showSize = false;
            bool showModified = false;
            static constexpr float kTypeWidth = 140.0f;
            static constexpr float kSizeWidth = 100.0f;
            static constexpr float kModifiedWidth = 160.0f;
        };

        ListColumns MakeListColumns(const float width)
        {
            ListColumns columns;
            columns.showType = width > 360.0f;
            columns.showSize = width > 500.0f;
            columns.showModified = width > 680.0f;
            float right = width;
            if (columns.showModified)
            {
                right -= ListColumns::kModifiedWidth;
                columns.modifiedX = right;
            }
            if (columns.showSize)
            {
                right -= ListColumns::kSizeWidth;
                columns.sizeX = right;
            }
            if (columns.showType)
            {
                right -= ListColumns::kTypeWidth;
                columns.typeX = right;
            }
            return columns;
        }

        // The ImGui id of the "Name" column ends where the next visible column starts
        float NameColumnRight(const ListColumns& columns, const float width)
        {
            if (columns.showType)
            {
                return columns.typeX;
            }
            if (columns.showSize)
            {
                return columns.sizeX;
            }
            if (columns.showModified)
            {
                return columns.modifiedX;
            }
            return width;
        }
    }

    bool ContentBrowser::IsServiceName(const std::string& name, const bool isFolder)
    {
        const std::string lower = ToLower(name);
        if (!lower.empty() && lower.front() == '.')
        {
            return true; // .git, .vs, .gitkeep ...
        }
        if (isFolder)
        {
            return lower == "saved"; // thumbnails, assistant chats, logs of the project
        }
        return EndsWith(lower, ".myemesh") || EndsWith(lower, ".myetex"); // caches that the resource manager writes
    }

    std::string ContentBrowser::FormatSize(const std::uint64_t bytes)
    {
        char buffer[32];
        if (bytes < 1024u)
        {
            std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
        }
        else if (bytes < 1024u * 1024u)
        {
            std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        }
        else if (bytes < 1024ull * 1024ull * 1024ull)
        {
            std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        return buffer;
    }

    std::string ContentBrowser::FormatModified(const std::int64_t unixSeconds)
    {
        if (unixSeconds <= 0)
        {
            return std::string();
        }

        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0)
        {
            return std::string();
        }

        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M", &local);
        return buffer;
    }

    void ContentBrowser::SetViewMode(const ContentViewMode mode)
    {
        viewMode_ = mode;
    }

    ContentViewMode ContentBrowser::GetViewMode() const
    {
        return viewMode_;
    }

    void ContentBrowser::SetSort(const ContentSortKey key, const bool ascending)
    {
        if (key == sortKey_ && ascending == sortAscending_)
        {
            return;
        }

        sortKey_ = key;
        sortAscending_ = ascending;
        SortEntries();
    }

    ContentSortKey ContentBrowser::GetSortKey() const
    {
        return sortKey_;
    }

    bool ContentBrowser::IsSortAscending() const
    {
        return sortAscending_;
    }

    void ContentBrowser::SetFoldersFirst(const bool foldersFirst)
    {
        if (foldersFirst == foldersFirst_)
        {
            return;
        }

        foldersFirst_ = foldersFirst;
        SortEntries();
    }

    bool ContentBrowser::GetFoldersFirst() const
    {
        return foldersFirst_;
    }

    void ContentBrowser::SetTileSize(const float size)
    {
        tileSize_ = std::clamp(size, kMinTileSize, kMaxTileSize);
    }

    float ContentBrowser::GetTileSize() const
    {
        return tileSize_;
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
        revealCurrent_ = true;
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
        if (entry.kind != ContentKind::Folder && hooks.onOpenAsset && hooks.onOpenAsset(entry.path, entry.kind))
        {
            return true;
        }

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
            if (IsServiceName(child.name, true) || !IsVisibleFolder(child.relative))
            {
                continue;
            }
            BuildTree(child, depth + 1);
            node.children.push_back(std::move(child));
        }
    }

    ContentEntry ContentBrowser::MakeEntry(const fs::directory_entry& item, const bool isFolder) const
    {
        const fs::path& absolute = item.path();
        ContentEntry entry;
        entry.name = absolute.filename().u8string();
        entry.relative = absolute.lexically_relative(root_).generic_u8string();
        entry.path = keyPrefix_.empty() ? entry.relative : keyPrefix_ + "/" + entry.relative;
        entry.kind = Classify(entry.relative, isFolder);

        std::error_code error;
        if (!isFolder)
        {
            const std::uintmax_t size = item.file_size(error);
            entry.sizeBytes = error ? 0u : static_cast<std::uint64_t>(size);
        }
        error.clear();
        const fs::file_time_type time = item.last_write_time(error);
        entry.modifiedTime = error ? 0 : ToUnixSeconds(time);
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

            ContentEntry entry = MakeEntry(item, isFolder);
            if (IsServiceName(entry.name, isFolder))
            {
                return;
            }
            if (isFolder ? !IsVisibleFolder(entry.relative)
                         : !ShowsFilesIn(fs::u8path(entry.relative).parent_path().generic_u8string()))
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

        folderStamp_ = FolderStamp(folder);
        SortEntries();
    }

    void ContentBrowser::SortEntries()
    {
        const ContentSortKey key = sortKey_;
        const bool ascending = sortAscending_;
        const bool foldersFirst = foldersFirst_;
        std::stable_sort(
            entries_.begin(),
            entries_.end(),
            [key, ascending, foldersFirst](const ContentEntry& lhs, const ContentEntry& rhs)
            {
                // Folders stay on top in either direction (unless "Folders first" is off)
                const bool lhsFolder = lhs.kind == ContentKind::Folder;
                const bool rhsFolder = rhs.kind == ContentKind::Folder;
                if (foldersFirst && lhsFolder != rhsFolder)
                {
                    return lhsFolder;
                }

                const std::string lhsName = ToLower(lhs.name);
                const std::string rhsName = ToLower(rhs.name);
                const auto byName = [&]()
                {
                    return lhsName != rhsName ? lhsName < rhsName : lhs.relative < rhs.relative;
                };

                int order = 0; // -1: lhs first, 1: rhs first, 0: equal on the key
                switch (key)
                {
                    case ContentSortKey::Name:
                        break;
                    case ContentSortKey::Type:
                    {
                        const std::string lhsType = KindName(lhs.kind);
                        const std::string rhsType = KindName(rhs.kind);
                        order = lhsType < rhsType ? -1 : (lhsType > rhsType ? 1 : 0);
                        break;
                    }
                    case ContentSortKey::Size:
                        order = lhs.sizeBytes < rhs.sizeBytes ? -1 : (lhs.sizeBytes > rhs.sizeBytes ? 1 : 0);
                        break;
                    case ContentSortKey::Modified:
                        order = lhs.modifiedTime < rhs.modifiedTime ? -1 : (lhs.modifiedTime > rhs.modifiedTime ? 1 : 0);
                        break;
                }

                if (order != 0)
                {
                    return ascending ? order < 0 : order > 0;
                }
                // Equal on the key: by name, in the same direction
                return ascending ? byName() : (lhsName != rhsName ? lhsName > rhsName : lhs.relative > rhs.relative);
            });
    }

    ContentThumbnail ContentBrowser::ResolveThumbnail(const ContentEntry& entry, const ContentBrowserHooks& hooks, const std::uint32_t pixelSize)
    {
        if (!hooks.thumbnail || entry.kind == ContentKind::Folder)
        {
            return ContentThumbnail{};
        }

        const auto found = thumbnails_.find(entry.path);
        const bool upToDate = found != thumbnails_.end() && found->second.modifiedTime == entry.modifiedTime &&
                              found->second.pixelSize == pixelSize && !found->second.thumbnail.pending &&
                              !found->second.thumbnail.live;
        if (upToDate)
        {
            return found->second.thumbnail;
        }

        // A picture that was already asked for and is still loading (or is live) is only checked again: that is a
        // lookup, so it does not take a place in the budget of new requests
        const bool recheck = found != thumbnails_.end() && found->second.modifiedTime == entry.modifiedTime &&
                             found->second.pixelSize == pixelSize;
        if (!recheck)
        {
            if (thumbnailBudget_ <= 0)
            {
                // Out of budget: the previous picture (or the loading state) stays until a later frame
                ++pendingThumbnails_;
                return found != thumbnails_.end() ? found->second.thumbnail : ContentThumbnail{};
            }
            --thumbnailBudget_;
        }

        CachedThumbnail cached;
        cached.thumbnail = hooks.thumbnail(entry, pixelSize);
        cached.modifiedTime = entry.modifiedTime;
        cached.pixelSize = pixelSize;
        thumbnails_[entry.path] = cached;
        if (cached.thumbnail.pending)
        {
            ++pendingThumbnails_;
        }
        return cached.thumbnail;
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
        else if (revealCurrent_ && currentFolder_.size() > node.relative.size() &&
                 currentFolder_.compare(0, node.relative.size(), node.relative) == 0 &&
                 currentFolder_[node.relative.size()] == '/')
        {
            // The current folder is below this node (a double click on a tile, a crumb): show the way to it
            ImGui::SetNextItemOpen(true);
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

    namespace
    {
        // "assets/textures/rl/ball.tga" -> "assets/textures/rl"
        std::string ParentOf(const std::string& path)
        {
            const std::size_t slash = path.find_last_of('/');
            return slash == std::string::npos ? std::string() : path.substr(0, slash);
        }

        // "Texture · 8.0 MB · assets/textures · 1024×1024" (the second line of the tooltip)
        std::string DescribeEntry(const ContentEntry& entry, const ContentThumbnail& thumbnail)
        {
            std::string text = KindName(entry.kind);
            if (entry.kind != ContentKind::Folder)
            {
                text += " \xC2\xB7 " + ContentBrowser::FormatSize(entry.sizeBytes);
            }
            const std::string parent = ParentOf(entry.path);
            if (!parent.empty())
            {
                text += " \xC2\xB7 " + parent;
            }
            if (entry.kind == ContentKind::Texture && thumbnail.width > 0 && thumbnail.height > 0)
            {
                text += " \xC2\xB7 " + std::to_string(thumbnail.width) + "\xC3\x97" + std::to_string(thumbnail.height);
            }
            return text;
        }

        // 8 px checkerboard under a picture that has transparent pixels
        void DrawChecker(ImDrawList* drawList, const ImVec2 min, const ImVec2 max, const float alpha)
        {
            const float cell = 8.0f;
            const ImU32 dark = style::WithAlpha(IM_COL32(0x2A, 0x2A, 0x2A, 255), alpha);
            const ImU32 light = style::WithAlpha(IM_COL32(0x3A, 0x3A, 0x3A, 255), alpha);
            int row = 0;
            for (float y = min.y; y < max.y; y += cell, ++row)
            {
                int column = 0;
                for (float x = min.x; x < max.x; x += cell, ++column)
                {
                    drawList->AddRectFilled(
                        ImVec2(x, y),
                        ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)),
                        ((row + column) % 2) != 0 ? light : dark);
                }
            }
        }

        // Fits a picture of width x height into a square box and returns its rectangle
        void FitInto(const ImVec2 boxMin, const float box, const std::uint32_t width, const std::uint32_t height, ImVec2& outMin, ImVec2& outMax)
        {
            const float scale = std::min(box / static_cast<float>(width), box / static_cast<float>(height));
            const float drawW = std::max(std::floor(static_cast<float>(width) * scale), 1.0f);
            const float drawH = std::max(std::floor(static_cast<float>(height) * scale), 1.0f);
            outMin = ImVec2(std::floor(boxMin.x + (box - drawW) * 0.5f), std::floor(boxMin.y + (box - drawH) * 0.5f));
            outMax = ImVec2(outMin.x + drawW, outMin.y + drawH);
        }

        // The source of a running drag dims its tile or row
        float DragFade(const ContentEntry& entry)
        {
            if (const ImGuiPayload* payload = ImGui::GetDragDropPayload();
                payload != nullptr && payload->Data != nullptr && payload->DataSize > 0 &&
                entry.path == static_cast<const char*>(payload->Data))
            {
                return 0.45f;
            }
            return 1.0f;
        }
    }

    void ContentBrowser::DrawTile(const ContentEntry& entry, const float tileSize, const ContentBrowserHooks& hooks)
    {
        // Every tile has the same size: a square thumbnail, two lines of name (reserved even for a short name)
        // and the type. Spec p2 1.3: 104 x 168 at Tile Size 104.
        const float tileHeight = tileSize + kTileTextHeight;
        const bool isFolder = entry.kind == ContentKind::Folder;

        ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            selectedPath_ = entry.path;
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ContentThumbnail thumbnail = ImGui::IsRectVisible(min, max)
            ? ResolveThumbnail(entry, hooks, static_cast<std::uint32_t>(tileSize))
            : ContentThumbnail{};
        {
            const std::string detail = DescribeEntry(entry, thumbnail);
            Tooltip(entry.name.c_str(), nullptr, detail.c_str());
        }

        const float alpha = DragFade(entry);
        const auto faded = [alpha](const ImU32 color) { return alpha < 1.0f ? style::WithAlpha(color, alpha) : color; };

        const bool selected = selectedPath_ == entry.path;
        const ImU32 typeColor = KindColor(entry.kind);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImFont* nameFont = nullptr;
        float nameSize = 0.0f;
        RoleFontOf(FontRole::Secondary, nameFont, nameSize);
        ImFont* typeFont = nullptr;
        float typeSize = 0.0f;
        RoleFontOf(FontRole::Tiny, typeFont, typeSize);

        // Card: files have one (kPanel, kControl on hover); a folder only shows hover and selection
        if (isFolder)
        {
            if (selected)
            {
                drawList->AddRectFilled(min, max, faded(style::kSelectTileFill), style::kRounding);
                drawList->AddRect(min, max, faded(style::kPrimary), style::kRounding, 0, 2.0f);
            }
            else if (hovered)
            {
                drawList->AddRectFilled(min, max, faded(style::kControl), style::kRounding);
            }
        }
        else
        {
            const ImU32 cardFill = selected ? style::kSelectTileFill : (hovered ? style::kControl : style::kPanel);
            drawList->AddRectFilled(min, max, faded(cardFill), style::kRounding);
            if (selected)
            {
                drawList->AddRect(min, max, faded(style::kPrimary), style::kRounding, 0, 2.0f);
            }
        }

        // Thumbnail: a dark square (the whole width of the tile), the picture or the icon on it, the type stripe under it
        const ImVec2 thumbMin = min;
        const ImVec2 thumbMax(max.x, min.y + tileSize);
        if (!isFolder)
        {
            drawList->AddRectFilled(thumbMin, thumbMax, faded(style::kViewportBackdrop), style::kRounding, ImDrawFlags_RoundCornersTop);
            drawList->AddRectFilled(ImVec2(thumbMin.x, thumbMax.y), ImVec2(thumbMax.x, thumbMax.y + style::kTileStripe), faded(typeColor));
        }

        const ImVec2 thumbCenter((thumbMin.x + thumbMax.x) * 0.5f, (thumbMin.y + thumbMax.y) * 0.5f);
        if (thumbnail.textureId != 0 && thumbnail.width > 0 && thumbnail.height > 0 && !thumbnail.pending)
        {
            ImVec2 imageMin;
            ImVec2 imageMax;
            FitInto(thumbMin, tileSize, thumbnail.width, thumbnail.height, imageMin, imageMax);
            if (thumbnail.hasAlpha)
            {
                DrawChecker(drawList, imageMin, imageMax, alpha);
            }
            ImU32 tint = thumbnail.tint;
            if (alpha < 1.0f)
            {
                tint = style::WithAlpha(tint, alpha * static_cast<float>((tint >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0f);
            }
            drawList->AddImage(
                static_cast<ImTextureID>(thumbnail.textureId),
                imageMin,
                imageMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                tint);

            // A tiny texture is easy to take for a missing one: say its size
            if (entry.kind == ContentKind::Texture && thumbnail.width < 32 && thumbnail.height < 32)
            {
                const std::string label = std::to_string(thumbnail.width) + "\xC3\x97" + std::to_string(thumbnail.height);
                const float labelWidth = typeFont->CalcTextSizeA(typeSize, FLT_MAX, 0.0f, label.c_str()).x;
                drawList->AddRectFilled(
                    ImVec2(thumbMin.x + 4.0f, thumbMin.y + 4.0f),
                    ImVec2(thumbMin.x + 4.0f + labelWidth + 8.0f, thumbMin.y + 4.0f + typeSize + 4.0f),
                    IM_COL32(0, 0, 0, 200),
                    3.0f);
                drawList->AddText(typeFont, typeSize, ImVec2(thumbMin.x + 8.0f, thumbMin.y + 6.0f), faded(style::kTextStrong), label.c_str());
            }
        }
        else if (thumbnail.pending)
        {
            // Loading: a muted icon and a running 2 px line at the bottom of the square
            DrawIcon(drawList, IconSize::Tile40, EntryIcon(entry), thumbCenter, IM_COL32(0x3D, 0x3D, 0x3D, 255));
            const float travel = tileSize * 0.4f;
            const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.9f, 1.0f);
            const float barX = thumbMin.x + (tileSize + travel) * phase - travel;
            drawList->AddRectFilled(
                ImVec2(std::max(barX, thumbMin.x), thumbMax.y - 2.0f),
                ImVec2(std::min(barX + travel, thumbMax.x), thumbMax.y),
                style::kPrimary);
        }
        else
        {
            DrawIcon(
                drawList,
                isFolder ? IconSize::Folder58 : IconSize::Tile40,
                EntryIcon(entry),
                thumbCenter,
                faded(typeColor));
        }

        // Name: two lines wrapped by characters (centred under a folder), then the type in dim
        std::string line1;
        std::string line2;
        WrapName(nameFont, nameSize, entry.name, tileSize - 16.0f, line1, line2);
        const float nameTop = thumbMax.y + (isFolder ? 0.0f : style::kTileStripe) + 5.0f;
        const auto textX = [&](const char* text, ImFont* font, const float size)
        {
            if (!isFolder)
            {
                return min.x + 8.0f;
            }
            const float textWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
            return std::floor((min.x + max.x) * 0.5f - textWidth * 0.5f);
        };
        drawList->AddText(nameFont, nameSize, ImVec2(textX(line1.c_str(), nameFont, nameSize), nameTop), faded(style::kTextStrong), line1.c_str());
        if (!line2.empty())
        {
            drawList->AddText(nameFont, nameSize, ImVec2(textX(line2.c_str(), nameFont, nameSize), nameTop + 16.0f), faded(style::kTextStrong), line2.c_str());
        }
        const char* typeName = KindName(entry.kind);
        drawList->AddText(
            typeFont,
            typeSize,
            ImVec2(textX(typeName, typeFont, typeSize), nameTop + 36.0f),
            faded(style::kTextDim),
            typeName);
    }

    void ContentBrowser::DrawListRow(const ContentEntry& entry, const float width, const bool odd, const ContentBrowserHooks& hooks)
    {
        ImGui::InvisibleButton("##row", ImVec2(width, kListRowHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            selectedPath_ = entry.path;
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ContentThumbnail thumbnail = ImGui::IsRectVisible(min, max) ? ResolveThumbnail(entry, hooks, 64u) : ContentThumbnail{};
        {
            const std::string detail = DescribeEntry(entry, thumbnail);
            Tooltip(entry.name.c_str(), nullptr, detail.c_str());
        }

        const float alpha = DragFade(entry);
        const auto faded = [alpha](const ImU32 color) { return alpha < 1.0f ? style::WithAlpha(color, alpha) : color; };

        const bool selected = selectedPath_ == entry.path;
        const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const float centerY = std::floor((min.y + max.y) * 0.5f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (selected)
        {
            drawList->AddRectFilled(min, max, faded(panelFocused ? style::kPrimary : style::kSelectUnfocused));
        }
        else if (hovered)
        {
            drawList->AddRectFilled(min, max, faded(style::kControl));
        }
        else if (odd)
        {
            drawList->AddRectFilled(min, max, style::WithAlpha(style::kText, 0.03f));
        }

        // Type stripe, then a 20 x 20 picture (or the icon)
        const ImU32 typeColor = KindColor(entry.kind);
        drawList->AddRectFilled(ImVec2(min.x + 10.0f, centerY - 8.0f), ImVec2(min.x + 13.0f, centerY + 8.0f), faded(typeColor));
        const ImVec2 iconCenter(min.x + 28.0f, centerY);
        if (thumbnail.textureId != 0 && thumbnail.width > 0 && thumbnail.height > 0 && !thumbnail.pending)
        {
            ImVec2 imageMin;
            ImVec2 imageMax;
            FitInto(ImVec2(iconCenter.x - 10.0f, iconCenter.y - 10.0f), 20.0f, thumbnail.width, thumbnail.height, imageMin, imageMax);
            drawList->AddImage(
                static_cast<ImTextureID>(thumbnail.textureId),
                imageMin,
                imageMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                thumbnail.tint);
        }
        else
        {
            DrawIcon(drawList, IconSize::Row14, EntryIcon(entry), iconCenter, faded(typeColor));
        }

        const ListColumns columns = MakeListColumns(width);
        ImFont* bodyFont = nullptr;
        float bodySize = 0.0f;
        RoleFontOf(FontRole::Body, bodyFont, bodySize);
        ImFont* smallFont = nullptr;
        float smallSize = 0.0f;
        RoleFontOf(FontRole::Secondary, smallFont, smallSize);
        ImFont* monoFont = nullptr;
        float monoSize = 0.0f;
        RoleFontOf(FontRole::Mono, monoFont, monoSize);

        const float nameRight = NameColumnRight(columns, width) - 8.0f;
        drawList->PushClipRect(ImVec2(min.x + 46.0f, min.y), ImVec2(min.x + nameRight, max.y), true);
        drawList->AddText(bodyFont, bodySize, ImVec2(min.x + 46.0f, std::floor(centerY - bodySize * 0.5f)), faded(style::kTextStrong), entry.name.c_str());
        drawList->PopClipRect();

        const ImU32 dim = selected ? style::kTextStrong : style::kTextDim;
        const float smallY = std::floor(centerY - smallSize * 0.5f);
        if (columns.showType)
        {
            drawList->AddText(smallFont, smallSize, ImVec2(min.x + columns.typeX, smallY), faded(dim), KindName(entry.kind));
        }
        if (columns.showSize)
        {
            const std::string size = entry.kind == ContentKind::Folder ? std::string("\xE2\x80\x93") : FormatSize(entry.sizeBytes);
            const float sizeWidth = monoFont->CalcTextSizeA(monoSize, FLT_MAX, 0.0f, size.c_str()).x;
            drawList->AddText(
                monoFont,
                monoSize,
                ImVec2(min.x + columns.sizeX + ListColumns::kSizeWidth - 16.0f - sizeWidth, std::floor(centerY - monoSize * 0.5f)),
                faded(dim),
                size.c_str());
        }
        if (columns.showModified)
        {
            const std::string modified = FormatModified(entry.modifiedTime);
            drawList->AddText(smallFont, smallSize, ImVec2(min.x + columns.modifiedX, smallY), faded(dim), modified.c_str());
        }
    }

    void ContentBrowser::HandleEntry(
        const ContentEntry& entry,
        const ContentBrowserHooks& hooks,
        ContentEntry& toActivate,
        bool& activate)
    {
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            toActivate = entry;
            activate = true;
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
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(KindColor(entry.kind)));
            ImGui::TextUnformatted(EntryIcon(entry));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginPopupContextItem("##entry_menu"))
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
    }

    namespace
    {
        // A row of a View Options section: icon and label, a check on the right for the chosen one.
        // The popup stays open so the effect can be seen at once.
        bool OptionRow(const char* label, const bool chosen)
        {
            const bool pressed = ImGui::Selectable(label, false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(0.0f, 22.0f));
            if (chosen)
            {
                const ImVec2 max = ImGui::GetItemRectMax();
                const ImVec2 min = ImGui::GetItemRectMin();
                DrawIcon(ImGui::GetWindowDrawList(), IconSize::Row14, ICON_CHECK, ImVec2(max.x - 12.0f, (min.y + max.y) * 0.5f), style::kText);
            }
            return pressed;
        }

        void SectionCaption(const char* text)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            PopFontRole();
        }

        const char* SortKeyLabel(const ContentSortKey key)
        {
            switch (key)
            {
                case ContentSortKey::Name: return "Name";
                case ContentSortKey::Type: return "Type";
                case ContentSortKey::Size: return "Size";
                case ContentSortKey::Modified: return "Modified";
            }
            return "Name";
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

        // ... or while the editor itself is in front: look at the folder's write time once a second
        const double now = ImGui::GetTime();
        if (now - lastPollTime_ >= kPollSeconds)
        {
            lastPollTime_ = now;
            if (FolderStamp(ToAbsolute(root_, currentFolder_)) != folderStamp_)
            {
                Refresh();
            }
        }
        thumbnailBudget_ = kThumbnailsPerFrame;
        pendingThumbnails_ = 0;

        // Edge to edge: the tools row, the two wells and the footer lay themselves out
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 region = ImGui::GetContentRegionAvail();
        const float width = std::max(region.x, 160.0f);
        const float height = std::max(region.y, kToolsHeight + kFooterHeight + 40.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // ---- 1. Tools row: Refresh | breadcrumbs ...... Sort, Tiles / List, search, View Options
        {
            const float centerY = origin.y + kToolsHeight * 0.5f;
            const float buttonY = origin.y + (kToolsHeight - style::kPanelIconButton) * 0.5f;
            const float fieldY = origin.y + (kToolsHeight - style::kFrameHeight) * 0.5f;
            float x = origin.x + 8.0f;

            ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
            if (IconButton("##content_refresh", ICON_REFRESH_CW, "Refresh"))
            {
                Refresh();
            }
            x += style::kPanelIconButton + 8.0f;
            drawList->AddLine(ImVec2(x, origin.y + 10.0f), ImVec2(x, origin.y + kToolsHeight - 10.0f), style::kBorderLight, 1.0f);
            x += 10.0f;

            // From the right: View Options, search, Tiles | List, Sort
            const float buttonWidth = style::kPanelIconButton;
            const bool roomForSort = width > 640.0f;
            const float sortWidth = roomForSort ? 132.0f : 0.0f;
            const float optionsX = origin.x + width - 8.0f - buttonWidth;
            const float searchWidth = std::min(240.0f, std::max(width * 0.22f, 110.0f));
            const float searchX = optionsX - 8.0f - searchWidth;
            const float listX = searchX - 8.0f - buttonWidth;
            const float tilesX = listX - 2.0f - buttonWidth;
            const float sortX = tilesX - 8.0f - sortWidth;
            const float crumbsRight = (roomForSort ? sortX : tilesX) - 12.0f;

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

            // Sort: [icon  Name  arrow  chevron]. The arrow flips the direction, the rest opens the list of keys.
            if (roomForSort)
            {
                const float arrowZone = 24.0f;
                const float chevronZone = 22.0f;
                const ImVec2 sortMin(sortX, fieldY);
                const ImVec2 sortMax(sortX + sortWidth, fieldY + style::kFrameHeight);
                drawList->AddRectFilled(sortMin, sortMax, style::kControl, style::kRounding);

                ImGui::SetCursorScreenPos(sortMin);
                const bool openMenu = ImGui::InvisibleButton("##sort_field", ImVec2(sortWidth - arrowZone - chevronZone, style::kFrameHeight));
                Tooltip("Sort by");
                ImGui::SetCursorScreenPos(ImVec2(sortX + sortWidth - arrowZone - chevronZone, fieldY));
                const bool flip = ImGui::InvisibleButton("##sort_direction", ImVec2(arrowZone, style::kFrameHeight));
                Tooltip(sortAscending_ ? "Ascending" : "Descending");
                ImGui::SetCursorScreenPos(ImVec2(sortX + sortWidth - chevronZone, fieldY));
                const bool openMenuByChevron = ImGui::InvisibleButton("##sort_chevron", ImVec2(chevronZone, style::kFrameHeight));

                DrawIcon(drawList, IconSize::Row14, ICON_ARROW_UP_DOWN, ImVec2(sortX + 14.0f, centerY), style::kText);
                ImFont* strongFont = nullptr;
                float strongSize = 0.0f;
                RoleFontOf(FontRole::Strong, strongFont, strongSize);
                drawList->AddText(strongFont, strongSize, ImVec2(sortX + 28.0f, std::floor(centerY - strongSize * 0.5f)), style::kTextStrong, SortKeyLabel(sortKey_));
                DrawIcon(
                    drawList,
                    IconSize::Chevron12,
                    sortAscending_ ? ICON_ARROW_UP : ICON_ARROW_DOWN,
                    ImVec2(sortX + sortWidth - chevronZone - arrowZone * 0.5f, centerY),
                    style::kTextDim);
                DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(sortX + sortWidth - chevronZone * 0.5f, centerY), style::kTextDim);

                if (openMenu || openMenuByChevron)
                {
                    ImGui::OpenPopup("##content_sort_menu");
                }
                if (flip)
                {
                    SetSort(sortKey_, !sortAscending_);
                }
                ImGui::SetNextWindowPos(ImVec2(sortX, fieldY + style::kFrameHeight + 2.0f));
                if (ImGui::BeginPopup("##content_sort_menu"))
                {
                    for (const ContentSortKey key : {ContentSortKey::Name, ContentSortKey::Type, ContentSortKey::Size, ContentSortKey::Modified})
                    {
                        if (ImGui::MenuItem(SortKeyLabel(key), nullptr, key == sortKey_))
                        {
                            SetSort(key, sortAscending_);
                        }
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::SetCursorScreenPos(ImVec2(tilesX, buttonY));
            if (IconButton("##content_view_tiles", ICON_LAYOUT_GRID, "Tiles", viewMode_ == ContentViewMode::Tiles))
            {
                viewMode_ = ContentViewMode::Tiles;
            }
            ImGui::SetCursorScreenPos(ImVec2(listX, buttonY));
            if (IconButton("##content_view_list", ICON_LIST, "List", viewMode_ == ContentViewMode::List))
            {
                viewMode_ = ContentViewMode::List;
            }

            ImGui::SetCursorScreenPos(ImVec2(searchX, fieldY));
            std::string searchText = search_;
            const std::string hint = "Search " + (currentFolder_.empty() ? rootLabel_ : fs::u8path(currentFolder_).filename().u8string());
            if (SearchField("##content_search", &searchText, hint.c_str(), searchWidth))
            {
                SetSearch(searchText);
            }

            ImGui::SetCursorScreenPos(ImVec2(optionsX, buttonY));
            if (IconButton("##content_view_options", ICON_SLIDERS_HORIZONTAL, "View Options"))
            {
                ImGui::OpenPopup("##content_view_options_popup");
            }
            ImGui::SetNextWindowPos(ImVec2(optionsX + buttonWidth, buttonY + buttonWidth + 2.0f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f));
            if (ImGui::BeginPopup("##content_view_options_popup"))
            {
                SectionCaption("VIEW");
                if (OptionRow(ICON_LAYOUT_GRID "  Tiles", viewMode_ == ContentViewMode::Tiles))
                {
                    viewMode_ = ContentViewMode::Tiles;
                }
                if (OptionRow(ICON_LIST "  List", viewMode_ == ContentViewMode::List))
                {
                    viewMode_ = ContentViewMode::List;
                }

                ImGui::Separator();
                SectionCaption("TILE SIZE");
                ImGui::BeginDisabled(viewMode_ == ContentViewMode::List);
                {
                    float size = tileSize_;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 44.0f);
                    if (ImGui::SliderFloat("##tile_size", &size, kMinTileSize, kMaxTileSize, "", ImGuiSliderFlags_NoInput))
                    {
                        SetTileSize(std::round(size / 8.0f) * 8.0f);
                    }
                    ImGui::SameLine();
                    PushFontRole(FontRole::Mono);
                    ImGui::Text("%d", static_cast<int>(tileSize_));
                    PopFontRole();
                }
                ImGui::EndDisabled();

                ImGui::Separator();
                SectionCaption("SORT BY");
                for (const ContentSortKey key : {ContentSortKey::Name, ContentSortKey::Type, ContentSortKey::Size, ContentSortKey::Modified})
                {
                    if (OptionRow(SortKeyLabel(key), key == sortKey_))
                    {
                        SetSort(key, sortAscending_);
                    }
                }
                ImGui::Separator();
                if (OptionRow("Ascending", sortAscending_))
                {
                    SetSort(sortKey_, true);
                }
                if (OptionRow("Descending", !sortAscending_))
                {
                    SetSort(sortKey_, false);
                }
                ImGui::Separator();
                bool foldersFirst = foldersFirst_;
                if (Checkbox("Folders first", &foldersFirst))
                {
                    SetFoldersFirst(foldersFirst);
                }
                ImGui::EndPopup();
            }
        }

        // ---- 2. Sources tree and the items
        const float bodyTop = origin.y + kToolsHeight;
        const float bodyHeight = std::max(height - kToolsHeight - kFooterHeight, 40.0f);
        const float treeWidth = std::clamp(width * 0.2f, 140.0f, 230.0f);
        const float itemsLeft = origin.x + treeWidth + style::kSplitter;
        const float itemsWidth = std::max(width - treeWidth - style::kSplitter, 40.0f);

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
        revealCurrent_ = false;

        const bool listMode = viewMode_ == ContentViewMode::List;
        float itemsTop = bodyTop;
        float itemsHeight = bodyHeight;

        // List header: Name / Type / Size / Modified, a click sorts
        if (listMode)
        {
            const float headerHeight = kListHeaderHeight;
            drawList->AddRectFilled(ImVec2(itemsLeft, bodyTop), ImVec2(itemsLeft + itemsWidth, bodyTop + headerHeight), style::kHeader);
            drawList->AddLine(
                ImVec2(itemsLeft, bodyTop + headerHeight - 1.0f),
                ImVec2(itemsLeft + itemsWidth, bodyTop + headerHeight - 1.0f),
                style::kInput,
                1.0f);

            // The rows keep room for the scrollbar, so the columns of the header and of the rows line up
            const float columnsWidth = itemsWidth - ImGui::GetStyle().ScrollbarSize;
            const ListColumns columns = MakeListColumns(columnsWidth);
            struct Column
            {
                ContentSortKey key;
                float x;
                float width;
                bool show;
                bool rightAligned;
            };
            const float nameRight = NameColumnRight(columns, columnsWidth);
            const Column headerColumns[] = {
                {ContentSortKey::Name, 0.0f, nameRight, true, false},
                {ContentSortKey::Type, columns.typeX, ListColumns::kTypeWidth, columns.showType, false},
                {ContentSortKey::Size, columns.sizeX, ListColumns::kSizeWidth, columns.showSize, true},
                {ContentSortKey::Modified, columns.modifiedX, ListColumns::kModifiedWidth, columns.showModified, false},
            };

            ImFont* font = nullptr;
            float size = 0.0f;
            RoleFontOf(FontRole::Secondary, font, size);
            for (const Column& column : headerColumns)
            {
                if (!column.show)
                {
                    continue;
                }

                const char* label = SortKeyLabel(column.key);
                const float cellX = itemsLeft + column.x;
                ImGui::SetCursorScreenPos(ImVec2(cellX, bodyTop));
                ImGui::PushID(label);
                const bool pressed = ImGui::InvisibleButton("##header", ImVec2(column.width, headerHeight - 1.0f));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered)
                {
                    drawList->AddRectFilled(ImVec2(cellX, bodyTop), ImVec2(cellX + column.width, bodyTop + headerHeight - 1.0f), style::kControl);
                }

                const bool active = column.key == sortKey_;
                const float textWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x;
                float textX = column.key == ContentSortKey::Name ? cellX + 10.0f : cellX;
                if (column.rightAligned)
                {
                    textX = cellX + column.width - 16.0f - textWidth;
                }
                const float textY = std::floor(bodyTop + (headerHeight - size) * 0.5f);
                drawList->AddText(font, size, ImVec2(textX, textY), active ? style::kTextStrong : style::kTextDim, label);
                if (active)
                {
                    // The arrow follows the label; in a right aligned column it goes in front, away from the next column
                    const float arrowX = column.rightAligned ? textX - 9.0f : textX + textWidth + 9.0f;
                    DrawIcon(
                        drawList,
                        IconSize::Chevron12,
                        sortAscending_ ? ICON_ARROW_UP : ICON_ARROW_DOWN,
                        ImVec2(arrowX, bodyTop + headerHeight * 0.5f),
                        style::kTextDim);
                }

                if (pressed)
                {
                    SetSort(column.key, active ? !sortAscending_ : true);
                }
            }

            itemsTop += headerHeight;
            itemsHeight = std::max(bodyHeight - headerHeight, 20.0f);
        }

        ImGui::SetCursorScreenPos(ImVec2(itemsLeft, itemsTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, listMode ? ImVec2(0.0f, 0.0f) : ImVec2(kTileGap, kTileGap));
        const bool tilesShown = ImGui::BeginChild("##content_tiles", ImVec2(itemsWidth, itemsHeight), ImGuiChildFlags_None);
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
            else if (listMode)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                const float rowWidth = itemsWidth - ImGui::GetStyle().ScrollbarSize;

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(entries_.size()), kListRowHeight);
                while (clipper.Step())
                {
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                    {
                        const ContentEntry& entry = entries_[static_cast<std::size_t>(index)];
                        ImGui::PushID(index);
                        DrawListRow(entry, rowWidth, (index % 2) == 1, hooks);
                        HandleEntry(entry, hooks, toActivate, activate);
                        ImGui::PopID();
                    }
                }
                clipper.End();
                ImGui::PopStyleVar();
            }
            else
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileGap, kTileGap));
                const float available = ImGui::GetContentRegionAvail().x;
                const int columns = std::max(1, static_cast<int>((available + kTileGap) / (tileSize_ + kTileGap)));
                const int rows = (static_cast<int>(entries_.size()) + columns - 1) / columns;
                const float tileHeight = tileSize_ + kTileTextHeight;

                ImGuiListClipper clipper;
                clipper.Begin(rows, tileHeight + kTileGap);
                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        for (int column = 0; column < columns; ++column)
                        {
                            const int index = row * columns + column;
                            if (index >= static_cast<int>(entries_.size()))
                            {
                                break;
                            }
                            if (column > 0)
                            {
                                ImGui::SameLine();
                            }

                            const ContentEntry& entry = entries_[static_cast<std::size_t>(index)];
                            ImGui::PushID(index);
                            DrawTile(entry, tileSize_, hooks);
                            HandleEntry(entry, hooks, toActivate, activate);
                            ImGui::PopID();
                        }
                    }
                }
                clipper.End();
                ImGui::PopStyleVar();
            }
        }
        ImGui::EndChild();

        // ---- 3. Footer: "N items (1 selected)" and, on the right, the thumbnails that are still coming
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
            const float textY = std::floor(footerTop + (kFooterHeight - size) * 0.5f);
            drawList->AddText(font, size, ImVec2(origin.x + 12.0f, textY), style::kTextDim, text.c_str());

            if (pendingThumbnails_ > 0)
            {
                const std::string pending = "Generating thumbnails\xE2\x80\xA6 " + std::to_string(pendingThumbnails_);
                const float pendingWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, pending.c_str()).x;
                drawList->AddText(font, size, ImVec2(origin.x + width - 12.0f - pendingWidth, textY), style::kTextDim, pending.c_str());
            }
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, height));

        // After the loops: these rebuild the list that the items above came from
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
