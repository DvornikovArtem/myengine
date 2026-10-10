// ProjectContext.h

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace myengine::core
{
    inline constexpr int kProjectFormatVersion = 1;
    inline constexpr char kProjectFileExtension[] = ".myproject";
    inline constexpr char kEngineProjectFileName[] = "myengine.myproject";

    // The content of <Name>.myproject. Every path is relative to the folder that holds the file.
    struct ProjectDescriptor
    {
        std::string name = "Project";
        int engineVersion = kProjectFormatVersion;
        std::string contentDir = "Content";
        std::string mapsDir = "Maps";
        std::string scriptsDir = "Content/Scripts";
        std::string prefabsDir = "Content/Prefabs";
        std::string defaultMap = "Maps/Main.json";
    };

    // The project the process works with (one per process, through ServiceLocator).
    // Content paths are looked up in the project first and then in the engine content
    // (shaders, fonts and the base materials), so any project can use the engine assets.
    class ProjectContext
    {
    public:
        // Reads a .myproject; on failure the context is left as it was and *error says why
        bool Load(const std::filesystem::path& projectFile, std::string* error = nullptr);
        // The engine folder as a project with the assets/ layout (no .myproject on disk)
        void InitializeDefault();

        bool IsInitialized() const;
        bool IsEngineProject() const; // the project root is the engine content root

        const std::filesystem::path& Root() const;
        const std::filesystem::path& ProjectFile() const; // empty for the built-in default
        const ProjectDescriptor& Descriptor() const;
        const std::string& Name() const;

        std::filesystem::path ContentDir() const; // absolute
        std::filesystem::path MapsDir() const;
        std::filesystem::path ScriptsDir() const;
        std::filesystem::path PrefabsDir() const;
        std::filesystem::path DefaultMap() const;

        // A path inside the project as "Maps/Main.json"; paths outside the project stay absolute
        std::string ToProjectRelative(const std::filesystem::path& absolute) const;

        // Absolute paths are normalised; relative ones are looked up in the project, the engine content,
        // the working directory and next to the executable, the first existing one wins.
        // A path that exists nowhere resolves inside the project (where a new file would be created).
        std::filesystem::path ResolveContentPath(const std::filesystem::path& path) const;

        // The engine content root: the source tree when it has assets/, otherwise the executable folder
        static std::filesystem::path EngineRoot();
        static std::filesystem::path ExecutableDirectory();

    private:
        std::filesystem::path root_;
        std::filesystem::path projectFile_;
        ProjectDescriptor descriptor_;
        bool initialized_ = false;
    };

    namespace project
    {
        bool ReadDescriptor(const std::filesystem::path& file, ProjectDescriptor& descriptor, std::string* error = nullptr);
        bool WriteDescriptor(const std::filesystem::path& file, const ProjectDescriptor& descriptor, std::string* error = nullptr);

        bool IsValidProjectName(const std::string& name);
        // The map that a new project and "New Map" start with: a camera and a floor, in the scene format
        std::string EmptyMapText();
        // <parent>/<name>/{<name>.myproject, Content/{Scripts,Prefabs,Materials,Models,Textures}, Maps/Main.json}
        bool CreateProject(
            const std::filesystem::path& parentDirectory,
            const std::string& name,
            std::filesystem::path& projectFile,
            std::string* error = nullptr);

        // %APPDATA%/myengine/recent_projects.json
        std::filesystem::path DefaultRecentProjectsFile();
        std::vector<std::filesystem::path> LoadRecentProjects(const std::filesystem::path& file);
        void AddRecentProject(
            const std::filesystem::path& file,
            const std::filesystem::path& projectFile,
            std::size_t maxCount = 10);

        // System dialogs; an empty path means that the dialog was cancelled
        std::filesystem::path PickProjectFile();
        std::filesystem::path PickFolder();
    }
}
