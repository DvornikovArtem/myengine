/* DoomSwarm.c
 *
 * The flies of the swarm: Lost Souls of DOOM that the host drives. See DoomSwarm.h.
 */

#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "info.h"
#include "m_fixed.h"
#include "p_local.h"
#include "p_mobj.h"
#include "r_main.h"
#include "r_state.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"

#include "DoomSwarmC.h"

#define DI_NODIR 8  /* p_enemy.c keeps its own enum */

/* p_enemy.c */
extern void P_NewChaseDir(mobj_t* actor);

/* the lumps the host asked for */
static char (*g_spriteNames)[8] = NULL;
static unsigned char** g_spriteData = NULL;
static int* g_spriteSizes = NULL;
static unsigned int g_spriteCount = 0;
static char (*g_soundNames)[8] = NULL;
static unsigned char** g_soundData = NULL;
static int* g_soundSizes = NULL;
static unsigned int g_soundCount = 0;

static void FreeAssets(void)
{
    unsigned int i;

    for (i = 0; i < g_spriteCount; ++i)
    {
        free(g_spriteData[i]);
    }
    for (i = 0; i < g_soundCount; ++i)
    {
        free(g_soundData[i]);
    }
    free(g_spriteNames);
    free(g_spriteData);
    free(g_spriteSizes);
    free(g_soundNames);
    free(g_soundData);
    free(g_soundSizes);
    g_spriteNames = NULL;
    g_spriteData = NULL;
    g_spriteSizes = NULL;
    g_soundNames = NULL;
    g_soundData = NULL;
    g_soundSizes = NULL;
    g_spriteCount = 0;
    g_soundCount = 0;
}

void dgs_SetAssets(unsigned int spriteCount, const char (*spriteNames)[8], const unsigned char* const* spriteData, const int* spriteSizes,
                   unsigned int soundCount, const char (*soundNames)[8], const unsigned char* const* soundData, const int* soundSizes)
{
    unsigned int i;

    FreeAssets();
    g_spriteCount = spriteCount;
    g_soundCount = soundCount;
    g_spriteNames = malloc(sizeof(char[8]) * (spriteCount + 1));
    g_spriteData = malloc(sizeof(unsigned char*) * (spriteCount + 1));
    g_spriteSizes = malloc(sizeof(int) * (spriteCount + 1));
    g_soundNames = malloc(sizeof(char[8]) * (soundCount + 1));
    g_soundData = malloc(sizeof(unsigned char*) * (soundCount + 1));
    g_soundSizes = malloc(sizeof(int) * (soundCount + 1));
    for (i = 0; i < spriteCount; ++i)
    {
        memcpy(g_spriteNames[i], spriteNames[i], 8);
        g_spriteSizes[i] = spriteSizes[i];
        g_spriteData[i] = malloc((size_t) spriteSizes[i]);
        memcpy(g_spriteData[i], spriteData[i], (size_t) spriteSizes[i]);
    }
    for (i = 0; i < soundCount; ++i)
    {
        memcpy(g_soundNames[i], soundNames[i], 8);
        g_soundSizes[i] = soundSizes[i];
        g_soundData[i] = malloc((size_t) soundSizes[i]);
        memcpy(g_soundData[i], soundData[i], (size_t) soundSizes[i]);
    }
}

int dgs_AssetsSet(void)
{
    return g_spriteCount > 0;
}

