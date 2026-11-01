//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//


#ifndef __W_WAD__
#define __W_WAD__

#include <stdio.h>

#include "doomtype.h"
#include "d_mode.h"

#include "w_file.h"


//
// TYPES
//

//
// WADFILE I/O related stuff.
//

typedef struct lumpinfo_s lumpinfo_t;

struct lumpinfo_s
{
    char	name[8];
    wad_file_t *wad_file;
    int		position;
    int		size;
    void       *cache;

    // Used for hash table lookups

    lumpinfo_t *next;
};


extern lumpinfo_t *lumpinfo;
extern unsigned int numlumps;

wad_file_t *W_AddFile (char *filename);

int	W_CheckNumForName (char* name);
int	W_GetNumForName (char* name);

int	W_LumpLength (unsigned int lump);
void    W_ReadLump (unsigned int lump, void *dest);

void*	W_CacheLumpNum (int lump, int tag);
void*	W_CacheLumpName (char* name, int tag);

void    W_GenerateHashTable(void);

// myengine: lumps that live in memory (the sprites and the sound of the flies of the swarm, game/doom/src/DoomSwarm.c). The names between the markers
// `first` and `last` (the sprite section) that start with `prefix` (4 characters) get `replaceFirst` as their first character, so that the sprite scan of
// DOOM does not find them; lumps outside the markers (wall patches) keep their names. W_InsertMemoryLumps puts `count` lumps before the last lump called
// `before` (the end of the sprite section), or at the end when there is none. Called between the loading of the WADs and R_Init, when no lump is cached yet.
void    W_RenameLumps(const char *first, const char *last, const char *prefix, char replaceFirst);
void    W_InsertMemoryLumps(const char *before, unsigned int count, const char (*names)[8], const unsigned char *const *data, const int *sizes);

extern unsigned int W_LumpNameHash(const char *s);

void    W_ReleaseLumpNum(int lump);
void    W_ReleaseLumpName(char *name);

void W_CheckCorrectIWAD(GameMission_t mission);

#endif
