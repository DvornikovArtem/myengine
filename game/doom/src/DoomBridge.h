// DoomBridge.h
//
// The C side of the port (DoomBridge.c, DoomSound.c): everything that has to see the headers of DOOM itself. The headers of DOOM
// define their own `boolean`, `true` and `false`, so no C++ file includes them; this header names no DOOM type and is read by both.
// Internal to the library: the host uses DoomGame.h.

#ifndef MYENGINE_DOOM_BRIDGE_H
#define MYENGINE_DOOM_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- running DOOM ---------------------------------------------------------------------------------------------------------- */

/* 0: DOOM is running. 1: DOOM left (I_Quit or I_Error): dgb_ExitCode / dgb_ExitMessage say how. After a 1 nothing may be called again. */
int dgb_Create(int argc, char** argv);
int dgb_Tick(void);
int dgb_ExitCode(void);
const char* dgb_ExitMessage(void);

void dgb_PostMouse(int buttons, int dx, int dy);

typedef struct dgb_player
{
    int inGame;
    int health;
    int armor;
    int x, y, z;          /* 16.16 fixed point map units */
    unsigned int angle;   /* 0 .. 2^32 for the full circle */
    int readyWeapon;
    int kills;
    int items;
    int secrets;
} dgb_player;

typedef struct dgb_state
{
    int gameTic;
    int gameState;        /* 0 level, 1 intermission, 2 finale, 3 demo screen */
    int episode;
    int map;
    int menuActive;
    int paused;
    int demoPlayback;
    int consolePlayer;
    int skill;
    int deathmatch;
    int netGame;
} dgb_state;

void dgb_GetState(dgb_state* out);
/* 0: no such player in the game */
int dgb_GetPlayer(int player, dgb_player* out);
/* The tic command the player really had in the last tic (after the hook) */
int dgb_GetTicCmd(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons);

typedef void (*dgb_ticcmd_hook)(int player, signed char* forwardmove, signed char* sidemove, short* angleturn, unsigned char* buttons);
void dgb_SetTicCmdHook(dgb_ticcmd_hook hook);

/* ---- sound ----------------------------------------------------------------------------------------------------------------- */

/* The sound device behind DOOM's sound and music modules. The host (DoomGame.cpp) fills it; every function may be NULL. */
typedef struct dgb_audio
{
    void* user;
    /* A sound effect as 16 bit mono; returns a handle > 0, 0 when it cannot be kept */
    int (*register_sfx)(void* user, const char* name, const short* samples, int frames, int rate);
    /* vol 0..127, sep 0..254 (127 is the middle); returns a voice id >= 0 or -1 when the sound is not played */
    int (*start_sfx)(void* user, int sfx, int channel, int vol, int sep);
    void (*update_sfx)(void* user, int channel, int vol, int sep);
    void (*stop_sfx)(void* user, int channel);
    int (*sfx_playing)(void* user, int channel);
    void (*update)(void* user);
    /* Once, when the music starts: the GENMIDI lump of the WAD, the instruments of the OPL chip (it stays valid) */
    void (*music_instruments)(void* user, const unsigned char* genmidi, int length);
    /* A standard MIDI file made from the MUS lump; returns a handle > 0 */
    int (*music_register)(void* user, const unsigned char* midi, int length);
    void (*music_unregister)(void* user, int handle);
    void (*music_play)(void* user, int handle, int loop);
    void (*music_stop)(void* user);
    void (*music_pause)(void* user);
    void (*music_resume)(void* user);
    void (*music_volume)(void* user, int volume127);
    int (*music_playing)(void* user);
} dgb_audio;

/* Before dgb_Create. NULL: no sound at all (the modules do not initialise) */
void dgb_SetAudio(const dgb_audio* audio);

#ifdef __cplusplus
}
#endif

#endif
