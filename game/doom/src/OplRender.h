/* OplRender.h
 *
 * The music of DOOM rendered to PCM (opl_render.c). Names no DOOM type: DoomMusic.cpp reads it.
 */

#ifndef MYENGINE_DOOM_OPLRENDER_H
#define MYENGINE_DOOM_OPLRENDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once, on the thread that runs DOOM, after the WAD is loaded: `genmidi` is the GENMIDI lump of the WAD (it must stay valid for good).
   0: the lump is not a GENMIDI lump. */
int oplr_start(const unsigned char* genmidi, int genmidiLength, unsigned int sampleRate);

/* Renders a MIDI file (an image in memory) once, from the start to the last note and its release. One song at a time, any thread but the
   same one is not needed. Returns the frames of the stereo 16 bit buffer in *out (free it with oplr_free), 0 when it fails.
   maxSeconds caps the length of the song. *cancel (read while it renders) set to non-zero stops it early (a new song was asked for). */
long oplr_render(const unsigned char* midi, int length, short** out, int maxSeconds, unsigned int* rateOut, const volatile int* cancel);
void oplr_free(short* buffer);

#ifdef __cplusplus
}
#endif

#endif
