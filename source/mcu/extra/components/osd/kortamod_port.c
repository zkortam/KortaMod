#include "kortamod_port.h"
#include "kortamod_core.h"
#include "kortamod_link.h"
#include "kortamod_sfx.h"
#include "kortamod_web.h"

#include "battery.h"
#include "brightness.h"
#include "button.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"
#include "dpad_ctl.h"
#include "frameblend.h"
#include "fw.h"
#include "low_batt_icon_ctl.h"
#include "lvgl.h"
#include "menu_mgr.h"
#include "osd.h"
#include "palette.h"
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "style.h"
#include "serial_num.h"
#include "settings.h"
#include "silent.h"
#include "style.h"
#include "tab_shared.h"

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#if LV_COLOR_DEPTH != 16
#error "KortaMod requires LVGL 16-bit color"
#endif

_Static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "unexpected LVGL color size");

enum { CLOCK_EPOCH = 1704067200 };

static KortaState state;
static bool initialized;
static bool nvs_ok;
static bool prefs_dirty;
static bool settings_dirty;
static bool storage_failed;
static uint32_t storage_changed_at;
static bool had_visible;
static uint32_t wake_at_ms;  /* esp_timer ms of the next timer/alarm deadline, 0 = none */
static uint16_t last_note_rev;
/* All notes in one NVS blob: a format byte, then each note NUL-terminated, newest
 * first. NVS writes a blob as a new version before erasing the old one, so a power
 * cut during a save leaves either the old list or the new one, never a mix. */
/* Format 1: notes as NUL-terminated strings. Format 2: a pinned count, then the same. */
enum { NOTE_BLOB_FORMAT = 2 };
enum { ALARM_REC = 4 + KORTA_ALARM_LABEL };
static char note_blob[2 + KORTA_NOTE_SLOTS * (KORTA_NOTE_CAPACITY + 1)];
static bool command_ready;
static nvs_handle_t korta_nvs;
static volatile uint32_t vol_event;
static uint8_t last_vol = 255;
static bool vol_seen;
static lv_obj_t *image;
static uint32_t usb_clock_pending;
static int usb_zone_pending = 32767;
/* Console "korta web on|off": the REPL task may not touch shell state. */
static int web_console_pending;  /* 0 none, 1 on, 2 off, 3 dismiss a shown message */
/* A Wi-Fi change from the site waits until its answer has gone out, because
 * switching networks drops the very connection that carries the answer. */
static uint32_t remote_join_at;
static char remote_ssid[33];
static char remote_pass[64];
static char saved_ssid[33];
static char saved_pass[64];
/* A network is remembered only after it connects; a mistyped key is not kept. */
static bool join_pending;
static char pending_ssid[33];
static char pending_pass[64];
/* A notes read error other than "not found" must not let an empty list overwrite them. */
static bool notes_readonly;
static uint8_t close_phase;
static bool close_want_visible;
static uint32_t close_ms;

enum {
    CLOSE_IDLE = 0,
    CLOSE_WAIT = 1,
    CLOSE_DOWN = 2,
};

static const uint16_t CLOSE_PADS =
    kButtonBits_A | kButtonBits_B | kButtonBits_Up | kButtonBits_Down |
    kButtonBits_Left | kButtonBits_Right |
    (1u << kButton_Start) | (1u << kButton_Select);

static uint16_t map_held(uint16_t raw) {
    uint16_t held = 0;
    if (raw & (1u << kButton_Up)) held |= 1u << KORTA_UP;
    if (raw & (1u << kButton_Right)) held |= 1u << KORTA_RIGHT;
    if (raw & (1u << kButton_Down)) held |= 1u << KORTA_DOWN;
    if (raw & (1u << kButton_Left)) held |= 1u << KORTA_LEFT;
    if (raw & (1u << kButton_A)) held |= 1u << KORTA_A;
    if (raw & (1u << kButton_B)) held |= 1u << KORTA_B;
    if (raw & (1u << kButton_Start)) held |= 1u << KORTA_START;
    if (raw & (1u << kButton_Select)) held |= 1u << KORTA_SELECT;
    return held;
}

static void apply_zone_minutes(int minutes) {
    if (minutes < -720) minutes = -720;
    if (minutes > 840) minutes = 840;
    int posix = -minutes;
    char tz[16];
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", posix < 0 ? '-' : '+',
        abs(posix) / 60, abs(posix) % 60);
    setenv("TZ", tz, 1);
    tzset();
}

static uint8_t read_brightness(void) {
    uint32_t value = 0;
    if (Settings_Retrieve(kSettingKey_Brightness, &value) == kOSD_Result_Ok && value >= 1 && value <= 16) {
        return (uint8_t)value;
    }
    uint8_t level = (uint8_t)(Brightness_GetLevel() + 1u);
    if (level < 1 || level > 16) level = 7;
    return level;
}

