#include "kortamod_port.h"
#include "kortamod_core.h"
#include "kortamod_link.h"
#include "kortamod_sfx.h"

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
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "settings.h"
#include "silent.h"
#include "style.h"
#include "tab_shared.h"

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
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
static bool wake_deadline;
static char last_note[KORTA_NOTE_CAPACITY + 1];
static bool command_ready;
static nvs_handle_t korta_nvs;
static volatile uint32_t vol_event;
static uint8_t last_vol = 255;
static bool vol_seen;
static lv_obj_t *image;
static uint32_t usb_clock_pending;
static char saved_ssid[33];
static char saved_pass[64];
static uint8_t close_phase;
static uint32_t close_ms;

/* Implemented by the MCU transport in main/fpga_tx.c. */
extern void FPGA_Tx_SetMenuMute(bool muted);

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

static void apply_zone(int zone) {
    if (zone < -12) zone = -12;
    if (zone > 14) zone = 14;
    char tz[8];
    snprintf(tz, sizeof tz, "UTC%d", -zone);
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
    char note[KORTA_NOTE_CAPACITY + 1];
    memset(note, 0, sizeof note);
    if (nvs_open("korta", NVS_READWRITE, &korta_nvs) == ESP_OK) {
        nvs_ok = true;
        nvs_get_u8(korta_nvs, "flags", &flags);
        nvs_get_i8(korta_nvs, "zone", &zone);
        nvs_get_u8(korta_nvs, "al_h", &alarm_h);
        nvs_get_u8(korta_nvs, "al_m", &alarm_m);
        size_t note_len = sizeof note;
        if (nvs_get_str(korta_nvs, "note", note, &note_len) != ESP_OK) note[0] = '\0';
        size_t ssid_len = sizeof saved_ssid;
        if (nvs_get_str(korta_nvs, "ssid", saved_ssid, &ssid_len) != ESP_OK) saved_ssid[0] = '\0';
        size_t pass_len = sizeof saved_pass;
        if (nvs_get_str(korta_nvs, "pass", saved_pass, &pass_len) != ESP_OK) saved_pass[0] = '\0';
        uint8_t system = 50, ring = 80;
        if (nvs_get_u8(korta_nvs, "vol_s", &system) == ESP_OK && system <= 100) state.vol_sys = system;
        if (nvs_get_u8(korta_nvs, "vol_r", &ring) == ESP_OK && ring <= 100) state.vol_ring = ring;
    }
    korta_apply_prefs(&state, flags, zone, alarm_h, alarm_m, note);
    apply_zone(state.zone);
    memcpy(last_note, state.note, sizeof last_note);
}

static void write_prefs(void) {
    prefs_dirty = true;
    storage_changed_at = (uint32_t)(esp_timer_get_time() / 1000);
}

