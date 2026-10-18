/* WebAssembly entry points for the site's game preview (docs/games.md). */
#include "kortamod_game.h"

#include <stddef.h>
#include <stdint.h>

/* The compiler may emit these for struct copies; there is no libc here. */
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int v, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)v; return d; }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) { const uint8_t *a = x, *b = y; for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }

static uint8_t memory[64 * 1024];
static char source[16384];
static KGame *game;

#define EXPORT(name) __attribute__((export_name(#name)))

EXPORT(kg_init) int kg_init(uint32_t seed) { game = kgame_create(memory, sizeof memory, seed); return game ? 0 : 1; }
EXPORT(kg_source) char *kg_source(void) { return source; }
EXPORT(kg_load) int kg_load(uint32_t len) { if (!game || len > sizeof source) return 1; return kgame_load(game, source, len) ? 1 : 0; }
EXPORT(kg_frame) int kg_frame(uint32_t buttons) { return game && kgame_frame(game, buttons) == 0 ? 0 : 1; }
EXPORT(kg_error) const char *kg_error(void) { return game ? kgame_error(game) : "engine not started"; }
EXPORT(kg_pixels) const uint8_t *kg_pixels(void) { return game ? kgame_pixels(game) : 0; }
EXPORT(kg_palette) const uint32_t *kg_palette(void) { return kgame_palette_rgb; }
EXPORT(kg_log) const char *kg_log(void) { return kgame_log(game); }
EXPORT(kg_saved) float kg_saved(void) { float v = 0; if (game) kgame_saved(game, &v); return v; }
EXPORT(kg_set_saved) void kg_set_saved(float v) { if (game) kgame_set_saved(game, v); }
