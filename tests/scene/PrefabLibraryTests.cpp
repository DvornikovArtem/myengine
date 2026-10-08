#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>

#include <myengine/core/FileWatcher.h>
#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>

namespace
{
    namespace ecs = myengine::ecs;
    namespace components = ecs::components;
    using json = nlohmann::json;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        Check(static_cast<bool>(file), "Could not read test JSON");
        json data;
        file >> data;
        return data;
    }

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        Check(static_cast<bool>(file), "Could not write test JSON");
        // Even rapid successive writes have distinct mtimes on the test filesystem.
        static unsigned revision = 0;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::milliseconds(++revision));
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine T4 шаблоны " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique test directory");
            const auto assets = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/prefabs";
            for (const auto* name : {"coin", "chaser", "firework"})
            {
                const auto file = std::string(name) + ".prefab.json";
                std::filesystem::copy_file(assets / file, directory / file);
            }
            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(directory / "test.log"), "Logger initialization failed");
            Check(library.Initialize(directory, logger.get()), "PrefabLibrary initialization failed");
        }

        ~Fixture()
        {
            library.Shutdown(); // wait for the scan before deleting its files / shutting down jobs
            logger.reset();
            if (!keepArtifacts)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }

        template <class Predicate>
        void WaitFor(Predicate&& ready)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;)
            {
                library.Poll();
                if (ready())
                {
                    return;
                }
                Check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for prefab cache invalidation");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        std::filesystem::path directory;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::scene::PrefabLibrary library;
        bool keepArtifacts = false;
    };

    void TestFirstFullScan(Fixture& fixture)
    {
        const auto path = fixture.directory / "initial.prefab.json";
        WriteText(path, "first scan contents");
        const auto mainThread = std::this_thread::get_id();
        bool delivered = false;
        myengine::core::FileWatcher watcher(std::chrono::milliseconds(0));
        watcher.Watch(path, [&](const myengine::core::FileChange& change)
            {
                Check(std::this_thread::get_id() == mainThread, "FileWatcher callback ran on a worker");
                Check(change.contents == "first scan contents", "First full rescan did not read the file");
                delivered = true;
            });
        watcher.RequestFullRescan();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!delivered)
        {
            watcher.Poll();
            Check(std::chrono::steady_clock::now() < deadline, "First full rescan was ignored");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        watcher.Stop();
        std::filesystem::remove(path);
    }

    void TestCopies(Fixture& fixture)
    {
        auto& library = fixture.library;
        Check(library.ListPrefabs() == std::vector<std::string>{"chaser", "coin", "firework"}, "Prefab list is incomplete or unsorted");
        ecs::World world;
        const auto existing = world.CreateEntity(); // prefab local id 1 must not refer to this entity
        world.Emplace<components::TagComponent>(existing).name = "Existing";
        const auto original = myengine::scene::SerializeEntity(world, existing);
        const auto coin = library.Instantiate(world, "coin");
        const components::Vec3 position{10, 2, 3};
        const auto other = library.Instantiate(world, "coin", &position);
        Check(coin != ecs::kInvalidEntity && other != ecs::kInvalidEntity && coin != other && coin != existing, "Copies reused an entity id");
        Check(world.GetEntities().size() == 5, "Coin children were not instantiated");
        Check(myengine::scene::SerializeEntity(world, existing) == original, "Spawning changed an existing entity");
        Check(world.Get<components::TransformComponent>(coin).position.y == 0.5f, "Default root position was lost");
        Check(world.Get<components::TransformComponent>(other).position.x == 10, "Root position override was ignored");
        Check(world.Get<components::TransformComponent>(other).scale.x == 0.4f, "Position override changed the scale");
        const auto child = world.Get<components::HierarchyComponent>(coin).children.at(0);
        const auto otherChild = world.Get<components::HierarchyComponent>(other).children.at(0);
        Check(child != otherChild && world.Get<components::HierarchyComponent>(child).parent == coin && world.Get<components::HierarchyComponent>(otherChild).parent == other, "Parent ids were not remapped");
        Check(world.Get<components::TransformComponent>(otherChild).position.x == 0, "Child local position was overridden");
        Check(world.Get<components::TagComponent>(child).name == "CoinGlow", "Child components were lost");
        auto& props = world.Get<components::ScriptComponent>(coin).scripts.at(0).props;
        props["score_value"] = 99;
        Check(world.Get<components::ScriptComponent>(other).scripts.at(0).props.at("score_value") == 1, "Copies share script props");
        Check(library.GetPrefabJson("coin")->at("entities")[0]["Script"]["scripts"][0]["props"]["score_value"] == 1, "An instance changed the cached template");

        // Child-first records, three levels and arbitrary local ids; root is not the first record.
        const json tree = {{"entities", json::array({
            {{"id", 30}, {"Tag", {{"name", "Leaf"}}}, {"Hierarchy", {{"parent", 20}}}},
            {{"id", 20}, {"Tag", {{"name", "Middle"}}}, {"Hierarchy", {{"parent", 10}}}},
            {{"id", 10}, {"Tag", {{"name", "Root"}}}}
        })}};
        WriteText(fixture.directory / "tree.prefab.json", tree.dump());
        const auto root = library.Instantiate(world, "tree", &position);
        Check(root != 0 && world.Get<components::TagComponent>(root).name == "Root", "Wrong root for a child-first prefab");
        const auto middle = world.Get<components::HierarchyComponent>(root).children.at(0);
        const auto leaf = world.Get<components::HierarchyComponent>(middle).children.at(0);
        Check(world.Get<components::HierarchyComponent>(leaf).parent == middle, "Grandchild link was not restored");
        Check(world.Get<components::TransformComponent>(root).position.z == 3, "Missing root Transform was not created for position override");
    }

    void TestInvalidData(Fixture& fixture)
    {
        ecs::World world;
        world.Emplace<components::TagComponent>(world.CreateEntity()).name = "Keep";
        const auto original = myengine::scene::SerializeWorldToString(world);
        auto valid = ReadJson(fixture.directory / "coin.prefab.json");
        std::vector<json> invalid;
        invalid.push_back(json::object());
        invalid.push_back({{"entities", json::array()}});
        for (const json id : {json(0), json(-1), json("1"), json(4294967296ULL)})
        {
            auto data = valid;
            data["entities"][0]["id"] = id;
            invalid.push_back(std::move(data));
        }
        auto duplicate = valid;
        duplicate["entities"][1]["id"] = 1;
        invalid.push_back(duplicate);
        for (const json parent : {json(99), json(2), json(-1), json("1")})
        {
            auto data = valid;
            data["entities"][1]["Hierarchy"]["parent"] = parent;
            invalid.push_back(std::move(data));
        }
        auto cycle = valid;
        cycle["entities"][0]["Hierarchy"]["parent"] = 2;
        invalid.push_back(cycle);
        auto forest = valid;
        forest["entities"][1].erase("Hierarchy");
        invalid.push_back(forest);
        auto badProps = valid;
        badProps["entities"][0]["Script"]["scripts"][0]["props"] = json::array({1});
        invalid.push_back(badProps);
        auto badComponent = valid;
        badComponent["entities"][1]["Tag"]["name"] = 10;
        invalid.push_back(badComponent);
        for (std::size_t index = 0; index < invalid.size(); ++index)
        {
            const auto name = "invalid_" + std::to_string(index);
            WriteText(fixture.directory / (name + ".prefab.json"), invalid[index].dump());
            Check(fixture.library.Instantiate(world, name) == 0, "Malformed prefab was accepted");
            Check(!fixture.library.GetLastError().empty(), "Malformed prefab has no diagnostic");
            Check(myengine::scene::SerializeWorldToString(world) == original, "Malformed prefab left partial entities");
        }
        WriteText(fixture.directory / "syntax.prefab.json", "{unfinished");
        Check(fixture.library.Instantiate(world, "syntax") == 0, "Invalid JSON syntax was accepted");
        for (const auto* name : {"missing", "../coin", "..\\coin", "", "C:\\coin", "CON", "nul", "COM1.extra"})
        {
            Check(fixture.library.Instantiate(world, name) == 0, "Missing or unsafe prefab name was accepted");
        }
        const components::Vec3 position{std::numeric_limits<float>::quiet_NaN(), 0, 0};
        Check(fixture.library.Instantiate(world, "coin", &position) == 0, "Non-finite position was accepted");
        Check(myengine::scene::SerializeWorldToString(world) == original, "Failed spawn changed the world");

        auto* cached = fixture.library.GetPrefabJson("coin");
        *cached = badComponent;
        Check(fixture.library.Instantiate(world, "coin") == 0, "Invalid unsaved editor data was accepted");
        Check(!fixture.library.SavePrefab("coin"), "Invalid editor data was saved");
        Check(ReadJson(fixture.directory / "coin.prefab.json") == valid, "A failed save corrupted the file");
        *cached = valid;
    }

    void TestSaveAndWatcher(Fixture& fixture)
    {
        auto& library = fixture.library;
        const auto path = fixture.directory / "coin.prefab.json";
        const auto original = ReadJson(path);
        auto* edited = library.GetPrefabJson("coin");
        (*edited)["entities"][0]["Script"]["scripts"][0]["props"]["score_value"] = 7;
        Check(library.SavePrefab("coin"), "Saving edited props failed");
        Check(ReadJson(path)["entities"][0]["Script"]["scripts"][0]["props"]["score_value"] == 7, "Saved props did not reach disk");
        ecs::World world;
        const auto old = library.Instantiate(world, "coin");
        Check(world.Get<components::ScriptComponent>(old).scripts[0].props["score_value"] == 7, "Saved props were not used for spawning");
        Check(library.GetPrefabJson("COIN") != nullptr, "Windows case-insensitive prefab lookup failed");

        // Failed atomic replace keeps the previous file; cleanup removes only our temporary sibling.
        Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "Could not make the test prefab read-only");
        const bool savedReadOnly = library.SavePrefab("coin");
        const bool writableAgain = SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0;
        Check(writableAgain && !savedReadOnly, "Saving over a read-only file was not rejected");
        Check(ReadJson(path)["entities"][0]["Script"]["scripts"][0]["props"]["score_value"] == 7, "Failed atomic replace changed the file");
        for (const auto& entry : std::filesystem::directory_iterator(fixture.directory))
        {
            Check(entry.path().filename().u8string().find(".tmp.") == std::string::npos, "Failed save leaked a temporary file");
        }

        auto updated = original;
        updated["entities"][0]["Script"]["scripts"][0]["props"]["score_value"] = 12;
        WriteText(path, updated.dump());
        fixture.WaitFor([&]()
            {
                const auto* data = library.GetPrefabJson("coin");
                return data != nullptr && data->at("entities")[0]["Script"]["scripts"][0]["props"]["score_value"] == 12;
            });
        const auto fresh = library.Instantiate(world, "coin");
        Check(world.Get<components::ScriptComponent>(fresh).scripts[0].props["score_value"] == 12, "Watcher did not update the next copy");
        Check(world.Get<components::ScriptComponent>(old).scripts[0].props["score_value"] == 7, "Watcher changed an existing instance");
        fixture.WaitFor([&]()
            {
                const auto* alias = library.GetPrefabJson("COIN");
                return alias != nullptr && alias->at("entities")[0]["Script"]["scripts"][0]["props"]["score_value"] == 12;
            });

        WriteText(path, "{half written");
        fixture.WaitFor([&]() { return library.GetPrefabJson("coin") == nullptr; });
        const auto count = world.GetEntities().size();
        Check(library.Instantiate(world, "coin") == 0 && world.GetEntities().size() == count, "Broken updated prefab left partial entities");
        WriteText(path, original.dump());
        fixture.WaitFor([&]() { return library.GetPrefabJson("coin") != nullptr; });
        Check(library.Instantiate(world, "coin") != 0, "Repaired prefab did not recover");
        std::filesystem::remove(path);
        fixture.WaitFor([&]() { return library.GetPrefabJson("coin") == nullptr; });
        WriteText(path, original.dump());
        Check(library.Instantiate(world, "coin") != 0, "Recreated prefab did not load");
        WriteText(fixture.directory / "added.prefab.json", original.dump());
        const auto names = library.ListPrefabs();
        Check(std::find(names.begin(), names.end(), "added") != names.end(), "New prefab is absent from the list");
    }

    void TestAssetContract()
    {
        const auto source = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
        myengine::scene::PrefabLibrary assets;
        Check(assets.Initialize(source / "assets/prefabs"), "Could not initialize the real prefab directory");
        ecs::World world;
        for (const auto* name : {"coin", "chaser", "firework"})
        {
            const auto root = assets.Instantiate(world, name);
            Check(root != 0 && world.Has<components::ScriptComponent>(root), "Required prefab has no ScriptComponent");
            for (const auto& entity : assets.GetPrefabJson(name)->at("entities"))
            {
                if (entity.contains("MeshRenderer"))
                {
                    const auto& mesh = entity.at("MeshRenderer");
                    Check(std::filesystem::is_regular_file(source / std::filesystem::u8path(mesh.at("meshPath").get<std::string>())), "Prefab mesh file is missing");
                    Check(std::filesystem::is_regular_file(source / std::filesystem::u8path(mesh.at("materialPath").get<std::string>())), "Prefab material file is missing");
                }
            }
        }
        const auto materials = source / "assets/materials";
        const auto gold = ReadJson(materials / "gold.material.json");
        Check(std::filesystem::is_regular_file(materials / std::filesystem::u8path(gold.at("shader").get<std::string>())), "Gold shader is missing");
        Check(std::filesystem::is_regular_file(materials / std::filesystem::u8path(gold.at("texture").get<std::string>())), "Gold texture is missing");
        Check(gold.at("tint").size() == 4, "Gold material tint is invalid");
    }
}

int main()
{
    myengine::jobs::Initialize(2);
    int result = 1;
    try
    {
        Fixture fixture;
        try
        {
            TestFirstFullScan(fixture);
            TestCopies(fixture);
            TestInvalidData(fixture);
            TestSaveAndWatcher(fixture);
            TestAssetContract();
            std::cout << "OK: prefab copies, id remapping, hierarchy, props, cache reload, atomic save and invalid data\n";
            result = 0;
        }
        catch (const std::exception& error)
        {
            fixture.keepArtifacts = true;
            std::cerr << "FAILED: " << error.what() << "\nLog: " << (fixture.directory / "test.log").u8string() << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    myengine::jobs::Shutdown();
    return result;
}