static void load_prefs(void) {
    uint8_t flags = 2;
    int8_t zone = 0;
    uint8_t alarm_h = 7;
    uint8_t alarm_m = 0;
    if (nvs_open("korta", NVS_READWRITE, &korta_nvs) == ESP_OK) {
        nvs_ok = true;
        nvs_get_u8(korta_nvs, "flags", &flags);
        nvs_get_i8(korta_nvs, "zone", &zone);
        nvs_get_u8(korta_nvs, "al_h", &alarm_h);
        nvs_get_u8(korta_nvs, "al_m", &alarm_m);
        size_t ssid_len = sizeof saved_ssid;
        if (nvs_get_str(korta_nvs, "ssid", saved_ssid, &ssid_len) != ESP_OK) saved_ssid[0] = '\0';
        size_t pass_len = sizeof saved_pass;
        if (nvs_get_str(korta_nvs, "pass", saved_pass, &pass_len) != ESP_OK) saved_pass[0] = '\0';
        uint8_t system = 50, ring = 80;
        if (nvs_get_u8(korta_nvs, "vol_s", &system) == ESP_OK && system <= 100) state.vol_sys = system;
        if (nvs_get_u8(korta_nvs, "vol_r", &ring) == ESP_OK && ring <= 100) state.vol_ring = ring;
        uint8_t hold = 0;
        if (nvs_get_u8(korta_nvs, "br_hold", &hold) == ESP_OK) {
            /* Power was cut while Flash or an alert held the backlight. The stock
             * brightness module had already saved that level as the user's. */
            if (hold >= 1 && hold <= 16) {
                /* Write the setting itself too: Brightness_Update is ignored in
                 * low-power mode, which would leave the override saved. */
                Settings_Update(kSettingKey_Brightness, hold);
                Brightness_Update(hold);
                settings_dirty = true;
            }
            nvs_erase_key(korta_nvs, "br_hold");
        }
    }
    korta_apply_prefs(&state, flags, zone, alarm_h, alarm_m);
    if (nvs_ok) {
        /* Alarms after korta7: format byte, then per alarm on, hour, minute, days, label.
         * Before, the one alarm above (al_h, al_m, flag 8) rang daily; it stays alarm 1. */
        uint8_t blob[1 + KORTA_ALARM_MAX * ALARM_REC];
        size_t len = sizeof blob;
        if (nvs_get_blob(korta_nvs, "alarms", blob, &len) == ESP_OK && len == sizeof blob && blob[0] == 1) {
            for (int i = 0; i < KORTA_ALARM_MAX; i++) {
                const uint8_t *r = blob + 1 + i * ALARM_REC;
                KortaAlarm *a = &state.alarms[i];
                if (r[1] > 23 || r[2] > 59 || r[3] > KORTA_DAYS_ALL) continue;
                a->on = r[0] != 0;
                a->hour = r[1];
                a->minute = r[2];
                a->days = r[3];
                memcpy(a->label, r + 4, KORTA_ALARM_LABEL);
                a->label[KORTA_ALARM_LABEL] = '\0';
            }
        }
    }
    if (nvs_ok) {
        size_t len = sizeof note_blob;
        esp_err_t got = nvs_get_blob(korta_nvs, "notes", note_blob, &len);
        if (got == ESP_OK) {
            size_t head = len >= 2 && note_blob[0] == 2 ? 2 : len >= 1 && note_blob[0] == 1 ? 1 : 0;
            for (size_t at = head; head && at < len;) {
                size_t n = strnlen(note_blob + at, len - at);
                if (n == len - at) break;  /* unterminated tail */
                korta_load_note(&state, note_blob + at);
                at += n + 1;
            }
            if (head == 2) korta_load_pins(&state, (uint8_t)note_blob[1]);
        } else if (got != ESP_ERR_NVS_NOT_FOUND) {
            notes_readonly = true;
        } else {
            /* Before korta6 there was one note under "note". It is erased only
             * after the list blob has been written successfully. */
            char note[KORTA_NOTE_CAPACITY + 1];
            len = sizeof note;
            if (nvs_get_str(korta_nvs, "note", note, &len) == ESP_OK) {
                korta_load_note(&state, note);
                prefs_dirty = true;
            }
        }
    }
    int16_t zone_minutes = (int16_t)(state.zone * 60);
    if (nvs_ok && nvs_get_i16(korta_nvs, "zone_m", &zone_minutes) == ESP_OK
        && zone_minutes >= -720 && zone_minutes <= 840) {
        state.zone_minutes = zone_minutes;
        state.zone = (int8_t)(zone_minutes / 60);
    }
    apply_zone_minutes(state.zone_minutes);
    last_note_rev = state.note_rev;
}

static void write_prefs(void) {
    prefs_dirty = true;
    storage_changed_at = (uint32_t)(esp_timer_get_time() / 1000);
}

