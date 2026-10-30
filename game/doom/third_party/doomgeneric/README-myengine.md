# doomgeneric in myengine

The source of the classic DOOM (id Software, 1997, GPL v2) as it is in doomgeneric, a port made to ease porting.

- Source: https://github.com/ozkl/doomgeneric
- Commit: `e257a09527062bfb986b6e5e0780839b6cbd5b71`
- License: GNU GPL version 2 (`COPYING`, the same file as `LICENSE` of the repository). The game data (WAD) is not here: Freedoom (a free
  replacement) is downloaded by `tools/doom/fetch_freedoom.ps1`, the original DOOM.WAD is the player's own.

## What was taken

Every `.c` and `.h` of `doomgeneric/doomgeneric/` except the platform parts that the engine replaces: `doomgeneric_allegro.c`,
`doomgeneric_emscripten.c`, `doomgeneric_linuxvt.c`, `doomgeneric_sdl.c`, `doomgeneric_soso.c`, `doomgeneric_sosox.c`, `doomgeneric_win.c`,
`doomgeneric_xlib.c` (the platform functions `DG_*` are in `game/doom/src/DoomGame.cpp`), and the SDL / Allegro sound
(`i_allegromusic.c`, `i_allegrosound.c`, `i_sdlmusic.c`, `i_sdlsound.c`: the sound and music modules are `game/doom/src/DoomSound.c`).
`gusconf.c` and `icon.c` are in the copy but are not built (`game/doom/CMakeLists.txt`).

## What was changed

All the changes are marked with the word `myengine` in a comment, and are small:

| File | Change |
| --- | --- |
| `doomgeneric.h` | declares `DG_Exit` (how DOOM leaves) and `DG_TicCmdHook` (the command of a player is given to the host before the tic) |
| `i_system.c` | `I_Quit` and `I_Error` call `DG_Exit` instead of `exit()`: the host gets control back, the process lives |
| `d_main.c` | no `exit(0)` after the ENDOOM screen (`I_Quit` goes on and leaves through `DG_Exit`) |
| `g_game.c` | `G_Ticker` calls `DG_TicCmdHook` for every player in the game, after the command is built (not while a demo plays) |
| `m_config.c` | `-configdir <dir>`: the folder of `default.cfg` and of the saves (the default was the current folder) |
| `i_sound.c` | no `#include <SDL_mixer.h>` (no SDL here) |
| `doomtype.h` | `PACKED_STRUCT` for MSVC (the music code of Chocolate Doom, `third_party/opl`, packs its structs with it) |
