/* DoomBridge.c
 *
 * The C side of the port: leaving DOOM without exit(), the state of the game for the host, the mouse and the hook of the tic commands.
 * See DoomBridge.h; docs/game/doom.md describes how the pieces fit.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "doomgeneric.h"

#include "d_event.h"
#include "d_loop.h"
#include "d_main.h"
#include "d_player.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_mobj.h"
#include "i_video.h"
#include "r_main.h"
#include "tables.h"
#include "w_wad.h"
#include "z_zone.h"

#include "DoomBridge.h"

/* P_AimLineAttack keeps these in p_map.c/p_sight.c. The bridge saves and
   restores them so a visibility probe cannot alter a later weapon trace. */
extern mobj_t* shootthing;
extern fixed_t shootz;
extern int la_damage;
extern fixed_t attackrange;
extern fixed_t aimslope;
extern fixed_t topslope;
extern fixed_t bottomslope;

void (*DG_TicCmdHook)(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons) = NULL;

static jmp_buf g_leave;
static int g_armed = 0;
static int g_exitCode = 0;
static char g_exitMessage[512] = "";

/* DOOM calls this instead of exit() (I_Quit, I_Error). It does not return into DOOM: the stack goes back to dgb_Create / dgb_Tick */
void DG_Exit(int code, const char* message)
{
    g_exitCode = code;
    g_exitMessage[0] = '\0';
    if (message != NULL)
    {
        strncpy(g_exitMessage, message, sizeof(g_exitMessage) - 1);
        g_exitMessage[sizeof(g_exitMessage) - 1] = '\0';
    }
    if (g_armed)
    {
        g_armed = 0;
        longjmp(g_leave, 1);
    }
    exit(code);
}

int dgb_Create(int argc, char** argv)
{
    if (setjmp(g_leave) != 0)
    {
        return 1;
    }
    g_armed = 1;
    doomgeneric_Create(argc, argv);
    g_armed = 0;
    return 0;
}

int dgb_Tick(void)
{
    if (setjmp(g_leave) != 0)
    {
        return 1;
    }
    g_armed = 1;
    doomgeneric_Tick();
    g_armed = 0;
    return 0;
}

int dgb_ExitCode(void)
{
    return g_exitCode;
}

const char* dgb_ExitMessage(void)
{
    return g_exitMessage;
}

void dgb_PostMouse(int buttons, int dx, int dy)
{
    event_t event;
    memset(&event, 0, sizeof(event));
    event.type = ev_mouse;
    event.data1 = buttons;
    event.data2 = dx;
    event.data3 = dy;
    D_PostEvent(&event);
}

void dgb_GetState(dgb_state* out)
{
    out->gameTic = gametic;
    out->gameState = (int)gamestate;
    out->episode = gameepisode;
    out->map = gamemap;
    out->menuActive = menuactive ? 1 : 0;
    out->paused = paused ? 1 : 0;
    out->demoPlayback = demoplayback ? 1 : 0;
    out->consolePlayer = consoleplayer;
    out->skill = (int)gameskill;
    out->deathmatch = deathmatch;
    out->netGame = netgame ? 1 : 0;
}

int dgb_doorsOpened[MAXPLAYERS];   /* myengine N5 (measurement only): p_doors.c counts the doors a player opens with "use" */

int dgb_GetPlayer(int player, dgb_player* out)
{
    const player_t* p;
    memset(out, 0, sizeof(*out));
    if (player < 0 || player >= MAXPLAYERS || !playeringame[player])
    {
        return 0;
    }
    p = &players[player];
    out->inGame = 1;
    out->health = p->mo != NULL ? p->mo->health : p->health;
    out->armor = p->armorpoints;
    if (p->mo != NULL)
    {
        out->x = p->mo->x;
        out->y = p->mo->y;
        out->z = p->mo->z;
        out->angle = p->mo->angle;
    }
    if (p->attacker != NULL && p->attacker != p->mo)
    {
        out->hasAttacker = 1;
        out->attackerX = p->attacker->x;
        out->attackerY = p->attacker->y;
    }
    out->readyWeapon = (int)p->readyweapon;
    {
        extern int dgb_attackShots[MAXPLAYERS];
        extern int dgb_attackHits[MAXPLAYERS];
        extern int dgb_attackDamage[MAXPLAYERS];
        out->shots = dgb_attackShots[player];
        out->hits = dgb_attackHits[player];
        out->damageDealt = dgb_attackDamage[player];
    }
    out->doorsOpened = dgb_doorsOpened[player];
    out->sector = -1;
    if (p->mo != NULL && p->mo->subsector != NULL && p->mo->subsector->sector != NULL)
    {
        const sector_t* sector = p->mo->subsector->sector;
        int line;
        out->sector = (int)(sector - sectors);
        for (line = 0; line < sector->linecount; ++line)
        {
            switch (sector->lines[line]->special)
            {
            case 1: case 26: case 27: case 28: case 31: case 32: case 33: case 34: case 117: case 118:
                out->doorSector = 1;
                break;
            default:
                break;
            }
        }
    }
    out->kills = p->killcount;
    out->items = p->itemcount;
    out->secrets = p->secretcount;
    {
        int other;
        for (other = 0; other < MAXPLAYERS; ++other)
        {
            if (other == player)
            {
                out->frags -= players[player].frags[other];
            }
            else
            {
                out->frags += players[player].frags[other];
                out->deaths += players[other].frags[player];
            }
        }
    }
    return 1;
}