static void commit_storage(void) {
    if (prefs_dirty && nvs_ok) {
        uint8_t flags = (uint8_t)((state.night ? 1u : 0u) | (state.flow ? 2u : 0u)
            | (state.hour12 ? 4u : 0u) | (state.alarm_on ? 8u : 0u) | (state.wifi_keep ? 16u : 0u));
        esp_err_t err = ESP_OK;
        /* Retain the dirty state on any failed write so a later poll retries. */
#define SAVE(call) do { esp_err_t result = (call); if (result != ESP_OK) err = result; } while (0)
        SAVE(nvs_set_u8(korta_nvs, "flags", flags));
        SAVE(nvs_set_i8(korta_nvs, "zone", state.zone));
        SAVE(nvs_set_u8(korta_nvs, "al_h", state.alarm_h));
        SAVE(nvs_set_u8(korta_nvs, "al_m", state.alarm_m));
        SAVE(nvs_set_str(korta_nvs, "note", state.note));
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
    if (percent == last_vol) return;
    last_vol = percent;
    __atomic_store_n(&vol_event, 0x80000000u | (uint32_t)percent, __ATOMIC_RELEASE);
}

static void arm_close_overlay(void)
{
    if (close_phase == CLOSE_IDLE) {
        close_phase = CLOSE_WAIT;
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
        Button_Poke(kButtonBits_MenuEnAlt);
        close_ms = now_ms ? now_ms : 1;
        close_phase = CLOSE_DOWN;
        return;
    }
    if (close_phase == CLOSE_DOWN) {
        if ((uint32_t)(now_ms - close_ms) < 50) return;
        Button_Poke(kButtonBits_None);
        close_phase = CLOSE_IDLE;
        close_ms = 0;
    }
}

static void poke_menu_pulse(void)
{
    arm_close_overlay();
}

bool Korta_IsPlaying(void)
{
    return close_phase == CLOSE_IDLE && !OSD_IsVisible();
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
    snprintf(saved_ssid, sizeof saved_ssid, "%s", state.join_ssid);
    snprintf(saved_pass, sizeof saved_pass, "%s", state.password);
    write_prefs();
}

static void apply_action(const KortaAction *action) {
    switch (action->kind) {
        case KORTA_ACT_BRIGHTNESS:
            Brightness_Update((uint8_t)action->value);
            settings_dirty = true;
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
            apply_zone((int)action->value - 12);
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
        case KORTA_ACT_WIFI_FORGET:
            saved_ssid[0] = '\0';
            saved_pass[0] = '\0';
            write_prefs();
            KortaLink_Command(KORTA_LINK_FORGET, NULL, NULL);
            break;
        case KORTA_ACT_LAUNCH:
            poke_menu_pulse();
            break;
        case KORTA_ACT_ALERT:
            MenuMgr_ShowTab(kTabID_Korta);
            if (!OSD_IsVisible()) poke_menu_pulse();
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

static int korta_command(int argc, char **argv) {
    if (argc >= 3 && argv && argv[1] && argv[2] && strcmp(argv[1], "time") == 0) {
        char *end = NULL;
        unsigned long stamp = strtoul(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0' && stamp >= CLOCK_EPOCH && stamp <= 4000000000ul) {
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
    printf("korta time %lu known %d\r\n", (unsigned long)now, now >= CLOCK_EPOCH);
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
        .help = "Status. korta time <unix> sets the clock",
        .func = &korta_command,
    };
    command_ready = esp_console_cmd_register(&command) == ESP_OK;
}

void Korta_Poll(void) {
    if (!initialized) {
        korta_init(&state);
        load_prefs();
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
    if (stamp >= CLOCK_EPOCH) {
        struct timeval tv = {.tv_sec = (time_t)stamp};
        settimeofday(&tv, NULL);
    }
    KortaStatus status;
    fill_status(&status);
    korta_set_status(&state, &status);
    KortaLink link;
    KortaLink_Snapshot(&link);
    link.saved = saved_ssid[0] != '\0';
    if (!link.ssid[0] && saved_ssid[0]) snprintf(link.ssid, sizeof link.ssid, "%s", saved_ssid);
    korta_set_link(&state, &link);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    service_close_overlay(now_ms);
    FPGA_Tx_SetMenuMute(OSD_IsVisible() && MenuMgr_CurrentTab() == kTabID_Korta);
    uint32_t ev = __atomic_exchange_n(&vol_event, 0, __ATOMIC_ACQ_REL);
    if (ev & 0x40000000u) korta_sync_volume(&state, (uint8_t)ev);
    else if (ev & 0x80000000u) korta_note_volume(&state, (uint8_t)ev, now_ms);
    if (!OSD_IsVisible() || MenuMgr_CurrentTab() != kTabID_Korta) {
        korta_service(&state, now_ms);
        KortaAction idle;
        while (korta_take_action(&state, &idle)) apply_action(&idle);
        uint8_t idle_sfx = korta_take_sfx(&state);
        if (idle_sfx) {
            uint8_t vol = state.hw.silent ? 0 : (idle_sfx == KORTA_SFX_ALERT ? state.vol_ring : state.vol_sys);
            KortaSfx_Play(idle_sfx, vol);
        }
        if (memcmp(last_note, state.note, sizeof last_note) != 0) {
            memcpy(last_note, state.note, sizeof last_note);
            write_prefs();
        }
        __atomic_store_n(&wake_deadline, state.alarm_on || state.timer_run, __ATOMIC_RELEASE);
        if (had_visible || (uint32_t)(now_ms - storage_changed_at) >= 1000u) {
            commit_storage();
            storage_changed_at = now_ms;
        }
        had_visible = false;
        state.primed = false;
        state.edge_seen = 0;
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
    if (memcmp(last_note, state.note, sizeof last_note) != 0) {
        memcpy(last_note, state.note, sizeof last_note);
        write_prefs();
    }
    __atomic_store_n(&wake_deadline, state.alarm_on || state.timer_run, __ATOMIC_RELEASE);
    if (!held && (uint32_t)(now_ms - storage_changed_at) >= 1000u) {
        commit_storage();
        storage_changed_at = now_ms;
    }
    if (leave) {
        for (int i = 0; i < kNumButtons; i++) (void)Button_GetState((Button_t)i);
        commit_storage();
        arm_close_overlay();
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

bool Korta_HasWakeDeadline(void) {
    return __atomic_load_n(&wake_deadline, __ATOMIC_ACQUIRE);
}