static void commit_storage(void) {
    if (prefs_dirty && nvs_ok) {
        uint8_t flags = (uint8_t)((state.night ? 1u : 0u) | (state.flow ? 2u : 0u)
            | (state.hour12 ? 4u : 0u) | (state.alarms[0].on ? 8u : 0u) | (state.wifi_keep ? 16u : 0u)
            | (state.web_keep ? 32u : 0u));
        esp_err_t err = ESP_OK;
        /* Retain the dirty state on any failed write so a later poll retries. */
#define SAVE(call) do { esp_err_t result = (call); if (result != ESP_OK) err = result; } while (0)
        SAVE(nvs_set_u8(korta_nvs, "flags", flags));
        SAVE(nvs_set_i8(korta_nvs, "zone", state.zone));
        SAVE(nvs_set_i16(korta_nvs, "zone_m", state.zone_minutes));
        SAVE(nvs_set_u8(korta_nvs, "al_h", state.alarms[0].hour));
        SAVE(nvs_set_u8(korta_nvs, "al_m", state.alarms[0].minute));
        uint8_t alarms[1 + KORTA_ALARM_MAX * ALARM_REC] = {1};
        for (int i = 0; i < KORTA_ALARM_MAX; i++) {
            uint8_t *r = alarms + 1 + i * ALARM_REC;
            const KortaAlarm *a = &state.alarms[i];
            r[0] = a->on; r[1] = a->hour; r[2] = a->minute; r[3] = a->days;
            memcpy(r + 4, a->label, KORTA_ALARM_LABEL);
        }
        SAVE(nvs_set_blob(korta_nvs, "alarms", alarms, sizeof alarms));
        size_t used = 0;
        note_blob[used++] = NOTE_BLOB_FORMAT;
        note_blob[used++] = (char)state.note_pinned;
        for (int i = 0; i < state.note_count && i < KORTA_NOTE_SLOTS; i++) {
            size_t n = state.note_lens[i];
            memcpy(note_blob + used, state.notes[i], n);
            used += n;
            note_blob[used++] = '\0';
        }
        esp_err_t notes = notes_readonly ? ESP_FAIL : nvs_set_blob(korta_nvs, "notes", note_blob, used);
        if (!notes_readonly) SAVE(notes);
        if (notes == ESP_OK) {
            esp_err_t legacy = nvs_erase_key(korta_nvs, "note");
            if (legacy != ESP_ERR_NVS_NOT_FOUND) SAVE(legacy);
        }
        SAVE(nvs_set_u8(korta_nvs, "vol_s", state.vol_sys));
        SAVE(nvs_set_u8(korta_nvs, "vol_r", state.vol_ring));
        SAVE(nvs_set_str(korta_nvs, "ssid", saved_ssid));
        SAVE(nvs_set_str(korta_nvs, "pass", saved_pass));
        if (err == ESP_OK) SAVE(nvs_commit(korta_nvs));
#undef SAVE
        storage_failed = err != ESP_OK;
        if (!storage_failed) prefs_dirty = false;
        else ESP_LOGE("Korta", "Save failed: %s", esp_err_to_name(err));
    }
    if (settings_dirty && Settings_Commit() == kOSD_Result_Ok) settings_dirty = false;
}

static void fill_status(KortaStatus *status) {
    memset(status, 0, sizeof *status);
    status->storage_ok = nvs_ok && !storage_failed;
    uint8_t percent = 0;
    uint16_t millivolts = 0;
    bool lipo = false;
    bool charging = false;
    status->battery_known = Battery_Read(&percent, &millivolts, &lipo, &charging);
    status->battery_pct = percent;
    status->battery_mv = millivolts;
    status->lipo = lipo;
    status->charging = charging;
    time_t now = time(NULL);
    if (now >= CLOCK_EPOCH) {
        struct tm local;
        localtime_r(&now, &local);
        status->time_known = true;
        status->year = (uint16_t)(local.tm_year + 1900);
        status->month = (uint8_t)(local.tm_mon + 1);
        status->day = (uint8_t)local.tm_mday;
        status->hour = (uint8_t)local.tm_hour;
        status->min = (uint8_t)local.tm_min;
        status->sec = (uint8_t)local.tm_sec;
    }
    status->brightness = read_brightness();
    status->silent = SilentMode_GetState() == kSilentModeState_On;
    status->blend = FrameBlend_GetState() == kFrameBlendState_On;
    status->cc_lcd = ColorCorrectLCD_GetState() == kColorCorrectLCDState_On;
    status->cc_usb = ColorCorrectUSB_GetState() == kColorCorrectUSBState_On;
    status->transit = ScreenTransitCtl_GetState() == kScreenTransitCtlState_On;
    status->dpad = (uint8_t)DPadCtl_GetState();
    status->low_batt = (uint8_t)LowBattIconCtl_GetState();
    status->player = PlayerNum_GetNum();
    if (status->player < 1 || status->player > 8) status->player = 1;
    unsigned palette = (unsigned)Style_GetCurrID();
    status->palette = palette < 15 ? (uint8_t)palette : 0;
    status->game_colors = status->palette == 0;
    status->gbc_game = Style_IsGBCMode();
    static const PaletteType_t parts[3] = {kPalette_Bg, kPalette_Obj0, kPalette_Obj1};
    for (int r = 0; r < 3 && !status->game_colors; r++) {
        uint64_t set = Pal_GetColor((StyleID_t)status->palette, parts[r]);
        for (int c = 0; c < 4; c++) {
            /* GBC color is 15-bit BGR555, lightest first in the low bits. */
            unsigned bgr = (unsigned)(set >> (16 * c)) & 0x7FFFu;
            unsigned r5 = bgr & 31u, g5 = (bgr >> 5) & 31u, b5 = (bgr >> 10) & 31u;
            status->pal[r][c] = (uint16_t)((r5 << 11) | (((g5 << 1) | (g5 >> 4)) << 5) | b5);
        }
    }
    status->heap_kb = (uint16_t)(esp_get_free_heap_size() / 1024u);
    const esp_app_desc_t *app = esp_app_get_description();
    if (app && app->version[0]) {
        size_t i = 0;
        while (i + 1 < sizeof status->mcu && app->version[i]) {
            status->mcu[i] = app->version[i];
            i++;
        }
        status->mcu[i] = '\0';
    }
    uint8_t major = 0;
    uint8_t minor = 0;
    Firmware_GetFPGA(&major, &minor);
    if (major || minor) snprintf(status->fpga, sizeof status->fpga, "%u.%u", major, minor);
    snprintf(status->chromatic, sizeof status->chromatic, "%s", CONFIG_CHROMATIC_FW_VER_STR);
    if (SerialNum_IsPresent()) snprintf(status->serial, sizeof status->serial, "%.10s", SerialNum_GetString());
    /* Stock FPGA never reports CART_DET. Opening the overlay is gated on a cart. */
    status->cart_in = true;
    status->volume = last_vol;
}