static int dgb_IsVisibleHostile(const mobj_t* source, const mobj_t* target)
{
    if (target == NULL || target == source || target->health <= 0 || (target->flags & MF_SHOOTABLE) == 0)
    {
        return 0;
    }
    /* MF_COUNTKILL covers normal monsters. Lost Souls are shootable hostile
       MT_SKULLs but this vendored DOOM does not mark them MF_COUNTKILL. The
       other player is hostile in deathmatch only. Barrels and corpses are
       deliberately not accepted by this authored combat sensor. */
    return (target->flags & MF_COUNTKILL) != 0 || target->type == MT_SKULL ||
           (deathmatch && target->type == MT_PLAYER);
}

int dgb_GetVisibleTarget(int player, int angleDelta, dgb_visible_target* out)
{
    static const int32_t offsets[] = {0, 1 << 26, -(1 << 26), 1 << 27, -(1 << 27), 3 << 26, -(3 << 26), 1 << 28, -(1 << 28)};
    mobj_t* source;
    mobj_t* savedLineTarget;
    mobj_t* savedShootThing;
    fixed_t savedShootz;
    int savedDamage;
    fixed_t savedAttackRange;
    fixed_t savedAimSlope;
    fixed_t savedTopSlope;
    fixed_t savedBottomSlope;
    int i;

    if (out == NULL)
    {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (gamestate != GS_LEVEL || player < 0 || player >= MAXPLAYERS || !playeringame[player] ||
        players[player].playerstate != PST_LIVE || players[player].mo == NULL || players[player].mo->health <= 0)
    {
        return 0;
    }
    source = players[player].mo;

    /* P_AimLineAttack uses shared DOOM globals also used by the real weapon
       code. Probe on the game thread, then restore every one before returning. */
    savedLineTarget = linetarget;
    savedShootThing = shootthing;
    savedShootz = shootz;
    savedDamage = la_damage;
    savedAttackRange = attackrange;
    savedAimSlope = aimslope;
    savedTopSlope = topslope;
    savedBottomSlope = bottomslope;
    for (i = 0; i < (int)(sizeof(offsets) / sizeof(offsets[0])); ++i)
    {
        mobj_t* candidate;
        P_SetPathTraverseSensor(1);
        P_AimLineAttack(source, source->angle + (angle_t)(angleDelta + offsets[i]), MISSILERANGE);
        P_SetPathTraverseSensor(0);
        candidate = linetarget;
        if (dgb_IsVisibleHostile(source, candidate))
        {
            out->found = 1;
            out->angleDelta = offsets[i];
            break;
        }
    }
    linetarget = savedLineTarget;
    shootthing = savedShootThing;
    shootz = savedShootz;
    la_damage = savedDamage;
    attackrange = savedAttackRange;
    aimslope = savedAimSlope;
    topslope = savedTopSlope;
    bottomslope = savedBottomSlope;
    return out->found;
}

/* N5: the whiskers of the fly, an authored local sensor outside the connectome (like the antennae): short rays from the player that stop at the
   first line the player cannot pass (one-sided, blocking, a step higher than 24 units, an opening lower than the player). Only lines are seen,
   not things; the world is not changed (the line opening globals are restored, the intercept list is bounded like the target probe). */
static mobj_t* g_whiskerMo;
static fixed_t g_whiskerFrac;

static boolean dgb_WhiskerTraverse(intercept_t* in)
{
    line_t* li;
    if (!in->isaline)
    {
        return true;
    }
    li = in->d.line;
    if ((li->flags & ML_TWOSIDED) == 0 || (li->flags & ML_BLOCKING) != 0)
    {
        g_whiskerFrac = in->frac;
        return false;
    }
    P_LineOpening(li);
    if (openrange < g_whiskerMo->height || opentop - g_whiskerMo->z < g_whiskerMo->height || openbottom - g_whiskerMo->z > 24 * FRACUNIT)
    {
        g_whiskerFrac = in->frac;
        return false;
    }
    return true;
}

int dgb_GetWhiskers(int player, const int* angleDeltas, int count, int range, int* outDistance)
{
    mobj_t* source;
    fixed_t savedTop, savedBottom, savedRange, savedLowFloor;
    int i;
    if (outDistance == NULL || angleDeltas == NULL || count <= 0)
    {
        return 0;
    }
    for (i = 0; i < count; ++i)
    {
        outDistance[i] = range;
    }
    if (gamestate != GS_LEVEL || player < 0 || player >= MAXPLAYERS || !playeringame[player] ||
        players[player].playerstate != PST_LIVE || players[player].mo == NULL || players[player].mo->health <= 0)
    {
        return 0;
    }
    source = players[player].mo;
    savedTop = opentop;
    savedBottom = openbottom;
    savedRange = openrange;
    savedLowFloor = lowfloor;
    g_whiskerMo = source;
    for (i = 0; i < count; ++i)
    {
        const angle_t angle = source->angle + (angle_t)angleDeltas[i];
        const fixed_t length = range * FRACUNIT;
        const fixed_t x2 = source->x + FixedMul(length, finecosine[angle >> ANGLETOFINESHIFT]);
        const fixed_t y2 = source->y + FixedMul(length, finesine[angle >> ANGLETOFINESHIFT]);
        g_whiskerFrac = FRACUNIT;
        P_SetPathTraverseSensor(1);
        P_PathTraverse(source->x, source->y, x2, y2, PT_ADDLINES, dgb_WhiskerTraverse);
        P_SetPathTraverseSensor(0);
        outDistance[i] = FixedMul(range * FRACUNIT, g_whiskerFrac) / FRACUNIT;
    }
    g_whiskerMo = NULL;
    opentop = savedTop;
    openbottom = savedBottom;
    openrange = savedRange;
    lowfloor = savedLowFloor;
    return 1;
}

void dgb_RestartLevel(void)
{
    int i;
    if (gamestate != GS_LEVEL && gamestate != GS_INTERMISSION && gamestate != GS_FINALE)
    {
        return;
    }
    for (i = 0; i < MAXPLAYERS; ++i)
    {
        if (playeringame[i])
        {
            players[i].playerstate = PST_REBORN;
        }
    }
    gameaction = ga_loadlevel;
}

int dgb_RenderPlayerView(int player, unsigned int* out)
{
    static const byte* palette = NULL;
    int saved;
    int x;
    int y;
    int rows;

    if (player < 0 || player >= MAXPLAYERS || !playeringame[player] || players[player].mo == NULL || gamestate != GS_LEVEL || I_VideoBuffer == NULL)
    {
        return 0;
    }
    if (palette == NULL)
    {
        palette = (const byte*)W_CacheLumpName("PLAYPAL", PU_STATIC);
    }

    /* the picture of the console player is in I_VideoBuffer: the next D_Display draws it again, the frame the host got (DG_ScreenBuffer) is a copy */
    saved = displayplayer;
    displayplayer = player;
    R_RenderPlayerView(&players[player]);
    displayplayer = saved;

    rows = SCREENHEIGHT - 32;   /* above the status bar */
    memset(out, 0, sizeof(unsigned int) * SCREENWIDTH * SCREENHEIGHT);
    for (y = 0; y < rows; ++y)
    {
        for (x = 0; x < SCREENWIDTH; ++x)
        {
            const int index = I_VideoBuffer[y * SCREENWIDTH + x];
            const unsigned int r = gammatable[usegamma][palette[index * 3 + 0]];
            const unsigned int g = gammatable[usegamma][palette[index * 3 + 1]];
            const unsigned int b = gammatable[usegamma][palette[index * 3 + 2]];
            out[y * SCREENWIDTH + x] = (r << 16) | (g << 8) | b;
        }
    }
    return 1;
}

int dgb_GetTicCmd(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons)
{
    if (player < 0 || player >= MAXPLAYERS || !playeringame[player])
    {
        return 0;
    }
    *forwardmove = players[player].cmd.forwardmove;
    *sidemove = players[player].cmd.sidemove;
    *angleturn = players[player].cmd.angleturn;
    *buttons = players[player].cmd.buttons;
    return 1;
}

void dgb_SetTicCmdHook(dgb_ticcmd_hook hook)
{
    DG_TicCmdHook = hook;
}
