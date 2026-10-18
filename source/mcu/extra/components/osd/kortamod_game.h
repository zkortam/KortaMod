#pragma once
/* KortaMod games: a small JavaScript-subset engine with a 160x144, 16-colour
 * screen. Portable C with no allocation of its own: the caller hands it one
 * block of memory. Built for the Chromatic, for host tests and for the
 * site's in-browser preview (WebAssembly), so a game behaves the same in all.
 *
 * A game defines optional init(), update() and draw(); update() and draw()
 * run 30 times a second. See kortamod_game.c for the language and API. */
#include <stddef.h>
#include <stdint.h>

#define KGAME_W 160
#define KGAME_H 144
#define KGAME_FPS 30
#define KGAME_SOURCE_MAX 16000
#define KGAME_FB_BYTES (KGAME_W * KGAME_H / 2)
#define KGAME_ARENA_MIN (12 * 1024)   /* bytecode + game heap */
#define KGAME_MEMORY_MIN (36 * 1024)  /* kgame_create's single block */

/* Button bits for kgame_frame. */
enum { KGAME_LEFT = 1, KGAME_RIGHT = 2, KGAME_UP = 4, KGAME_DOWN = 8, KGAME_A = 16, KGAME_B = 32 };

typedef struct KGame KGame;

/* Places the engine in one block of `memory`; NULL if it is smaller than KGAME_MEMORY_MIN. */
KGame *kgame_create(void *memory, size_t size, uint32_t seed);
/* The same in three blocks, for a fragmented heap: engine state (kgame_state_size()
 * bytes), the screen (KGAME_FB_BYTES) and the arena for bytecode and the game's own
 * heap (at least KGAME_ARENA_MIN; more lets bigger games run). */
size_t kgame_state_size(void);
KGame *kgame_create_parts(void *state, size_t state_size, void *pixels, void *arena, size_t arena_size, uint32_t seed);
/* Compiles the source, runs its top level and init(). 0 on success. */
int kgame_load(KGame *game, const char *source, size_t length);
/* One frame: update() then draw(). 0 on success; after an error the game stops. */
int kgame_frame(KGame *game, unsigned buttons);
/* "line 12: score is not defined", or "" without an error. */
const char *kgame_error(const KGame *game);
/* 4-bit pixels, two per byte (high nibble first), row-major. */
const uint8_t *kgame_pixels(const KGame *game);
/* RGB565 and 0xRRGGBB values of the 16 colours. */
extern const uint16_t kgame_palette565[16];
extern const uint32_t kgame_palette_rgb[16];
/* The number a game keeps with save(); kgame_saved reports a change once. */
void kgame_set_saved(KGame *game, float value);
int kgame_saved(KGame *game, float *value);
/* Text written by console.log since the last call (the preview shows it). */
const char *kgame_log(KGame *game);
/* Memory use, for tuning and for the preview's readout. heap_used is the high-water mark. */
typedef struct { uint32_t code, code_cap, heap_used, heap_size, engine, constants, globals, frames; } KGameUsage;
void kgame_usage(const KGame *game, KGameUsage *usage);
/* Devices with a small task stack: stop compiling or running with "nested too
 * deeply" before the engine uses more than `bytes` of C stack (0 = unlimited). */
void kgame_set_stack_limit(KGame *game, uint32_t bytes);