void Korta_OnVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    if (!vol_seen) {
        vol_seen = true;
        last_vol = percent;
        __atomic_store_n(&vol_event, 0x40000000u | (uint32_t)percent, __ATOMIC_RELEASE);
        return;
    }
    /* The wheel is a 7-bit reading that can flicker by one step at rest; a
     * one-step dead band keeps the bar from popping up untouched. The ends
     * always register so 0 and 100 are reachable exactly. */
    int step = percent > last_vol ? percent - last_vol : last_vol - percent;
    if (step == 0 || (step < 2 && percent != 0 && percent != 100)) return;
    last_vol = percent;
    __atomic_store_n(&vol_event, 0x80000000u | (uint32_t)percent, __ATOMIC_RELEASE);
}

/* The FPGA has one Menu toggle. Remember which way it should go, so a press by
 * the user during the wait cannot turn an "open" into a "close" or back. */
static void arm_menu_toggle(bool want_visible)
{
    if (close_phase == CLOSE_IDLE) {
        __atomic_store_n(&close_phase, CLOSE_WAIT, __ATOMIC_RELEASE);
        close_want_visible = want_visible;
        close_ms = 0;
    }
}

static void service_close_overlay(uint32_t now_ms)
{
    uint16_t pads = Button_GetHeld() & CLOSE_PADS;
    if (close_phase == CLOSE_WAIT) {
        if (pads) {
            close_ms = 0;
            return;
        }
        if (close_ms == 0) close_ms = now_ms ? now_ms : 1;
        if ((uint32_t)(now_ms - close_ms) < 40) return;
        if (OSD_IsVisible() == close_want_visible) {
            __atomic_store_n(&close_phase, CLOSE_IDLE, __ATOMIC_RELEASE);
            return;
        }
        Button_Poke(kButtonBits_MenuEnAlt);
        close_ms = now_ms ? now_ms : 1;
        __atomic_store_n(&close_phase, CLOSE_DOWN, __ATOMIC_RELEASE);
        return;
    }
    if (close_phase == CLOSE_DOWN) {
        if ((uint32_t)(now_ms - close_ms) < 50) return;
        Button_Poke(kButtonBits_None);
        __atomic_store_n(&close_phase, CLOSE_IDLE, __ATOMIC_RELEASE);
        close_ms = 0;
    }
}

bool Korta_KeepAwake(void)
{
    return __atomic_load_n(&close_phase, __ATOMIC_ACQUIRE) != CLOSE_IDLE;
}

/* The next moment a running timer or an armed alarm needs the MCU, so light
 * sleep wakes for it rather than every second. */
static uint32_t next_wake(uint32_t now_ms)
{
    uint32_t best = 0;
    if (state.timer_run && state.timer_count) {
        int32_t left = (int32_t)(state.timer_deadline - state.now_ms);
        best = now_ms + (left > 0 ? (uint32_t)left : 1u);
    }
    /* The next armed alarm time; on a day it doesn't ring the core lets it pass. */
    for (int i = 0; i < KORTA_ALARM_MAX && state.time_known; i++) {
        const KortaAlarm *a = &state.alarms[i];
        if (!a->on) continue;
        time_t t = time(NULL);
        struct tm local;
        localtime_r(&t, &local);
        int32_t now_s = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
        int32_t at_s = a->hour * 3600 + a->minute * 60;
        int32_t wait_s = (at_s - now_s + 86400) % 86400;
        uint32_t when = now_ms + (uint32_t)wait_s * 1000u + 250u;
        if (!best || (int32_t)(when - best) < 0) best = when;
    }
    return best ? best : 0;
}

uint32_t Korta_WakeInMs(void)
{
    uint32_t at = __atomic_load_n(&wake_at_ms, __ATOMIC_ACQUIRE);
    if (!at) return 0;
    int32_t left = (int32_t)(at - (uint32_t)(esp_timer_get_time() / 1000));
    return left > 0 ? (uint32_t)left : 1u;
}

static void apply_clock(void) {
    struct tm local = {0};
    local.tm_year = (int)state.year - 1900;
    local.tm_mon = (int)state.month - 1;
    local.tm_mday = state.day;
    local.tm_hour = state.hour;
    local.tm_min = state.min;
    local.tm_sec = state.sec;
    local.tm_isdst = 0;
    time_t stamp = mktime(&local);
    if (stamp == (time_t)-1) return;
    struct timeval tv = {.tv_sec = stamp};
    settimeofday(&tv, NULL);

}

