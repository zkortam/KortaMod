#include "kortamod_link.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

enum { SCAN_KEEP = 16, RESCAN_MS = 12000 };

typedef struct {
    uint8_t op;
    char ssid[33];
    char password[64];
} LinkCmd;

static QueueHandle_t queue;
static SemaphoreHandle_t lock;
static SemaphoreHandle_t started;
static wifi_ap_record_t scan_buf[SCAN_KEEP];
static uint16_t scan_count;
static KortaLink published;
static bool task_up;
static uint32_t pending_commands;
static bool radio_inited;
static bool radio_started;
static bool sntp_started;
static bool join_armed;
static bool connection_wanted;
static uint32_t join_started_at;
static wifi_config_t target_config;
static esp_netif_t *station;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static uint8_t resume_phase;
static uint32_t scanned_at;
static uint32_t join_arm_ms;
static uint32_t ev_scan;
static uint32_t ev_ip;
static uint32_t got_ip;
static int32_t ev_disc;
static int join_failures;
static char target_pass[64];

static void publish_phase(uint8_t phase) {
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    published.phase = phase;
    xSemaphoreGive(lock);
}

static void copy_text(char *dest, size_t size, const char *src) {
    if (!dest || !size) return;
    dest[0] = '\0';
    if (!src) return;
    size_t n = strnlen(src, size - 1);
    memcpy(dest, src, n);
    dest[n] = '\0';
}

static bool denied_reason(int reason) {
    return reason == WIFI_REASON_AUTH_EXPIRE
        || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        || reason == WIFI_REASON_AUTH_FAIL
        || reason == WIFI_REASON_HANDSHAKE_TIMEOUT
        || reason == WIFI_REASON_CONNECTION_FAIL
        || reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (started) xSemaphoreGive(started);
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        __atomic_store_n(&ev_scan, 1, __ATOMIC_RELEASE);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && data) {
        const wifi_event_sta_disconnected_t *disc = data;
        __atomic_store_n(&ev_disc, disc->reason ? (int32_t)disc->reason : 1, __ATOMIC_RELEASE);
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    uint32_t addr = 0;
    if (data) addr = ((const ip_event_got_ip_t *)data)->ip_info.ip.addr;
    __atomic_store_n(&got_ip, addr, __ATOMIC_RELEASE);
    __atomic_store_n(&ev_ip, 1, __ATOMIC_RELEASE);
}

static void start_sntp(void) {
    if (sntp_started) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    sntp_started = true;
}

