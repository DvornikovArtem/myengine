// RlCars.cpp
// The catalog of the cars (RlCars.h): the json files of the garage, the numbers a car sets, the bars of the garage.

#include <rl/RlCars.h>

#include <rl/RlRng.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace rl
{
    namespace
    {
        using json = nlohmann::json;
        namespace fs = std::filesystem;

        uint64_t Fnv(uint64_t hash, const std::string& text)
        {
            for (const char c : text)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        float Interp(const float* xs, const float* ys, int count, float x)
        {
            if (x <= xs[0])
            {
                return ys[0];
            }
            for (int i = 1; i < count; ++i)
            {
                if (x <= xs[i])
                {
                    const float t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
                    return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
                }
            }
            return ys[count - 1];
        }

        // The numbers of the "car" group of a tuning by their names (the same names as in rl_tuning.json)
        class CarFields final : public RlTuningVisitor
        {
        public:
            struct Field
            {
                float* values = nullptr;
                int count = 0;
            };
            std::map<std::string, Field> fields;

            void Begin(const char* group) override { inCar_ = std::strcmp(group, "car") == 0; }
            void End() override { inCar_ = false; }
            void Section(const char*) override {}
            void Float(const char* name, float& value) override
            {
                if (inCar_)
                {
                    fields[name] = {&value, 1};
                }
            }
            void Int(const char*, int&) override {}
            void Floats(const char* name, float* values, int count) override
            {
                if (inCar_)
                {
                    fields[name] = {values, count};
                }
            }

        private:
            bool inCar_ = false;
        };

        // the numbers of one section of a file ("hitbox" without its "rocketSim" note, "physics") into `into`
        bool TakeSection(const json& root, const char* key, json& into, std::string& error)
        {
            const auto it = root.find(key);
            if (it == root.end())
            {
                return true;
            }
            if (!it->is_object())
            {
                error = std::string("'") + key + "' must be an object";
                return false;
            }
            for (const auto& entry : it->items())
            {
                if (entry.value().is_object())
                {
                    continue;  // the notes ("rocketSim": the size and the offset of the hitbox in uu)
                }
                into[entry.key()] = entry.value();
            }
            return true;
        }

        bool ParseSpec(const std::string& text, RlCarSpec& spec, std::string& error)
        {
            const json root = json::parse(text, nullptr, false);
            if (root.is_discarded() || !root.is_object())
            {
                error = "not json";
                return false;
            }
            if (!root.contains("id") || !root["id"].is_string() || root["id"].get<std::string>().empty())
            {
                error = "missing 'id'";
                return false;
            }
            spec = RlCarSpec{};
            spec.id = root["id"].get<std::string>();
            spec.hitboxClass = root.value("hitboxClass", std::string());
            spec.order = root.value("order", 0);

            json numbers = json::object();
            if (!TakeSection(root, "hitbox", numbers, error))
            {
                return false;
            }
            spec.variant.hasBody = root.contains("hitbox") && root["hitbox"].is_object() && !root["hitbox"].empty();
            if (!TakeSection(root, "physics", numbers, error))
            {
                return false;
            }

            RlTuning holder;
            CarFields fields;
            holder.Visit(fields);
            for (const auto& entry : numbers.items())
            {
                const auto field = fields.fields.find(entry.key());
                if (field == fields.fields.end())
                {
                    error = "'" + entry.key() + "' is not a number of the car (see the car group of rl_tuning.json)";
                    return false;
                }
                const json& value = entry.value();
                if (field->second.count == 1)
                {
                    if (!value.is_number())
                    {
                        error = "'" + entry.key() + "' must be a number";
                        return false;
                    }
                    field->second.values[0] = value.get<float>();
                }
                else
                {
                    if (!value.is_array() || static_cast<int>(value.size()) != field->second.count)
                    {
                        error = "'" + entry.key() + "' must be a list of " + std::to_string(field->second.count) + " numbers";
                        return false;
                    }
                    for (int i = 0; i < field->second.count; ++i)
                    {
                        if (!value[static_cast<size_t>(i)].is_number())
                        {
                            error = "'" + entry.key() + "' must be a list of numbers";
                            return false;
                        }
                        field->second.values[i] = value[static_cast<size_t>(i)].get<float>();
                    }
                }
                spec.variant.fields.push_back(entry.key());
            }
            spec.variant.values = holder.car;
            const RlCarTuning& c = spec.variant.values;
            const auto sets = [&](const char* name) { return std::find(spec.variant.fields.begin(), spec.variant.fields.end(), name) != spec.variant.fields.end(); };
            if ((sets("halfExtentX") && !(c.halfExtentX > 0.2f && c.halfExtentX < 1.0f)) || (sets("halfExtentY") && !(c.halfExtentY > 0.1f && c.halfExtentY < 0.6f)) ||
                (sets("halfExtentZ") && !(c.halfExtentZ > 0.3f && c.halfExtentZ < 1.2f)))
            {
                error = "the hitbox has no sensible size (half extents in metres)";
                return false;
            }
            if (sets("mass") && !(c.mass >= 50.0f && c.mass <= 500.0f))
            {
                error = "the mass must be between 50 and 500 kg";
                return false;
            }
            if (sets("maxSpeed") && !(c.maxSpeed >= 15.0f && c.maxSpeed <= 40.0f))
            {
                error = "the top speed must be between 15 and 40 m/s";
                return false;
            }
            return true;
        }
    }

    RlCarTuning RlApplyCarVariant(const RlCarTuning& base, const RlCarVariant& variant)
    {
        if (variant.fields.empty())
        {
            return base;
        }
        RlTuning result;
        result.car = base;
        RlTuning source;
        source.car = variant.values;
        CarFields into, from;
        result.Visit(into);
        source.Visit(from);
        for (const std::string& name : variant.fields)
        {
            const auto a = into.fields.find(name);
            const auto b = from.fields.find(name);
            if (a != into.fields.end() && b != from.fields.end())
            {
                for (int i = 0; i < a->second.count; ++i)
                {
                    a->second.values[i] = b->second.values[i];
                }
            }
        }
        RlCarTuning car = result.car;
        if (variant.hasBody)
        {
            // a car put on the floor stands as high as its hitbox is above the origin of the car (RocketSim puts the origin 17 uu up for all of them)
            car.spawnHeight = base.spawnHeight + ((-car.comOffsetY) - (-base.comOffsetY));
        }
        return car;
    }

    RlCarVariant RlBodyOnly(const RlCarVariant& variant)
    {
        static const char* const kBody[] = {"halfExtentX", "halfExtentY", "halfExtentZ", "comOffsetY", "comOffsetZ", "wheelMountXFront", "wheelMountXRear", "wheelMountYOffset",
                                            "wheelFrontZ", "wheelRearZ", "wheelRadiusFront", "wheelRadiusRear", "suspensionRestFront", "suspensionRestRear"};
        RlCarVariant result = variant;
        result.fields.clear();
        for (const std::string& name : variant.fields)
        {
            for (const char* body : kBody)
            {
                if (name == body)
                {
                    result.fields.push_back(name);
                    break;
                }
            }
        }
        return result;
    }

    RlCarStats RlCarStatsOf(const RlCarTuning& car)
    {
        RlCarStats s;
        s.topSpeed = car.maxSpeed;
        s.mass = car.mass;
        s.turning = Interp(car.steerSpeed, car.steerAngle, 6, 10.0f);
        s.length = car.halfExtentZ * 2.0f;
        s.width = car.halfExtentX * 2.0f;
        s.height = car.halfExtentY * 2.0f;
        // 0 -> 14 m/s on the throttle: the drive force falls with the speed, so the time is the integral of dv / a(v)
        constexpr float kTarget = 14.0f;
        constexpr int kSteps = 280;
        float time = 0.0f;
        for (int i = 0; i < kSteps; ++i)
        {
            const float v = (static_cast<float>(i) + 0.5f) * kTarget / static_cast<float>(kSteps);
            const float a = car.throttleAccel * Interp(car.driveSpeed, car.driveFactor, 3, v);
            time += (kTarget / static_cast<float>(kSteps)) / std::max(a, 0.01f);
        }
        s.accelerationTime = time;
        return s;
    }

    bool RlCarCatalog::LoadTexts(const std::vector<std::string>& texts, std::string* error)
    {
        std::vector<RlCarSpec> specs;
        for (size_t i = 0; i < texts.size(); ++i)
        {
            RlCarSpec spec;
            std::string why;
            if (!ParseSpec(texts[i], spec, why))
            {
                if (error != nullptr)
                {
                    *error = "car " + std::to_string(i) + ": " + why;
                }
                return false;
            }
            for (const RlCarSpec& other : specs)
            {
                if (other.id == spec.id)
                {
                    if (error != nullptr)
                    {
                        *error = "the car '" + spec.id + "' is twice in the catalog";
                    }
                    return false;
                }
            }
            specs.push_back(std::move(spec));
        }
        if (specs.empty() || static_cast<int>(specs.size()) > kMaxCarTypes)
        {
            if (error != nullptr)
            {
                *error = "the catalog needs 1 to " + std::to_string(kMaxCarTypes) + " cars";
            }
            return false;
        }
        std::stable_sort(specs.begin(), specs.end(), [](const RlCarSpec& a, const RlCarSpec& b) { return a.order != b.order ? a.order < b.order : a.id < b.id; });
        // the hash covers the ids in their order and every number that a car sets: other numbers make another hash
        uint64_t hash = 1469598103934665603ull;
        for (const RlCarSpec& spec : specs)
        {
            hash = Fnv(hash, spec.id);
            RlTuning source;
            source.car = spec.variant.values;
            CarFields from;
            source.Visit(from);
            std::vector<std::string> names = spec.variant.fields;
            std::sort(names.begin(), names.end());
            for (const std::string& name : names)
            {
                hash = Fnv(hash, "|" + name);
                const auto field = from.fields.find(name);
                for (int i = 0; field != from.fields.end() && i < field->second.count; ++i)
                {
                    char text[32];
                    std::snprintf(text, sizeof(text), ",%.6g", static_cast<double>(field->second.values[i]));
                    hash = Fnv(hash, text);
                }
            }
        }
        cars_ = std::move(specs);
        hash_ = hash;
        return true;
    }

    bool RlCarCatalog::LoadDirectory(const std::string& folder, std::string* error)
    {
        std::error_code ec;
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::u8path(folder), ec))
        {
            const std::string name = entry.path().filename().generic_u8string();
            const std::string physics = ".physics.json";
            const bool isPhysics = name.size() > physics.size() && name.compare(name.size() - physics.size(), physics.size(), physics) == 0;
            if (entry.is_regular_file() && entry.path().extension() == ".json" && !isPhysics)
            {
                files.push_back(entry.path());
            }
        }
        if (ec)
        {
            if (error != nullptr)
            {
                *error = "cannot read the folder of the cars '" + folder + "': " + ec.message();
            }
            return false;
        }
        std::sort(files.begin(), files.end());
        std::vector<std::string> texts;
        for (const fs::path& file : files)
        {
            std::ifstream in(file, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            texts.push_back(buffer.str());
        }
        std::string why;
        if (!LoadTexts(texts, &why))
        {
            if (error != nullptr)
            {
                *error = folder + ": " + why;
            }
            return false;
        }
        return true;
    }

    RlCarCatalog RlCarCatalog::Fallback()
    {
        RlCarCatalog catalog;
        RlCarSpec spec;
        spec.id = "pulse";
        spec.hitboxClass = "Octane";
        catalog.cars_.push_back(spec);
        catalog.hash_ = Fnv(1469598103934665603ull, "pulse");
        return catalog;
    }

    int RlCarCatalog::Find(const std::string& id) const
    {
        for (size_t i = 0; i < cars_.size(); ++i)
        {
            if (cars_[i].id == id)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    RlCarTuning RlCarCatalog::Tuning(const RlCarTuning& base, const int index) const
    {
        return RlApplyCarVariant(base, cars_[static_cast<size_t>(Valid(index))].variant);
    }

    void RlChooseCars(const RlCarCatalog& catalog, const uint64_t seed, const int slots, const uint32_t humanMask, const int* wanted, uint8_t* out)
    {
        RlRng rng(seed ^ 0xCA45ull);
        for (int slot = 0; slot < kMaxCars; ++slot)
        {
            out[slot] = 0;
            if (slot >= slots)
            {
                continue;
            }
            const int request = wanted != nullptr ? wanted[slot] : -1;
            const bool human = (humanMask & (1u << slot)) != 0;
            // every slot draws, so that the car of a bot does not depend on who of the others is a bot
            const int pick = rng.Range(catalog.Count());
            if (request >= 0 && request < catalog.Count())
            {
                out[slot] = static_cast<uint8_t>(request);
            }
            else if (human)
            {
                out[slot] = static_cast<uint8_t>(kDefaultCar);
            }
            else
            {
                out[slot] = static_cast<uint8_t>(pick);
            }
        }
    }
}
