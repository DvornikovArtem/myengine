#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <myengine/core/ProjectContext.h>

namespace
{
    namespace fs = std::filesystem;
    namespace core = myengine::core;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WriteFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    struct Fixture
    {
        Fixture()
        {
            base = fs::temp_directory_path() / ("myengine_project_tests_" + std::to_string(GetCurrentProcessId()));
            fs::remove_all(base);
            fs::create_directories(base);
        }

        ~Fixture()
        {
            std::error_code error;
            fs::remove_all(base, error);
        }

        fs::path base;
    };

    void TestDescriptor()
    {
        Fixture fixture;

        // A file with only a name gets the defaults of the format
        const fs::path minimal = fixture.base / "Minimal.myproject";
        WriteFile(minimal, "\xEF\xBB\xBF{ \"name\": \"Minimal\" }");
        core::ProjectDescriptor descriptor;
        std::string error;
        Check(core::project::ReadDescriptor(minimal, descriptor, &error), "A minimal project (with a BOM) is read");
        Check(descriptor.name == "Minimal", "The name");
        Check(descriptor.contentDir == "Content" && descriptor.mapsDir == "Maps", "The default folders");
        Check(descriptor.scriptsDir == "Content/Scripts" && descriptor.prefabsDir == "Content/Prefabs", "The default scripts and prefabs");
        Check(descriptor.defaultMap == "Maps/Main.json", "The default map");

        // Without a name the file name is used
        const fs::path unnamed = fixture.base / "Unnamed.myproject";
        WriteFile(unnamed, "{}");
        Check(core::project::ReadDescriptor(unnamed, descriptor, &error) && descriptor.name == "Unnamed", "The name falls back to the file name");

        // Round trip
        core::ProjectDescriptor written;
        written.name = "Round";
        written.contentDir = "assets";
        written.mapsDir = "assets/scenes";
        written.scriptsDir = "assets/scripts";
        written.prefabsDir = "assets/prefabs";
        written.defaultMap = "assets/scenes/a.json";
        const fs::path roundTrip = fixture.base / "Round.myproject";
        Check(core::project::WriteDescriptor(roundTrip, written, &error), "The descriptor is written");
        core::ProjectDescriptor read;
        Check(core::project::ReadDescriptor(roundTrip, read, &error), "The written descriptor is read back");
        Check(read.name == "Round" && read.contentDir == "assets" && read.defaultMap == "assets/scenes/a.json", "The fields survive");

        // Refused files
        const fs::path escaping = fixture.base / "Escaping.myproject";
        WriteFile(escaping, "{ \"name\": \"X\", \"contentDir\": \"../outside\" }");
        Check(!core::project::ReadDescriptor(escaping, read, &error), "A folder outside the project is refused");
        const fs::path absolute = fixture.base / "Absolute.myproject";
        WriteFile(absolute, "{ \"name\": \"X\", \"mapsDir\": \"C:/maps\" }");
        Check(!core::project::ReadDescriptor(absolute, read, &error), "An absolute folder is refused");
        const fs::path newer = fixture.base / "Newer.myproject";
        WriteFile(newer, "{ \"name\": \"X\", \"engineVersion\": 99 }");
        Check(!core::project::ReadDescriptor(newer, read, &error), "A project of a newer engine is refused");
        const fs::path broken = fixture.base / "Broken.myproject";
        WriteFile(broken, "{ not json");
        Check(!core::project::ReadDescriptor(broken, read, &error), "Broken JSON is refused");
        Check(!core::project::ReadDescriptor(fixture.base / "Missing.myproject", read, &error), "A missing file is refused");
    }

    void TestCreateAndContext()
    {
        Fixture fixture;

        fs::path projectFile;
        std::string error;
        Check(!core::project::CreateProject(fixture.base, "bad/name", projectFile, &error), "A name with a slash is refused");
        Check(!core::project::CreateProject(fixture.base / "missing", "Game", projectFile, &error), "A missing parent folder is refused");
        Check(core::project::CreateProject(fixture.base, "Game", projectFile, &error), "A project is created");
        Check(projectFile == fixture.base / "Game" / "Game.myproject", "The project file is <folder>/<name>/<name>.myproject");

        for (const char* folder : {"Content/Scripts", "Content/Prefabs", "Content/Materials", "Content/Models", "Content/Textures", "Maps"})
        {
            Check(fs::is_directory(fixture.base / "Game" / folder), "The scaffold folder exists");
        }

        // The empty map is a scene with a camera and a floor
        std::ifstream mapFile(fixture.base / "Game/Maps/Main.json");
        const nlohmann::json map = nlohmann::json::parse(mapFile);
        Check(map["entities"].size() == 2, "The first map has a camera and a floor");
        Check(map["entities"][0].contains("Camera") && map["entities"][1].contains("MeshRenderer"), "The first map components");

        Check(!core::project::CreateProject(fixture.base, "Game", projectFile, &error), "An existing non-empty folder is refused");

        core::ProjectContext context;
        Check(!context.IsInitialized(), "A new context is empty");
        Check(context.Load(projectFile, &error), "The new project loads");
        Check(context.IsInitialized() && context.Name() == "Game", "The project name");
        Check(context.Root() == fixture.base / "Game", "The project root is the folder of the file");
        Check(context.ContentDir() == fixture.base / "Game" / "Content", "The content folder");
        Check(context.MapsDir() == fixture.base / "Game" / "Maps", "The maps folder");
        Check(context.ScriptsDir() == fixture.base / "Game" / "Content" / "Scripts", "The scripts folder");
        Check(fs::exists(context.DefaultMap()), "The default map exists");
        Check(context.ToProjectRelative(context.DefaultMap()) == "Maps/Main.json", "A project path");
        Check(context.ToProjectRelative(fixture.base / "elsewhere.json") == (fixture.base / "elsewhere.json").generic_u8string(),
            "A path outside the project stays absolute");
        Check(!context.IsEngineProject(), "A created project is not the engine project");
    }

