// WallSense.h
//
// N4: OUR learned layer, not the connectome (docs/flybrain/flybrain.md, "Обученная муха (N4)"). The fly feels a wall with its legs (FlyPlayer: it pushes
// forward and does not move); this layer learns to see it coming. It keeps a memory of pictures, as the mushroom body keeps what was seen when something
// happened: the picture of the columns of the eyes cut into a grid of cells (the mean light of a cell, the mean of the half of the picture taken away, the
// length made 1 - so the place counts, not the brightness), each with a mark: a touch of a wall followed within `horizonTics` (a positive) or not. The chance
// of a touch now is the mark of the nearest pictures in the memory (k nearest by the cosine, weighted by it); when it is high, the left and the right half of
// the picture are compared on their own, so the fly knows which side the wall is on. One touch is enough to remember a place, and the fly goes round the same rooms: it
// learns in tens of seconds - but the places of a map, not "a wall". So there is a memory per place (the WAD and the map, SetPlace); a new map is
// learned again. When the chance is high the fly stops (the walk of the brain is taken away, never backwards) and turns to the half that sees less wall
// until the chance is low. With an empty memory the brain alone steers.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace doomfly
{
    struct WallSenseConfig
    {
        bool enabled = false;
        bool learning = true;                   // false: the memory stays as it is
        uint32_t cellsX = 8;                    // even: the halves
        uint32_t cellsY = 6;
        uint32_t horizonTics = 18;              // a touch within this many tics after a picture makes it a positive
        uint32_t everyTics = 2;                 // a picture of every this many tics goes into the memory
        uint32_t capacity = 3000;               // pictures per place (~4 minutes of walking a map); the oldest go
        uint32_t neighbours = 7;
        float avoidFrom = 0.5f;                 // at this chance the fly stops and turns away ...
        float avoidUntil = 0.25f;               // ... until the chance is below this one (to the same side all the time: no dithering in a corner)
        uint32_t avoidMaxTics = 45;             // ... or for this many tics at most (then the brain walks for 20 tics: the memory may be wrong)
        float turnDegrees = 6.0f;               // the turn per tic while it turns away
    };

    class WallSense
    {
    public:
        // `columnX`, `columnY`: where every column of the eyes looks (0..1 of the picture)
        void Build(const WallSenseConfig& config, const std::vector<float>& columnX, const std::vector<float>& columnY);
        bool Enabled() const { return config_.enabled; }
        // The place the fly is in now ("doom1 E1M1"): its own memory (a new place starts empty)
        void SetPlace(const std::string& place);
        const std::string& Place() const { return place_; }
        // One tic: the light of every column; `touching`: the legs push against a wall now (the mark of the pictures before); `walking`: the fly walks
        // forward (only such pictures are remembered: backing away from a wall or turning on the spot says nothing of what lies ahead)
        void Tick(const std::vector<float>& columnLuminance, bool touching, bool walking);

        float Chance() const { return chance_; }                  // of a touch soon (the whole picture)
        float Brake() const { return brake_; }                    // 0 or 1: the walk is taken away while the fly turns away from a wall
        float TurnDegrees() const { return turn_; }               // + right
        uint32_t Positives() const;                               // the pictures before a touch in the memory of this place
        uint32_t Remembered() const;                              // all the pictures of this place
        size_t Places() const { return memories_.size() - memories_.count(std::string()); }   // the named ones

        // The memories of all the places (the file of the trained fly: assets/flybrain/fly_trained.bin; the codes are kept as bytes there)
        bool Save(const std::string& path, std::string* error = nullptr) const;
        // merge: the places of the file are added (a place that is in both takes the one of the file); otherwise they replace all
        bool Load(const std::string& path, std::string* error = nullptr, bool merge = false);

    private:
        struct Memory
        {
            std::vector<float> codes;           // [picture][2 * half_]
            std::vector<uint8_t> marks;         // 1: a touch followed
            size_t next = 0;                    // the slot the next picture takes when the memory is full
            uint32_t positives = 0;
        };
        struct Pending
        {
            std::vector<float> code;            // left half, then right half
            bool positive = false;
        };
        void Remember(const Pending& picture);
        float Recall(const std::vector<float>& code, uint32_t half) const;

        WallSenseConfig config_;
        std::vector<uint32_t> cellOf_;
        uint32_t half_ = 0;                     // the length of the code of one half
        std::map<std::string, Memory> memories_;
        std::string place_;
        Memory* memory_ = nullptr;              // of place_
        std::deque<Pending> recent_;
        uint64_t tics_ = 0;
        float chance_ = 0.0f;
        float brake_ = 0.0f;
        float turn_ = 0.0f;
        float turnSign_ = 1.0f;
        uint32_t avoidTics_ = 0;                // > 0: turning away now
        uint32_t restTics_ = 0;                 // after a turn that ran out of time: the brain walks for a while
        bool touched_ = false;
    };
}
