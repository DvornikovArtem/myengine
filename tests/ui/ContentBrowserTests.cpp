#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>
#include <imgui/imgui.h>

#include <myengine/ui/ContentBrowser.h>

namespace
{
    namespace fs = std::filesystem;
    using myengine::ui::ContentBrowser;
    using myengine::ui::ContentBrowserHooks;
    using myengine::ui::ContentEntry;
    using myengine::ui::ContentKind;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void Touch(const fs::path& path)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << "x";
    }

    // <temp>/myengine_content_browser_<pid>/assets, laid out like the real asset folder
    struct Fixture
    {
        Fixture()
        {
            base = fs::temp_directory_path() / ("myengine_content_browser_" + std::to_string(GetCurrentProcessId()));
            fs::remove_all(base);
            root = base / "assets";
            Touch(root / "scenes/level.json");
            Touch(root / "scenes/old/archive.json");
            Touch(root / "prefabs/coin.prefab.json");
            Touch(root / "materials/gold.material.json");
            Touch(root / "models/crate.obj");
            Touch(root / "models/crate.myemesh"); // the mesh cache that the resource manager writes
            Touch(root / "textures/debug.bmp");
            Touch(root / "scripts/coin.py");
            Touch(root / "shaders/lit.hlsl");
            Touch(root / "shaders/lit.shader.json");
            Touch(root / "manifests/demo.json");
            Touch(root / "readme.txt");
        }

        ~Fixture()
        {
            std::error_code error;
            fs::remove_all(base, error);
        }

        fs::path base;
        fs::path root;
    };

    std::vector<std::string> Names(const ContentBrowser& browser)
    {
        std::vector<std::string> names;
        for (const auto& entry : browser.GetVisibleEntries())
        {
            names.push_back(entry.name);
        }
        return names;
    }

    const ContentEntry* Find(const ContentBrowser& browser, const std::string& name)
    {
        for (const auto& entry : browser.GetVisibleEntries())
        {
            if (entry.name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    void TestClassify()
    {
        Check(ContentBrowser::Classify("models", true) == ContentKind::Folder, "A directory is a folder");
        Check(ContentBrowser::Classify("scenes/level.json", false) == ContentKind::Scene, ".json in scenes is a scene");
        Check(ContentBrowser::Classify("manifests/demo.json", false) == ContentKind::Other, ".json elsewhere is not a scene");
        Check(ContentBrowser::Classify("Maps/Main.json", false) == ContentKind::Scene, ".json in Maps is a scene");
        Check(ContentBrowser::Classify("Maps/Sub/Level.json", false) == ContentKind::Scene, "a nested map is a scene");
        Check(ContentBrowser::Classify("prefabs/coin.prefab.json", false) == ContentKind::Prefab, "prefab");
        Check(ContentBrowser::Classify("materials/gold.material.json", false) == ContentKind::Material, "material");
        Check(ContentBrowser::Classify("shaders/lit.shader.json", false) == ContentKind::Shader, "shader descriptor");
        Check(ContentBrowser::Classify("shaders/lit.hlsl", false) == ContentKind::Shader, "hlsl source");
        Check(ContentBrowser::Classify("scripts/coin.py", false) == ContentKind::Script, "script");
        Check(ContentBrowser::Classify("models/CRATE.OBJ", false) == ContentKind::Mesh, "extensions are case-insensitive");
        Check(ContentBrowser::Classify("textures/a.PNG", false) == ContentKind::Texture, "texture");
        Check(ContentBrowser::Classify("readme.txt", false) == ContentKind::Other, "unknown files are other");
    }

    void TestNavigation()
    {
        Fixture fixture;
        ContentBrowser browser;
        browser.SetRoot(fixture.root);

        Check(browser.GetRootName() == "assets", "The root name is the folder name");
        Check(browser.GetCurrentFolder().empty(), "The browser starts in the root");

        // Folders first, then files, each by name
        const auto rootNames = Names(browser);
        const std::vector<std::string> expected{"manifests", "materials", "models", "prefabs", "scenes", "scripts", "shaders", "textures", "readme.txt"};
        Check(rootNames == expected, "The root lists folders first, then files by name");

        Check(browser.OpenFolder("models"), "Opening an existing folder");
        Check(browser.GetCurrentFolder() == "models", "Current folder");
        const auto breadcrumbs = browser.GetBreadcrumbs();
        Check(breadcrumbs.size() == 2 && breadcrumbs[0] == "Content" && breadcrumbs[1] == "models", "Breadcrumbs");
        const ContentEntry* crate = Find(browser, "crate.obj");
        Check(crate != nullptr, "The mesh is listed");
        Check(crate->kind == ContentKind::Mesh, "The mesh kind");
        Check(crate->path == "assets/models/crate.obj", "The asset key starts with the root folder name");
        Check(crate->relative == "models/crate.obj", "The relative path");
        Check(Find(browser, "crate.myemesh") == nullptr, "Engine cache files are not shown");

        Check(browser.OpenFolder("scenes/old"), "Opening a nested folder with a slash");
        Check(browser.GetBreadcrumbs().size() == 3, "Breadcrumbs of a nested folder");
        Check(Find(browser, "archive.json") != nullptr, "The nested scene is listed");

        Check(!browser.OpenFolder("nope"), "A missing folder is refused");
        Check(!browser.OpenFolder("../outside"), "A path above the root is refused");
        Check(browser.GetCurrentFolder() == "scenes/old", "A refused navigation keeps the folder");

        Check(browser.OpenFolder(""), "Back to the root");
        Check(browser.GetBreadcrumbs().size() == 1, "Root breadcrumbs");
    }

    void TestSearch()
    {
        Fixture fixture;
        ContentBrowser browser;
        browser.SetRoot(fixture.root);

        browser.SetSearch("COIN");
        const auto found = Names(browser);
        Check(found.size() == 2, "Search finds the prefab and the script, case-insensitively");
        Check(Find(browser, "coin.prefab.json") != nullptr && Find(browser, "coin.py") != nullptr, "Search looks into subfolders");

        browser.SetSearch("scenes");
        Check(Find(browser, "scenes") != nullptr && Find(browser, "scenes")->kind == ContentKind::Folder, "Search matches folders too");

        browser.SetSearch("zzz-nothing");
        Check(browser.GetVisibleEntries().empty(), "No matches gives an empty list");

        browser.SetSearch("crate");
        Check(browser.OpenFolder("models"), "Navigation works while a search is active");
        Check(browser.GetSearch().empty(), "Opening a folder clears the search");
        Check(browser.GetVisibleEntries().size() == 1, "The folder content is back");
    }

    void TestActivate()
    {
        Fixture fixture;
        ContentBrowser browser;
        browser.SetRoot(fixture.root);

        std::string scene;
        std::string prefab;
        std::string material;
        ContentBrowserHooks hooks;
        hooks.openScene = [&](const std::string& path) { scene = path; };
        hooks.openPrefab = [&](const std::string& name) { prefab = name; };
        hooks.openMaterial = [&](const std::string& path) { material = path; };

        Check(browser.OpenFolder("scenes"), "scenes");
        Check(browser.Activate(*Find(browser, "level.json"), hooks), "A scene is activated");
        Check(scene == "assets/scenes/level.json", "The scene is opened by its asset key");

        Check(browser.OpenFolder("prefabs"), "prefabs");
        Check(browser.Activate(*Find(browser, "coin.prefab.json"), hooks), "A prefab is activated");
        Check(prefab == "coin", "The prefab is opened by its name");

        Check(browser.OpenFolder("materials"), "materials");
        Check(browser.Activate(*Find(browser, "gold.material.json"), hooks), "A material is activated");
        Check(material == "assets/materials/gold.material.json", "The material is opened by its asset key");

        Check(browser.OpenFolder(""), "root");
        Check(browser.Activate(*Find(browser, "models"), hooks), "A folder is entered");
        Check(browser.GetCurrentFolder() == "models", "The folder became current");

        Check(!browser.Activate(*Find(browser, "crate.obj"), hooks), "A mesh has no open action, it is dragged into the viewport");
        Check(browser.OpenFolder("textures"), "textures");
        Check(!browser.Activate(*Find(browser, "debug.bmp"), hooks), "A texture has no open action");
    }

    // A project folder with Content/ and Maps/ next to each other: the browser shows only those two,
    // and the asset keys are paths inside the project
    void TestProjectRoot()
    {
        Fixture fixture;
        const fs::path project = fixture.base / "game";
        Touch(project / "Content/Models/box.obj");
        Touch(project / "Content/Scripts/mover.py");
        Touch(project / "Maps/Main.json");
        Touch(project / "Maps/Sub/Level2.json");
        Touch(project / "Saved/cache.bin");
        Touch(project / "game.myproject");

        ContentBrowser browser;
        browser.SetRoot(project, "", {"Content", "Maps"});

        const auto top = Names(browser);
        const std::vector<std::string> expected{"Content", "Maps"};
        Check(top == expected, "Only the content and maps folders are shown, no project files");

        Check(browser.OpenFolder("Content/Models"), "Opening a folder of the content");
        const ContentEntry* box = Find(browser, "box.obj");
        Check(box != nullptr && box->path == "Content/Models/box.obj", "The key is a path inside the project");

        Check(browser.OpenFolder("Maps"), "Opening the maps folder");
        const ContentEntry* main = Find(browser, "Main.json");
        Check(main != nullptr && main->kind == ContentKind::Scene, "A .json in Maps is a map");
        Check(main->path == "Maps/Main.json", "The map key");
        Check(browser.OpenFolder("Maps/Sub"), "A nested maps folder");
        const ContentEntry* level = Find(browser, "Level2.json");
        Check(level != nullptr && level->kind == ContentKind::Scene, "A nested map is a map");

        Check(!browser.OpenFolder("Saved") || Find(browser, "cache.bin") == nullptr, "Other project folders are not listed");

        browser.SetSearch("mover");
        Check(browser.OpenFolder("") && browser.GetVisibleEntries().size() == 2, "The root lists the two folders again");
        browser.SetSearch("mover");
        Check(Find(browser, "mover.py") != nullptr, "Search finds a script below Content");
        browser.SetSearch("cache");
        Check(browser.GetVisibleEntries().empty(), "Search does not enter hidden folders");
        browser.SetSearch("game");
        Check(browser.GetVisibleEntries().empty(), "The project file is not listed");
    }

    void TestRefreshAndMissingRoot()
    {
        Fixture fixture;
        ContentBrowser browser;
        browser.SetRoot(fixture.root);
        Check(browser.OpenFolder("scenes/old"), "nested folder");

        Touch(fixture.root / "scenes/old/second.json");
        Check(Find(browser, "second.json") == nullptr, "The list does not change by itself");
        browser.Refresh();
        Check(Find(browser, "second.json") != nullptr, "Refresh picks up a new file");

        fs::remove_all(fixture.root / "scenes/old");
        browser.Refresh();
        Check(browser.GetCurrentFolder().empty(), "A removed current folder falls back to the root");

        ContentBrowser empty;
        Check(empty.GetVisibleEntries().empty(), "A browser without a root has nothing to show");
        Check(!empty.OpenFolder(""), "A browser without a root cannot navigate");
    }

    // One headless frame: the panel must draw the tiles and the tree without touching anything it should not
    void TestDrawFrame()
    {
        Fixture fixture;
        ContentBrowser browser;
        browser.SetRoot(fixture.root);

        IMGUI_CHECKVERSION();
        ImGuiContext* context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1024.0f, 768.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ContentBrowserHooks hooks;
        hooks.meshPayloadType = "TEST_MESH";
        hooks.materialPayloadType = "TEST_MATERIAL";
        hooks.texturePayloadType = "TEST_TEXTURE";

        for (int frame = 0; frame < 3; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f));
            ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoSavedSettings);
            browser.Draw(hooks);
            ImGui::End();
            ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "The content browser did not draw anything");
        }

        ImGui::DestroyContext(context);
    }
}

int main()
{
    try
    {
        TestClassify();
        TestNavigation();
        TestSearch();
        TestActivate();
        TestProjectRoot();
        TestRefreshAndMissingRoot();
        TestDrawFrame();
        std::cout << "OK: content browser classifies assets, lists and searches folders, navigates, activates and draws\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
