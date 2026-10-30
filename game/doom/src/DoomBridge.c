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
#include "d_main.h"
#include "d_player.h"
#include "doomstat.h"
#include "i_video.h"
#include "r_main.h"
#include "tables.h"
#include "w_wad.h"
#include "z_zone.h"

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
    if (p->attacker != NULL && p->attacker != p->mo)
    {
        out->hasAttacker = 1;
        out->attackerX = p->attacker->x;
        out->attackerY = p->attacker->y;
    }
    out->readyWeapon = (int)p->readyweapon;
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
