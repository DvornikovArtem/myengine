// RlCars.h
//
// The cars of the garage (docs/game/rl-cars.md): assets/data/cars/<id>.json, one file per car, written by tools/rl_assets (cars.py, cardata.py). A car is
// a name and a class, the hitbox of Rocket League it follows, its meshes and paint, the profile previews of the carousel and the numbers it is driven
// with (`physics`, the values of Pulse until G4i-physics). This module only reads the files; the garage screen (RlGameFlow) and the scene
// (RlGameSystem) use it. The bars of the garage compare the cars with each other: the smallest value of a stat among the cars of the set is
// kSegmentsMin segments, the biggest kSegmentsMax (docs/game/rl-ui.md, "Garage").

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rl/RlCars.h>

namespace myengine::game
{
    constexpr int kSegmentsMin = 3;
    constexpr int kSegmentsMax = 10;

    // The five bars of the garage, in the order they are drawn
    enum class RlCarStat : int
    {
        TopSpeed = 0,     // maxSpeed: more is better
        Acceleration,     // throttleAccel (0 -> 14 m/s without the boost, inverted): more is better
        Weight,           // mass: a trade-off, heavier wins the hits and accelerates slower
        Turning,          // the angle of the wheels at 10 m/s: more is better
        Height,           // the height of the hitbox: a trade-off
        Count
    };

    const char* RlCarStatName(RlCarStat stat);       // "Top speed", "Acceleration", "Weight", "Turning", "Height"
    bool RlCarStatBetterIsMore(RlCarStat stat);      // the first, second and fourth: a gain is green, a loss red; the others are neutral

    struct RlCarDef
    {
        std::string id;                 // "pulse": the key in game_settings.json
        std::string name;               // "Pulse"
        std::string className;          // "Balanced"
        std::string hitboxClass;        // "Octane": the note "Hitbox ~ Octane"
        std::string description;
        int order = 0;

        float halfExtent[3] = {0.4335f, 0.1933f, 0.60254f};   // X right (the width), Y up (the height), Z forward (the length), metres

        // The wheels as the hitbox class of the car has them (index 0 front, 1 rear): the visual of the garage and of the podium puts them there; the car of the
        // match is driven by rl_tuning.json until G4i-physics and its wheels stay where the simulation has them
        float wheelMountX[2] = {0.259f, 0.295f};
        float wheelZ[2] = {0.37374f, -0.47626f};
        float wheelRadius[2] = {0.125f, 0.15f};
        float suspensionRest[2] = {0.26755f, 0.25055f};

        std::string bodyMesh;
        std::string lightsMesh;
        float bodyScale = 1.0f;         // the scale of the entity of the body (the meshes are normalised for the culling sphere)
        float lightsScale = 1.0f;
        std::string material[2];        // the paint of the blue and of the orange team
        float boostNozzle[3] = {0.0f, -0.06f, -0.74f};        // where the flame of the boost starts, from the centre of the hitbox
        std::string preview[2];         // PNG, the profile on a transparent background: blue, orange

        float stat[static_cast<int>(RlCarStat::Count)] = {};  // the raw values of the bars, see RlCarStat

        float Length() const { return halfExtent[2] * 2.0f; }
        float Width() const { return halfExtent[0] * 2.0f; }
        float HeightOfHitbox() const { return halfExtent[1] * 2.0f; }
    };

    class RlCarCatalog
    {
    public:
        // Reads every *.json of the folder (a file that does not read is named in `errors` and skipped); the cars are ordered by "order".
        bool LoadDirectory(const std::filesystem::path& directory, std::vector<std::string>* errors = nullptr);
        bool LoadFromText(const std::string& text, std::string* error = nullptr);   // one car (tests)

        const std::vector<RlCarDef>& Cars() const { return cars_; }
        bool Empty() const { return cars_.empty(); }
        const RlCarDef* Find(const std::string& id) const;
        int IndexOf(const std::string& id) const;                 // -1 when there is no such car
        // The car of the id, or the default one (Pulse, else the first one) when the id is not in the set; nullptr only for an empty catalog
        const RlCarDef* FindOrDefault(const std::string& id) const;
        const RlCarDef* Default() const;

        // The number of lit segments of a bar of the car, kSegmentsMin..kSegmentsMax by the place of its value between the smallest and the biggest of the
        // set; cars that do not differ in a stat all get the middle
        int Segments(const RlCarDef& car, RlCarStat stat) const;

    private:
        std::vector<RlCarDef> cars_;
    };

    // assets/data/cars of the open project
    std::filesystem::path CarsDirectory();
    // The catalog of the game, read once (the shipped files)
    const RlCarCatalog& Cars();
    // The same files as the simulation reads them (game/rl/RlCars.h: the hitbox, the wheels and the numbers of the car of every slot of a match, the id of a
    // car on the wire is its index here). The catalog above is what the garage shows, this one is what the match plays with; both are ordered by "order".
    const rl::RlCarCatalog& SimCars();
    int SimCarIndex(const std::string& id);   // the index of the car in SimCars(); the default car when there is no such id
}
