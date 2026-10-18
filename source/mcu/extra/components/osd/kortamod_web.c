#include "kortamod_web.h"
#include "kortamod_games.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The companion site. The device keeps one HTTPS connection to it while the
 * phone link is on: it reports its state and pairing code, and collects what
 * paired browsers asked for. Nothing listens on the device itself. */
#define CLOUD_HOST "kortamod.app"
#define CLOUD_SYNC_URL "https://" CLOUD_HOST "/api/device/sync"

enum {
    ID_HEX = 16,
    SECRET_HEX = 64,
    RESPONSE_MAX = 24 * 1024,
    CMD_WAIT_MS = 2000,
    CMD_MAX_AGE_MS = 12000,      /* without a deadline from the site, older commands are refused */
    RESULTS_MAX = 8,
    POLL_DEFAULT_MS = 20000,
    POLL_MIN_MS = 1000,
    POLL_MAX_MS = 240000,        /* the site asks for this when no browser is paired */
    POLL_WATCHED_MS = 2000,      /* the Phone screen is open: someone may be pairing */
    POLL_PAIRING_MS = 4000,      /* a code was shown recently and may still be typed in */
    PAIRING_WINDOW_MS = 600000,  /* a code's lifetime on the site */
    BACKOFF_MAX_MS = 60000,
    CONNECTED_GRACE_MS = 15000,  /* "connected" while a sync is at most this overdue */
};

static nvs_handle_t store;
static bool store_ok;
static SemaphoreHandle_t lock;      /* everything below that the UI task also touches */
static SemaphoreHandle_t cmd_done;
static TaskHandle_t task;
static char device_id[ID_HEX + 1];
static char device_secret[SECRET_HEX + 1];
static KortaWebView view;
static bool view_ready;
static bool enabled, online, watched;
static uint32_t shown_ms;           /* when the code was last on screen (or the link turned on) */
static bool forget_pending;
static char code[7];
static uint8_t paired;
static uint32_t last_ok_ms;
static uint32_t poll_ms = POLL_DEFAULT_MS;  /* the interval the site last asked for */
static int last_status = -1;   /* -1 never tried */

/* Command handoff to the UI task: 0 idle, 1 posted, 2 being applied. */
static uint8_t cmd_state;
static KortaWebCmd cmd;
static int cmd_result;

typedef struct { char id[17]; int result; } Result;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void new_code(void) {
    snprintf(code, sizeof code, "%06lu", (unsigned long)(esp_random() % 1000000u));
}

static void random_hex(char *out, int hex_chars) {
    for (int i = 0; i < hex_chars; i += 8) {
        char chunk[9];
        snprintf(chunk, sizeof chunk, "%08lx", (unsigned long)esp_random());
        memcpy(out + i, chunk, (size_t)(hex_chars - i < 8 ? hex_chars - i : 8));
    }
    out[hex_chars] = '\0';
}

/* The device's identity on the site, made once and kept in NVS. */
static void load_identity(void) {
    size_t a = sizeof device_id, b = sizeof device_secret;
    bool have = store_ok && nvs_get_str(store, "cl_id", device_id, &a) == ESP_OK
        && nvs_get_str(store, "cl_sec", device_secret, &b) == ESP_OK
        && strlen(device_id) == ID_HEX && strlen(device_secret) == SECRET_HEX;
    if (have) return;
    random_hex(device_id, ID_HEX);  /* not the MAC: the id should not identify hardware */
    random_hex(device_secret, SECRET_HEX);
    if (store_ok && nvs_set_str(store, "cl_id", device_id) == ESP_OK && nvs_set_str(store, "cl_sec", device_secret) == ESP_OK) {
        nvs_commit(store);
    }
}

/* Hands the parsed command in `cmd` to the UI task and waits for its result. The
 * UI task works on `cmd` in place while it holds it (state 1 posted, 2 taken);
 * the cloud task only fills it at state 0 (see wait_for_cmd). */
