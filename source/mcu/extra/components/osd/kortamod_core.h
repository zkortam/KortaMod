#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KORTA_WIDTH 160
#define KORTA_HEIGHT 144
#define KORTA_NOTE_CAPACITY 200
#define KORTA_NOTE_SLOTS 16
#define KORTA_APP_COUNT 13
#define KORTA_PAGE_SIZE 9
#define KORTA_NET_MAX 8
#define KORTA_ALARM_MAX 4
#define KORTA_ALARM_LABEL 12
#define KORTA_DAYS_ALL 0x7F   /* bit 0 Monday .. bit 6 Sunday; 0 rings once */
#define KORTA_GAME_MAX 32
#define KORTA_GAME_NAME 12

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
    KORTA_CONTROLS,
    KORTA_NOTE_OPEN,
    KORTA_PHONE,
    KORTA_MESSAGE,          /* a message or "find me" sent from the companion site */
    KORTA_ARCADE,           /* an installed game is running (the port draws it) */
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
    KORTA_ACT_ALERT,
    /* value: the user's own level while a view overrides the backlight; 0 when it ends.
     * Lets the port undo an override that a power cut interrupted. */
    KORTA_ACT_BRIGHT_HOLD,
    KORTA_ACT_WEB,          /* value 1: serve the phone link while Wi-Fi is up; 0: stop */
    KORTA_ACT_WEB_FORGET,   /* revoke every paired browser */
    KORTA_ACT_GAME_START,   /* value: index into the installed games */
    KORTA_ACT_GAME_STOP
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
    KORTA_SFX_TYPE,
    KORTA_SFX_DICE,
    KORTA_SFX_COIN
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
    /* Palette preview, RGB565, lightest first: BG, OBJ0, OBJ1. game_colors: the game picks. */
    uint16_t pal[3][4];
    bool game_colors;
    bool gbc_game;
    uint8_t volume;
    bool cart_in;
    uint16_t heap_kb;
    char mcu[20];
    char fpga[12];
    char chromatic[8];
    char serial[12];
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
    /* Phone link, filled by the port. */
    bool web_ready;          /* server listening on the network */
    uint8_t web_paired;      /* browsers holding a token */
    char web_code[8];        /* pairing code shown on this screen only */
    char web_host[32];       /* mDNS name, e.g. chromatic-b7f8.local */
} KortaLink;

typedef struct {
    bool on;
    uint8_t hour, minute;
    uint8_t days;
    char label[KORTA_ALARM_LABEL + 1];
} KortaAlarm;