    void TestResolveContentPath()
    {
        Fixture fixture;
        fs::path projectFile;
        std::string error;
        Check(core::project::CreateProject(fixture.base, "Game", projectFile, &error), "A project is created");

        core::ProjectContext context;
        Check(context.Load(projectFile, &error), "The project loads");

        // A file of the project wins over the engine file with the same path
        const fs::path own = fixture.base / "Game/assets/models/crate.obj";
        WriteFile(own, "o own");
        Check(context.ResolveContentPath("assets/models/crate.obj") == fs::weakly_canonical(own), "The project file wins");
        fs::remove(own);

        // The engine content is the fallback: the base material and the shaders are available to every project
        const fs::path engineMaterial = core::ProjectContext::EngineRoot() / "assets/materials/default.material.json";
        Check(fs::exists(engineMaterial), "The engine material exists (the test runs from the source tree)");
        const fs::path resolved = context.ResolveContentPath("assets/materials/default.material.json");
        Check(resolved == fs::weakly_canonical(engineMaterial), "A missing project file falls back to the engine content");

        // A path that exists nowhere points inside the project, where a new file would be created
        const fs::path fresh = context.ResolveContentPath("Content/Models/new_mesh.obj");
        Check(fresh == (fixture.base / "Game/Content/Models/new_mesh.obj").lexically_normal(), "A new file resolves inside the project");

        // Absolute paths are not searched
        Check(context.ResolveContentPath(own) == own.lexically_normal() || context.ResolveContentPath(own) == fs::weakly_canonical(own),
            "An absolute path stays itself");
    }

    void TestEngineProjectFile()
    {
        // The repository is the default project: myengine.myproject next to assets/, the assets are not moved
        const fs::path file = core::ProjectContext::EngineRoot() / core::kEngineProjectFileName;
        Check(fs::is_regular_file(file), "myengine.myproject is in the engine folder");

        core::ProjectContext context;
        std::string error;
        Check(context.Load(file, &error), "The default project loads");
        Check(context.IsEngineProject(), "The default project root is the engine root");
        Check(context.Descriptor().contentDir == "assets", "The default project keeps assets/");
        Check(fs::is_directory(context.ContentDir()) && fs::is_directory(context.MapsDir()), "Its folders exist");
        Check(fs::is_directory(context.ScriptsDir()) && fs::is_directory(context.PrefabsDir()), "Its scripts and prefabs exist");
        Check(fs::is_regular_file(context.DefaultMap()), "Its default map exists");

        core::ProjectContext builtIn;
        builtIn.InitializeDefault();
        Check(builtIn.IsEngineProject() && builtIn.ProjectFile().empty(), "The built-in default has no file");
        Check(builtIn.DefaultMap() == context.DefaultMap(), "The built-in default matches the file");
    }

    void TestRecentProjects()
    {
        Fixture fixture;
        const fs::path list = fixture.base / "app" / "recent_projects.json";
        Check(core::project::LoadRecentProjects(list).empty(), "No list yet");

        const fs::path a = fixture.base / "A" / "A.myproject";
        const fs::path b = fixture.base / "B" / "B.myproject";
        const fs::path c = fixture.base / "C" / "C.myproject";
        core::project::AddRecentProject(list, a);
        core::project::AddRecentProject(list, b);
        core::project::AddRecentProject(list, c);
        auto recent = core::project::LoadRecentProjects(list);
        Check(recent.size() == 3 && recent[0] == c && recent[2] == a, "The newest project is first");

        core::project::AddRecentProject(list, a);
        recent = core::project::LoadRecentProjects(list);
        Check(recent.size() == 3 && recent[0] == a && recent[1] == c, "An opened project moves to the front, without a duplicate");

        core::project::AddRecentProject(list, b, 2);
        recent = core::project::LoadRecentProjects(list);
        Check(recent.size() == 2 && recent[0] == b && recent[1] == a, "The list is trimmed");

        WriteFile(list, "not json");
        Check(core::project::LoadRecentProjects(list).empty(), "A broken list is empty, not an error");
    }
}

int main()
{
    try
    {
        TestDescriptor();
        TestCreateAndContext();
        TestResolveContentPath();
        TestEngineProjectFile();
        TestRecentProjects();
        std::cout << "OK: project descriptor, scaffold, content path lookup with engine fallback, default project, recent list\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
