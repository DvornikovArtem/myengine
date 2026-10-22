// RlCars.h
// The cars of the match (G4i, docs/game/rl-cars.md): what the simulation needs of a car, read from the files of the garage, assets/data/cars/<id>.json.
//
// A file has two sections that the simulation reads, both under the names of the "car" group of rl_tuning.json (RlCarTuning):
//   "hitbox"   the hitbox and the wheels of the class of Rocket League (Octane, Dominus, Plank, Breakout, Hybrid, Merc) as the CarConfig of RocketSim
//              (github.com/ZealanL/RocketSim, MIT) has them, in metres; the subsection "rocketSim" (size and offset in uu) is the source of the numbers
//   "physics"  the numbers the car is driven with: the mass and the inertia, the drive, the speed limits, the steering, the air control, the boost.
//              Pulse has the numbers of the tuning; the others differ from it by 10 to 20 percent (docs/game/rl-cars.md says how and why).
// The meshes, the names and the texts of the files belong to the garage (engine/src/game/RlCars.cpp); this module does not look at them.
//
// A car is a RlCarVariant: the numbers it sets and their values. The tuning of the match is the base, the variant is put on it, so a number that
// a file does not name stays what rl_tuning.json says. The order of the files ("order") is the id of a car on the wire (a car is a byte), and the
// hash of the catalog is what two sides compare before they play together. No engine code in here.

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rl
{
    constexpr int kMaxCarTypes = 16;
    constexpr int kDefaultCar = 0;  // the first car of the catalog (Pulse)

    struct RlCarVariant
    {
        std::vector<std::string> fields;  // the names of the numbers of the car group that the car sets ("mass", "steerAngle", "halfExtentX" ...)
        RlCarTuning values;               // their values; the other numbers of this struct are not used
        bool hasBody = false;             // the file has a hitbox: a car on the floor stands as high as the hitbox is above the origin of the car
    };

    // The numbers of the car as the tuning has them with the variant put on top
    RlCarTuning RlApplyCarVariant(const RlCarTuning& base, const RlCarVariant& variant);
    // The same car with the hitbox and the wheels only (the numbers of the class of Rocket League, none of the differences of ours): what the test
    // against RocketSim runs
    RlCarVariant RlBodyOnly(const RlCarVariant& variant);

    // What the garage shows as bars (the garage has its own copy of this arithmetic in the engine; the tests compare them)
    struct RlCarStats
    {
        float topSpeed = 0.0f;         // m/s, with the boost
        float accelerationTime = 0.0f; // s from 0 to 14 m/s on the throttle alone (the lower the better)
        float mass = 0.0f;             // kg
        float turning = 0.0f;          // rad: the angle of the front wheels at 10 m/s
        float length = 0.0f;           // m, the hitbox
        float width = 0.0f;
        float height = 0.0f;
    };
    RlCarStats RlCarStatsOf(const RlCarTuning& car);

    struct RlCarSpec
    {
        std::string id;               // "pulse"
        std::string hitboxClass;      // "Octane": the class of the hitbox in the RocketSim configs
        int order = 0;
        RlCarVariant variant;
    };

    class RlCarCatalog
    {
    public:
        // Reads every <id>.json of the folder (the *.physics.json are skipped); the cars are ordered by "order" (then by id). Returns false (and the reason)
        // when the folder or a file is wrong; the catalog then keeps what it had.
        bool LoadDirectory(const std::string& folder, std::string* error = nullptr);
        // A catalog from the text of the files (the tests)
        bool LoadTexts(const std::vector<std::string>& texts, std::string* error = nullptr);
        // One car, "pulse", with the numbers of the tuning: what a game without the files plays
        static RlCarCatalog Fallback();

        int Count() const { return static_cast<int>(cars_.size()); }
        const RlCarSpec& Spec(int index) const { return cars_[static_cast<size_t>(index)]; }
        int Find(const std::string& id) const;  // -1 when there is no such car
        // The index if it is one of the cars, else the default
        int Valid(int index) const { return index >= 0 && index < Count() ? index : kDefaultCar; }
        RlCarTuning Tuning(const RlCarTuning& base, int index) const;
        RlCarStats Stats(const RlCarTuning& base, int index) const { return RlCarStatsOf(Tuning(base, index)); }
        // The hash of all the numbers: two sides that play together need the same catalog
        uint64_t Hash() const { return hash_; }

    private:
        std::vector<RlCarSpec> cars_;
        uint64_t hash_ = 0;
    };

    // The cars of the slots of a match: `wanted[slot]` is the car of a human (-1 or out of range: the default car), a bot with -1 gets a random one
    // from `seed` (the same on every side). Slots beyond `slots` are 0.
    void RlChooseCars(const RlCarCatalog& catalog, uint64_t seed, int slots, uint32_t humanMask, const int* wanted, uint8_t* out);
}
