// The cars of the garage (G4i, docs/game/rl-cars.md), without a window: the data files assets/data/cars/*.json (six cars, the hitbox of each is the one of
// the car config of RocketSim, Pulse is the car of rl_tuning.json), the meshes and the paint they name (the files are there, the body fits its hitbox, the
// budget of triangles), the profile previews, the bars of the garage (3..10 segments by the set), the broken data (a file that does not read, an unknown
// key falls back to Pulse) and the key of the car in game_settings.json.

#include <myengine/game/RlCars.h>
#include <myengine/game/RlGameSettings.h>

#include <rl/RlTuning.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MYENGINE_SOURCE_DIR
#error MYENGINE_SOURCE_DIR is not defined
#endif

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine::game;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const float a, const float b, const float tolerance) { return std::fabs(a - b) <= tolerance; }

    fs::path Source() { return fs::u8path(MYENGINE_SOURCE_DIR); }

    // The six hitboxes of RocketSim (src/Sim/Car/CarConfig/CarConfig.cpp): size (length, width, height) and offset (forward, up) in uu, the wheels:
    // radius, relaxed suspension, connection point (forward, right) of the front and of the rear axle
    struct Expect
    {
        const char* id;
        const char* name;
        const char* klass;
        const char* hitbox;
        float size[3];
        float offset[2];
        float frontRadius, frontRest, frontConn[2];
        float rearRadius, rearRest, rearConn[2];
    };

    const Expect kExpected[6] = {
        {"pulse", "Pulse", "Balanced", "Octane", {120.507f, 86.6994f, 38.6591f}, {13.8757f, 20.755f}, 12.50f, 38.755f, {51.25f, 25.90f}, 15.00f, 37.055f, {-33.75f, 29.50f}},
        {"vector", "Vector", "Long", "Dominus", {130.427f, 85.7799f, 33.8f}, {9.0f, 15.75f}, 12.00f, 33.95f, {50.30f, 31.10f}, 13.50f, 33.85f, {-34.75f, 33.00f}},
        {"slate", "Slate", "Flat", "Plank", {131.32f, 87.1704f, 31.8944f}, {9.00857f, 12.0942f}, 12.50f, 31.9242f, {49.97f, 27.80f}, 17.00f, 27.9242f, {-35.43f, 20.28f}},
        {"dart", "Dart", "Low-long", "Breakout", {133.992f, 83.021f, 32.8f}, {12.5f, 11.75f}, 13.50f, 29.7f, {51.50f, 26.67f}, 15.00f, 29.666f, {-35.75f, 35.00f}},
        {"fusion", "Fusion", "Hybrid", "Hybrid", {129.519f, 84.6879f, 36.6591f}, {13.8757f, 20.755f}, 12.50f, 38.755f, {51.25f, 25.90f}, 15.00f, 37.055f, {-34.00f, 29.50f}},
        {"bulwark", "Bulwark", "Tall", "Merc", {123.22f, 79.2103f, 44.1591f}, {11.3757f, 21.505f}, 15.00f, 39.505f, {51.25f, 25.90f}, 15.00f, 39.105f, {-33.75f, 29.50f}},
    };

    RlCarCatalog LoadCatalog()
    {
        RlCarCatalog catalog;
        std::vector<std::string> errors;
        catalog.LoadDirectory(Source() / "assets" / "data" / "cars", &errors);
        for (const std::string& error : errors)
        {
            std::cerr << "car error: " << error << "\n";
        }
        Check(errors.empty(), "every file of assets/data/cars reads");
        return catalog;
    }

    // ---- the mesh: the vertices of an OBJ of the pack in the world metres (z un-mirrored, the unit scale applied)
    struct Bounds
    {
        float low[3] = {1e9f, 1e9f, 1e9f};
        float high[3] = {-1e9f, -1e9f, -1e9f};
        int triangles = 0;
        bool ok = false;
    };

    Bounds MeshBounds(const fs::path& path, const float scale)
    {
        Bounds b;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.rfind("v ", 0) == 0)
            {
                std::istringstream in(line.substr(2));
                float v[3];
                in >> v[0] >> v[1] >> v[2];
                v[0] *= scale;
                v[1] *= scale;
                v[2] *= -scale;
                for (int i = 0; i < 3; ++i)
                {
                    b.low[i] = std::min(b.low[i], v[i]);
                    b.high[i] = std::max(b.high[i], v[i]);
                }
                b.ok = true;
            }
            else if (line.rfind("f ", 0) == 0)
            {
                ++b.triangles;
            }
        }
        return b;
    }

    // a PNG: the signature and the IHDR (the size, the colour type 6 = RGBA)
    bool PngInfo(const fs::path& path, std::uint32_t& width, std::uint32_t& height, int& colorType)
    {
        std::ifstream file(path, std::ios::binary);
        unsigned char head[33] = {};
        file.read(reinterpret_cast<char*>(head), sizeof(head));
        static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (file.gcount() < 33 || !std::equal(signature, signature + 8, head))
        {
            return false;
        }
        width = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
        height = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
        colorType = head[25];
        return true;
    }

    void TestData()
    {
        const RlCarCatalog catalog = LoadCatalog();
        Check(catalog.Cars().size() == 6, "six cars");
        for (int i = 0; i < 6; ++i)
        {
            const Expect& e = kExpected[i];
            const RlCarDef& car = catalog.Cars()[static_cast<std::size_t>(i)];
            const std::string where = std::string(e.id) + ": ";
            Check(car.id == e.id && car.name == e.name && car.className == e.klass && car.hitboxClass == e.hitbox, where + "the id, the name, the class and the hitbox class of the table");
            Check(!car.description.empty() && car.description.size() <= 64, where + "a one line description");
            // the hitbox: the numbers of RocketSim in metres (our axes: X the width, Y the height, Z the length)
            Check(Near(car.halfExtent[0], e.size[1] * 0.005f, 1e-4f) && Near(car.halfExtent[1], e.size[2] * 0.005f, 1e-4f) && Near(car.halfExtent[2], e.size[0] * 0.005f, 1e-4f),
                  where + "the half extents are the hitbox of RocketSim");
            // the wheels: the connection points from the centre of the hitbox, the radius, the spring as the car section of rl_tuning.json has it (relaxed - 12 uu)
            Check(Near(car.wheelZ[0], (e.frontConn[0] - e.offset[0]) * 0.01f, 1e-4f) && Near(car.wheelZ[1], (e.rearConn[0] - e.offset[0]) * 0.01f, 1e-4f), where + "the wheels are where RocketSim has them (z)");
            Check(Near(car.wheelMountX[0], e.frontConn[1] * 0.01f, 1e-4f) && Near(car.wheelMountX[1], e.rearConn[1] * 0.01f, 1e-4f), where + "and in x");
            Check(Near(car.wheelRadius[0], e.frontRadius * 0.01f, 1e-4f) && Near(car.wheelRadius[1], e.rearRadius * 0.01f, 1e-4f), where + "the radii");
            Check(Near(car.suspensionRest[0], (e.frontRest - 12.0f) * 0.01f, 1e-4f) && Near(car.suspensionRest[1], (e.rearRest - 12.0f) * 0.01f, 1e-4f), where + "the springs");
            // the files the car names
            const fs::path root = Source();
            Check(fs::exists(root / car.bodyMesh) && fs::exists(root / car.lightsMesh), where + "the meshes are there");
            for (int team = 0; team < 2; ++team)
            {
                Check(fs::exists(root / car.material[team]), where + "the paint of the team is there");
                // assets/materials/rl2/<name>.material.json: the texture of the same name
                std::string texture = car.material[team];
                texture.replace(texture.find("/materials/"), 11, "/textures/");
                texture.replace(texture.rfind(".material.json"), 14, ".tga");
                Check(fs::exists(root / texture), where + "and its texture");
                Check(!car.preview[team].empty() && fs::exists(root / car.preview[team]), where + "the preview is there");
                std::uint32_t w = 0, h = 0;
                int colorType = 0;
                Check(PngInfo(root / car.preview[team], w, h, colorType) && w == 512 && h == 256 && colorType == 6, where + "the preview is a 512 x 256 RGBA PNG");
            }
            // the body fits its hitbox: as long as it, not much wider, the roof over it, the underside a little under it
            const Bounds body = MeshBounds(root / car.bodyMesh, car.bodyScale);
            Check(body.ok && car.bodyScale > 0.5f && car.bodyScale < 2.0f, where + "the body mesh reads");
            const float length = body.high[2] - body.low[2];
            Check(length >= car.Length() * 0.97f && length <= car.Length() + 0.45f, where + "the body is as long as the hitbox (" + std::to_string(length) + " m)");
            Check(body.high[0] - body.low[0] >= car.Width() * 0.95f && body.high[0] - body.low[0] <= car.Width() + 0.25f, where + "and as wide");
            Check(body.high[1] >= car.halfExtent[1] && body.high[1] <= car.halfExtent[1] + 0.17f, where + "the roof is over the hitbox");
            Check(body.low[1] <= -car.halfExtent[1] * 0.8f && body.low[1] >= -car.halfExtent[1] - 0.12f, where + "the underside is a little under it");
            // the budget: the body, the lights and four wheels under 4000 triangles
            const Bounds lights = MeshBounds(root / car.lightsMesh, car.lightsScale);
            const Bounds wheel = MeshBounds(root / "assets/models/rl2/car_wheel.obj", 0.189f);
            Check(body.triangles + lights.triangles + 4 * wheel.triangles <= 4000, where + "under 4000 triangles");
            // the flame starts behind the tail
            Check(car.boostNozzle[2] < -car.halfExtent[2] && car.boostNozzle[2] > -car.halfExtent[2] - 0.3f, where + "the nozzle is behind the tail");
        }
    }

    void TestPulseIsTheCarOfTheTuning()
    {
        // until G4i-physics the match is driven by rl_tuning.json: Pulse is its car
        const RlCarCatalog catalog = LoadCatalog();
        rl::RlTuning tuning;
        std::string error;
        Check(tuning.LoadFromFile((Source() / "assets" / "data" / "rl_tuning.json").generic_u8string(), &error), "rl_tuning.json reads: " + error);
        const RlCarDef* pulse = catalog.Find("pulse");
        Check(pulse != nullptr, "Pulse");
        const rl::RlCarTuning& c = tuning.car;
        Check(Near(pulse->halfExtent[0], c.halfExtentX, 1e-3f) && Near(pulse->halfExtent[1], c.halfExtentY, 1e-3f) && Near(pulse->halfExtent[2], c.halfExtentZ, 1e-3f), "the hitbox of Pulse is the one of the tuning");
        Check(Near(pulse->wheelZ[0], c.wheelFrontZ, 1e-3f) && Near(pulse->wheelZ[1], c.wheelRearZ, 1e-3f) && Near(pulse->wheelMountX[0], c.wheelMountXFront, 1e-3f) &&
                  Near(pulse->wheelMountX[1], c.wheelMountXRear, 1e-3f),
              "the wheels of Pulse are the ones of the tuning");
        Check(Near(pulse->wheelRadius[0], c.wheelRadiusFront, 1e-3f) && Near(pulse->wheelRadius[1], c.wheelRadiusRear, 1e-3f) && Near(pulse->suspensionRest[0], c.suspensionRestFront, 1e-3f) &&
                  Near(pulse->suspensionRest[1], c.suspensionRestRear, 1e-3f),
              "and the radii and the springs");
        Check(Near(pulse->stat[static_cast<int>(RlCarStat::TopSpeed)], c.maxSpeed, 1e-3f) && Near(pulse->stat[static_cast<int>(RlCarStat::Weight)], c.mass, 1e-3f) &&
                  Near(pulse->stat[static_cast<int>(RlCarStat::Acceleration)], c.throttleAccel, 1e-3f),
              "the physics of Pulse is the one of the tuning");
    }

    void TestBars()
    {
        const RlCarCatalog catalog = LoadCatalog();
        const RlCarDef* slate = catalog.Find("slate");
        const RlCarDef* bulwark = catalog.Find("bulwark");
        const RlCarDef* pulse = catalog.Find("pulse");
        Check(slate != nullptr && bulwark != nullptr && pulse != nullptr, "the cars");
        // the height differs from car to car: the lowest is 3 segments, the tallest 10, the rest in between by their height
        Check(catalog.Segments(*slate, RlCarStat::Height) == kSegmentsMin, "the flat Slate is the lowest car: 3 segments");
        Check(catalog.Segments(*bulwark, RlCarStat::Height) == kSegmentsMax, "the tall Bulwark is the highest: 10 segments");
        int previous = 0;
        for (const char* id : {"slate", "dart", "vector", "fusion", "pulse", "bulwark"})
        {
            const int segments = catalog.Segments(*catalog.Find(id), RlCarStat::Height);
            Check(segments >= previous, std::string("the bar of the height grows with the hitbox: ") + id);
            previous = segments;
        }
        // a stat in which the cars do not differ is drawn in the middle (all have the physics of Pulse until G4i-physics)
        bool same = true;
        for (const RlCarDef& car : catalog.Cars())
        {
            same = same && Near(car.stat[static_cast<int>(RlCarStat::Weight)], pulse->stat[static_cast<int>(RlCarStat::Weight)], 1e-6f);
        }
        if (same)
        {
            Check(catalog.Segments(*pulse, RlCarStat::Weight) == (kSegmentsMin + kSegmentsMax) / 2, "equal weights: the middle");
        }
        for (const RlCarDef& car : catalog.Cars())
        {
            for (int s = 0; s < static_cast<int>(RlCarStat::Count); ++s)
            {
                const int segments = catalog.Segments(car, static_cast<RlCarStat>(s));
                Check(segments >= kSegmentsMin && segments <= kSegmentsMax, "the segments are 3..10");
            }
        }
        // numbers of cars that differ in the physics: the bars follow them (a made-up pair)
        RlCarCatalog pair;
        const std::string base = R"({"id":"a","name":"A","class":"X","hitboxClass":"Y","hitbox":{"halfExtentX":0.4,"halfExtentY":0.2,"halfExtentZ":0.6},"mesh":{"body":"b.obj","materials":{"blue":"b","orange":"o"}},"physics":{"mass":MASS,"maxSpeed":SPEED,"throttleAccel":16,"steerSpeed":[0,5,10,15],"steerAngle":[0.5,0.3,0.2,0.1]}})";
        auto make = [&](const char* id, const char* mass, const char* speed)
        {
            std::string text = base;
            text.replace(text.find("\"a\""), 3, std::string("\"") + id + "\"");
            text.replace(text.find("MASS"), 4, mass);
            text.replace(text.find("SPEED"), 5, speed);
            std::string error;
            Check(pair.LoadFromText(text, &error), "a car reads: " + error);
        };
        make("light", "150", "25");
        make("heavy", "240", "21");
        make("mid", "195", "23");
        Check(pair.Segments(*pair.Find("heavy"), RlCarStat::Weight) == kSegmentsMax && pair.Segments(*pair.Find("light"), RlCarStat::Weight) == kSegmentsMin, "the heavy car is the top of the weight bar");
        Check(pair.Segments(*pair.Find("light"), RlCarStat::TopSpeed) == kSegmentsMax && pair.Segments(*pair.Find("heavy"), RlCarStat::TopSpeed) == kSegmentsMin, "the fast car is the top of the speed bar");
        const int mid = pair.Segments(*pair.Find("mid"), RlCarStat::Weight);
        Check(mid > kSegmentsMin && mid < kSegmentsMax, "the middle car is between");
        Check(RlCarStatBetterIsMore(RlCarStat::TopSpeed) && RlCarStatBetterIsMore(RlCarStat::Acceleration) && RlCarStatBetterIsMore(RlCarStat::Turning) &&
                  !RlCarStatBetterIsMore(RlCarStat::Weight) && !RlCarStatBetterIsMore(RlCarStat::Height),
              "speed, acceleration and turning are gains, weight and height are trade-offs");
    }

    void TestBrokenData()
    {
        RlCarCatalog catalog;
        std::string error;
        Check(!catalog.LoadFromText("not json", &error) && !error.empty(), "a text that is not json is refused");
        Check(!catalog.LoadFromText(R"({"id":"x"})", &error), "a car without its parts is refused");
        Check(!catalog.LoadFromText(R"({"id":"x","name":"X","hitbox":{"halfExtentX":0},"mesh":{"body":"a.obj","materials":{"blue":"a","orange":"b"}},"physics":{}})", &error), "a hitbox without a size is refused");
        Check(catalog.Empty() && catalog.Default() == nullptr && catalog.FindOrDefault("pulse") == nullptr, "an empty catalog has no car");

        const fs::path folder = fs::temp_directory_path() / "myengine_cars_test";
        fs::remove_all(folder);
        fs::create_directories(folder);
        {
            std::ifstream good(Source() / "assets" / "data" / "cars" / "vector.json", std::ios::binary);
            std::stringstream text;
            text << good.rdbuf();
            std::ofstream(folder / "vector.json", std::ios::binary) << text.str();
            std::ofstream(folder / "broken.json", std::ios::binary) << "{ not json";
            std::ofstream(folder / "notes.txt", std::ios::binary) << "ignored";
        }
        std::vector<std::string> errors;
        Check(catalog.LoadDirectory(folder, &errors), "the folder has a car");
        Check(catalog.Cars().size() == 1 && errors.size() == 1 && errors[0].find("broken.json") == 0, "a file that does not read is named and skipped");
        Check(catalog.FindOrDefault("pulse") != nullptr && catalog.FindOrDefault("pulse")->id == "vector", "without Pulse the default is the first car");
        Check(catalog.FindOrDefault("no-such-car") == catalog.Default(), "an unknown key is the default car");
        fs::remove_all(folder);

        // the real set: an unknown key is Pulse
        const RlCarCatalog all = LoadCatalog();
        Check(all.FindOrDefault("no-such-car") != nullptr && all.FindOrDefault("no-such-car")->id == "pulse", "an unknown key is Pulse");
        Check(all.IndexOf("slate") == 2 && all.IndexOf("nothing") == -1, "the place of a car in the set");
    }

    void TestSettingsKey()
    {
        RlGameSettings settings;
        Check(settings.car == "pulse", "the first start: Pulse");
        settings.car = "dart";
        RlGameSettings back;
        std::string error;
        Check(back.FromJson(settings.ToJson(), &error) && back.car == "dart", "the car comes back from the file: " + error);
        RlGameSettings odd;
        Check(odd.FromJson(R"({"car": "../../etc/passwd"})", &error), "a key with slashes reads");
        Check(odd.car.find('/') == std::string::npos && odd.car.find('.') == std::string::npos, "and is cleaned to letters, digits and - _");
        RlGameSettings empty;
        Check(empty.FromJson(R"({"car": ""})", &error) && empty.car == "pulse", "an empty key is Pulse");
        RlGameSettings kind;
        Check(kind.FromJson(R"({"car": 5})", &error) && kind.car == "pulse", "a key of the wrong kind is ignored");
    }
}

int main()
{
    try
    {
        TestData();
        TestPulseIsTheCarOfTheTuning();
        TestBars();
        TestBrokenData();
        TestSettingsKey();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "rl_cars tests passed\n";
    return 0;
}
