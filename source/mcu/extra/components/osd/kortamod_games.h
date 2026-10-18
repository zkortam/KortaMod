#pragma once
/* Installed games, kept in the `games` flash partition (data, subtype 0x40) that
 * the installer adds. Each of 32 slots of 16 KB holds a header and the source.
 * Thread-safe: the phone link writes, the UI task lists and reads. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kortamod_core.h"

#define KORTA_GAMES_SLOT_SIZE 16384
#define KORTA_GAMES_SOURCE_MAX 16000

typedef struct {
    uint8_t slot;
    char name[KORTA_GAME_NAME + 1];
    uint32_t size;
    uint32_t order;      /* install sequence: Home shows games in this order */
} KortaGameInfo;

enum {
    KORTA_GAMES_OK = 0,
    KORTA_GAMES_INVALID = 3,     /* too big, no free slot, bad data */
    KORTA_GAMES_NO_STORAGE = 5,  /* the flash layout has no games partition */
};

void KortaGames_Init(void);
bool KortaGames_Available(void);
/* Up to KORTA_GAME_MAX games in install order. */
int KortaGames_List(KortaGameInfo *out, int max);
/* Install: Begin picks a free slot, Write streams the source, Finish checks the
 * SHA-256 (32 bytes) and commits the header. Abort leaves the slot free. */
int KortaGames_Begin(uint32_t size);
int KortaGames_Write(const void *data, size_t len);
int KortaGames_Finish(const char *name, const uint8_t sha256[32]);
void KortaGames_Abort(void);
int KortaGames_Delete(int slot);
/* Reads a game's source into buf (NUL-terminated); its length, or -1. */
int KortaGames_Read(int slot, char *buf, size_t cap);
/* Maps a game's source from flash without copying it to RAM; *source and *len
 * stay valid until KortaGames_Unmap(handle). The handle is 0 on failure. */
uint32_t KortaGames_Map(int slot, const char **source, size_t *len);
void KortaGames_Unmap(uint32_t handle);