static int apply_command(void) {
    xSemaphoreTake(cmd_done, 0);
    xSemaphoreTake(lock, portMAX_DELAY);
    cmd_state = 1;
    xSemaphoreGive(lock);
    if (xSemaphoreTake(cmd_done, pdMS_TO_TICKS(CMD_WAIT_MS)) == pdTRUE) return cmd_result;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool taken = cmd_state == 2;
    if (!taken) cmd_state = 0;
    xSemaphoreGive(lock);
    if (taken && xSemaphoreTake(cmd_done, pdMS_TO_TICKS(1000)) == pdTRUE) return cmd_result;
    return KORTA_WEB_BUSY;
}

/* Turns one command from the site into a KortaWebCmd; false if malformed. */
static bool parse_command(const cJSON *c, KortaWebCmd *out) {
    const cJSON *kind = cJSON_GetObjectItem(c, "kind");
    if (!cJSON_IsString(kind)) return false;
    memset(out, 0, sizeof *out);
    if (strcmp(kind->valuestring, "notes") == 0) {
        const cJSON *rev = cJSON_GetObjectItem(c, "rev"), *items = cJSON_GetObjectItem(c, "items");
        const cJSON *pinned = cJSON_GetObjectItem(c, "pinned");
        int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1;
        int pins = cJSON_IsNumber(pinned) ? pinned->valueint : 0;
        if (!cJSON_IsNumber(rev) || n < 0 || n > KORTA_NOTE_SLOTS || pins < 0 || pins > n) return false;
        out->kind = KORTA_WEB_CMD_NOTES;
        out->rev = (uint16_t)rev->valueint;
        out->count = (uint8_t)n;
        out->pinned = (uint8_t)pins;
        int i = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, items) {
            if (!cJSON_IsString(item)) return false;
            korta_clean_text(out->notes[i++], sizeof out->notes[0], item->valuestring);
        }
        return true;
    }
    if (strcmp(kind->valuestring, "alarm") == 0) {
        const cJSON *on = cJSON_GetObjectItem(c, "on"), *h = cJSON_GetObjectItem(c, "hour"), *m = cJSON_GetObjectItem(c, "minute");
        const cJSON *index = cJSON_GetObjectItem(c, "index"), *days = cJSON_GetObjectItem(c, "days");
        const cJSON *label = cJSON_GetObjectItem(c, "label");
        if (!cJSON_IsBool(on) || !cJSON_IsNumber(h) || !cJSON_IsNumber(m)) return false;
        out->kind = KORTA_WEB_CMD_ALARM;
        out->alarm_on = cJSON_IsTrue(on);
        out->alarm = (uint8_t)(!cJSON_IsNumber(index) ? 0 : index->valueint < 0 || index->valueint >= KORTA_ALARM_MAX ? 255 : index->valueint);
        out->hour = (uint8_t)(h->valueint < 0 || h->valueint > 23 ? 255 : h->valueint);
        out->minute = (uint8_t)(m->valueint < 0 || m->valueint > 59 ? 255 : m->valueint);
        out->days = (uint8_t)(!cJSON_IsNumber(days) ? KORTA_DAYS_ALL : days->valueint < 0 || days->valueint > KORTA_DAYS_ALL ? 255 : days->valueint);
        if (cJSON_IsString(label)) korta_clean_text(out->text, KORTA_ALARM_LABEL + 1, label->valuestring);
        return true;
    }
    if (strcmp(kind->valuestring, "setting") == 0) {
        static const char *const names[KORTA_SET_COUNT] = {
            "brightness", "night", "flow", "silent", "blend", "cc_lcd", "cc_usb",
            "transit", "dpad4", "low_batt", "player", "palette", "hour12"
        };
        const cJSON *key = cJSON_GetObjectItem(c, "key"), *value = cJSON_GetObjectItem(c, "value");
        if (!cJSON_IsString(key) || !(cJSON_IsNumber(value) || cJSON_IsBool(value))) return false;
        out->kind = KORTA_WEB_CMD_SETTING;
        out->setting = -1;
        for (int i = 0; i < KORTA_SET_COUNT; i++) if (strcmp(key->valuestring, names[i]) == 0) out->setting = i;
        out->value = cJSON_IsBool(value) ? cJSON_IsTrue(value) : value->valueint;
        return out->setting >= 0;
    }
    if (strcmp(kind->valuestring, "message") == 0) {
        const cJSON *text = cJSON_GetObjectItem(c, "text"), *find = cJSON_GetObjectItem(c, "find");
        if (!cJSON_IsString(text)) return false;
        out->kind = KORTA_WEB_CMD_MESSAGE;
        korta_clean_text(out->text, sizeof out->text, text->valuestring);
        out->find = cJSON_IsTrue(find);
        return true;
    }
    if (strcmp(kind->valuestring, "timer") == 0) {
        static const char *const actions[] = {"set", "start", "stop", "reset"};
        const cJSON *action = cJSON_GetObjectItem(c, "action");
        if (!cJSON_IsString(action)) return false;
        out->kind = KORTA_WEB_CMD_TIMER;
        out->timer_action = -1;
        for (int i = 0; i < 4; i++) if (strcmp(action->valuestring, actions[i]) == 0) out->timer_action = i;
        out->countdown = cJSON_IsTrue(cJSON_GetObjectItem(c, "countdown"));
        const cJSON *seconds = cJSON_GetObjectItem(c, "seconds");
        out->seconds = cJSON_IsNumber(seconds) ? seconds->valueint : 0;
        return out->timer_action >= 0;
    }
    if (strcmp(kind->valuestring, "wifi") == 0) {
        const cJSON *ssid = cJSON_GetObjectItem(c, "ssid"), *pass = cJSON_GetObjectItem(c, "password");
        if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) > 32) return false;
        if (pass && (!cJSON_IsString(pass) || strlen(pass->valuestring) > 63)) return false;
        out->kind = KORTA_WEB_CMD_WIFI;
        snprintf(out->ssid, sizeof out->ssid, "%s", ssid->valuestring);
        snprintf(out->password, sizeof out->password, "%s", pass ? pass->valuestring : "");
        return true;
    }
    if (strcmp(kind->valuestring, "update") == 0) {
        const cJSON *url = cJSON_GetObjectItem(c, "url"), *size = cJSON_GetObjectItem(c, "size");
        const cJSON *sha = cJSON_GetObjectItem(c, "sha256"), *sig = cJSON_GetObjectItem(c, "signature");
        if (!cJSON_IsString(url) || !cJSON_IsNumber(size) || !cJSON_IsString(sha) || !cJSON_IsString(sig)
            || strlen(url->valuestring) >= sizeof out->url || strncmp(url->valuestring, "https://", 8) != 0
            || strlen(sha->valuestring) != 64 || strlen(sig->valuestring) >= sizeof out->signature
            || size->valuedouble <= 0 || size->valuedouble > 0x100000)
            return false;
        out->kind = KORTA_WEB_CMD_UPDATE;
        snprintf(out->url, sizeof out->url, "%s", url->valuestring);
        snprintf(out->sha256, sizeof out->sha256, "%s", sha->valuestring);
        snprintf(out->signature, sizeof out->signature, "%s", sig->valuestring);
        out->size = (uint32_t)size->valuedouble;
        return true;
    }
    if (strcmp(kind->valuestring, "clock") == 0) {
        const cJSON *epoch = cJSON_GetObjectItem(c, "epoch"), *zone = cJSON_GetObjectItem(c, "zone_minutes");
        if (!cJSON_IsNumber(epoch) || !cJSON_IsNumber(zone)) return false;
        if (epoch->valuedouble < 1704067200.0 || epoch->valuedouble > 4000000000.0) return false;
        if (zone->valueint < -720 || zone->valueint > 840) return false;
        out->kind = KORTA_WEB_CMD_CLOCK;
        out->epoch = (uint32_t)epoch->valuedouble;
        out->zone_minutes = (int16_t)zone->valueint;
        return true;
    }
    return false;
}