/* Called by D_DoomMain once the WAD files are in and before R_Init */
void DG_AfterIwad(void)
{
    if (g_spriteCount == 0)
    {
        return;
    }
    /* the sprites of the Lost Soul of the WAD (Freedoom has them, the shareware DOOM has none) go out of the way of the sprite scan, ours come in; only in
     * the sprite section: the wall patch SKULFACE of Freedoom (texture A-BROWN4) keeps its name */
    W_RenameLumps("S_START", "S_END", "SKUL", 'X');
    W_InsertMemoryLumps("S_END", g_spriteCount, (const char(*)[8]) g_spriteNames, (const unsigned char* const*) g_spriteData, g_spriteSizes);
    if (g_soundCount > 0)
    {
        W_InsertMemoryLumps(NULL, g_soundCount, (const char(*)[8]) g_soundNames, (const unsigned char* const*) g_soundData, g_soundSizes);
    }
    /* the swarm has no monster of its own to be afraid of mixing up with: a dead fly stays where it fell (the state does not end in S_NULL, which removes the mobj
     * and leaves the host with a pointer to nothing); the host removes the corpse */
    states[S_SKULL_DIE6].tics = -1;
    /* a fly that is hurt and lives (a fist, a pellet) must not wake up the AI of a Lost Soul: P_DamageMobj would put it in the pain state (that ends in A_Chase) or in
     * the see state, and A_SkullAttack plays a sound the shareware WAD does not have and charges at the player. No pain, no see state, no attack. */
    mobjinfo[MT_SKULL].painchance = 0;
    mobjinfo[MT_SKULL].seestate = S_NULL;
    mobjinfo[MT_SKULL].missilestate = S_NULL;
}

/* ---- the flies ------------------------------------------------------------------------------------------------------------ */

static mobj_t* g_flies[DGS_MAX_FLIES];
static int g_alive[DGS_MAX_FLIES];

static fixed_t ToFixed(double value)
{
    return (fixed_t) (value * 65536.0);
}

static double FromFixed(fixed_t value)
{
    return (double) value / 65536.0;
}

static angle_t ToAngle(double degrees)
{
    double turns = degrees / 360.0;

    turns -= (double) (long long) turns;
    if (turns < 0.0)
    {
        turns += 1.0;
    }
    return (angle_t) (unsigned long long) (turns * 4294967296.0);
}

static double FromAngle(angle_t angle)
{
    return (double) angle * 360.0 / 4294967296.0;
}

static mobj_t* Fly(int handle)
{
    if (handle < 0 || handle >= DGS_MAX_FLIES)
    {
        return NULL;
    }
    return g_flies[handle];
}

static mobj_t* Player(void)
{
    return players[consoleplayer].mo;
}

int dgs_SpotOk(double x, double y, double* floorZ, double* ceilingZ)
{
    subsector_t* subsector;
    sector_t* sector;
    double floorHeight;
    double ceilingHeight;

    if (gamestate != GS_LEVEL || numnodes == 0)
    {
        return 0;
    }
    subsector = R_PointInSubsector(ToFixed(x), ToFixed(y));
    if (subsector == NULL)
    {
        return 0;
    }
    sector = subsector->sector;
    floorHeight = FromFixed(sector->floorheight);
    ceilingHeight = FromFixed(sector->ceilingheight);
    if (ceilingHeight - floorHeight < 72.0)
    {
        return 0;
    }
    *floorZ = floorHeight;
    *ceilingZ = ceilingHeight;
    return 1;
}

int dgs_Spawn(double x, double y, double z, double angleDegrees, int health, int requireSight)
{
    int handle;
    mobj_t* mo;

    if (gamestate != GS_LEVEL)
    {
        return -1;
    }
    for (handle = 0; handle < DGS_MAX_FLIES; ++handle)
    {
        if (g_flies[handle] == NULL)
        {
            break;
        }
    }
    if (handle == DGS_MAX_FLIES)
    {
        return -1;
    }
    mo = P_SpawnMobj(ToFixed(x), ToFixed(y), ToFixed(z), MT_SKULL);
    if (mo == NULL)
    {
        return -1;
    }
    /* a Lost Soul that flies by the will of the host: no state that ends, no AI, no solid body (the flies do not stack on each other), no target */
    mo->tics = -1;
    mo->frame = 0 | FF_FULLBRIGHT;
    mo->health = health;
    mo->radius = 14 * FRACUNIT;
    mo->height = 40 * FRACUNIT;
    mo->flags = MF_SHOOTABLE | MF_FLOAT | MF_NOGRAVITY | MF_COUNTKILL;
    mo->target = NULL;
    mo->angle = ToAngle(angleDegrees);
    mo->movedir = DI_NODIR;
    mo->movecount = 0;
    mo->reactiontime = 0;
    if (mo->z < mo->floorz || mo->z + mo->height > mo->ceilingz || !P_CheckPosition(mo, mo->x, mo->y))
    {
        P_RemoveMobj(mo);
        return -1;
    }
    if (requireSight && Player() != NULL && !P_CheckSight(mo, Player()))
    {
        P_RemoveMobj(mo);
        return -1;
    }
    g_flies[handle] = mo;
    g_alive[handle] = 1;
    return handle;
}