static bool ensure_radio(void) {
    if (radio_inited) return true;
    /* Settings owns NVS. A radio error must never erase user data. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return false;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    station = esp_netif_create_default_wifi_sta();
    if (!station) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) goto failed_netif;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) goto failed_wifi;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, &wifi_handler) != ESP_OK) goto failed_wifi;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, &ip_handler) != ESP_OK) goto failed_wifi;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) goto failed_wifi;
    radio_inited = true;
    return true;
failed_wifi:
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    if (wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    ip_handler = wifi_handler = NULL;
    esp_wifi_deinit();
failed_netif:
    esp_netif_destroy_default_wifi(station);
    station = NULL;
    return false;
}

static bool radio_on(void) {
    if (!ensure_radio()) {
        publish_phase(KORTA_WIFI_FAIL);
        return false;
    }
    if (!radio_started) {
        if (started) xSemaphoreTake(started, 0);
        if (esp_wifi_start() != ESP_OK) {
            publish_phase(KORTA_WIFI_FAIL);
            return false;
        }
        if (started) (void)xSemaphoreTake(started, pdMS_TO_TICKS(2000));
        esp_wifi_set_ps(WIFI_PS_NONE);
        __atomic_store_n(&radio_started, true, __ATOMIC_RELEASE);
        publish_phase(KORTA_WIFI_IDLE);
    }
    return true;
}

static void fill_config(wifi_config_t *cfg, const char *ssid, const char *password) {
    memset(cfg, 0, sizeof *cfg);
    if (ssid) {
        size_t n = strnlen(ssid, 32);
        memcpy(cfg->sta.ssid, ssid, n);
    }
    if (password) {
        size_t n = strnlen(password, 63);
        memcpy(cfg->sta.password, password, n);
    }
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.threshold.rssi = -127;
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
    cfg->sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    cfg->sta.failure_retry_cnt = 3;
}

static void publish_link(uint8_t phase, const char *ssid, int8_t rssi, uint32_t ip, bool set_ip) {
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    published.phase = phase;
    if (ssid) copy_text(published.ssid, sizeof published.ssid, ssid);
    published.rssi = rssi;
    if (set_ip) published.ip = ip;
    xSemaphoreGive(lock);
}

static void connect_to(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || !radio_on()) return;
    size_t plen = password ? strnlen(password, 63) : 0;
    copy_text(target_pass, sizeof target_pass, password);
    if (plen > 0 && plen < 8) {
        publish_link(KORTA_WIFI_DENIED, ssid, 0, 0, true);
        return;
    }
    wifi_config_t cfg;
    fill_config(&cfg, ssid, password);
    int best = -1;
    for (uint16_t i = 0; i < scan_count; i++) {
        if (strncmp((const char *)scan_buf[i].ssid, ssid, 32) != 0) continue;
        if (best < 0 || scan_buf[i].rssi > scan_buf[best].rssi) best = (int)i;
    }
    int8_t rssi = 0;
    if (best >= 0) {
        rssi = scan_buf[best].rssi;
        cfg.sta.channel = scan_buf[best].primary;
        wifi_auth_mode_t auth = scan_buf[best].authmode;
        bool sae = auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
        cfg.sta.sae_pwe_h2e = sae ? WPA3_SAE_PWE_BOTH : WPA3_SAE_PWE_UNSPECIFIED;
    }
    /* Stop the old association/scan before installing the new configuration. */
    esp_wifi_scan_stop();
    target_config = cfg;
    join_failures = 0;
    connection_wanted = true;
    join_armed = true;
    join_arm_ms = (uint32_t)(esp_timer_get_time() / 1000);
    __atomic_store_n(&ev_disc, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ev_ip, 0, __ATOMIC_RELEASE);
    publish_link(KORTA_WIFI_JOIN, ssid, rssi, 0, true);
    esp_wifi_disconnect();
}

static void begin_scan(void) {
    if (!radio_started) return;
    uint8_t phase = KORTA_WIFI_IDLE;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        phase = published.phase;
        xSemaphoreGive(lock);
    }
    if (phase == KORTA_WIFI_OFF || phase == KORTA_WIFI_JOIN || phase == KORTA_WIFI_SCAN) return;
    wifi_scan_config_t scan = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = true,
        .scan_time.active.min = 80,
        .scan_time.active.max = 220,
    };
    if (esp_wifi_scan_start(&scan, false) != ESP_OK) return;
    resume_phase = phase;
    publish_phase(KORTA_WIFI_SCAN);
    scanned_at = (uint32_t)(esp_timer_get_time() / 1000);
}

static void finish_scan(void) {
    /* Scan completion can arrive after Off, Forget, or a new Join. */
    if (!radio_started) return;
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > SCAN_KEEP) count = SCAN_KEEP;
    if (esp_wifi_scan_get_ap_records(&count, scan_buf) != ESP_OK) count = 0;
    scan_count = count;

    KortaNet found[KORTA_NET_MAX];
    memset(found, 0, sizeof found);
    int kept = 0;
    for (uint16_t i = 0; i < count; i++) {
        const char *ssid = (const char *)scan_buf[i].ssid;
        if (!ssid[0]) continue;
        bool locked = scan_buf[i].authmode != WIFI_AUTH_OPEN && scan_buf[i].authmode != WIFI_AUTH_OWE;
        int slot = -1;
        for (int j = 0; j < kept; j++) {
            if (strncmp(found[j].ssid, ssid, 32) == 0) slot = j;
        }
        if (slot >= 0) {
            if (scan_buf[i].rssi > found[slot].rssi) {
                found[slot].rssi = scan_buf[i].rssi;
                found[slot].locked = locked;
            }
            continue;
        }
        if (kept < KORTA_NET_MAX) {
            slot = kept++;
        } else {
            int weak = 0;
            for (int j = 1; j < KORTA_NET_MAX; j++) if (found[j].rssi < found[weak].rssi) weak = j;
            if (scan_buf[i].rssi <= found[weak].rssi) continue;
            slot = weak;
        }
        memset(&found[slot], 0, sizeof found[slot]);
        copy_text(found[slot].ssid, sizeof found[slot].ssid, ssid);
        found[slot].rssi = scan_buf[i].rssi;
        found[slot].locked = locked;
    }
    for (int i = 0; i < kept; i++) {
        for (int j = i + 1; j < kept; j++) {
            if (found[j].rssi > found[i].rssi) {
                KortaNet swap = found[i];
                found[i] = found[j];
                found[j] = swap;
            }
        }
    }
    uint8_t phase = resume_phase == KORTA_WIFI_SCAN || resume_phase == KORTA_WIFI_OFF ? KORTA_WIFI_IDLE : resume_phase;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (published.phase != KORTA_WIFI_SCAN) phase = published.phase;
        published.phase = phase;
        published.count = (uint8_t)kept;
        memcpy(published.nets, found, sizeof found);
        xSemaphoreGive(lock);
    }
}