/* The request body: what browsers may see, the pairing code, earlier results. */
static char *build_sync(const Result *results, int count, bool forget) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON_AddStringToObject(root, "code", code);
    if (view_ready) {
        cJSON *state = cJSON_AddObjectToObject(root, "state");
        cJSON *notes = cJSON_AddObjectToObject(state, "notes");
        cJSON_AddNumberToObject(notes, "rev", view.rev);
        cJSON_AddBoolToObject(notes, "editing", view.editing);
        cJSON_AddNumberToObject(notes, "capacity", KORTA_NOTE_CAPACITY);
        cJSON_AddNumberToObject(notes, "slots", KORTA_NOTE_SLOTS);
        cJSON_AddNumberToObject(notes, "pinned", view.pinned);
        cJSON *items = cJSON_AddArrayToObject(notes, "items");
        for (int i = 0; i < view.count && i < KORTA_NOTE_SLOTS; i++) cJSON_AddItemToArray(items, cJSON_CreateString(view.notes[i]));
        if (view.alert) {
            cJSON *alert = cJSON_AddObjectToObject(state, "alert");
            cJSON_AddStringToObject(alert, "kind", view.alert == 1 ? "alarm" : "timer");
            if (view.alert == 1) cJSON_AddStringToObject(alert, "label", view.alert_label);
        }
        cJSON *alarms = cJSON_AddArrayToObject(state, "alarms");
        for (int i = 0; i < KORTA_ALARM_MAX; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddBoolToObject(a, "on", view.alarms[i].on);
            cJSON_AddNumberToObject(a, "hour", view.alarms[i].hour);
            cJSON_AddNumberToObject(a, "minute", view.alarms[i].minute);
            cJSON_AddNumberToObject(a, "days", view.alarms[i].days);
            cJSON_AddStringToObject(a, "label", view.alarms[i].label);
            cJSON_AddItemToArray(alarms, a);
        }
        cJSON *clock = cJSON_AddObjectToObject(state, "clock");
        cJSON_AddBoolToObject(clock, "known", view.time_known);
        cJSON_AddNumberToObject(clock, "epoch", (double)time(NULL));
        cJSON_AddNumberToObject(clock, "zone_minutes", view.zone_minutes);
        cJSON_AddBoolToObject(clock, "hour12", view.hour12);
        cJSON *device = cJSON_AddObjectToObject(state, "device");
        char name[24];
        snprintf(name, sizeof name, "Chromatic %.4s", device_id + ID_HEX - 4);
        cJSON_AddStringToObject(device, "name", name);
        cJSON_AddStringToObject(device, "mcu", view.mcu);
        cJSON_AddStringToObject(device, "fpga", view.fpga);
        cJSON_AddStringToObject(device, "chromatic", view.chromatic);
        cJSON_AddBoolToObject(device, "ota", view.ota_available);
        cJSON_AddBoolToObject(device, "games", view.games_ready);
        if (view.ota_phase) {
            static const char *const phases[] = {"idle", "download", "verify", "restart", "finish", "failed"};
            cJSON *u = cJSON_AddObjectToObject(state, "update");
            cJSON_AddStringToObject(u, "phase", phases[view.ota_phase < 6 ? view.ota_phase : 0]);
            cJSON_AddNumberToObject(u, "percent", view.ota_percent);
            if (view.ota_error[0]) cJSON_AddStringToObject(u, "error", view.ota_error);
        }
        cJSON *games = cJSON_AddArrayToObject(state, "games");
        for (int i = 0; i < view.game_count && i < KORTA_GAME_MAX; i++) {
            cJSON *gm = cJSON_CreateObject();
            cJSON_AddNumberToObject(gm, "slot", view.games[i].slot);
            cJSON_AddStringToObject(gm, "name", view.games[i].name);
            cJSON_AddNumberToObject(gm, "size", view.games[i].size);
            cJSON_AddItemToArray(games, gm);
        }
        if (view.battery_known) cJSON_AddNumberToObject(device, "battery", view.battery_pct);
        cJSON_AddBoolToObject(device, "charging", view.charging);
        cJSON *set = cJSON_AddObjectToObject(state, "settings");
        cJSON_AddNumberToObject(set, "brightness", view.settings.brightness);
        cJSON_AddBoolToObject(set, "night", view.settings.night);
        cJSON_AddBoolToObject(set, "flow", view.settings.flow);
        cJSON_AddBoolToObject(set, "hour12", view.settings.hour12);
        cJSON_AddBoolToObject(set, "silent", view.settings.silent);
        cJSON_AddBoolToObject(set, "blend", view.settings.blend);
        cJSON_AddBoolToObject(set, "cc_lcd", view.settings.cc_lcd);
        cJSON_AddBoolToObject(set, "cc_usb", view.settings.cc_usb);
        cJSON_AddBoolToObject(set, "transit", view.settings.transit);
        cJSON_AddBoolToObject(set, "dpad4", view.settings.dpad4);
        cJSON_AddNumberToObject(set, "low_batt", view.settings.low_batt);
        cJSON_AddNumberToObject(set, "player", view.settings.player);
        cJSON_AddNumberToObject(set, "palette", view.settings.palette);
        cJSON_AddBoolToObject(set, "gbc", view.settings.gbc);
        cJSON *timer = cJSON_AddObjectToObject(state, "timer");
        cJSON_AddBoolToObject(timer, "countdown", view.timer.countdown);
        cJSON_AddBoolToObject(timer, "running", view.timer.running);
        cJSON_AddBoolToObject(timer, "done", view.timer.done);
        cJSON_AddNumberToObject(timer, "set_s", view.timer.set_s);
        cJSON_AddNumberToObject(timer, "ms", view.timer.shown_ms);
        cJSON *wifi = cJSON_AddObjectToObject(state, "wifi");
        static const char *const phases[] = {"off", "idle", "scan", "join", "online", "fail", "denied"};
        cJSON_AddStringToObject(wifi, "phase", view.wifi.phase < 7 ? phases[view.wifi.phase] : "off");
        cJSON_AddStringToObject(wifi, "ssid", view.wifi.ssid);
        cJSON *nets = cJSON_AddArrayToObject(wifi, "networks");
        for (int i = 0; i < view.wifi.count && i < KORTA_NET_MAX; i++) {
            cJSON *n = cJSON_CreateObject();
            cJSON_AddStringToObject(n, "ssid", view.wifi.nets[i].ssid);
            cJSON_AddNumberToObject(n, "rssi", view.wifi.nets[i].rssi);
            cJSON_AddBoolToObject(n, "locked", view.wifi.nets[i].locked);
            cJSON_AddItemToArray(nets, n);
        }
    }
    xSemaphoreGive(lock);
    cJSON *list = cJSON_AddArrayToObject(root, "results");
    for (int i = 0; i < count; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "id", results[i].id);
        cJSON_AddNumberToObject(r, "result", results[i].result);
        cJSON_AddItemToArray(list, r);
    }
    if (forget) cJSON_AddBoolToObject(root, "forget", true);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/* One POST. Returns the HTTP status (0 on transport failure) and a malloc'd body. */