static void remember_network(void) {
    snprintf(pending_ssid, sizeof pending_ssid, "%s", state.join_ssid);
    snprintf(pending_pass, sizeof pending_pass, "%s", state.password);
    join_pending = true;
}

static void settle_join(const KortaLink *link) {
    if (!join_pending || strcmp(link->ssid, pending_ssid) != 0) return;
    if (link->phase == KORTA_WIFI_ONLINE) {
        memcpy(saved_ssid, pending_ssid, sizeof saved_ssid);
        memcpy(saved_pass, pending_pass, sizeof saved_pass);
        write_prefs();
        join_pending = false;
    } else if (link->phase == KORTA_WIFI_DENIED || link->phase == KORTA_WIFI_FAIL || link->phase == KORTA_WIFI_OFF) {
        join_pending = false;
        /* A network that would not take us must not strand the device: go back
         * to the one that worked, so a remote change can always be undone. */
        if (link->phase != KORTA_WIFI_OFF && saved_ssid[0] && strcmp(saved_ssid, pending_ssid) != 0) {
            KortaLink_Command(KORTA_LINK_JOIN, saved_ssid, saved_pass);
        }
    }
    if (!join_pending) memset(pending_pass, 0, sizeof pending_pass);
}

static void apply_action(const KortaAction *action) {
    switch (action->kind) {
        case KORTA_ACT_BRIGHTNESS:
            Brightness_Update((uint8_t)action->value);
            settings_dirty = true;
            break;
        case KORTA_ACT_BRIGHT_HOLD:
            /* Written before the override is applied and erased after it ends. */
            if (nvs_ok) {
                if (action->value) nvs_set_u8(korta_nvs, "br_hold", (uint8_t)action->value);
                else nvs_erase_key(korta_nvs, "br_hold");
            }
            break;
        case KORTA_ACT_SILENT:
            SilentMode_Update(action->value ? kSilentModeState_On : kSilentModeState_Off);
            settings_dirty = true;
            break;
        case KORTA_ACT_BLEND:
            FrameBlend_Update(action->value ? kFrameBlendState_On : kFrameBlendState_Off);
            settings_dirty = true;
            break;
        case KORTA_ACT_CC_LCD:
            ColorCorrectLCD_Update(action->value ? kColorCorrectLCDState_On : kColorCorrectLCDState_Off);
            settings_dirty = true;
            break;
        case KORTA_ACT_CC_USB:
            ColorCorrectUSB_Update(action->value ? kColorCorrectUSBState_On : kColorCorrectUSBState_Off);
            settings_dirty = true;
            break;
        case KORTA_ACT_TRANSIT:
            ScreenTransitCtl_Update(action->value ? kScreenTransitCtlState_On : kScreenTransitCtlState_Off);
            settings_dirty = true;
            break;
        case KORTA_ACT_DPAD:
            DPadCtl_Update(action->value ? kDPadCtlState_RejectDiag : kDPadCtlState_AcceptDiag);
            settings_dirty = true;
            break;
        case KORTA_ACT_LOWBATT:
            if (action->value < 3) {
                LowBattIconCtl_Update((LowBattIconCtlState_t)action->value);
                settings_dirty = true;
            }
            break;
        case KORTA_ACT_PLAYER:
            if (action->value >= 1 && action->value <= 8) {
                PlayerNum_Update((uint8_t)action->value);
                settings_dirty = true;
            }
            break;
        case KORTA_ACT_PALETTE:
            if (action->value < kNumPalettes) {
                StyleID_t id = (StyleID_t)action->value;
                Style_Update(id);
                SettingValue_t saved = {.eType = kSettingDataType_U8, .U8 = (uint8_t)id};
                Style_ApplySetting(&saved);
                Settings_Update(kSettingKey_PaletteStyleID, id);
                settings_dirty = true;
            }
            break;
        case KORTA_ACT_CLOCK:
            apply_clock();
            break;
        case KORTA_ACT_ZONE:
            apply_zone_minutes(state.zone_minutes);
            write_prefs();
            break;
        case KORTA_ACT_PREFS:
            write_prefs();
            break;
        case KORTA_ACT_WIFI_ON:
            if (saved_ssid[0]) KortaLink_Command(KORTA_LINK_JOIN, saved_ssid, saved_pass);
            else KortaLink_Command(KORTA_LINK_ON, NULL, NULL);
            break;
        case KORTA_ACT_WIFI_OFF:
            KortaLink_Command(KORTA_LINK_OFF, NULL, NULL);
            break;
        case KORTA_ACT_WIFI_SCAN:
            KortaLink_Command(KORTA_LINK_SCAN, NULL, NULL);
            break;
        case KORTA_ACT_WIFI_JOIN:
            remember_network();
            KortaLink_Command(KORTA_LINK_JOIN, state.join_ssid, state.password);
            break;
        case KORTA_ACT_WEB:
            break;  /* web_keep is read every poll by service_phone_link */
        case KORTA_ACT_WEB_FORGET:
            KortaWeb_ForgetAll();
            break;
        case KORTA_ACT_WIFI_FORGET:
            saved_ssid[0] = '\0';
            saved_pass[0] = '\0';
            write_prefs();
            KortaLink_Command(KORTA_LINK_FORGET, NULL, NULL);
            break;
        case KORTA_ACT_LAUNCH:
            arm_menu_toggle(false);
            break;
        case KORTA_ACT_ALERT:
            MenuMgr_ShowTab(kTabID_Korta);
            if (!OSD_IsVisible()) arm_menu_toggle(true);
            break;
        default:
            break;
    }
}

