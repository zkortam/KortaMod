#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KORTA_WIDTH 160
#define KORTA_HEIGHT 144
#define KORTA_NOTE_CAPACITY 120
#define KORTA_APP_COUNT 13
#define KORTA_PAGE_SIZE 9
#define KORTA_NET_MAX 8

typedef enum {
    KORTA_UP, KORTA_RIGHT, KORTA_DOWN, KORTA_LEFT,
    KORTA_A, KORTA_B, KORTA_START, KORTA_SELECT
} KortaButton;

typedef enum {
    KORTA_HOME = 0,
    KORTA_NOTES,
    KORTA_KEYBOARD,
    KORTA_CLOCK,
    KORTA_ALARM,
    KORTA_TIMER,
    KORTA_CALC,
    KORTA_FIND,
    KORTA_FILES,
    KORTA_SETUP,
    KORTA_POWER,
    KORTA_PLAY,
    KORTA_LINKS,
    KORTA_INFO,
    KORTA_QUICK,
    KORTA_CLEAR_CONFIRM,
    KORTA_ALARM_RING,
    KORTA_TIMER_RING,
    KORTA_TIME,
    KORTA_SETTINGS,
    KORTA_DICE,
    KORTA_COIN,
    KORTA_COUNT,
    KORTA_CONV,
    KORTA_FLASH,
    KORTA_COLOR,
    KORTA_SOUND,
    KORTA_PLAYER,
    KORTA_CONTROLS,
    KORTA_VIEW_COUNT
} KortaView;

enum {
    KORTA_WIFI_OFF = 0,
    KORTA_WIFI_IDLE,
    KORTA_WIFI_SCAN,
    KORTA_WIFI_JOIN,
    KORTA_WIFI_ONLINE,
    KORTA_WIFI_FAIL,
    KORTA_WIFI_DENIED
};

typedef enum {
    KORTA_ACT_NONE = 0,
    KORTA_ACT_BRIGHTNESS,
    KORTA_ACT_SILENT,
    KORTA_ACT_BLEND,
    KORTA_ACT_CC_LCD,
    KORTA_ACT_CC_USB,
    KORTA_ACT_TRANSIT,
    KORTA_ACT_DPAD,
    KORTA_ACT_LOWBATT,
    KORTA_ACT_PLAYER,
    KORTA_ACT_PALETTE,
    KORTA_ACT_CLOCK,
    KORTA_ACT_ZONE,
    KORTA_ACT_PREFS,
    KORTA_ACT_WIFI_ON,
    KORTA_ACT_WIFI_OFF,
    KORTA_ACT_WIFI_SCAN,
    KORTA_ACT_WIFI_JOIN,
    KORTA_ACT_WIFI_FORGET,
    KORTA_ACT_LAUNCH,
    KORTA_ACT_ALERT
} KortaActionKind;

enum {
    KORTA_SFX_NONE = 0,
    KORTA_SFX_MOVE,
    KORTA_SFX_OK,
    KORTA_SFX_BACK,
    KORTA_SFX_OPEN,
    KORTA_SFX_TOGGLE,
    KORTA_SFX_DENY,
    KORTA_SFX_ALERT,
    KORTA_SFX_TICK,
    KORTA_SFX_TYPE
};

typedef struct {
    KortaActionKind kind;
    uint16_t value;
} KortaAction;

typedef struct {
    bool storage_ok;
    bool battery_known;
    uint8_t battery_pct;
    bool charging;
    bool lipo;
    uint16_t battery_mv;
    bool time_known;
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    uint8_t brightness;
    bool silent, blend, cc_lcd, cc_usb, transit;
    uint8_t dpad;
    uint8_t low_batt;
    uint8_t player;
    uint8_t palette;
    uint8_t volume;
    bool cart_in;
    uint16_t heap_kb;
    char mcu[20];
    char fpga[12];
} KortaStatus;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool locked;
} KortaNet;