static int post(esp_http_client_handle_t client, const char *body, char **reply) {
    *reply = NULL;
    int len = (int)strlen(body);
    if (esp_http_client_open(client, len) != ESP_OK) return 0;
    if (esp_http_client_write(client, body, len) != len) { esp_http_client_close(client); return 0; }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *buf = malloc(RESPONSE_MAX);
    int got = 0;
    if (buf) {
        int n;
        while (got < RESPONSE_MAX - 1 && (n = esp_http_client_read(client, buf + got, RESPONSE_MAX - 1 - got)) > 0) got += n;
        buf[got] = '\0';
    }
    if (!esp_http_client_is_complete_data_received(client)) {
        /* Partial or oversized: drop the connection rather than misread the next reply. */
        esp_http_client_close(client);
    }
    *reply = buf;
    return status;
}

static esp_http_client_handle_t open_client(void) {
    char auth[8 + SECRET_HEX + 1];
    snprintf(auth, sizeof auth, "Bearer %s", device_secret);
    esp_http_client_config_t config = {
        .url = CLOUD_SYNC_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_header(client, "Authorization", auth);
    return client;
}

/* The last game install problem, for `korta game` on the console. */
static char game_diag[96];
static void diag(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(game_diag, sizeof game_diag, fmt, ap);
    va_end(ap);
}
const char *KortaWeb_GameDiag(void) { return game_diag; }
/* The UI task may still hold `cmd` after a timeout: wait until it lets go. */
static bool wait_for_cmd(void) {
    for (int i = 0; i < 500; i++) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool free_now = cmd_state == 0;
        xSemaphoreGive(lock);
        if (free_now) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static bool parse_hex(const char *s, uint8_t *out, int bytes) {
    if (!s || (int)strlen(s) != bytes * 2) return false;
    for (int i = 0; i < bytes; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}
/* Fetches a game's source over the sync connection (the same host, so the
 * keep-alive session is reused) straight into flash. */
static int download_game(esp_http_client_handle_t client, const char *gid, uint32_t size, const uint8_t sha[32], const char *name) {
    int r = KortaGames_Begin(size);
    if (r) { diag("begin %d (size %lu)", r, (unsigned long)size); return r; }
    char url[96];
    snprintf(url, sizeof url, "https://" CLOUD_HOST "/api/device/game?gid=%s", gid);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    bool ok = false;
    esp_err_t opened = esp_http_client_open(client, 0);
    int status = 0;
    int64_t length = -2;
    uint32_t got = 0;
    if (opened == ESP_OK) {
        length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        /* Function responses arrive chunked (no length): the SHA-256 check covers the content. */
        if (status == 200 && (length <= 0 || length == (int64_t)size)) {
            char buf[512];
            int n;
            while (got < size && (n = esp_http_client_read(client, buf, sizeof buf)) > 0) {
                if (got + (uint32_t)n > size || KortaGames_Write(buf, (size_t)n)) break;
                got += (uint32_t)n;
            }
            ok = got == size;
        }
        if (!ok || !esp_http_client_is_complete_data_received(client)) esp_http_client_close(client);
    }
    esp_http_client_set_url(client, CLOUD_SYNC_URL);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (!ok) {
        diag("download open %d status %d length %ld got %lu of %lu", (int)opened, status, (long)length, (unsigned long)got, (unsigned long)size);
        KortaGames_Abort();
        return KORTA_GAMES_INVALID;
    }
    r = KortaGames_Finish(name, sha);
    if (r) diag("finish %d", r);
    return r;
}
/* Game installs and deletes run here, in the phone link's task; the UI task
 * then reloads the list. 0 ok, 2 busy, 3 invalid, 5 no storage. */
static int game_command(esp_http_client_handle_t client, const cJSON *c) {
    const cJSON *op = cJSON_GetObjectItem(c, "op");
    if (!cJSON_IsString(op)) return KORTA_WEB_INVALID;
    if (!KortaGames_Available()) return KORTA_GAMES_NO_STORAGE;
    int r;
    if (strcmp(op->valuestring, "install") == 0) {
        const cJSON *gid = cJSON_GetObjectItem(c, "gid"), *name = cJSON_GetObjectItem(c, "name");
        const cJSON *size = cJSON_GetObjectItem(c, "size"), *sha = cJSON_GetObjectItem(c, "sha256");
        uint8_t digest[32], id[8];
        if (!cJSON_IsString(gid) || !parse_hex(gid->valuestring, id, 8) || !cJSON_IsString(name)
            || !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > KORTA_GAMES_SOURCE_MAX
            || !cJSON_IsString(sha) || !parse_hex(sha->valuestring, digest, 32)) return KORTA_WEB_INVALID;
        char clean[KORTA_GAME_NAME + 1];
        korta_clean_text(clean, sizeof clean, name->valuestring);
        r = download_game(client, gid->valuestring, (uint32_t)size->valuedouble, digest, clean);
    } else if (strcmp(op->valuestring, "delete") == 0) {
        const cJSON *slot = cJSON_GetObjectItem(c, "slot");
        if (!cJSON_IsNumber(slot)) return KORTA_WEB_INVALID;
        r = KortaGames_Delete(slot->valueint);
    } else return KORTA_WEB_INVALID;
    if (r == KORTA_GAMES_OK) {
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = KORTA_WEB_CMD_GAMES;
        r = apply_command();
    }
    return r;
}

static void cloud_task(void *arg) {
    (void)arg;
    esp_http_client_handle_t client = NULL;
    Result results[RESULTS_MAX];
    int result_count = 0;
    uint32_t wait_ms = 0, backoff_ms = 2000;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        xSemaphoreTake(lock, portMAX_DELAY);
        bool run = enabled && online;
        bool forget = forget_pending;
        uint32_t quick = watched ? POLL_WATCHED_MS : now_ms() - shown_ms < PAIRING_WINDOW_MS ? POLL_PAIRING_MS : 0;
        bool have_state = view_ready;
        xSemaphoreGive(lock);
        if (!run) {
            /* Off or no network: hold no connection and no TLS memory. */
            if (client) { esp_http_client_cleanup(client); client = NULL; }
            result_count = 0;
            wait_ms = 1000;
            continue;
        }
        if (!client && !(client = open_client())) { wait_ms = backoff_ms; continue; }
        char *body = build_sync(results, result_count, forget);
        char *reply = NULL;
        int status = body ? post(client, body, &reply) : 0;
        free(body);
        __atomic_store_n(&last_status, status, __ATOMIC_RELEASE);
        cJSON *json = (status == 200 && reply) ? cJSON_Parse(reply) : NULL;
        free(reply);
        if (!json) {
            /* Transport failure, an error status, or garbage: start over, slowly. */
            esp_http_client_cleanup(client);
            client = NULL;
            wait_ms = backoff_ms;
            backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }
        backoff_ms = 2000;
        result_count = 0;
        const cJSON *now = cJSON_GetObjectItem(json, "now");
        const cJSON *count = cJSON_GetObjectItem(json, "paired");
        const cJSON *rotate = cJSON_GetObjectItem(json, "rotate_code");
        const cJSON *poll = cJSON_GetObjectItem(json, "poll_ms");
        xSemaphoreTake(lock, portMAX_DELAY);
        last_ok_ms = now_ms();
        if (forget) {
            forget_pending = false;
            if (store_ok && nvs_erase_key(store, "cl_forget") == ESP_OK) nvs_commit(store);
        }
        if (cJSON_IsNumber(count)) paired = (uint8_t)(count->valueint > 255 ? 255 : count->valueint);
        if (cJSON_IsTrue(rotate)) new_code();
        xSemaphoreGive(lock);
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(json, "commands")) {
            const cJSON *id = cJSON_GetObjectItem(c, "id"), *t = cJSON_GetObjectItem(c, "t");
            if (!cJSON_IsString(id) || strlen(id->valuestring) != 16 || result_count >= RESULTS_MAX) continue;
            Result *r = &results[result_count++];
            snprintf(r->id, sizeof r->id, "%s", id->valuestring);
            /* Past its deadline the browser has been told it failed: never apply it late. */
            const cJSON *exp = cJSON_GetObjectItem(c, "exp");
            bool late = cJSON_IsNumber(now) && (cJSON_IsNumber(exp) ? now->valuedouble > exp->valuedouble
                : cJSON_IsNumber(t) && now->valuedouble - t->valuedouble > CMD_MAX_AGE_MS);
            const cJSON *kind = cJSON_GetObjectItem(c, "kind");
            if (late) r->result = 4;
            else if (!wait_for_cmd()) r->result = KORTA_WEB_BUSY;
            else if (cJSON_IsString(kind) && strcmp(kind->valuestring, "game") == 0) r->result = game_command(client, c);
            else if (!parse_command(c, &cmd)) r->result = KORTA_WEB_INVALID;
            else {
                r->result = apply_command();
                memset(cmd.password, 0, sizeof cmd.password);  /* the UI task has its copy */
            }
        }
        cJSON_Delete(json);
        /* Report results at once; otherwise poll as fast as the site asks. */
        int ask = cJSON_IsNumber(poll) ? poll->valueint : POLL_DEFAULT_MS;
        if (ask < POLL_MIN_MS) ask = POLL_MIN_MS;
        if (ask > POLL_MAX_MS) ask = POLL_MAX_MS;
        xSemaphoreTake(lock, portMAX_DELAY);
        poll_ms = (uint32_t)ask;
        xSemaphoreGive(lock);
        if (quick && (uint32_t)ask > quick) ask = (int)quick;
        /* The first syncs can precede the UI's first state: send it as soon as it exists. */
        if (!have_state && ask > POLL_MIN_MS) ask = POLL_MIN_MS;
        wait_ms = result_count ? 0 : (uint32_t)ask;
    }
}

void KortaWeb_Init(nvs_handle_t nvs) {
    if (lock) return;
    lock = xSemaphoreCreateMutex();
    cmd_done = xSemaphoreCreateBinary();
    store = nvs;
    store_ok = nvs != 0;
    load_identity();
    uint8_t forget = 0;
    if (store_ok && nvs_get_u8(store, "cl_forget", &forget) == ESP_OK && forget) forget_pending = true;
    new_code();
    /* TLS needs a deep stack; the task idles on a notification when off. */
    xTaskCreate(cloud_task, "korta-cloud", 10240, NULL, 3, &task);
}

void KortaWeb_Service(bool on, bool network, bool screen) {
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    /* Opening the Phone screen syncs at once, so its code and count are current. */
    bool changed = on != enabled || network != online || (screen && !watched);
    if (on && !enabled) { new_code(); shown_ms = now_ms(); }
    if (screen) shown_ms = now_ms();
    enabled = on;
    online = network;
    watched = screen;
    xSemaphoreGive(lock);
    if (changed && task) xTaskNotifyGive(task);
}

static uint8_t alert_before;
KortaWebView *KortaWeb_ViewBegin(void) {
    if (!lock) return NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    alert_before = view.alert;
    return &view;
}
void KortaWeb_ViewEnd(void) {
    if (!lock) return;
    /* A ring is reported at once, so phones are notified while it rings. */
    bool ring = view.alert && view.alert != alert_before;
    view_ready = true;
    xSemaphoreGive(lock);
    if (ring && task) xTaskNotifyGive(task);
}

KortaWebCmd *KortaWeb_TakeCommand(void) {
    if (!lock) return NULL;
    KortaWebCmd *got = NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cmd_state == 1) {
        cmd_state = 2;
        got = &cmd;
    }
    xSemaphoreGive(lock);
    return got;
}