static const char *phase_name(uint8_t phase) {
    switch (phase) {
        case KORTA_WIFI_IDLE: return "idle";
        case KORTA_WIFI_SCAN: return "scan";
        case KORTA_WIFI_JOIN: return "join";
        case KORTA_WIFI_ONLINE: return "online";
        case KORTA_WIFI_FAIL: return "fail";
        case KORTA_WIFI_DENIED: return "denied";
        default: return "off";
    }
}

/* Sets the clock from a laptop (USB console) or a phone. zone is minutes east
 * of UTC, or out of range to keep the current zone. UI task only. */
static void sync_clock(uint32_t stamp, int zone) {
    if (zone >= -720 && zone <= 840) {
        state.zone_minutes = (int16_t)zone;
        state.zone = (int8_t)(zone / 60);
        apply_zone_minutes(zone);
        write_prefs();
    }
    struct timeval tv = {.tv_sec = (time_t)stamp};
    settimeofday(&tv, NULL);
}

/* Runs the phone link's server, publishes what browsers may read, and applies
 * their requests here on the UI task, through the core. */
static void service_phone_link(KortaLink *link) {
    if (remote_join_at && (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - remote_join_at) >= 0) {
        remote_join_at = 0;
        korta_web_wifi(&state, remote_ssid, remote_pass);
        memset(remote_pass, 0, sizeof remote_pass);
    }
    int console = __atomic_exchange_n(&web_console_pending, 0, __ATOMIC_ACQ_REL);
    if (console == 3) {
        if (state.view == KORTA_MESSAGE) korta_hidden(&state);
    } else if (console) {
        state.web_keep = console == 1;
        write_prefs();
    }
    KortaWeb_Service(state.web_keep, link->phase == KORTA_WIFI_ONLINE, state.view == KORTA_PHONE);
    KortaWeb_Status(link);
    if (!link->web_ready) return;
    /* Apply a request first and publish afterwards, so the reply a browser gets
     * is never followed by a read of the older state. */
    static KortaWebCmd cmd;
    bool took = KortaWeb_TakeCommand(&cmd);
    int result = KORTA_WEB_INVALID;
    if (took && cmd.kind == KORTA_WEB_CMD_NOTES) {
        const char *texts[KORTA_NOTE_SLOTS];
        for (int i = 0; i < KORTA_NOTE_SLOTS; i++) texts[i] = cmd.notes[i];
        result = korta_web_notes(&state, cmd.rev, texts, cmd.count, cmd.pinned);
    } else if (took && cmd.kind == KORTA_WEB_CMD_ALARM) {
        result = korta_web_alarm(&state, cmd.alarm, cmd.alarm_on, cmd.hour, cmd.minute, cmd.days, cmd.text);
    } else if (took && cmd.kind == KORTA_WEB_CMD_SETTING) {
        result = korta_web_setting(&state, cmd.setting, cmd.value);
    } else if (took && cmd.kind == KORTA_WEB_CMD_MESSAGE) {
        result = korta_web_message(&state, cmd.text, cmd.find);
    } else if (took && cmd.kind == KORTA_WEB_CMD_TIMER) {
        result = korta_web_timer(&state, cmd.timer_action, cmd.countdown, cmd.seconds);
    } else if (took && cmd.kind == KORTA_WEB_CMD_WIFI) {
        result = korta_web_wifi_check(cmd.ssid, cmd.password);
        if (result == KORTA_WEB_OK) {
            memcpy(remote_ssid, cmd.ssid, sizeof remote_ssid);
            memcpy(remote_pass, cmd.password, sizeof remote_pass);
            remote_join_at = (uint32_t)(esp_timer_get_time() / 1000) + 2000u;
            if (!remote_join_at) remote_join_at = 1;
        }
        memset(cmd.password, 0, sizeof cmd.password);
    } else if (took && cmd.kind == KORTA_WEB_CMD_CLOCK) {
        sync_clock(cmd.epoch, cmd.zone_minutes);
        KortaStatus status;
        fill_status(&status);
        korta_set_status(&state, &status);
        result = KORTA_WEB_OK;
    }
    static KortaWebView view;
    view.rev = state.note_rev;
    view.count = state.note_count;
    view.pinned = state.note_pinned;
    memcpy(view.notes, state.notes, sizeof view.notes);
    view.editing = state.view == KORTA_KEYBOARD && !state.typing_pass && !state.typing_find;
    memcpy(view.alarms, state.alarms, sizeof view.alarms);
    view.alert = state.view == KORTA_ALARM_RING ? 1 : state.view == KORTA_TIMER_RING ? 2 : 0;
    memcpy(view.alert_label, state.alarms[state.alarm_ringing % KORTA_ALARM_MAX].label, sizeof view.alert_label);
    view.hour12 = state.hour12;
    view.time_known = state.time_known;
    view.zone_minutes = state.zone_minutes;
    view.battery_known = state.hw.battery_known;
    view.battery_pct = state.hw.battery_pct;
    view.charging = state.hw.charging;
    memcpy(view.mcu, state.hw.mcu, sizeof view.mcu);
    memcpy(view.fpga, state.hw.fpga, sizeof view.fpga);
    memcpy(view.chromatic, state.hw.chromatic, sizeof view.chromatic);
    view.settings.brightness = state.view == KORTA_FLASH || state.view == KORTA_ALARM_RING || state.view == KORTA_TIMER_RING
        || (state.view == KORTA_MESSAGE && state.msg_find) ? state.bright_keep : state.hw.brightness;
    view.settings.night = state.night;
    view.settings.flow = state.flow;
    view.settings.hour12 = state.hour12;
    view.settings.silent = state.hw.silent;
    view.settings.blend = state.hw.blend;
    view.settings.cc_lcd = state.hw.cc_lcd;
    view.settings.cc_usb = state.hw.cc_usb;
    view.settings.transit = state.hw.transit;
    view.settings.dpad4 = state.hw.dpad != 0;
    view.settings.low_batt = state.hw.low_batt;
    view.settings.player = state.hw.player;
    view.settings.palette = state.hw.palette;
    view.settings.gbc = state.hw.gbc_game;
    view.timer.countdown = state.timer_count;
    view.timer.running = state.timer_run;
    view.timer.done = state.timer_done;
    view.timer.set_s = state.timer_set;
    view.timer.shown_ms = korta_timer_ms(&state);
    view.wifi.phase = state.link.phase;
    memcpy(view.wifi.ssid, state.link.ssid, sizeof view.wifi.ssid);
    view.wifi.count = state.link.count;
    memcpy(view.wifi.nets, state.link.nets, sizeof view.wifi.nets);
    KortaWeb_Publish(&view);
    if (took) KortaWeb_Complete(result);
}

