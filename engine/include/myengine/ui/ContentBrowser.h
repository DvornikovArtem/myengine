// ContentBrowser.h

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace myengine::ui
{
    enum class ContentKind
    {
        Folder,
        Scene,
        Script,
        Prefab,
        Material,
        Mesh,
        Texture,
        Shader,
        Other,
    };

    // Tiles: a grid of equal squares with a thumbnail. List: one row per item with Name / Type / Size / Modified.
    enum class ContentViewMode
    {
        Tiles,
        List,
    };

    // Folders always come first; the key orders the items inside the folder group and the file group.
    enum class ContentSortKey
    {
        Name,
        Type,
        Size,
        Modified,
    };

    struct ContentEntry
    {
        std::string name; // file or folder name, as shown on the tile
        std::string relative; // generic path under the content root: "models/crate.obj", "" is the root itself
        std::string path; // asset key: the root folder name + relative, "assets/models/crate.obj"
        ContentKind kind = ContentKind::Other;
        std::uint64_t sizeBytes = 0; // files only
        std::int64_t modifiedTime = 0; // seconds since the Unix epoch, 0 when unknown
    };

    // The picture for a tile: an ImGui texture id (UiManager maps it to a render::TextureHandle).
    // textureId 0 means "no picture": the tile shows the icon of its type.
    struct ContentThumbnail
    {
        std::uint64_t textureId = 0;
        std::uint32_t width = 0; // pixels of the source, to keep the aspect ratio
        std::uint32_t height = 0;
        std::uint32_t tint = 0xFFFFFFFFu; // IM_COL32 colour that multiplies the picture
        bool hasAlpha = false; // the picture has transparent pixels: a checkerboard is drawn under it
        bool pending = false; // not ready yet, the browser asks again on the next frame
        // The id can change or die at any time (the preview service reuses and evicts its render targets): the
        // browser does not keep it and asks again every frame, which must be cheap
        bool live = false;
    };

    // What the browser does for the editor; every callback is optional.
    // The payload types are the ImGui drag-and-drop types that the viewport accepts (nullptr: no drag source).
    struct ContentBrowserHooks
    {
        // Double click or "Open" on a file (never a folder). assetPath is the asset key ("assets/textures/debug.bmp").
        // Return true when the editor opened the asset; false lets the browser fall back to the callbacks below
        // (and for a script, to the external editor). The viewers of AV register themselves here.
        std::function<bool(const std::string& assetPath, ContentKind kind)> onOpenAsset;
        // The fallback actions of the kinds that the editor has always opened
        std::function<void(const std::string& scenePath)> openScene;
        std::function<void(const std::string& prefabName)> openPrefab; // file name without ".prefab.json"
        std::function<void(const std::string& materialPath)> openMaterial;
        // The picture of an item at about pixelSize x pixelSize. Asked only for items that are on screen, a few
        // new items per frame; the answer is kept until the file or the size changes (unless it is `live` or
        // `pending`: then the item is asked again every frame). Folders are never asked.
        std::function<ContentThumbnail(const ContentEntry& entry, std::uint32_t pixelSize)> thumbnail;
        const char* meshPayloadType = nullptr;
        const char* materialPayloadType = nullptr;
        const char* texturePayloadType = nullptr;
        // The assistant takes these too (a prefab, a script, any other file: a map, a shader, a text file)
        const char* prefabPayloadType = nullptr;
        const char* scriptPayloadType = nullptr;
        const char* filePayloadType = nullptr;
    };

    // Content Browser: a folder tree, breadcrumbs, search and tiles or a list for the asset root on disk.
    // Read only: it does not create, rename or delete files.
    class ContentBrowser
    {
    public:
        ContentBrowser();

        // The folder that is shown as "Content", normally <project>/assets. Resets the navigation.
        // Asset keys are the folder name plus the path below it ("assets/models/crate.obj").
        void SetRoot(const std::filesystem::path& root);
        // A root whose keys start with keyPrefix (the root's path inside the project, "" when it is the project
        // folder itself). visiblePaths limits what is shown: only these folders (paths below the root) and
        // the folders that lead to them, e.g. {"Content", "Maps"} for a project folder.
        void SetRoot(
            const std::filesystem::path& root,
            const std::string& keyPrefix,
            const std::vector<std::string>& visiblePaths);
        const std::filesystem::path& GetRoot() const;
        // The name of the top node and of the first crumb: "Content" by default, the project name for a project folder
        void SetRootLabel(const std::string& label);
        const std::string& GetRootName() const; // folder name of the root: "assets"

        void Refresh(); // rescan the tree and the current folder
        bool OpenFolder(const std::string& relativePath); // "" is the root; false if there is no such folder
        const std::string& GetCurrentFolder() const;
        std::vector<std::string> GetBreadcrumbs() const; // {"Content", "prefabs"}

        void SetSearch(std::string text); // case-insensitive name filter, applied to the current folder and below
        const std::string& GetSearch() const;

        void SetViewMode(ContentViewMode mode);
        ContentViewMode GetViewMode() const;
        void SetSort(ContentSortKey key, bool ascending);
        ContentSortKey GetSortKey() const;
        bool IsSortAscending() const;
        void SetFoldersFirst(bool foldersFirst); // on by default
        bool GetFoldersFirst() const;
        void SetTileSize(float size); // clamped to 72..160
        float GetTileSize() const;

        // Folders first, then files, both in the sort order. With a search text: matches in the current folder and below.
        const std::vector<ContentEntry>& GetVisibleEntries() const;

        // Double click: enter a folder, open a scene / prefab / material through the hooks, a script in the external editor
        bool Activate(const ContentEntry& entry, const ContentBrowserHooks& hooks);

        void Draw(const ContentBrowserHooks& hooks); // inside an ImGui window

        static ContentKind Classify(const std::string& relativePath, bool isFolder);
        static std::filesystem::path ToAbsolute(const std::filesystem::path& root, const std::string& relativePath);
        // Engine files and folders that sit next to the assets and are not content: .git, Saved, *.myemesh, *.myetex
        static bool IsServiceName(const std::string& name, bool isFolder);
        static std::string FormatSize(std::uint64_t bytes); // "812 B", "14.2 KB", "3.1 MB"
        static std::string FormatModified(std::int64_t unixSeconds); // "2026-10-11 14:05", "" when unknown

    private:
        struct FolderNode
        {
            std::string name;
            std::string relative;
            std::vector<FolderNode> children;
        };

        struct CachedThumbnail
        {
            ContentThumbnail thumbnail;
            std::int64_t modifiedTime = 0; // the file version it was made for
            std::uint32_t pixelSize = 0;   // the size it was asked for
        };

        void BuildTree(FolderNode& node, int depth) const;
        bool IsVisibleFolder(const std::string& relative) const; // with visiblePaths: on the way to or below one of them
        bool ShowsFilesIn(const std::string& relative) const; // with visiblePaths: inside one of them
        void RebuildEntries();
        void SortEntries();
        ContentEntry MakeEntry(const std::filesystem::directory_entry& item, bool isFolder) const;
        ContentThumbnail ResolveThumbnail(const ContentEntry& entry, const ContentBrowserHooks& hooks, std::uint32_t pixelSize);

        void DrawTree(const FolderNode& node, bool isRoot, bool panelFocused);
        void DrawTile(const ContentEntry& entry, float tileSize, const ContentBrowserHooks& hooks);
        void DrawListRow(const ContentEntry& entry, float width, bool odd, const ContentBrowserHooks& hooks);
        // Double click, context menu and drag source of the item that was just drawn
        void HandleEntry(
            const ContentEntry& entry,
            const ContentBrowserHooks& hooks,
            ContentEntry& toActivate,
            bool& activate);

        std::filesystem::path root_;
        std::string rootName_;
        std::string keyPrefix_;
        std::string rootLabel_ = "Content";
        std::vector<std::string> visiblePaths_;
        std::string currentFolder_;
        std::string search_;
        std::string selectedPath_;
        std::vector<ContentEntry> entries_;
        FolderNode tree_;
        ContentViewMode viewMode_ = ContentViewMode::Tiles;
        ContentSortKey sortKey_ = ContentSortKey::Name;
        bool sortAscending_ = true;
        bool foldersFirst_ = true;
        float tileSize_ = 104.0f; // 72..160, set in View Options
        bool wasFocused_ = false;
        bool revealCurrent_ = false; // open the tree nodes above the current folder on the next draw

        std::unordered_map<std::string, CachedThumbnail> thumbnails_; // by asset key
        int thumbnailBudget_ = 0; // requests left in this frame
        int pendingThumbnails_ = 0; // pictures on screen that are not ready, counted in this frame
        std::int64_t folderStamp_ = 0; // last write time of the current folder when the list was built
        double lastPollTime_ = 0.0;
    };
}