void KortaWeb_Complete(int result) {
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cmd_state == 2) {
        cmd_result = result;
        cmd_state = 0;
        xSemaphoreGive(cmd_done);
    }
    xSemaphoreGive(lock);
}

void KortaWeb_Status(KortaLink *link) {
    if (!link) return;
    link->web_ready = false;
    link->web_paired = 0;
    link->web_code[0] = '\0';
    link->web_host[0] = '\0';
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t due = poll_ms + CONNECTED_GRACE_MS;
    link->web_ready = enabled && online && last_ok_ms && (now_ms() - last_ok_ms) < due;
    link->web_paired = paired;
    if (link->web_ready) {
        memcpy(link->web_code, code, sizeof code);
        snprintf(link->web_host, sizeof link->web_host, "%s", CLOUD_HOST);
    }
    xSemaphoreGive(lock);
}

int KortaWeb_LastStatus(void) {
    return __atomic_load_n(&last_status, __ATOMIC_ACQUIRE);
}

void KortaWeb_ForgetAll(void) {
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    forget_pending = true;
    paired = 0;
    /* Survives a reboot until the site has confirmed the revocation. */
    if (store_ok && nvs_set_u8(store, "cl_forget", 1) == ESP_OK) nvs_commit(store);
    xSemaphoreGive(lock);
    if (task) xTaskNotifyGive(task);
}