static int korta_command(int argc, char **argv) {
    if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "web") == 0) {
        if (argc >= 3 && argv[2] && strcmp(argv[2], "on") == 0) __atomic_store_n(&web_console_pending, 1, __ATOMIC_RELEASE);
        else if (argc >= 3 && argv[2] && strcmp(argv[2], "off") == 0) __atomic_store_n(&web_console_pending, 2, __ATOMIC_RELEASE);
        else if (argc >= 3 && argv[2] && strcmp(argv[2], "forget") == 0) KortaWeb_ForgetAll();
        else if (argc >= 3 && argv[2] && strcmp(argv[2], "dismiss") == 0) __atomic_store_n(&web_console_pending, 3, __ATOMIC_RELEASE);
        KortaLink link;
        memset(&link, 0, sizeof link);
        KortaWeb_Status(&link);
        printf("korta web enabled %d ready %d paired %u code %s host %s last %d\r\n", state.web_keep, link.web_ready,
            link.web_paired, link.web_code[0] ? link.web_code : "-", link.web_host[0] ? link.web_host : "-",
            KortaWeb_LastStatus());
        return 0;
    }
    if (argc >= 3 && argv && argv[1] && argv[2] && strcmp(argv[1], "time") == 0) {
        char *end = NULL;
        unsigned long stamp = strtoul(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0' && stamp >= CLOCK_EPOCH && stamp <= 4000000000ul) {
            if (argc >= 4 && argv[3]) {
                char *zone_end = NULL;
                long zone = strtol(argv[3], &zone_end, 10);
                if (zone_end == argv[3] || *zone_end != '\0' || zone < -720 || zone > 840) return 1;
                __atomic_store_n(&usb_zone_pending, (int)zone, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&usb_clock_pending, (uint32_t)stamp, __ATOMIC_RELEASE);
        } else return 1;
    }
    uint8_t percent = 0;
    uint16_t millivolts = 0;
    bool lipo = false;
    bool charging = false;
    bool known = Battery_Read(&percent, &millivolts, &lipo, &charging);
    KortaLink link;
    KortaLink_Snapshot(&link);
    printf("korta bat %s %u%% %umV %s %s\r\n", known ? "known" : "unknown", percent, millivolts,
        lipo ? "lipo" : "aa", charging ? "charging" : "battery");
    time_t now = time(NULL);
    printf("korta time %lu known %d zone_minutes %+d\r\n", (unsigned long)now, now >= CLOCK_EPOCH, state.zone_minutes);
    printf("korta wifi %s rssi %d ssid %s ip %u.%u.%u.%u\r\n", phase_name(link.phase), link.rssi, link.ssid,
        (unsigned)(link.ip & 255u), (unsigned)((link.ip >> 8) & 255u),
        (unsigned)((link.ip >> 16) & 255u), (unsigned)((link.ip >> 24) & 255u));
    printf("korta heap %u internal %u\r\n", (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_free_internal_heap_size());
    return 0;
}

static void register_command(void) {
    if (command_ready) return;
    esp_console_cmd_t command = {
        .command = "korta",
        .help = "Status. korta time <unix> [UTC offset minutes] syncs the laptop clock; korta web [on|off|forget|dismiss] shows or sets the phone link",
        .func = &korta_command,
    };
    command_ready = esp_console_cmd_register(&command) == ESP_OK;
}

void Korta_Poll(void) {
    if (!initialized) {
        korta_init(&state);
        load_prefs();
        KortaWeb_Init(nvs_ok ? korta_nvs : 0);
        KortaSfx_Init();
        KortaLink_Start();
        register_command();
        initialized = true;
        if (state.wifi_keep) {
            if (saved_ssid[0]) KortaLink_Command(KORTA_LINK_JOIN, saved_ssid, saved_pass);
            else KortaLink_Command(KORTA_LINK_ON, NULL, NULL);
        }
    }
    uint32_t stamp = __atomic_exchange_n(&usb_clock_pending, 0, __ATOMIC_ACQ_REL);
    if (stamp >= CLOCK_EPOCH) sync_clock(stamp, __atomic_exchange_n(&usb_zone_pending, 32767, __ATOMIC_ACQ_REL));
    KortaStatus status;
    fill_status(&status);
    korta_set_status(&state, &status);
    KortaLink link;
    KortaLink_Snapshot(&link);
    settle_join(&link);
    link.saved = saved_ssid[0] != '\0';
    if (!link.ssid[0] && saved_ssid[0]) snprintf(link.ssid, sizeof link.ssid, "%s", saved_ssid);
    service_phone_link(&link);
    korta_set_link(&state, &link);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    service_close_overlay(now_ms);
    uint32_t ev = __atomic_exchange_n(&vol_event, 0, __ATOMIC_ACQ_REL);
    if (ev & 0x40000000u) korta_sync_volume(&state, (uint8_t)ev);
    else if (ev & 0x80000000u) korta_note_volume(&state, (uint8_t)ev, now_ms);
    if (!OSD_IsVisible() || MenuMgr_CurrentTab() != kTabID_Korta) {
        if (had_visible) korta_hidden(&state);
        korta_service(&state, now_ms);
        KortaAction idle;
        while (korta_take_action(&state, &idle)) apply_action(&idle);
        uint8_t idle_sfx = korta_take_sfx(&state);
        if (idle_sfx) {
            uint8_t vol = state.hw.silent ? 0 : (idle_sfx == KORTA_SFX_ALERT ? state.vol_ring : state.vol_sys);
            KortaSfx_Play(idle_sfx, vol);
        }
        if (last_note_rev != state.note_rev) {
            last_note_rev = state.note_rev;
            write_prefs();
        }
        __atomic_store_n(&wake_at_ms, next_wake(now_ms), __ATOMIC_RELEASE);
        if (had_visible || (uint32_t)(now_ms - storage_changed_at) >= 1000u) {
            commit_storage();
            storage_changed_at = now_ms;
        }
        had_visible = false;
        state.primed = false;
        return;
    }
    had_visible = true;
    uint16_t held = map_held(Button_GetHeld());
    bool leave = korta_frame(&state, held, now_ms);
    KortaAction action;
    while (korta_take_action(&state, &action)) apply_action(&action);
    uint8_t sfx = korta_take_sfx(&state);
    if (sfx) {
        uint8_t vol = state.hw.silent ? 0 : (sfx == KORTA_SFX_ALERT ? state.vol_ring : state.vol_sys);
        KortaSfx_Play(sfx, vol);
    }
    if (last_note_rev != state.note_rev) {
        last_note_rev = state.note_rev;
        write_prefs();
    }
    __atomic_store_n(&wake_at_ms, next_wake(now_ms), __ATOMIC_RELEASE);
    if (!held && (uint32_t)(now_ms - storage_changed_at) >= 1000u) {
        commit_storage();
        storage_changed_at = now_ms;
    }
    if (leave) {
        for (int i = 0; i < kNumButtons; i++) (void)Button_GetState((Button_t)i);
        commit_storage();
        arm_menu_toggle(false);
    }
}

bool Korta_Paint(uint16_t *dst) {
    if (!initialized || dst == NULL) return false;
    if (!OSD_IsVisible() || MenuMgr_CurrentTab() != kTabID_Korta) return false;
    korta_render(&state, dst);
    return true;
}

OSD_Result_t Korta_Draw(void *arg) {
    Tab_DrawCtx_t *context = (Tab_DrawCtx_t *)arg;
    if (context == NULL || context->pScreen == NULL) return kOSD_Result_Err_NullDataPtr;
    if (!initialized) return kOSD_Result_Ok;
    /* A 1x1 poke is enough to schedule a flush. The frame is painted into the
     * DMA buffer there, so the shell does not keep a second 46 KB framebuffer. */
    if (image == NULL) {
        image = lv_obj_create((lv_obj_t *)context->pScreen);
        lv_obj_set_size(image, 1, 1);
        lv_obj_align(image, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(image, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(image, 0, 0);
        lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_invalidate(image);
    return kOSD_Result_Ok;
}

OSD_Result_t Korta_OnButton(Button_t button, ButtonState_t button_state, void *arg) {
    (void)button;
    (void)button_state;
    (void)arg;
    return kOSD_Result_Ok;
}

OSD_Result_t Korta_OnTransition(void *arg) {
    (void)arg;
    if (image != NULL) {
        lv_obj_del(image);
        image = NULL;
    }
    return kOSD_Result_Ok;
}


