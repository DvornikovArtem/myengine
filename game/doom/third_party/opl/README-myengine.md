# The OPL music of DOOM in myengine

The original DOOM played its music on the FM chip (OPL2 / OPL3) of an AdLib or Sound Blaster card. FlyDoom does the same with an emulation of
the chip: the MIDI file made from the MUS lump of the WAD is played by the music module of Chocolate Doom into Nuked OPL3, and the result is PCM.

| Part | Source | License |
| --- | --- | --- |
| `i_oplmusic.c`, `midifile.c`, `midifile.h` | Chocolate Doom, https://github.com/chocolate-doom/chocolate-doom, commit `a2dfd51fcc00d272dc8a17a97ccd6683333ff4ea`, files `src/i_oplmusic.c`, `src/midifile.c`, `src/midifile.h` | GNU GPL v2 or later |
| `opl.h`, `opl_queue.c`, `opl_queue.h` | the same commit, `opl/` | GNU GPL v2 or later |
| `opl3.c`, `opl3.h` | Nuked OPL3 1.8, https://github.com/nukeykt/Nuked-OPL3, commit `765ec962e473aeb767e4cba74ffdc8f588ffbfe8` (`COPYING-nuked-opl3.txt`) | GNU LGPL v2.1 or later |

The driver under the music module (the part that feeds a sound card in real time in Chocolate Doom) is `game/doom/src/opl_render.c`: it has no
clock, it runs the callbacks of the song in order and returns the whole song as PCM.

## What was changed

Marked with `myengine` in a comment:

- `i_oplmusic.c`: the GENMIDI lump (the instruments) is given by the host instead of being read from the WAD (the code runs on a worker thread,
  the WAD and the zone memory of DOOM are not thread safe); `music_opl_module` is not `const` (the declaration of `i_sound.h` of doomgeneric is
  not); `PACKED_STRUCT` for MSVC; the largest MIDI file is 4 MB, not 96 KB (a long song made from a MUS lump does not fit in 96 KB);
  `remove` instead of `M_remove`.
- `midifile.c`: `fopen` instead of `M_fopen`; declarations of `SDL_SwapBE32`, `SDL_SwapBE16` and `I_Realloc` (they are in `game/doom/src/opl_render.c`; without a declaration the 64-bit pointer of `I_Realloc` was cut to an int).