static void refresh_rssi(void) {
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof ap);
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    published.rssi = ap.rssi;
    if (ap.ssid[0]) copy_text(published.ssid, sizeof published.ssid, (const char *)ap.ssid);
    xSemaphoreGive(lock);
}

static void handle(const LinkCmd *cmd) {
    if (cmd->op == KORTA_LINK_OFF) {
        connection_wanted = false;
        join_armed = false;
        __atomic_store_n(&ev_ip, 0, __ATOMIC_RELEASE);
        if (sntp_started) { esp_sntp_stop(); sntp_started = false; }
        if (radio_started) {
            esp_wifi_disconnect();
            esp_wifi_stop();
            __atomic_store_n(&radio_started, false, __ATOMIC_RELEASE);
            if (started) xSemaphoreTake(started, 0);
        }
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            published.phase = KORTA_WIFI_OFF;
            published.count = 0;
            published.rssi = 0;
            published.ip = 0;
            published.ssid[0] = '\0';
            xSemaphoreGive(lock);
        }
        return;
    }
    if (cmd->op == KORTA_LINK_ON) {
        if (radio_on()) begin_scan();
        return;
    }
    if (cmd->op == KORTA_LINK_SCAN) {
        if (radio_on()) begin_scan();
        return;
    }
    if (cmd->op == KORTA_LINK_JOIN) {
        connect_to(cmd->ssid, cmd->password);
        return;
    }
    if (cmd->op == KORTA_LINK_FORGET) {
        connection_wanted = false;
        join_armed = false;
        __atomic_store_n(&ev_ip, 0, __ATOMIC_RELEASE);
        if (sntp_started) { esp_sntp_stop(); sntp_started = false; }
        if (radio_started) esp_wifi_disconnect();
        wifi_config_t blank;
        memset(&blank, 0, sizeof blank);
        if (radio_inited) esp_wifi_set_config(WIFI_IF_STA, &blank);
        target_pass[0] = '\0';
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            published.phase = radio_started ? KORTA_WIFI_IDLE : KORTA_WIFI_OFF;
            published.ssid[0] = '\0';
            published.rssi = 0;
            published.ip = 0;
            xSemaphoreGive(lock);
        }
        begin_scan();
    }
}

