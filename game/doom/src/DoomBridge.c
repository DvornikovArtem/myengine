/* DoomBridge.c
 *
 * The C side of the port: leaving DOOM without exit(), the state of the game for the host, the mouse and the hook of the tic commands.
 * See DoomBridge.h; docs/game/doom.md describes how the pieces fit.
 */

#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "doomgeneric.h"

#include "d_event.h"
#include "d_loop.h"
#include "d_player.h"
#include "doomstat.h"

#include "DoomBridge.h"

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
    out->readyWeapon = (int)p->readyweapon;
    out->kills = p->killcount;
    out->items = p->itemcount;
    out->secrets = p->secretcount;
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
