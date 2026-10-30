#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char * title);

// myengine: called instead of exit() by I_Quit (code 0, message NULL) and I_Error (code -1, the text of the error). The host
// must not return from it into DOOM (it leaves with longjmp); if it does return, the caller calls exit().
void DG_Exit(int code, const char* message);

// myengine: the command of player `player` for this tic, after DOOM has built it from the keyboard and the mouse (or read it from the
// network). The hook may change it. NULL (the default): nothing changes. Not called while a demo plays.
// myengine: how many players live in this process (-players N, 1..4): the console player and the ones that DG_TicCmdHook commands
extern int dg_players;

extern void (*DG_TicCmdHook)(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
