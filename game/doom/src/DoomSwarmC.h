// DoomSwarm.h
//
// The swarm of flies in DOOM (docs/game/flydoom-fly.md, "Swarm"): the C side (DoomSwarm.c) that sees the headers of DOOM. A fly is a Lost Soul of DOOM
// (a flying monster with no gravity) that the host drives: DOOM does not run its AI (the state has no end), the host sets the angle, the momentum and the
// sprite frame before every tic, and DOOM does what it does with every mobj: the walls, the shots of the player (a fly is shootable), the death. The
// sprites and the buzz of the fly replace the lumps of the Lost Soul (DoomSwarmAssets.cpp makes them), so the swarm needs a start with -flyswarm.
// This header names no type of DOOM: the C++ side includes it (doom/DoomSwarm.h is the API of the host).

#ifndef MYENGINE_DOOM_SWARM_H
#define MYENGINE_DOOM_SWARM_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- before DOOM starts -------------------------------------------------------------------------------------------------- */

/* The lumps that DG_AfterIwad puts into the lump table: the sprites go in front of the end of the sprite section (S_END), the sounds at the end. The data is
 * copied. `names` are 8 characters, padded with zeros. Calling it again replaces the lumps. */
void dgs_SetAssets(unsigned int spriteCount, const char (*spriteNames)[8], const unsigned char* const* spriteData, const int* spriteSizes,
                   unsigned int soundCount, const char (*soundNames)[8], const unsigned char* const* soundData, const int* soundSizes);
/* 1 when the assets were set (the game started with the swarm lumps in) */
int dgs_AssetsSet(void);

/* ---- the flies ----------------------------------------------------------------------------------------------------------- */

#define DGS_MAX_FLIES 64

typedef struct dgs_fly
{
    int alive;          /* 0: dead (it is falling or lies there) */
    int health;
    double x, y, z;     /* map units; z is the bottom of the fly */
    double angle;       /* degrees, 0 = east, counter-clockwise */
    double momx, momy, momz;
    double floorZ, ceilingZ;
    int frame;          /* the frame of the sprite now (0 .. 10, A .. K) */
} dgs_fly;

/* The place of a spawn: the sector there. 1 and the floor and the ceiling when the place is inside the map and the room is high enough for a fly. */
int dgs_SpotOk(double x, double y, double* floorZ, double* ceilingZ);

/* A new fly. Handle >= 0, or -1 when there is no room (the place is in a wall, in another thing, outside the map, or the table is full) */
/* requireSight: -1 too when the console player cannot see the place (a wave comes from where the player can see it) */
int dgs_Spawn(double x, double y, double z, double angleDegrees, int health, int requireSight);
/* 1 and the state when the handle is a fly of this level */
int dgs_Get(int handle, dgs_fly* out);
/* What the host decided for the next tic: the angle (the sprite and the bite do not depend on it), the momentum in map units per tic, the frame of the
 * sprite (0 or 1 while it flies). Ignored for a dead fly. */
void dgs_Drive(int handle, double angleDegrees, double momx, double momy, double momz, int frame);
/* The console player hurts the fly (as a weapon does): a fly that lives goes on flying as the host drives it, one that dies falls. For the tests. */
void dgs_Hurt(int handle, int damage);
/* The fly just died (the host saw alive = 0): let it fall */
void dgs_Fall(int handle);
/* Takes the fly out of the level (a corpse after some seconds, the end of a wave) */
void dgs_Remove(int handle);
/* Forgets every fly (a new level: DOOM has freed the mobjs) */
void dgs_Reset(void);

/* Does the fly see the console player (a line of sight with no wall in it) */
int dgs_Sees(int handle);
/* The direction of the walk to the player round the walls, as DOOM's monsters find it (P_NewChaseDir): degrees. `renew` forces a new choice. May move the fly a step. */
double dgs_NavAngle(int handle, int renew);
/* The bite: damage to the console player from the fly */
void dgs_Bite(int handle, int damage);
/* The buzz of the fly, heard from where it is (DOOM puts it on the left or the right and makes it quieter with the distance) */
void dgs_Buzz(int handle);

/* leveltime of DOOM (the tics of the level); it falls back when a level starts again */
int dgs_LevelTime(void);

#ifdef __cplusplus
}
#endif

#endif