static void link_task(void *arg) {
    (void)arg;
    for (;;) {
        LinkCmd cmd;
        if (xQueueReceive(queue, &cmd, pdMS_TO_TICKS(400)) == pdTRUE) {
            handle(&cmd);
            __atomic_sub_fetch(&pending_commands, 1, __ATOMIC_ACQ_REL);
        }
        wifi_ap_record_t current_ap;
        if (__atomic_exchange_n(&ev_ip, 0, __ATOMIC_ACQ_REL) && radio_started && connection_wanted
            && esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK
            && memcmp(current_ap.ssid, target_config.sta.ssid, 32) == 0) {
            __atomic_store_n(&ev_disc, 0, __ATOMIC_RELEASE);
            uint32_t addr = __atomic_load_n(&got_ip, __ATOMIC_ACQUIRE);
            join_failures = 0;
            join_armed = false;
            if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                published.phase = KORTA_WIFI_ONLINE;
                published.ip = addr;
                xSemaphoreGive(lock);
            }
            start_sntp();
            refresh_rssi();
        }
        int reason = __atomic_exchange_n(&ev_disc, 0, __ATOMIC_ACQ_REL);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (join_armed && radio_started && connection_wanted && (reason || now_ms - join_arm_ms > 1500)) {
            join_armed = false;
            join_started_at = now_ms;
            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &target_config);
            if (err == ESP_OK) err = esp_wifi_connect();
            reason = 0;
            if (err != ESP_OK) {
                join_failures++;
                if (join_failures >= 3) {
                    connection_wanted = false;
                    publish_link(KORTA_WIFI_FAIL, NULL, 0, 0, true);
                }
                else {
                    join_armed = true;
                    join_arm_ms = now_ms;
                }
            }
        }
        if (radio_started && connection_wanted && !join_armed && now_ms - join_started_at >= 30000u) {
            uint8_t phase;
            xSemaphoreTake(lock, portMAX_DELAY);
            phase = published.phase;
            xSemaphoreGive(lock);
            if (phase == KORTA_WIFI_JOIN) reason = WIFI_REASON_CONNECTION_FAIL;
        }
        if (reason && radio_started && connection_wanted) {
            uint8_t phase = KORTA_WIFI_OFF;
            if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                phase = published.phase;
                xSemaphoreGive(lock);
            }
            if (phase == KORTA_WIFI_JOIN || phase == KORTA_WIFI_ONLINE || phase == KORTA_WIFI_SCAN) {
                join_failures++;
                if (join_failures >= 3) {
                    connection_wanted = false;
                    uint8_t next = (target_pass[0] && denied_reason(reason)) ? KORTA_WIFI_DENIED : KORTA_WIFI_FAIL;
                    publish_link(next, NULL, 0, 0, true);
                } else {
                    publish_link(KORTA_WIFI_JOIN, NULL, 0, 0, true);
                    join_armed = true;
                    join_arm_ms = now_ms;
                    esp_wifi_disconnect();
                }
            }
        }
        if (__atomic_exchange_n(&ev_scan, 0, __ATOMIC_ACQ_REL)) finish_scan();
        uint8_t phase = KORTA_WIFI_OFF;
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            phase = published.phase;
            if (phase == KORTA_WIFI_ONLINE) {
                xSemaphoreGive(lock);
                refresh_rssi();
            } else {
                xSemaphoreGive(lock);
            }
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (radio_started && phase != KORTA_WIFI_OFF && phase != KORTA_WIFI_JOIN && phase != KORTA_WIFI_SCAN
            && now - scanned_at > RESCAN_MS) {
            begin_scan();
        }
    }
}

void KortaLink_Start(void) {
    if (task_up) return;
    lock = xSemaphoreCreateMutex();
    started = xSemaphoreCreateBinary();
    queue = xQueueCreate(6, sizeof(LinkCmd));
    if (lock && queue && started && xTaskCreate(link_task, "korta-link", 8192, NULL, 4, NULL) == pdPASS) {
        task_up = true;
        return;
    }
    if (queue) vQueueDelete(queue);
    if (started) vSemaphoreDelete(started);
    if (lock) vSemaphoreDelete(lock);
    queue = NULL;
    started = lock = NULL;
}

bool KortaLink_RadioOn(void) {
    return __atomic_load_n(&radio_started, __ATOMIC_ACQUIRE)
        || __atomic_load_n(&pending_commands, __ATOMIC_ACQUIRE) != 0;
}

void KortaLink_Command(int op, const char *ssid, const char *password) {
    if (!task_up || !queue) return;
    LinkCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.op = (uint8_t)op;
    copy_text(cmd.ssid, sizeof cmd.ssid, ssid);
    copy_text(cmd.password, sizeof cmd.password, password);
    __atomic_add_fetch(&pending_commands, 1, __ATOMIC_ACQ_REL);
    if (xQueueSend(queue, &cmd, 0) != pdTRUE) {
        __atomic_sub_fetch(&pending_commands, 1, __ATOMIC_ACQ_REL);
        publish_phase(KORTA_WIFI_FAIL);
    }
}

void KortaLink_Snapshot(KortaLink *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!task_up) { out->phase = KORTA_WIFI_FAIL; return; }
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    *out = published;
    xSemaphoreGive(lock);
}
