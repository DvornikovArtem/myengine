/* opl_render.c
 *
 * The music of DOOM, rendered offline: the OPL music module of Chocolate Doom (third_party/opl/i_oplmusic.c) plays a MIDI file into an emulation
 * of the OPL chip (Nuked OPL3, third_party/opl/opl3.c), and this file is the "driver" under it. Chocolate Doom's own drivers feed a sound card
 * in real time; this one has no clock of its own: it runs the callbacks of the song in order, generates the samples between them and returns
 * the whole song as PCM, which the host plays as a looped sound (DoomMusic.cpp).
 *
 * oplr_render runs on a worker thread, one song at a time, and touches nothing of DOOM except the music module (no zone memory, no WAD):
 * the instruments (the GENMIDI lump) were read by the main thread and are passed in oplr_start.
 *
 * The functions of opl.h that the music module calls are implemented here (the originals are in opl.c of Chocolate Doom, which also has the
 * drivers for real hardware and SDL).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

#include "opl.h"
#include "opl3.h"
#include "opl_queue.h"

#include "OplRender.h"

const unsigned char* oplr_genmidi_lump = NULL;

static opl3_chip g_chip;
static opl_callback_queue_t* g_queue = NULL;
static uint64_t g_time = 0;              /* us since the start of the song */
static uint64_t g_pauseOffset = 0;
static int g_paused = 0;
static unsigned int g_rate = 49716;
static int g_started = 0;

/* ---- what midifile.c of Chocolate Doom takes from SDL and from its i_system.c ---------------------------------------------------- */

uint32_t SDL_SwapBE32(uint32_t value)
{
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) | ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

uint16_t SDL_SwapBE16(uint16_t value)
{
    return (uint16_t)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

void* I_Realloc(void* ptr, size_t size)
{
    void* result = realloc(ptr, size);
    if (result == NULL && size != 0)
    {
        free(ptr);
    }
    return result;
}

/* ---- opl.h ------------------------------------------------------------------------------------------------------------------- */

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;
    OPL3_Reset(&g_chip, g_rate);
    if (g_queue == NULL)
    {
        g_queue = OPL_Queue_Create();
    }
    g_time = 0;
    g_pauseOffset = 0;
    g_paused = 0;
    return OPL_INIT_OPL3;
}

void OPL_Shutdown(void)
{
    if (g_queue != NULL)
    {
        OPL_Queue_Destroy(g_queue);
        g_queue = NULL;
    }
}

/* the rate is chosen by oplr_start (the native rate of the chip), not by the snd_samplerate of DOOM */
void OPL_SetSampleRate(unsigned int rate)
{
    (void)rate;
}

void OPL_WriteRegister(int reg, int value)
{
    OPL3_WriteRegBuffered(&g_chip, (uint16_t)reg, (uint8_t)value);
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void* data)
{
    OPL_Queue_Push(g_queue, callback, data, g_time - g_pauseOffset + us);
}

void OPL_ClearCallbacks(void)
{
    OPL_Queue_Clear(g_queue);
}

void OPL_AdjustCallbacks(float factor)
{
    OPL_Queue_AdjustCallbacks(g_queue, g_time, factor);
}

void OPL_Lock(void)
{
}

void OPL_Unlock(void)
{
}

void OPL_SetPaused(int paused)
{
    g_paused = paused;
}

/* Copied from opl.c of Chocolate Doom (the chip registers DMX sets at the start) */
void OPL_InitRegisters(int opl3)
{
    int r;

    // Initialize level registers

    for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x3f);
    }

    // Initialize other registers
    // These two loops write to registers that actually don't exist,
    // but this is what Doom does ...
    // Similarly, the <= is also intenational.

    for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // More registers ...

    for (r=1; r < OPL_REGS_LEVEL; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // Re-initialize the low registers:

    // Reset both timers and enable interrupts:
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x80);

    // "Allow FM chips to control the waveform of each operator":
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);

    if (opl3)
    {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);

        // Initialize level registers

        for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x3f);
        }

        // Initialize other registers
        // These two loops write to registers that actually don't exist,
        // but this is what Doom does ...
        // Similarly, the <= is also intenational.

        for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x00);
        }

        // More registers ...

        for (r=1; r < OPL_REGS_LEVEL; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x00);
        }
    }

    // Keyboard split point on (?)
    OPL_WriteRegister(OPL_REG_FM_MODE,         0x40);

    if (opl3)
    {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);
    }
}

