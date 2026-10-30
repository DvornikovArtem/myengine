// DoomSwarm.h
//
// The swarm of flies as a host sees it (docs/game/flydoom-fly.md, "Swarm"): a start of DOOM with -flyswarm puts the sprites and the buzz of a fly in the place
// of those of the Lost Soul (the shareware WAD has none), and then the host spawns flies, drives them tic by tic and reads what DOOM did with them. A fly is a
// mobj of DOOM: it is shot by the weapons of the player, bumps into walls and dies in the frames of the death of a Lost Soul.
// One thread: the thread that runs DoomGame. Everything here needs a DoomGame that was started with -flyswarm and is in a level.

#pragma once

#include <filesystem>
#include <string>

namespace doom::swarm
{
    constexpr const char* kStartArgument = "-flyswarm";
    constexpr int kMaxFlies = 64;

    // What DOOM knows about a fly
    struct Fly
    {
        bool alive = false;      // false: dead, it falls and lies there (Remove it after a while)
        int health = 0;
        double x = 0.0;          // map units; z is the bottom of the fly
        double y = 0.0;
        double z = 0.0;
        double angle = 0.0;      // degrees, 0 = east, counter-clockwise
        double momx = 0.0;
        double momy = 0.0;
        double momz = 0.0;
        double floorZ = 0.0;
        double ceilingZ = 0.0;
        int frame = 0;           // the frame of the sprite (0..10)
    };

    // ---- before DOOM starts (DoomGame::Create does it when the arguments have -flyswarm) ----
    // Draws the sprites (the palette is the one of the WAD) and the buzz, and hands them to DOOM. false and `error` when the WAD cannot be read.
    bool PrepareAssets(const std::filesystem::path& iwad, std::string& error);
    // The pictures of the flies as the tests and the tools want them: frame 0..10 (A..K), RGBA, kSpriteWidth x kSpriteHeight (alpha 0 is transparent)
    constexpr int kSpriteWidth = 56;
    constexpr int kSpriteHeight = 48;
    constexpr int kSpriteFrames = 11;
    void DrawSprite(int frame, unsigned char* rgba);

    // ---- in a level ----
    // The room at the place: its floor and ceiling, true when a fly fits in it
    bool SpotOk(double x, double y, double& floorZ, double& ceilingZ);
    // A new fly; the handle, or -1 when there is no room there
    // requireSight: -1 too when the console player has no line of sight to the place
    int Spawn(double x, double y, double z, double angleDegrees, int health, bool requireSight = false);
    bool Get(int handle, Fly& out);
    // The command for the next tic: the angle, the momentum in map units a tic, the frame of the sprite (0 or 1 in flight)
    void Drive(int handle, double angleDegrees, double momx, double momy, double momz, int frame);
    void Hurt(int handle, int damage);   // the console player hurts the fly as a weapon does (the tests; a fly that lives goes on as driven)
    void Fall(int handle);       // the fly died: let it fall to the floor
    void Remove(int handle);
    void Reset();                // a new level: forgets every fly (DOOM has freed them)
    bool Sees(int handle);       // a line of sight to the console player
    double NavAngle(int handle, bool renew);   // the way to the player round the walls (as the monsters of DOOM find it), degrees; may move the fly a step
    void Bite(int handle, int damage);         // damage to the console player
    void Buzz(int handle);       // the buzz of the fly (DOOM places it by the distance and the side)
    int LevelTime();             // the tics of the level; -1 outside a level; falls back when a level starts again
}
