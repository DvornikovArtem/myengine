// ContentBrowser.h

#pragma once

#include <filesystem>
#include <functional>
#include <string>
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

    struct ContentEntry
    {
        std::string name; // file or folder name, as shown on the tile
        std::string relative; // generic path under the content root: "models/crate.obj", "" is the root itself
        std::string path; // asset key: the root folder name + relative, "assets/models/crate.obj"
        ContentKind kind = ContentKind::Other;
    };

    // What the browser does for the editor; every callback is optional.
    // The payload types are the ImGui drag-and-drop types that the viewport accepts (nullptr: no drag source).
    struct ContentBrowserHooks
    {
        std::function<void(const std::string& scenePath)> openScene;
        std::function<void(const std::string& prefabName)> openPrefab; // file name without ".prefab.json"
        std::function<void(const std::string& materialPath)> openMaterial;
        const char* meshPayloadType = nullptr;
        const char* materialPayloadType = nullptr;
        const char* texturePayloadType = nullptr;
    };

    // Content Browser: a folder tree, breadcrumbs, search and tiles for the asset root on disk.
    // Read only: it does not create, rename or delete files.
    class ContentBrowser
    {
    public:
        ContentBrowser();

        // The folder that is shown as "Content", normally <project>/assets. Resets the navigation.
        void SetRoot(const std::filesystem::path& root);
        const std::filesystem::path& GetRoot() const;
        const std::string& GetRootName() const; // folder name of the root: "assets"

        void Refresh(); // rescan the tree and the current folder
        bool OpenFolder(const std::string& relativePath); // "" is the root; false if there is no such folder
        const std::string& GetCurrentFolder() const;
        std::vector<std::string> GetBreadcrumbs() const; // {"Content", "prefabs"}

        void SetSearch(std::string text); // case-insensitive name filter, applied to the current folder and below
        const std::string& GetSearch() const;

        // Folders first, then files by name. With a search text: matches in the current folder and below.
        const std::vector<ContentEntry>& GetVisibleEntries() const;

        // Double click: enter a folder, open a scene / prefab / material through the hooks, a script in the external editor
        bool Activate(const ContentEntry& entry, const ContentBrowserHooks& hooks);

        void Draw(const ContentBrowserHooks& hooks); // inside an ImGui window

        static ContentKind Classify(const std::string& relativePath, bool isFolder);
        static std::filesystem::path ToAbsolute(const std::filesystem::path& root, const std::string& relativePath);

    private:
        struct FolderNode
        {
            std::string name;
            std::string relative;
            std::vector<FolderNode> children;
        };

        void BuildTree(FolderNode& node, int depth) const;
        void RebuildEntries();
        ContentEntry MakeEntry(const std::filesystem::path& absolute, bool isFolder) const;
        void DrawTree(const FolderNode& node, bool isRoot);
        void DrawTile(const ContentEntry& entry, float tileSize, const ContentBrowserHooks& hooks);

        std::filesystem::path root_;
        std::string rootName_;
        std::string currentFolder_;
        std::string search_;
        std::string selectedPath_;
        std::vector<ContentEntry> entries_;
        FolderNode tree_;
        float tileSize_ = 92.0f;
        bool wasFocused_ = false;
    };
}
