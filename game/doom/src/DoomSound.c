/* DoomSound.c
 *
 * The sound module and the music module of DOOM (i_sound.h) over the audio device of the host (DoomBridge.h). doomgeneric leaves these
 * to the platform; here the modules only decode what is in the WAD and pass it on:
 *   - a sound effect lump (the DMX format: 8 bit unsigned mono) becomes 16 bit mono and is registered once with the host;
 *   - a music lump (MUS) becomes a standard MIDI file with mus2mid, which the host renders (DoomMusic.cpp, the OPL emulation).
 * What the sound is mixed with, and where it goes, is the host's business.
 */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"
#include "m_misc.h"

#include "DoomBridge.h"

/* i_sound.c binds these two variables when FEATURE_SOUND is on (they belong to the SDL sound module of the original) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static const dgb_audio* g_audio = NULL;
static boolean g_useSfxPrefix = true;

void dgb_SetAudio(const dgb_audio* audio)
{
    g_audio = audio;
}

/* ---- the sound effects ------------------------------------------------------------------------------------------------------- */

static void GetSfxLumpName(sfxinfo_t* sfx, char* buf, size_t buf_len)
{
    /* A linked sound is the lump of the sound it is linked to */
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    /* DOOM adds the DS prefix to the names of its sound lumps; Heretic and Hexen do not */
    if (g_useSfxPrefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static boolean Sfx_Init(boolean use_sfx_prefix)
{
    g_useSfxPrefix = use_sfx_prefix;
    return g_audio != NULL && g_audio->register_sfx != NULL;
}

static void Sfx_Shutdown(void)
{
}

static int Sfx_GetLumpNum(sfxinfo_t* sfxinfo)
{
    char namebuf[9];
    GetSfxLumpName(sfxinfo, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void Sfx_Update(void)
{
    if (g_audio != NULL && g_audio->update != NULL)
    {
        g_audio->update(g_audio->user);
    }
}

/* Decodes the lump of a sound effect and gives it to the host; driver_data keeps the handle of the host. false: not a sound. */
static boolean CacheSfx(sfxinfo_t* sfxinfo)
{
    int lumpnum = sfxinfo->lumpnum;
    byte* data;
    unsigned int lumplen;
    int samplerate;
    unsigned int length;
    short* samples;
    unsigned int i;
    int handle;

    if (lumpnum < 0)
    {
        return false;
    }
    data = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);

    /* The header: format 3, a 16 bit sample rate, a 32 bit length */
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }
    samplerate = (data[3] << 8) | data[2];
    length = ((unsigned int)data[7] << 24) | ((unsigned int)data[6] << 16) | ((unsigned int)data[5] << 8) | data[4];

    /* A length beyond the lump is not a sound; DMX also drops what is 48 samples or shorter */
    if (length > lumplen - 8 || length <= 48 || samplerate <= 0)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    /* DMX skips the first 16 and the last 16 samples of the lump (a padding) */
    data += 16;
    length -= 32;

    samples = (short*)malloc(length * sizeof(short));
    if (samples == NULL)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }
    for (i = 0; i < length; ++i)
    {
        samples[i] = (short)(((int)data[8 + i] - 128) * 256);
    }
    handle = g_audio->register_sfx(g_audio->user, sfxinfo->name, samples, (int)length, samplerate);
    free(samples);
    W_ReleaseLumpNum(lumpnum);

    if (handle <= 0)
    {
        return false;
    }
    sfxinfo->driver_data = (void*)(intptr_t)handle;
    return true;
}

static int Sfx_Start(sfxinfo_t* sfxinfo, int channel, int vol, int sep)
{
    if (g_audio == NULL || g_audio->start_sfx == NULL)
    {
        return -1;
    }
    if (sfxinfo->driver_data == NULL && !CacheSfx(sfxinfo))
    {
        return -1;
    }
    return g_audio->start_sfx(g_audio->user, (int)(intptr_t)sfxinfo->driver_data, channel, vol, sep);
}

static void Sfx_UpdateParams(int channel, int vol, int sep)
{
    if (g_audio != NULL && g_audio->update_sfx != NULL)
    {
        g_audio->update_sfx(g_audio->user, channel, vol, sep);
    }
}

static void Sfx_Stop(int channel)
{
    if (g_audio != NULL && g_audio->stop_sfx != NULL)
    {
        g_audio->stop_sfx(g_audio->user, channel);
    }
}

static boolean Sfx_IsPlaying(int channel)
{
    return g_audio != NULL && g_audio->sfx_playing != NULL && g_audio->sfx_playing(g_audio->user, channel) != 0;
}

static void Sfx_Precache(sfxinfo_t* sounds, int num_sounds)
{
    /* the sounds are decoded when they are played for the first time */
    (void)sounds;
    (void)num_sounds;
}

static snddevice_t sfx_devices[] =
{
    SNDDEVICE_PCSPEAKER,
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sfx_devices,
    sizeof(sfx_devices) / sizeof(*sfx_devices),
    Sfx_Init,
    Sfx_Shutdown,
    Sfx_GetLumpNum,
    Sfx_Update,
    Sfx_UpdateParams,
    Sfx_Start,
    Sfx_Stop,
    Sfx_IsPlaying,
    Sfx_Precache,
};

/* ---- the music --------------------------------------------------------------------------------------------------------------- */

static boolean Music_Init(void)
{
    if (g_audio == NULL || g_audio->music_register == NULL)
    {
        return false;
    }
    /* the instruments of the OPL chip are a lump of the WAD (DMX reads GENMIDI too) */
    if (g_audio->music_instruments != NULL && W_CheckNumForName("GENMIDI") >= 0)
    {
        const int lump = W_GetNumForName("GENMIDI");
        g_audio->music_instruments(g_audio->user, (const unsigned char*)W_CacheLumpNum(lump, PU_STATIC), (int)W_LumpLength(lump));
    }
    return true;
}

static void Music_Shutdown(void)
{
    if (g_audio != NULL && g_audio->music_stop != NULL)
    {
        g_audio->music_stop(g_audio->user);
    }
}

static void Music_SetVolume(int volume)
{
    if (g_audio != NULL && g_audio->music_volume != NULL)
    {
        g_audio->music_volume(g_audio->user, volume);
    }
}

static void Music_Pause(void)
{
    if (g_audio != NULL && g_audio->music_pause != NULL)
    {
        g_audio->music_pause(g_audio->user);
    }
}

static void Music_Resume(void)
{
    if (g_audio != NULL && g_audio->music_resume != NULL)
    {
        g_audio->music_resume(g_audio->user);
    }
}

static boolean IsMid(const byte* mem, int len)
{
    return len > 4 && memcmp(mem, "MThd", 4) == 0;
}

static void* Music_Register(void* data, int len)
{
    int handle = 0;

    if (g_audio == NULL || g_audio->music_register == NULL)
    {
        return NULL;
    }

    if (IsMid((const byte*)data, len))
    {
        handle = g_audio->music_register(g_audio->user, (const unsigned char*)data, len);
    }
    else
    {
        /* a MUS lump: the original music of DOOM */
        MEMFILE* instream = mem_fopen_read(data, (size_t)len);
        MEMFILE* outstream = mem_fopen_write();
        void* outbuf = NULL;
        size_t outbuf_len = 0;

        if (mus2mid(instream, outstream) == 0)
        {
            mem_get_buf(outstream, &outbuf, &outbuf_len);
            handle = g_audio->music_register(g_audio->user, (const unsigned char*)outbuf, (int)outbuf_len);
        }
        mem_fclose(instream);
        mem_fclose(outstream);
    }
    return (void*)(intptr_t)handle;
}

static void Music_Unregister(void* handle)
{
    if (g_audio != NULL && g_audio->music_unregister != NULL && handle != NULL)
    {
        g_audio->music_unregister(g_audio->user, (int)(intptr_t)handle);
    }
}

static void Music_Play(void* handle, boolean looping)
{
    if (g_audio != NULL && g_audio->music_play != NULL && handle != NULL)
    {
        g_audio->music_play(g_audio->user, (int)(intptr_t)handle, looping ? 1 : 0);
    }
}

static void Music_Stop(void)
{
    if (g_audio != NULL && g_audio->music_stop != NULL)
    {
        g_audio->music_stop(g_audio->user);
    }
}

static boolean Music_IsPlaying(void)
{
    return g_audio != NULL && g_audio->music_playing != NULL && g_audio->music_playing(g_audio->user) != 0;
}

static void Music_Poll(void)
{
}

static snddevice_t music_devices[] =
{
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_devices,
    sizeof(music_devices) / sizeof(*music_devices),
    Music_Init,
    Music_Shutdown,
    Music_SetVolume,
    Music_Pause,
    Music_Resume,
    Music_Register,
    Music_Unregister,
    Music_Play,
    Music_Stop,
    Music_IsPlaying,
    Music_Poll,
};
