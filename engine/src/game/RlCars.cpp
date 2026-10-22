#include <myengine/game/RlCars.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <myengine/core/ServiceLocator.h>

namespace myengine::game
{
    namespace
    {
        using json = nlohmann::json;

        float Number(const json& object, const char* key, const float fallback)
        {
            const auto it = object.find(key);
            return (it != object.end() && it->is_number()) ? it->get<float>() : fallback;
        }

        std::string Text(const json& object, const char* key)
        {
            const auto it = object.find(key);
            return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
        }

        // steerAngle[i] at steerSpeed[i] = 10 m/s: the angle of the wheels at a medium speed
        float SteerAt10(const json& physics)
        {
            const auto speeds = physics.find("steerSpeed");
            const auto angles = physics.find("steerAngle");
            if (speeds == physics.end() || angles == physics.end() || !speeds->is_array() || !angles->is_array() || speeds->size() != angles->size() || speeds->empty())
            {
                return 0.18f;
            }
            const std::size_t n = speeds->size();
            for (std::size_t i = 1; i < n; ++i)
            {
                const float s0 = (*speeds)[i - 1].get<float>();
                const float s1 = (*speeds)[i].get<float>();
                if (10.0f <= s1)
                {
                    const float t = s1 > s0 ? (10.0f - s0) / (s1 - s0) : 0.0f;
                    return (*angles)[i - 1].get<float>() + ((*angles)[i].get<float>() - (*angles)[i - 1].get<float>()) * t;
                }
            }
            return (*angles)[n - 1].get<float>();
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return false;
        }
    }

    const char* RlCarStatName(const RlCarStat stat)
    {
        switch (stat)
        {
        case RlCarStat::TopSpeed: return "Top speed";
        case RlCarStat::Acceleration: return "Acceleration";
        case RlCarStat::Weight: return "Weight";
        case RlCarStat::Turning: return "Turning";
        case RlCarStat::Height: return "Height";
        case RlCarStat::Count: break;
        }
        return "";
    }

    bool RlCarStatBetterIsMore(const RlCarStat stat)
    {
        return stat == RlCarStat::TopSpeed || stat == RlCarStat::Acceleration || stat == RlCarStat::Turning;
    }

