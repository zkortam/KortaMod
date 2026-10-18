#include "kortamod_games.h"

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

#include <string.h>

/* Slot layout: a 64-byte header, then the source. The header is written last,
 * so a slot is either complete or treated as free. */
#define MAGIC 0x314d474bu   /* "KGM1" */
#define HEADER 64
#define SLOTS 32

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t order;
    char name[16];
    uint8_t sha256[32];
    uint32_t check;          /* ~magic ^ size ^ order: guards against torn writes */
} Header;
_Static_assert(sizeof(Header) <= HEADER, "header fits");

static const esp_partition_t *part;
static SemaphoreHandle_t lock;
static int writing = -1;          /* slot being installed */
static uint32_t write_size, written;
static mbedtls_sha256_context sha;

static uint32_t check_of(const Header *h) { return ~h->magic ^ h->size ^ h->order; }
static bool read_header(int slot, Header *h) {
    if (esp_partition_read(part, (size_t)slot * KORTA_GAMES_SLOT_SIZE, h, sizeof *h) != ESP_OK) return false;
    return h->magic == MAGIC && h->check == check_of(h) && h->size > 0 && h->size <= KORTA_GAMES_SOURCE_MAX;
}

void KortaGames_Init(void) {
    if (lock) return;
    lock = xSemaphoreCreateMutex();
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "games");
    if (part && part->size < (size_t)SLOTS * KORTA_GAMES_SLOT_SIZE) part = NULL;
}

bool KortaGames_Available(void) { return part != NULL; }

int KortaGames_List(KortaGameInfo *out, int max) {
    if (!part || !out || max <= 0) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = 0;
    for (int slot = 0; slot < SLOTS; slot++) {
        Header h;
        if (!read_header(slot, &h)) continue;
        KortaGameInfo info = {.slot = (uint8_t)slot, .size = h.size, .order = h.order};
        memcpy(info.name, h.name, KORTA_GAME_NAME);
        info.name[KORTA_GAME_NAME] = '\0';
        /* Insert in install order; keep the first `max`. */
        int at = n < max ? n : max;
        while (at > 0 && out[at - 1].order > info.order) {
            if (at < max) out[at] = out[at - 1];
            at--;
        }
        if (at < max) { out[at] = info; if (n < max) n++; }
    }
    xSemaphoreGive(lock);
    return n;
}

int KortaGames_Begin(uint32_t size) {
    if (!part) return KORTA_GAMES_NO_STORAGE;
    if (size == 0 || size > KORTA_GAMES_SOURCE_MAX) return KORTA_GAMES_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    int free_slot = -1, count = 0;
    for (int slot = 0; slot < SLOTS; slot++) {
        Header h;
        if (read_header(slot, &h)) count++;
        else if (free_slot < 0) free_slot = slot;
    }
    if (count >= KORTA_GAME_MAX || free_slot < 0) { xSemaphoreGive(lock); return KORTA_GAMES_INVALID; }
    if (esp_partition_erase_range(part, (size_t)free_slot * KORTA_GAMES_SLOT_SIZE, KORTA_GAMES_SLOT_SIZE) != ESP_OK) {
        xSemaphoreGive(lock);
        return KORTA_GAMES_INVALID;
    }
    writing = free_slot;
    write_size = size;
    written = 0;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    /* The lock stays held until Finish or Abort: one install at a time. */
    return KORTA_GAMES_OK;
}

int KortaGames_Write(const void *data, size_t len) {
    if (writing < 0) return KORTA_GAMES_INVALID;
    if (written + len > write_size) return KORTA_GAMES_INVALID;
    if (esp_partition_write(part, (size_t)writing * KORTA_GAMES_SLOT_SIZE + HEADER + written, data, len) != ESP_OK)
        return KORTA_GAMES_INVALID;
    mbedtls_sha256_update(&sha, data, len);
    written += (uint32_t)len;
    return KORTA_GAMES_OK;
}

void KortaGames_Abort(void) {
    if (writing < 0) return;
    mbedtls_sha256_free(&sha);
    writing = -1;
    xSemaphoreGive(lock);
}

int KortaGames_Finish(const char *name, const uint8_t sha256[32]) {
    if (writing < 0) return KORTA_GAMES_INVALID;
    uint8_t got[32];
    mbedtls_sha256_finish(&sha, got);
    if (written != write_size || memcmp(got, sha256, sizeof got) != 0) { KortaGames_Abort(); return KORTA_GAMES_INVALID; }
    uint32_t order = 0;
    for (int slot = 0; slot < SLOTS; slot++) {
        Header h;
        if (slot != writing && read_header(slot, &h) && h.order >= order) order = h.order + 1;
    }
    Header h;
    memset(&h, 0, sizeof h);
    h.magic = MAGIC;
    h.size = write_size;
    h.order = order;
    strncpy(h.name, name ? name : "", sizeof h.name - 1);
    memcpy(h.sha256, got, sizeof got);
    h.check = check_of(&h);
    int result = esp_partition_write(part, (size_t)writing * KORTA_GAMES_SLOT_SIZE, &h, sizeof h) == ESP_OK
        ? KORTA_GAMES_OK : KORTA_GAMES_INVALID;
    mbedtls_sha256_free(&sha);
    writing = -1;
    xSemaphoreGive(lock);
    return result;
}

int KortaGames_Delete(int slot) {
    if (!part) return KORTA_GAMES_NO_STORAGE;
    if (slot < 0 || slot >= SLOTS) return KORTA_GAMES_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    Header h;
    int result = KORTA_GAMES_INVALID;
    if (read_header(slot, &h)) {
        /* Erasing the first sector is enough: the header lives there. */
        result = esp_partition_erase_range(part, (size_t)slot * KORTA_GAMES_SLOT_SIZE, 4096) == ESP_OK
            ? KORTA_GAMES_OK : KORTA_GAMES_INVALID;
    }
    xSemaphoreGive(lock);
    return result;
}

int KortaGames_Read(int slot, char *buf, size_t cap) {
    if (!part || slot < 0 || slot >= SLOTS || !buf) return -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    Header h;
    int n = -1;
    if (read_header(slot, &h) && h.size < cap
        && esp_partition_read(part, (size_t)slot * KORTA_GAMES_SLOT_SIZE + HEADER, buf, h.size) == ESP_OK) {
        buf[h.size] = '\0';
        n = (int)h.size;
    }
    xSemaphoreGive(lock);
    return n;
}

uint32_t KortaGames_Map(int slot, const char **source, size_t *len) {
    if (!part || slot < 0 || slot >= SLOTS || !source || !len) return 0;
    Header h;
    if (!read_header(slot, &h)) return 0;
    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(part, (size_t)slot * KORTA_GAMES_SLOT_SIZE + HEADER, h.size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK)
        return 0;
    *source = ptr;
    *len = h.size;
    return (uint32_t)handle + 1;   /* 0 means failure */
}

void KortaGames_Unmap(uint32_t handle) {
    if (handle) esp_partition_munmap((esp_partition_mmap_handle_t)(handle - 1));
}