typedef struct {
    uint8_t phase;
    int8_t rssi;
    bool saved;
    char ssid[33];
    uint8_t count;
    KortaNet nets[KORTA_NET_MAX];
    uint32_t ip;
} KortaLink;

typedef struct {
    KortaView view;
    KortaView quick_from;
    KortaView find_from;
    KortaView find_dest;
    KortaView ring_from;
    uint8_t find_focus;
    bool find_dest_active;
    bool find_restore;
    bool notes_from_files;
    uint8_t selected;
    uint8_t focus;
    uint8_t key_row;
    uint8_t key_col;
    uint8_t candidate;
    uint8_t pending_len;
    bool night;
    bool flow;
    bool caps;
    bool symbols;
    bool hour12;
    bool wifi_keep;
    bool via_settings;
    int8_t zone;
    int16_t zone_minutes;
    uint8_t dice;
    uint8_t coin;
    uint8_t roll_nonce;
    uint32_t dice_anim_until;
    uint32_t coin_anim_until;
    int16_t tally;
    uint8_t conv_kind;
    int16_t conv_n;
    uint8_t flash_saved;
    uint8_t vol_sys;
    uint8_t vol_media;
    uint8_t vol_ring;
    uint8_t vol_mode;
    uint8_t vol_sel;
    uint32_t vol_until;
    uint8_t sfx;
    char note[KORTA_NOTE_CAPACITY + 1];
    uint8_t note_len;
    char pending[32];

    bool time_known;
    bool clock_dirty;
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    uint8_t alarm_h, alarm_m;
    bool alarm_on;
    bool alarm_latched;

    bool timer_count;
    bool timer_run;
    bool timer_done;
    bool timer_alert_pending;
    uint16_t timer_set;
    uint32_t timer_mark;
    uint32_t timer_deadline;
    uint32_t timer_acc;
    uint32_t timer_remaining;

    int32_t calc_acc;
    int32_t calc_entry;
    uint8_t calc_op;
    bool calc_fresh;
    bool calc_err;

    char query[12];
    uint8_t query_len;
    uint8_t letter;
    bool typing_pass;
    bool typing_find;
    char join_ssid[33];
    char password[64];
    uint8_t pass_len;

    KortaStatus hw;
    KortaLink link;

    uint16_t held;
    uint16_t blocked_held;
    uint32_t now_ms;
    bool primed;
    uint8_t edge_seen;
    bool was_down[8];
    bool was_hold[8];
    bool long_fired[8];
    uint32_t edge_ms[8];
    uint32_t last_edge_ms[8];
    uint32_t next_repeat[8];
    uint16_t repeat_gap[8];
    bool held_home;
    bool b_from_home;

    KortaAction queue[8];
    uint8_t qh, qt;
} KortaState;

void korta_init(KortaState *state);
void korta_set_status(KortaState *state, const KortaStatus *status);
void korta_set_link(KortaState *state, const KortaLink *link);
void korta_apply_prefs(KortaState *state, uint8_t flags, int8_t zone, uint8_t alarm_h, uint8_t alarm_m, const char *note);
// Advance timers and alarm with no input. Safe while the overlay is hidden.
void korta_service(KortaState *state, uint32_t time_ms);
// One input frame. held bit N is KortaButton N. Returns true when Home asks the stock menu to take over.
bool korta_frame(KortaState *state, uint16_t held, uint32_t time_ms);
// A tap, outside the double-press window. Host tests and any edge-only caller.
bool korta_press(KortaState *state, KortaButton button);
bool korta_take_action(KortaState *state, KortaAction *out);
void korta_render(const KortaState *state, uint16_t *pixels);
void korta_note_volume(KortaState *state, uint8_t percent, uint32_t time_ms);
void korta_sync_volume(KortaState *state, uint8_t percent);
uint8_t korta_take_sfx(KortaState *state);