    bool RlCarCatalog::LoadFromText(const std::string& text, std::string* error)
    {
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return Fail(error, "the car is not a json object");
        }
        RlCarDef car;
        car.id = Text(root, "id");
        car.name = Text(root, "name");
        car.className = Text(root, "class");
        car.hitboxClass = Text(root, "hitboxClass");
        car.description = Text(root, "description");
        car.order = static_cast<int>(Number(root, "order", 0.0f));
        const auto hitbox = root.find("hitbox");
        const auto mesh = root.find("mesh");
        const auto preview = root.find("preview");
        const auto physics = root.find("physics");
        if (car.id.empty() || car.name.empty() || hitbox == root.end() || !hitbox->is_object() || mesh == root.end() || !mesh->is_object() || physics == root.end() ||
            !physics->is_object())
        {
            return Fail(error, "the car needs an id, a name, a hitbox, a mesh and physics");
        }
        car.halfExtent[0] = Number(*hitbox, "halfExtentX", car.halfExtent[0]);
        car.halfExtent[1] = Number(*hitbox, "halfExtentY", car.halfExtent[1]);
        car.halfExtent[2] = Number(*hitbox, "halfExtentZ", car.halfExtent[2]);
        if (!(car.halfExtent[0] > 0.05f) || !(car.halfExtent[1] > 0.05f) || !(car.halfExtent[2] > 0.1f))
        {
            return Fail(error, car.id + ": the hitbox has no size");
        }
        car.wheelMountX[0] = Number(*hitbox, "wheelMountXFront", car.wheelMountX[0]);
        car.wheelMountX[1] = Number(*hitbox, "wheelMountXRear", car.wheelMountX[1]);
        car.wheelZ[0] = Number(*hitbox, "wheelFrontZ", car.wheelZ[0]);
        car.wheelZ[1] = Number(*hitbox, "wheelRearZ", car.wheelZ[1]);
        car.wheelRadius[0] = Number(*hitbox, "wheelRadiusFront", car.wheelRadius[0]);
        car.wheelRadius[1] = Number(*hitbox, "wheelRadiusRear", car.wheelRadius[1]);
        car.suspensionRest[0] = Number(*hitbox, "suspensionRestFront", car.suspensionRest[0]);
        car.suspensionRest[1] = Number(*hitbox, "suspensionRestRear", car.suspensionRest[1]);
        car.bodyMesh = Text(*mesh, "body");
        car.lightsMesh = Text(*mesh, "lights");
        car.bodyScale = Number(*mesh, "bodyScale", 1.0f);
        car.lightsScale = Number(*mesh, "lightsScale", 1.0f);
        const auto materials = mesh->find("materials");
        if (materials != mesh->end() && materials->is_object())
        {
            car.material[0] = Text(*materials, "blue");
            car.material[1] = Text(*materials, "orange");
        }
        const auto nozzle = mesh->find("boostNozzle");
        if (nozzle != mesh->end() && nozzle->is_array() && nozzle->size() == 3)
        {
            for (int i = 0; i < 3; ++i)
            {
                car.boostNozzle[i] = (*nozzle)[static_cast<std::size_t>(i)].get<float>();
            }
        }
        if (car.bodyMesh.empty() || car.material[0].empty() || car.material[1].empty())
        {
            return Fail(error, car.id + ": the body mesh and the paint of both teams are needed");
        }
        if (preview != root.end() && preview->is_object())
        {
            car.preview[0] = Text(*preview, "blue");
            car.preview[1] = Text(*preview, "orange");
        }
        car.stat[static_cast<int>(RlCarStat::TopSpeed)] = Number(*physics, "maxSpeed", 23.0f);
        car.stat[static_cast<int>(RlCarStat::Acceleration)] = Number(*physics, "throttleAccel", 16.0f);
        car.stat[static_cast<int>(RlCarStat::Weight)] = Number(*physics, "mass", 180.0f);
        car.stat[static_cast<int>(RlCarStat::Turning)] = SteerAt10(*physics);
        car.stat[static_cast<int>(RlCarStat::Height)] = car.HeightOfHitbox();
        const auto existing = std::find_if(cars_.begin(), cars_.end(), [&](const RlCarDef& other) { return other.id == car.id; });
        if (existing != cars_.end())
        {
            *existing = car;
        }
        else
        {
            cars_.push_back(car);
        }
        std::stable_sort(cars_.begin(), cars_.end(), [](const RlCarDef& a, const RlCarDef& b) { return a.order != b.order ? a.order < b.order : a.id < b.id; });
        return true;
    }

    bool RlCarCatalog::LoadDirectory(const std::filesystem::path& directory, std::vector<std::string>* errors)
    {
        cars_.clear();
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file() && it->path().extension() == ".json")
            {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files)
        {
            std::ifstream stream(file, std::ios::binary);
            std::stringstream buffer;
            buffer << stream.rdbuf();
            std::string message;
            if (!stream || !LoadFromText(buffer.str(), &message))
            {
                if (errors != nullptr)
                {
                    errors->push_back(file.filename().generic_u8string() + ": " + (message.empty() ? std::string("cannot be read") : message));
                }
            }
        }
        return !cars_.empty();
    }

    const RlCarDef* RlCarCatalog::Find(const std::string& id) const
    {
        const auto it = std::find_if(cars_.begin(), cars_.end(), [&](const RlCarDef& car) { return car.id == id; });
        return it == cars_.end() ? nullptr : &*it;
    }

    int RlCarCatalog::IndexOf(const std::string& id) const
    {
        const auto it = std::find_if(cars_.begin(), cars_.end(), [&](const RlCarDef& car) { return car.id == id; });
        return it == cars_.end() ? -1 : static_cast<int>(it - cars_.begin());
    }

    const RlCarDef* RlCarCatalog::Default() const
    {
        if (const RlCarDef* pulse = Find("pulse"))
        {
            return pulse;
        }
        return cars_.empty() ? nullptr : &cars_.front();
    }

    const RlCarDef* RlCarCatalog::FindOrDefault(const std::string& id) const
    {
        const RlCarDef* car = Find(id);
        return car != nullptr ? car : Default();
    }

    int RlCarCatalog::Segments(const RlCarDef& car, const RlCarStat stat) const
    {
        const int index = static_cast<int>(stat);
        float low = car.stat[index];
        float high = car.stat[index];
        for (const RlCarDef& other : cars_)
        {
            low = std::min(low, other.stat[index]);
            high = std::max(high, other.stat[index]);
        }
        if (high - low < 1e-6f)
        {
            return (kSegmentsMin + kSegmentsMax) / 2;
        }
        const float t = (car.stat[index] - low) / (high - low);
        return kSegmentsMin + static_cast<int>(std::lround(t * static_cast<float>(kSegmentsMax - kSegmentsMin)));
    }

    std::filesystem::path CarsDirectory()
    {
        return core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/cars");
    }

    const RlCarCatalog& Cars()
    {
        static RlCarCatalog catalog;
        static bool loaded = false;
        if (!loaded || catalog.Empty())
        {
            catalog.LoadDirectory(CarsDirectory());
            loaded = !catalog.Empty();
        }
        return catalog;
    }
}