/* ---- the renderer ------------------------------------------------------------------------------------------------------------ */

int oplr_start(const unsigned char* genmidi, int genmidiLength, unsigned int sampleRate)
{
    if (g_started)
    {
        return 1;
    }
    if (genmidi == NULL || genmidiLength < 8 + 175 * 36 || memcmp(genmidi, "#OPL_II#", 8) != 0)
    {
        return 0;
    }
    oplr_genmidi_lump = genmidi;
    g_rate = sampleRate != 0 ? sampleRate : 49716;
    if (music_opl_module.Init == NULL || !music_opl_module.Init())
    {
        return 0;
    }
    /* the music module scales the notes by this volume; the host sets the real one on its bus */
    music_opl_module.SetMusicVolume(127);
    g_started = 1;
    return 1;
}

/* Appends `frames` stereo frames from the chip to the buffer */
static int Generate(short** buffer, long* used, long* capacity, unsigned int frames)
{
    while (frames > 0)
    {
        const unsigned int chunk = frames > 2048 ? 2048 : frames;
        if (*used + (long)chunk > *capacity)
        {
            long grown = *capacity * 2 + 4096;
            short* bigger = (short*)realloc(*buffer, (size_t)grown * 2 * sizeof(short));
            if (bigger == NULL)
            {
                return 0;
            }
            *buffer = bigger;
            *capacity = grown;
        }
        OPL3_GenerateStream(&g_chip, *buffer + *used * 2, chunk);
        *used += (long)chunk;
        frames -= chunk;
    }
    return 1;
}

long oplr_render(const unsigned char* midi, int length, short** out, int maxSeconds, unsigned int* rateOut, const volatile int* cancel)
{
    void* handle;
    short* buffer = NULL;
    long used = 0;
    long capacity = 0;
    const long maxFrames = (long)maxSeconds * (long)g_rate;

    *out = NULL;
    if (!g_started || midi == NULL || length <= 0)
    {
        return 0;
    }
    if (rateOut != NULL)
    {
        *rateOut = g_rate;
    }

    /* a clean chip and an empty queue for every song */
    OPL3_Reset(&g_chip, g_rate);
    OPL_Queue_Clear(g_queue);
    g_time = 0;
    g_pauseOffset = 0;
    g_paused = 0;
    OPL_InitRegisters(0);

    handle = music_opl_module.RegisterSong((void*)midi, length);
    if (handle == NULL)
    {
        return 0;
    }
    music_opl_module.PlaySong(handle, false);

    /* The song is over when no callback is left: the tracks schedule their events one after the other */
    while (!OPL_Queue_IsEmpty(g_queue) && used < maxFrames && (cancel == NULL || *cancel == 0))
    {
        const uint64_t next = OPL_Queue_Peek(g_queue) + g_pauseOffset;
        if (next > g_time)
        {
            uint64_t frames = ((next - g_time) * g_rate + OPL_SECOND - 1) / OPL_SECOND;
            if (frames > 0)
            {
                if (!Generate(&buffer, &used, &capacity, (unsigned int)frames))
                {
                    free(buffer);
                    music_opl_module.StopSong();
                    music_opl_module.UnRegisterSong(handle);
                    return 0;
                }
                g_time += (frames * OPL_SECOND) / g_rate;
            }
        }
        /* the callbacks that are due (a callback schedules the next one) */
        while (!OPL_Queue_IsEmpty(g_queue) && g_time >= OPL_Queue_Peek(g_queue) + g_pauseOffset)
        {
            opl_callback_t callback;
            void* data;
            if (!OPL_Queue_Pop(g_queue, &callback, &data))
            {
                break;
            }
            callback(data);
        }
    }

    /* the release of the last notes */
    Generate(&buffer, &used, &capacity, g_rate * 3 / 4);

    music_opl_module.StopSong();
    music_opl_module.UnRegisterSong(handle);
    *out = buffer;
    return used;
}

void oplr_free(short* buffer)
{
    free(buffer);
}