int dgs_Get(int handle, dgs_fly* out)
{
    mobj_t* mo = Fly(handle);

    if (mo == NULL)
    {
        return 0;
    }
    out->alive = (mo->health > 0 && (mo->flags & MF_SHOOTABLE) != 0) ? 1 : 0;
    out->health = mo->health;
    out->x = FromFixed(mo->x);
    out->y = FromFixed(mo->y);
    out->z = FromFixed(mo->z);
    out->angle = FromAngle(mo->angle);
    out->momx = FromFixed(mo->momx);
    out->momy = FromFixed(mo->momy);
    out->momz = FromFixed(mo->momz);
    out->floorZ = FromFixed(mo->floorz);
    out->ceilingZ = FromFixed(mo->ceilingz);
    out->frame = mo->frame & FF_FRAMEMASK;
    return 1;
}

void dgs_Drive(int handle, double angleDegrees, double momx, double momy, double momz, int frame)
{
    mobj_t* mo = Fly(handle);

    if (mo == NULL || mo->health <= 0 || (mo->flags & MF_SHOOTABLE) == 0)
    {
        return;
    }
    mo->target = NULL;   /* P_DamageMobj makes the shooter the target of a monster; a fly goes where the host drives it */
    mo->angle = ToAngle(angleDegrees);
    mo->momx = ToFixed(momx);
    mo->momy = ToFixed(momy);
    mo->momz = ToFixed(momz);
    mo->frame = (frame & FF_FRAMEMASK) | FF_FULLBRIGHT;
}

void dgs_Hurt(int handle, int damage)
{
    mobj_t* mo = Fly(handle);
    mobj_t* player = Player();

    if (mo != NULL && player != NULL)
    {
        P_DamageMobj(mo, player, player, damage);
    }
}

void dgs_Fall(int handle)
{
    mobj_t* mo = Fly(handle);

    if (mo != NULL)
    {
        mo->flags &= ~MF_NOGRAVITY;
        mo->momx = 0;
        mo->momy = 0;
        mo->momz = 0;
    }
}

void dgs_Remove(int handle)
{
    mobj_t* mo = Fly(handle);

    if (mo != NULL)
    {
        P_RemoveMobj(mo);
        g_flies[handle] = NULL;
    }
}

void dgs_Reset(void)
{
    memset(g_flies, 0, sizeof(g_flies));
    memset(g_alive, 0, sizeof(g_alive));
}

int dgs_Sees(int handle)
{
    mobj_t* mo = Fly(handle);
    mobj_t* player = Player();

    if (mo == NULL || player == NULL)
    {
        return 0;
    }
    return P_CheckSight(mo, player) ? 1 : 0;
}

double dgs_NavAngle(int handle, int renew)
{
    mobj_t* mo = Fly(handle);
    mobj_t* player = Player();

    if (mo == NULL || player == NULL)
    {
        return 0.0;
    }
    mo->target = player;
    if (renew || mo->movecount <= 0 || mo->movedir == DI_NODIR)
    {
        P_NewChaseDir(mo);
    }
    mo->movecount--;
    mo->target = NULL;
    if (mo->movedir == DI_NODIR)
    {
        return FromAngle(mo->angle);
    }
    return (double) mo->movedir * 45.0;
}

void dgs_Bite(int handle, int damage)
{
    mobj_t* mo = Fly(handle);
    mobj_t* player = Player();

    if (mo == NULL || player == NULL || player->health <= 0)
    {
        return;
    }
    P_DamageMobj(player, mo, mo, damage);
}

void dgs_Buzz(int handle)
{
    mobj_t* mo = Fly(handle);

    if (mo != NULL)
    {
        S_StartSound(mo, sfx_flybz);
    }
}

int dgs_LevelTime(void)
{
    return gamestate == GS_LEVEL ? leveltime : -1;
}
