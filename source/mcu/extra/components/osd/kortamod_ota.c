#include "kortamod_ota.h"

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public half of the release signing key (ECDSA P-256). The private key never
 * leaves the release machine; scripts/sign_firmware.py signs with it. */
static const char SIGNING_KEY[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYz4fJHb2kIk0Fq0uVCpHuef3gqUr\n"
    "Aa/JRfAz/hZYMdkssi3sr/ikUwq25jenHA0f8jvU4/v+5u/eU7yYEVJfzA==\n"
    "-----END PUBLIC KEY-----\n";

enum {
    BOOT_MAGIC = 0x4B4F5441,   /* "KOTA" */
    MAX_UNHEALTHY_BOOTS = 3,
    CHUNK = 4096,
    SIG_MAX = 80,              /* DER P-256 signatures are at most 72 bytes */
    URL_MAX = 160,
    TASK_STACK = 10240,
};

/* Survive panics, watchdog and software resets, which is how a broken image
 * shows itself; random after power-on, hence the magic. */
static RTC_NOINIT_ATTR uint32_t boot_magic;
static RTC_NOINIT_ATTR uint32_t boot_count;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static KortaOtaStatus status;
static bool busy;

static struct {
    char url[URL_MAX];
    uint32_t size;
    uint8_t sha256[32];
    uint8_t sig[SIG_MAX];
    size_t sig_len;
} job;

static void set_status(KortaOtaPhase phase, int percent, const char *error) {
    portENTER_CRITICAL(&mux);
    status.phase = phase;
    status.percent = (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent);
    if (error) snprintf(status.error, sizeof status.error, "%s", error);
    else if (phase != KORTA_OTA_FAILED) status.error[0] = '\0';
    portEXIT_CRITICAL(&mux);
}

static void fail(const char *why) {
    set_status(KORTA_OTA_FAILED, 0, why);
    portENTER_CRITICAL(&mux);
    busy = false;
    portEXIT_CRITICAL(&mux);
}

KortaOtaStatus KortaOta_Status(void) {
    portENTER_CRITICAL(&mux);
    KortaOtaStatus copy = status;
    portEXIT_CRITICAL(&mux);
    return copy;
}

static const esp_partition_t *factory_part(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}
static const esp_partition_t *staging_part(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

bool KortaOta_Available(void) {
    return factory_part() && staging_part()
        && esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
}

bool KortaOta_Pending(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
}

void KortaOta_EarlyBoot(void) {
    if (!KortaOta_Pending()) {
        boot_magic = 0;
        return;
    }
    if (boot_magic != BOOT_MAGIC) {
        boot_magic = BOOT_MAGIC;
        boot_count = 0;
    }
    boot_count++;
    if (boot_count > MAX_UNHEALTHY_BOOTS) {
        /* The new image never got as far as running normally: go back to the
         * firmware in factory, which the update never touched. */
        boot_magic = 0;
        const esp_partition_t *factory = factory_part();
        if (factory && esp_ota_set_boot_partition(factory) == ESP_OK) esp_restart();
    }
}

/* ---- finishing: copy the confirmed image from ota_0 into factory ---- */

static bool image_length(const esp_partition_t *part, uint32_t *length) {
    esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    esp_image_metadata_t meta;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;
    *length = meta.image_len;
    return *length > 0 && *length <= part->size;
}

static void finish_task(void *arg) {
    (void)arg;
    const esp_partition_t *from = staging_part(), *to = factory_part();
    uint8_t *buf = malloc(CHUNK);
    uint32_t length = 0;
    if (!from || !to || !buf || !image_length(from, &length) || length > to->size) {
        free(buf);
        fail("can't read the new firmware");
        vTaskDelete(NULL);
        return;
    }
    uint32_t total = (length + CHUNK - 1) / CHUNK * CHUNK;
    bool ok = true;
    for (uint32_t at = 0; ok && at < total; at += CHUNK) {
        /* One sector at a time with a pause between, so the screen and the
         * FPGA link keep running while factory is rewritten. */
        ok = esp_partition_read(from, at, buf, CHUNK) == ESP_OK
            && esp_partition_erase_range(to, at, CHUNK) == ESP_OK;
        bool blank = true;
        for (int i = 0; ok && blank && i < CHUNK; i++) blank = buf[i] == 0xFF;
        if (ok && !blank) ok = esp_partition_write(to, at, buf, CHUNK) == ESP_OK;
        set_status(KORTA_OTA_FINISH, (int)((uint64_t)(at + CHUNK) * 100 / total), NULL);
        vTaskDelay(1);
    }
    free(buf);
    uint8_t a[32], b[32];
    ok = ok && esp_partition_get_sha256(from, a) == ESP_OK && esp_partition_get_sha256(to, b) == ESP_OK
        && memcmp(a, b, sizeof a) == 0;
    if (!ok) {
        /* otadata still selects ota_0, so a restart runs the new firmware and
         * tries the copy again. */
        fail("copy to factory failed");
        vTaskDelete(NULL);
        return;
    }
    /* Selecting factory erases otadata: from now on the bootloader runs factory. */
    if (esp_ota_set_boot_partition(to) != ESP_OK) {
        fail("can't select factory");
        vTaskDelete(NULL);
        return;
    }
    set_status(KORTA_OTA_RESTART, 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void KortaOta_MarkHealthy(void) {
    if (!KortaOta_Pending()) return;
    portENTER_CRITICAL(&mux);
    bool start = !busy;
    busy = true;
    portEXIT_CRITICAL(&mux);
    if (!start) return;
    boot_magic = 0;   /* confirmed: stop counting boots */
    set_status(KORTA_OTA_FINISH, 0, NULL);
    if (xTaskCreate(finish_task, "korta-ota", 4096, NULL, 2, NULL) != pdPASS) fail("no memory");
}

/* ---- updating: download into ota_0, verify, restart into it ---- */

/* Build number of a "0.13.4-kortaN" version, or -1. */
static int build_of(const char *version, size_t max) {
    static const char prefix[] = "0.13.4-korta";
    size_t n = sizeof prefix - 1;
    if (strncmp(version, prefix, n) != 0) return -1;
    int value = 0;
    size_t i = n;
    for (; i < max && version[i] >= '0' && version[i] <= '9' && value < 100000; i++) value = value * 10 + (version[i] - '0');
    return i > n && (i == max || version[i] == '\0') ? value : -1;
}

static bool verify_signature(const uint8_t hash[32]) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)SIGNING_KEY, sizeof SIGNING_KEY) == 0
        && mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)
        && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, job.sig, job.sig_len) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