typedef struct {
    KortaView view;
    KortaView quick_from;
    KortaView find_from;
    KortaView find_dest;
    KortaView ring_from;
    uint8_t find_focus;
    bool find_dest_active;
    bool find_restore;
    bool search_direct;
    KortaView color_back;    /* the GB palette preview returns to Display or Home */
    bool restoring;          /* set_view is returning to a screen: restore its cursor */
    bool repeating;          /* on_tap is running for a held-button repeat */
    uint8_t back_focus[KORTA_VIEW_COUNT];
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
    bool web_keep;
    bool via_settings;
    int8_t zone;
    int16_t zone_minutes;
    uint8_t dice;
    uint8_t dice2;
    uint8_t dice_kind;
    uint8_t coin;
    uint16_t heads, tails;
    uint8_t roll_nonce;
    uint32_t rng;
    uint32_t dice_anim_until;
    uint32_t coin_anim_until;
    int16_t tally;
    uint8_t conv_kind;
    int16_t conv_n;
    uint8_t bright_keep;
    uint8_t flash_level;
    uint8_t flash_tint;
    uint8_t flash_mode;
    uint32_t flash_mark;
    uint32_t flash_hud_until;
    uint8_t vol_sys;
    uint8_t vol_media;
    uint8_t vol_ring;
    uint8_t vol_mode;
    uint8_t vol_sel;
    uint32_t vol_until;
    uint8_t sfx;
    char notes[KORTA_NOTE_SLOTS][KORTA_NOTE_CAPACITY + 1];
    uint8_t note_lens[KORTA_NOTE_SLOTS];
    uint8_t note_count;
    uint8_t note_pinned;  /* the first note_pinned notes are pinned, kept at the top */
    uint8_t note_cur;
    uint16_t note_rev;  /* changes on every edit; the phone link uses it to detect conflicts */
    char pending[32];

    bool time_known;
    bool clock_dirty;
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    /* Installed games, shown on Home after the built-in apps. */
    uint8_t game_count;
    char game_names[KORTA_GAME_MAX][KORTA_GAME_NAME + 1];
    uint8_t game_cur;        /* the game in KORTA_ARCADE */
    uint8_t arcade_phase;    /* KORTA_ARCADE_* */
    bool arcade_paused;
    uint8_t arcade_sel;      /* pause menu: 0 resume, 1 quit */
    char arcade_error[96];
    uint8_t update_phase;    /* firmware update from the site: KORTA_UPDATE_* */
    uint8_t update_percent;
    KortaAlarm alarms[KORTA_ALARM_MAX];
    uint8_t alarm_cur;       /* the alarm shown in the Alarm editor */
    uint8_t alarm_ringing;   /* which alarm the ring screen is for */
    uint16_t alarm_latch_min;/* minute of day alarm_latched applies to */
    bool alarm_latched;
    bool alarm_pending;      /* came due while the timer was ringing */
    char msg[KORTA_NOTE_CAPACITY + 1];
    bool msg_find;           /* flash the screen at full brightness */
    uint32_t msg_until;

    bool timer_count;
    bool timer_run;
    bool timer_done;
    bool timer_alert_pending;
    uint16_t timer_set;
    uint32_t timer_mark;
    uint32_t timer_deadline;
    uint32_t timer_acc;
    uint32_t timer_remaining;

    /* Fixed point, four decimal places. */
    int64_t calc_acc;
    int64_t calc_val;
    char calc_buf[16];
    uint8_t calc_op;
    bool calc_typing;
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
    bool was_down[8];
    bool long_fired[8];
    uint32_t edge_ms[8];
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
void korta_apply_prefs(KortaState *state, uint8_t flags, int8_t zone, uint8_t alarm_h, uint8_t alarm_m);
// Append one stored note in display order. Empty text and a full list are ignored.
void korta_load_note(KortaState *state, const char *text);
enum { KORTA_ARCADE_LOADING, KORTA_ARCADE_RUNNING, KORTA_ARCADE_ERROR };
enum { KORTA_UPDATE_NONE, KORTA_UPDATE_DOWNLOAD, KORTA_UPDATE_VERIFY, KORTA_UPDATE_RESTART, KORTA_UPDATE_FINISH };
/* While an update runs the screen shows its progress and buttons do nothing. */
void korta_set_update(KortaState *state, int phase, int percent);
/* The installed games' names, in Home order (each up to KORTA_GAME_NAME characters). */
void korta_set_games(KortaState *state, const char *const *names, int count);
/* Opens installed game `index` as if chosen on Home (console and tests). */
void korta_open_game(KortaState *state, int index);
/* The port reports a game finished loading, or failed (with a message). */
void korta_arcade_status(KortaState *state, int phase, const char *error);
/* After loading: the first n notes are pinned (clamped to the count). */
void korta_load_pins(KortaState *state, int n);

/* Requests from the phone link. Text is cleaned to what the screen can show. */
enum { KORTA_WEB_OK = 0, KORTA_WEB_CONFLICT, KORTA_WEB_BUSY, KORTA_WEB_INVALID };
// Replace the whole note list if it is still at revision `rev`. Busy while a note is being typed.
int korta_web_notes(KortaState *state, uint16_t rev, const char *const *texts, int count, int pinned);
/* One alarm: days is a KORTA_DAYS_* mask (0 rings once, then turns off); label may be NULL. */
int korta_web_alarm(KortaState *state, int index, bool on, int hour, int minute, int days, const char *label);
/* Settings a browser may change, with the same effect as the device's own rows. */
enum {
    KORTA_SET_BRIGHTNESS, KORTA_SET_NIGHT, KORTA_SET_FLOW, KORTA_SET_SILENT, KORTA_SET_BLEND,
    KORTA_SET_CC_LCD, KORTA_SET_CC_USB, KORTA_SET_TRANSIT, KORTA_SET_DPAD, KORTA_SET_LOWBATT,
    KORTA_SET_PLAYER, KORTA_SET_PALETTE, KORTA_SET_HOUR12, KORTA_SET_COUNT
};
int korta_web_setting(KortaState *state, int key, int value);
// Show text on the screen. find: flash it at full brightness to locate the device.
int korta_web_message(KortaState *state, const char *text, bool find);
enum { KORTA_TIMER_SET, KORTA_TIMER_START, KORTA_TIMER_STOP, KORTA_TIMER_RESET };
// action KORTA_TIMER_SET takes countdown (true) or stopwatch, and the length in seconds.
int korta_web_timer(KortaState *state, int action, bool countdown, int seconds);
// Join a network (password empty for an open one). The device remembers it only once connected.
int korta_web_wifi(KortaState *state, const char *ssid, const char *password);
// The same validation without acting, so the answer can be sent before Wi-Fi changes.
int korta_web_wifi_check(const char *ssid, const char *password);
// Copy text into dst (size >= 2) as printable ASCII: newlines and tabs become spaces,
// other bytes become '?', UTF-8 sequences count once. Returns the length written.
int korta_clean_text(char *dst, int size, const char *src);
// What the timer shows now: time left for a countdown, elapsed for a stopwatch.
uint32_t korta_timer_ms(const KortaState *state);
// Advance timers and alarm with no input. Safe while the overlay is hidden.
void korta_service(KortaState *state, uint32_t time_ms);
// The overlay was closed from outside (e.g. Menu). Ends Flash and any ringing alert,
// giving back the user's brightness.
void korta_hidden(KortaState *state);
// One input frame. held bit N is KortaButton N. Returns true when Home asks the stock menu to take over.
bool korta_frame(KortaState *state, uint16_t held, uint32_t time_ms);
// A tap, outside the double-press window. Host tests and any edge-only caller.
bool korta_press(KortaState *state, KortaButton button);
bool korta_take_action(KortaState *state, KortaAction *out);
void korta_render(const KortaState *state, uint16_t *pixels);
void korta_note_volume(KortaState *state, uint8_t percent, uint32_t time_ms);
void korta_sync_volume(KortaState *state, uint8_t percent);
uint8_t korta_take_sfx(KortaState *state);