static void update_task(void *arg) {
    (void)arg;
    const esp_partition_t *target = staging_part();
    uint8_t *buf = malloc(CHUNK);
    esp_http_client_config_t config = {
        .url = job.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = buf ? esp_http_client_init(&config) : NULL;
    esp_ota_handle_t ota = 0;
    const char *why = NULL;
    uint8_t hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (!target || !buf || !client) why = "no memory";
    else if (esp_http_client_open(client, 0) != ESP_OK) why = "can't reach the site";
    else {
        int64_t length = esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        if (code != 200) why = "download refused";
        else if (length >= 0 && length != (int64_t)job.size) why = "download size is wrong";
        else if (esp_ota_begin(target, job.size, &ota) != ESP_OK) why = "can't prepare the slot";
    }
    uint32_t got = 0;
    mbedtls_sha256_starts(&sha, 0);
    while (!why && got < job.size) {
        uint32_t want = job.size - got < CHUNK ? job.size - got : CHUNK;
        int n = esp_http_client_read(client, (char *)buf, (int)want);
        /* The first chunk must hold the image header and version. */
        while (got == 0 && n > 0 && n < 0x50 && (uint32_t)n < want) {
            int more = esp_http_client_read(client, (char *)buf + n, (int)want - n);
            if (more <= 0) break;
            n += more;
        }
        if (n <= 0) { why = "download interrupted"; break; }
        if (got == 0) {
            /* The image's own version (esp_app_desc_t at 0x20, version at 0x30)
             * must be newer: a signed older image is never reinstalled. */
            const esp_app_desc_t *running = esp_app_get_description();
            int now = build_of(running->version, sizeof running->version);
            int next = n >= 0x50 && buf[0] == 0xE9 && buf[0x20] == 0x32 && buf[0x21] == 0x54 && buf[0x22] == 0xCD && buf[0x23] == 0xAB
                ? build_of((const char *)buf + 0x30, 32) : -1;
            if (next < 0) { why = "not a KortaMod firmware"; break; }
            if (now >= 0 && next <= now) { why = "already up to date"; break; }
        }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) { why = "flash write failed"; break; }
        got += (uint32_t)n;
        set_status(KORTA_OTA_DOWNLOAD, (int)((uint64_t)got * 100 / job.size), NULL);
    }
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(buf);
    if (!why) {
        set_status(KORTA_OTA_VERIFY, 100, NULL);
        if (memcmp(hash, job.sha256, sizeof hash) != 0) why = "firmware is damaged";
        else if (!verify_signature(hash)) why = "firmware is not signed by KortaMod";
    }
    if (!why) {
        esp_err_t end = esp_ota_end(ota);   /* also validates the image format */
        ota = 0;
        if (end != ESP_OK) why = "firmware image is invalid";
        else if (esp_ota_set_boot_partition(target) != ESP_OK) why = "can't select the new firmware";
    }
    if (ota) esp_ota_abort(ota);
    if (why) {
        fail(why);
        vTaskDelete(NULL);
        return;
    }
    boot_magic = BOOT_MAGIC;   /* the new image starts with a fresh count */
    boot_count = 0;
    set_status(KORTA_OTA_RESTART, 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int KortaOta_Begin(const char *url, uint32_t size, const char *sha256_hex, const char *signature_b64, bool power_ok) {
    const char *why = NULL;
    const esp_partition_t *target = staging_part();
    if (!KortaOta_Available()) why = "reinstall with the installer first";
    else if (KortaOta_Pending()) why = "an update is still finishing";
    else if (!power_ok) why = "charge the Chromatic first";
    else if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) >= URL_MAX) why = "bad download address";
    else if (!size || size > target->size) why = "firmware doesn't fit";
    else if (!sha256_hex || strlen(sha256_hex) != 64) why = "bad checksum";
    else if (!signature_b64) why = "missing signature";
    if (why) {
        set_status(KORTA_OTA_FAILED, 0, why);
        return -1;
    }
    portENTER_CRITICAL(&mux);
    bool start = !busy;
    busy = true;
    portEXIT_CRITICAL(&mux);
    if (!start) return -2;   /* already updating; status shows progress */
    for (int i = 0; i < 32; i++) {
        int hi = hex_value(sha256_hex[2 * i]), lo = hex_value(sha256_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { fail("bad checksum"); return -1; }
        job.sha256[i] = (uint8_t)(hi << 4 | lo);
    }
    if (mbedtls_base64_decode(job.sig, sizeof job.sig, &job.sig_len, (const unsigned char *)signature_b64,
                              strlen(signature_b64)) != 0 || job.sig_len < 8) {
        fail("bad signature");
        return -1;
    }
    snprintf(job.url, sizeof job.url, "%s", url);
    job.size = size;
    set_status(KORTA_OTA_DOWNLOAD, 0, NULL);
    if (xTaskCreate(update_task, "korta-ota", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        fail("no memory");
        return -1;
    }
    return 0;
}
