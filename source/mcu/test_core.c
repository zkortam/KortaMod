#include "kortamod_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int cond, const char *file, int line, const char *expr) {
    if (!cond) {
        fprintf(stderr, "FAIL %s:%d %s\n", file, line, expr);
        failures++;
    }
}
#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static void press(KortaState *s, KortaButton button) { korta_press(s, button); }

static void open_app(KortaState *s, int index) {
    s->selected = (uint8_t)index;
    press(s, KORTA_A);
}

static void render_guarded(const KortaState *s) {
    static uint16_t guarded[KORTA_WIDTH * KORTA_HEIGHT + 2];
    guarded[0] = 0xA55A;
    guarded[KORTA_WIDTH * KORTA_HEIGHT + 1] = 0x5AA5;
    korta_render(s, guarded + 1);
    CHECK(guarded[0] == 0xA55A);
    CHECK(guarded[KORTA_WIDTH * KORTA_HEIGHT + 1] == 0x5AA5);
}

static void drain(KortaState *s) {
    KortaAction action;
    while (korta_take_action(s, &action)) {}
}

/* Types a key sequence: digits . + - * / = % C, N for +/-, < for backspace. */
static void calc_keys(KortaState *s, const char *keys) {
    static const char layout[] = "CN%/789*456-123+0.<=";
    for (const char *k = keys; *k; k++) {
        const char *at = strchr(layout, *k);
        if (!at) continue;
        s->focus = (uint8_t)(at - layout);
        press(s, KORTA_A);
    }
}

static int type_hello(void) {
    KortaState s;
    korta_init(&s);
    CHECK(s.view == KORTA_HOME && s.flow && s.selected == 0);
    render_guarded(&s);
    press(&s, KORTA_RIGHT);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    // H=GHI, E=DEF, L=JKL, L=JKL, O=MNO.
    press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT); press(&s, KORTA_A);
    press(&s, KORTA_LEFT); press(&s, KORTA_A);
    press(&s, KORTA_DOWN); press(&s, KORTA_LEFT); press(&s, KORTA_A); press(&s, KORTA_A);
    press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT); press(&s, KORTA_A);
    CHECK(s.pending_len == 5);
    press(&s, KORTA_LEFT); press(&s, KORTA_A);
    CHECK(s.note_count == 1 && strcmp(s.notes[0], "HELLO ") == 0);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_NOTE_OPEN);
    KortaAction action;
    int prefs = 0;
    while (korta_take_action(&s, &action)) if (action.kind == KORTA_ACT_PREFS) prefs++;
    CHECK(prefs == 1);

    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.night);
    render_guarded(&s);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_NOTE_OPEN);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES && s.focus == 1);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);
    drain(&s);
    CHECK(!korta_press(&s, KORTA_B));
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_LAUNCH);
    return 0;
}

static int hold_and_double(void) {
    KortaState s;
    korta_init(&s);
    CHECK(!korta_frame(&s, 0, 0));
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), 100));
    CHECK(s.selected == 1);
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), 320));
    CHECK(s.selected == 2);
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), 430));
    CHECK(s.selected == 9);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    s.selected = 5;
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_UP), 1000));
    CHECK(s.selected == 2);
    korta_frame(&s, 0, 1100);
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_UP), 1600));
    CHECK(s.view == KORTA_HOME && s.selected == 2);

    korta_init(&s);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_RIGHT);
    press(&s, KORTA_RIGHT);
    CHECK(s.selected == 8);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    press(&s, KORTA_UP);
    CHECK(s.view == KORTA_HOME);
    press(&s, KORTA_LEFT);
    CHECK(s.view == KORTA_FIND);
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_HOME && s.selected == 0);
    press(&s, KORTA_LEFT);
    CHECK(s.view == KORTA_FIND);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && s.typing_find);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FIND);
    korta_init(&s);
    press(&s, KORTA_RIGHT);
    press(&s, KORTA_RIGHT);
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_HOME && s.selected == 9);
    press(&s, KORTA_LEFT);
    CHECK(s.selected == 2);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), 40));
    korta_frame(&s, 0, 70);
    CHECK(!korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), 120));
    CHECK(s.view == KORTA_HOME && s.selected == 2);

    korta_init(&s);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_HOME);
    KortaAction launch;
    CHECK(korta_take_action(&s, &launch));
    CHECK(launch.kind == KORTA_ACT_LAUNCH);
    return 0;
}

static int apps(void) {
    KortaState s;
    korta_init(&s);
    open_app(&s, 2);
    CHECK(s.view == KORTA_TIME);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_CLOCK);
    drain(&s);
    press(&s, KORTA_RIGHT);
    CHECK(s.hour == 13 && s.time_known && s.clock_dirty);
    KortaAction action;
    CHECK(korta_take_action(&s, &action));
    CHECK(action.kind == KORTA_ACT_CLOCK);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_TIME && !s.clock_dirty);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    open_app(&s, 5);
    CHECK(s.view == KORTA_SETTINGS && s.hw.brightness == 7);
    press(&s, KORTA_A);  // Search opens the keyboard at once
    CHECK(s.view == KORTA_KEYBOARD && s.typing_find);
    press(&s, KORTA_B);  // nothing typed: straight back to Settings
    CHECK(s.view == KORTA_SETTINGS && s.focus == 0);
    press(&s, KORTA_A);
    s.key_row = 0; s.key_col = 2; press(&s, KORTA_A);  // E
    s.key_row = 3; s.key_col = 2; press(&s, KORTA_A);  // DONE
    CHECK(s.view == KORTA_FIND && s.focus == 1 && strcmp(s.query, "E") == 0);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETTINGS);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_SETUP);
    drain(&s);
    press(&s, KORTA_RIGHT);
    CHECK(s.hw.brightness == 8);
    CHECK(korta_take_action(&s, &action));
    CHECK(action.kind == KORTA_ACT_BRIGHTNESS && action.value == 8);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETTINGS && s.focus == 2);  // the cursor stays on Display
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_SOUND);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_RIGHT);
    CHECK(s.vol_sys == 55 && s.vol_media == 50 && s.vol_ring == 80);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_RIGHT);
    CHECK(s.vol_sys == 55 && s.vol_media == 50 && s.vol_ring == 85);

    korta_init(&s);
    korta_note_volume(&s, 40, 1000);
    CHECK(s.vol_mode == 1 && s.vol_media == 40 && s.vol_sys == 50);
    press(&s, KORTA_LEFT);
    CHECK(s.view == KORTA_HOME && s.vol_mode == 2);
    korta_note_volume(&s, 50, s.now_ms);
    CHECK(s.vol_mode == 2 && s.vol_media == 50 && s.vol_ring == 80 && s.vol_sys == 50);
    press(&s, KORTA_UP);
    CHECK(s.vol_sys == 55 && s.vol_media == 50 && s.vol_ring == 80);
    press(&s, KORTA_RIGHT);
    press(&s, KORTA_UP);
    press(&s, KORTA_UP);
    press(&s, KORTA_DOWN);
    CHECK(s.vol_ring == 85 && s.vol_media == 50 && s.vol_sys == 55);
    press(&s, KORTA_LEFT);
    press(&s, KORTA_DOWN);
    CHECK(s.vol_sys == 50 && s.vol_mode == 2 && s.view == KORTA_HOME);
    press(&s, KORTA_UP);
    korta_note_volume(&s, 60, s.now_ms);
    CHECK(s.vol_mode == 2 && s.vol_media == 60 && s.vol_sys == 55 && s.vol_ring == 85);
    press(&s, KORTA_B);
    CHECK(s.vol_mode == 0);
    press(&s, KORTA_LEFT);
    CHECK(s.view == KORTA_FIND);

    korta_init(&s);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.vol_mode == 2 && s.view == KORTA_QUICK);
    press(&s, KORTA_B);
    CHECK(s.vol_mode == 0 && s.view == KORTA_QUICK);

    korta_init(&s);
    korta_note_volume(&s, 40, 1000);
    CHECK(korta_take_sfx(&s) == KORTA_SFX_TICK);
    press(&s, KORTA_RIGHT);
    CHECK(korta_take_sfx(&s) == KORTA_SFX_MOVE);
    korta_init(&s);
    open_app(&s, 5);
    CHECK(s.view == KORTA_SETTINGS && korta_take_sfx(&s) == KORTA_SFX_OPEN);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME && korta_take_sfx(&s) == KORTA_SFX_BACK);

    korta_init(&s);
    press(&s, KORTA_SELECT);
    CHECK(s.view == KORTA_FIND);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && s.typing_find);
    s.key_row = 1; s.key_col = 0; press(&s, KORTA_A);
    s.key_row = 1; s.key_col = 8; press(&s, KORTA_A);
    s.key_row = 3; s.key_col = 2; press(&s, KORTA_A);
    CHECK(s.view == KORTA_FIND && strcmp(s.query, "AL") == 0 && s.focus == 1);
    press(&s, KORTA_A);  // DONE lands on the first result
    CHECK(s.view == KORTA_ALARM);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FIND && s.focus == 1 && strcmp(s.query, "AL") == 0);
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    open_app(&s, 3);
    CHECK(s.view == KORTA_CALC);
    calc_keys(&s, "1+2=");
    CHECK(s.calc_val == 30000 && !s.calc_err);
    calc_keys(&s, "/0=");
    CHECK(s.calc_err);

    korta_init(&s);
    s.view = KORTA_KEYBOARD;
    s.typing_pass = true;
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_LINKS);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    s.time_known = true;
    s.alarms[0].on = true;
    s.hour = 7;
    s.min = 0;
    korta_frame(&s, 0, 100);
    CHECK(s.view == KORTA_ALARM_RING);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_HOME);
    korta_frame(&s, 0, s.now_ms + 50);
    CHECK(s.view == KORTA_HOME);
    s.min = 1;
    korta_frame(&s, 0, s.now_ms + 50);
    s.min = 0;
    korta_frame(&s, 0, s.now_ms + 50);
    CHECK(s.view == KORTA_ALARM_RING);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    korta_frame(&s, (uint16_t)(1u << KORTA_SELECT), 100);
    korta_frame(&s, 0, 160);
    CHECK(s.view == KORTA_FIND);
    korta_init(&s);
    korta_frame(&s, 0, 0);
    korta_frame(&s, (uint16_t)(1u << KORTA_SELECT), 100);
    korta_frame(&s, (uint16_t)(1u << KORTA_SELECT), 400);
    CHECK(s.night);
    korta_frame(&s, 0, 460);
    CHECK(s.view == KORTA_HOME && s.night);

    korta_init(&s);
    open_app(&s, 1);
    press(&s, KORTA_A);
    uint32_t t = s.now_ms;
    korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), t + 20);
    korta_frame(&s, 0, t + 40);
    korta_frame(&s, (uint16_t)(1u << KORTA_RIGHT), t + 80);
    CHECK(s.view == KORTA_KEYBOARD && s.key_col == 2);
    press(&s, KORTA_LEFT);
    press(&s, KORTA_LEFT);
    CHECK(s.key_col == 0);
    press(&s, KORTA_LEFT);
    CHECK(s.key_col == 2);
    press(&s, KORTA_RIGHT);
    CHECK(s.key_col == 0);

    korta_init(&s);
    s.view = KORTA_KEYBOARD;
    s.typing_pass = true;
    s.caps = false;
    press(&s, KORTA_A);
    CHECK(s.pass_len == 1 && s.password[0] == 'q');

    korta_init(&s);
    open_app(&s, 2);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_TIMER && s.timer_set == 60 && s.timer_count);
    press(&s, KORTA_A);
    CHECK(s.timer_run);
    korta_frame(&s, 0, s.now_ms + 61000);
    CHECK(s.timer_done && !s.timer_run);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    s.view = KORTA_TIMER;
    s.timer_count = true;
    s.timer_set = 5;
    s.timer_run = true;
    s.timer_mark = 100;
    s.timer_deadline = 5100;
    korta_service(&s, 5200);
    CHECK(s.timer_done && !s.timer_run && s.view == KORTA_TIMER_RING);
    KortaAction alert;
    bool lit = false, alerted = false;
    while (korta_take_action(&s, &alert)) {
        if (alert.kind == KORTA_ACT_BRIGHTNESS && alert.value == 16) lit = true;
        if (alert.kind == KORTA_ACT_ALERT) alerted = true;
    }
    CHECK(lit && alerted);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_TIMER);

    korta_init(&s);
    korta_apply_prefs(&s, 16, 0, 7, 0);
    CHECK(s.wifi_keep && !s.flow);

    korta_init(&s);
    s.view = KORTA_PLAY;
    CHECK(!korta_press(&s, KORTA_A));
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_LAUNCH);

    korta_init(&s);
    s.selected = 0;
    s.hw.cart_in = false;
    CHECK(!korta_press(&s, KORTA_A));
    CHECK(s.view == KORTA_HOME);
    CHECK(!korta_take_action(&s, &action));

    korta_init(&s);
    korta_press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);
    s.focus = 4;
    CHECK(!korta_press(&s, KORTA_A));
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_LAUNCH);

    korta_init(&s);
    KortaLink link = {0};
    link.count = 2;
    korta_set_link(&s, &link);
    s.view = KORTA_LINKS;
    s.focus = 3;
    link.count = 1;
    korta_set_link(&s, &link);
    CHECK(s.focus == 1);
    return 0;
}

static void navigation_flows(void) {
    KortaState s;
    KortaAction action;

    korta_init(&s);
    open_app(&s, 4);
    CHECK(s.view == KORTA_FILES);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FILES);
    press(&s, KORTA_B);
    korta_load_note(&s, "FIRST");
    korta_load_note(&s, "SECOND");
    open_app(&s, 4);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTE_OPEN && s.note_cur == 1);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FILES && s.focus == 1);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    open_app(&s, 5);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_LINKS && !s.wifi_keep);
    CHECK(!korta_take_action(&s, &action));
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETTINGS);

    korta_init(&s);
    press(&s, KORTA_LEFT);
    CHECK(s.view == KORTA_FIND);
    s.focus = 2; // TIME result
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_TIME);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_CLOCK);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_TIME);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FIND && s.focus == 2);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);
    s.focus = 3;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_QUICK && s.vol_mode == 2);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_QUICK && s.vol_mode == 0);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    open_app(&s, 6);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16);
    drain(&s);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK && s.hw.brightness == 7);
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_BRIGHTNESS && action.value == 7);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME && s.hw.brightness == 7);

    korta_init(&s);
    open_app(&s, 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    s.time_known = true;
    s.alarms[0].on = true;
    s.hour = s.alarms[0].hour;
    s.min = s.alarms[0].minute;
    korta_service(&s, s.now_ms + 100);
    CHECK(s.view == KORTA_ALARM_RING && s.ring_from == KORTA_KEYBOARD);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);

    korta_init(&s);
    s.time_known = true;
    s.alarms[0].on = true;
    s.hour = s.alarms[0].hour;
    s.min = s.alarms[0].minute;
    s.timer_count = true;
    s.timer_run = true;
    s.timer_deadline = 100;
    korta_service(&s, 100);
    CHECK(s.view == KORTA_ALARM_RING && s.timer_alert_pending);
    press(&s, KORTA_A);
    korta_service(&s, s.now_ms + 20);
    CHECK(s.view == KORTA_TIMER_RING && !s.timer_alert_pending);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    s.view = KORTA_PLAY;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_HOME);
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_LAUNCH);

    korta_init(&s);
    open_app(&s, 7); // Dice on page 1
    CHECK(s.view == KORTA_DICE);
    press(&s, KORTA_A);
    CHECK(s.dice >= 1 && s.dice <= 6 && s.dice_anim_until > s.now_ms);
    render_guarded(&s);
    korta_service(&s, s.dice_anim_until + 1);
    render_guarded(&s);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);
    open_app(&s, 8);
    CHECK(s.view == KORTA_COIN);
    press(&s, KORTA_A);
    CHECK((s.coin == 1 || s.coin == 2) && s.coin_anim_until > s.now_ms);
    render_guarded(&s);
}

static void regression_cases(void) {
    KortaState s;
    KortaAction action;
    /* Opening Menu while Start/A is held must consume the entire press. */
    korta_init(&s);
    uint16_t buttons = (1u << KORTA_START) | (1u << KORTA_A);
    korta_frame(&s, buttons, 1000);
    korta_frame(&s, buttons, 2000);
    korta_frame(&s, 0, 2030);
    CHECK(s.view == KORTA_HOME && !korta_take_action(&s, &action));
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);

    korta_init(&s);
    s.view = KORTA_TIMER;
    press(&s, KORTA_A);
    uint32_t deadline = s.timer_deadline;
    korta_service(&s, s.now_ms + 10000u);
    press(&s, KORTA_A);
    uint32_t left = s.timer_remaining;
    CHECK(!s.timer_run && left > 48000u && left < 50000u);
    korta_service(&s, s.now_ms + 10000u);
    press(&s, KORTA_A);
    CHECK(s.timer_run && s.timer_deadline > deadline);
    CHECK(s.timer_deadline - s.timer_mark == left);
    korta_service(&s, s.timer_deadline);
    CHECK(s.timer_done && s.view == KORTA_TIMER_RING);

    /* Millisecond deadlines may wrap every 49 days. */
    korta_init(&s);
    s.view = KORTA_TIMER;
    korta_frame(&s, 0, UINT32_MAX - 1000u);
    press(&s, KORTA_A);
    CHECK(s.timer_run && s.timer_deadline < 60000u);
    korta_service(&s, UINT32_MAX - 50u);
    CHECK(s.timer_run);
    render_guarded(&s);
    korta_service(&s, s.timer_deadline);
    CHECK(s.timer_done);
    korta_init(&s);
    korta_note_volume(&s, 20, UINT32_MAX - 100u);
    korta_service(&s, UINT32_MAX - 50u);
    CHECK(s.vol_mode == 1);
    korta_service(&s, 3000u);
    CHECK(s.vol_mode == 0);

    /* Results past the fixed-point range report an error instead of wrapping. */
    korta_init(&s);
    s.view = KORTA_CALC;
    calc_keys(&s, "999999999999/.0001=");
    CHECK(s.calc_err);

    korta_init(&s);
    s.view = KORTA_LINKS;
    KortaLink link = {.phase = KORTA_WIFI_IDLE, .count = 1};
    strcpy(link.nets[0].ssid, "Locked");
    link.nets[0].locked = true;
    korta_set_link(&s, &link);
    s.focus = 1;
    press(&s, KORTA_A);
    CHECK(s.typing_pass && s.flow);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_LINKS && s.flow);

    /* An alarm arriving during a timer alert must not create a return loop. */
    korta_init(&s);
    s.view = KORTA_TIMER_RING;
    s.ring_from = KORTA_NOTES;
    s.time_known = s.alarms[0].on = true;
    s.hour = s.alarms[0].hour; s.min = s.alarms[0].minute;
    korta_service(&s, 1000);
    CHECK(s.view == KORTA_TIMER_RING && s.ring_from == KORTA_NOTES);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_ALARM_RING && s.ring_from == KORTA_NOTES);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES);

    korta_init(&s);
    open_app(&s, 6);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16);
    s.timer_run = true; s.timer_deadline = s.now_ms + 1;
    korta_service(&s, s.now_ms + 1);
    CHECK(s.view == KORTA_TIMER_RING && s.hw.brightness == 16 && s.bright_keep == 7);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16 && s.bright_keep == 7);
    press(&s, KORTA_B);
    CHECK(s.hw.brightness == 7);

    /* An alert lights the screen fully and gives the level back afterwards. */
    korta_init(&s);
    s.hw.brightness = 3;
    s.timer_run = true; s.timer_deadline = s.now_ms + 1;
    korta_service(&s, s.now_ms + 1);
    CHECK(s.view == KORTA_TIMER_RING && s.hw.brightness == 16);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_HOME && s.hw.brightness == 3);
}

static void random_navigation(void) {
    KortaState s;
    korta_init(&s);
    uint32_t random = 0x91ac73u;
    for (unsigned i = 0; i < 20000; i++) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        korta_press(&s, (KortaButton)(random % 8u));
        drain(&s);  /* The real port drains actions every frame. */
        CHECK(s.view < KORTA_VIEW_COUNT);
        CHECK(s.note_count <= KORTA_NOTE_SLOTS);
        CHECK(s.note_count == 0 || s.note_cur < s.note_count);
        for (int n = 0; n < KORTA_NOTE_SLOTS; n++) {
            CHECK(s.note_lens[n] <= KORTA_NOTE_CAPACITY && s.notes[n][s.note_lens[n]] == '\0');
            CHECK((n < s.note_count) == (s.note_lens[n] > 0) || s.view == KORTA_KEYBOARD);
        }
        CHECK(s.pass_len < sizeof s.password && s.password[s.pass_len] == '\0');
        if ((i % 100u) == 0) render_guarded(&s);
    }
}

static void dump_frames(void) {
    FILE *out = fopen("/tmp/korta_frames.bin", "wb");
    if (!out) return;
    KortaState s;
    korta_init(&s);
    KortaStatus status = s.hw;
    status.battery_known = true;
    status.battery_pct = 76;
    status.battery_mv = 3420;
    status.lipo = true;
    status.charging = true;
    status.brightness = 7;
    status.blend = true;
    status.cc_usb = true;
    status.player = 1;
    snprintf(status.mcu, sizeof status.mcu, "0.13.4-korta2");
    snprintf(status.fpga, sizeof status.fpga, "18.8");
    status.heap_kb = 120;
    korta_set_status(&s, &status);
    KortaLink link = {0};
    link.phase = KORTA_WIFI_ONLINE;
    link.rssi = -52;
    link.saved = true;
    link.ip = 0x0A01A8C0u;
    snprintf(link.ssid, sizeof link.ssid, "Home");
    link.count = 2;
    snprintf(link.nets[0].ssid, sizeof link.nets[0].ssid, "Home");
    link.nets[0].rssi = -52;
    link.nets[0].locked = true;
    snprintf(link.nets[1].ssid, sizeof link.nets[1].ssid, "Cafe");
    link.nets[1].rssi = -70;
    korta_set_link(&s, &link);
    korta_load_note(&s, "HELLO ");
    korta_load_note(&s, "SHOPPING: EGGS MILK BREAD COFFEE BATTERIES AA");
    s.time_known = true;
    s.hour = 15;
    s.min = 4;

    KortaView shots[] = {
        KORTA_HOME, KORTA_NOTES, KORTA_KEYBOARD, KORTA_TIME, KORTA_CLOCK, KORTA_ALARM, KORTA_TIMER,
        KORTA_CALC, KORTA_FIND, KORTA_FILES, KORTA_SETTINGS, KORTA_SETUP, KORTA_POWER, KORTA_PLAY,
        KORTA_LINKS, KORTA_INFO, KORTA_QUICK, KORTA_CLEAR_CONFIRM, KORTA_ALARM_RING, KORTA_TIMER_RING,
        KORTA_SOUND, KORTA_CONTROLS, KORTA_NOTE_OPEN, KORTA_FLASH, KORTA_COUNT, KORTA_CONV
    };
    uint16_t pixels[KORTA_WIDTH * KORTA_HEIGHT];
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        s.view = shots[i];
        if (shots[i] == KORTA_HOME) s.selected = 0;
        korta_render(&s, pixels);
        fwrite(pixels, sizeof pixels, 1, out);
        render_guarded(&s);
    }
    s.night = true;
    s.view = KORTA_HOME;
    s.selected = 6;
    korta_render(&s, pixels);
    fwrite(pixels, sizeof pixels, 1, out);
    s.selected = 4;
    s.view = KORTA_KEYBOARD;
    s.flow = false;
    korta_render(&s, pixels);
    fwrite(pixels, sizeof pixels, 1, out);
    fclose(out);
}

static int calc_is(const char *keys, long long want) {
    KortaState s;
    korta_init(&s);
    s.view = KORTA_CALC;
    calc_keys(&s, keys);
    long long got = s.calc_typing ? -999 : (long long)s.calc_val;
    if (s.calc_err || got != want) fprintf(stderr, "calc %s: got %lld err %d, want %lld\n", keys, got, s.calc_err, want);
    return !s.calc_err && got == want;
}

static void calc_cases(void) {
    CHECK(calc_is("7/2=", 35000));
    CHECK(calc_is("1/3=", 3333));
    CHECK(calc_is("2/3=", 6667));
    CHECK(calc_is("N1/3=", -3333));
    CHECK(calc_is("N2/3=", -6667));
    CHECK(calc_is("1.5*1.5=", 22500));
    CHECK(calc_is(".1+.2=", 3000));
    CHECK(calc_is("200+10%=", 2200000));
    CHECK(calc_is("50%", 5000));
    CHECK(calc_is("5N*3=", -150000));
    CHECK(calc_is("5+*3=", 150000));       // a second operator replaces the first
    CHECK(calc_is("2+3*4=", 200000));      // left to right, like a pocket calculator
    CHECK(calc_is("9=+1=", 100000));       // continue from a result
    CHECK(calc_is("123456.789*1000=", 1234567890000LL));
    CHECK(calc_is("N.5*N.5=", 2500));
    CHECK(calc_is("12=N", -120000));       // negate a result
    CHECK(calc_is("8-N3=", 110000));       // sign on the second operand

    KortaState s;
    korta_init(&s);
    s.view = KORTA_CALC;
    calc_keys(&s, "1.234567");
    CHECK(strcmp(s.calc_buf, "1.2345") == 0);
    calc_keys(&s, "<<<<<<<");
    CHECK(strcmp(s.calc_buf, "0") == 0);
    calc_keys(&s, "1234567890123456");
    CHECK(strlen(s.calc_buf) == 12);
    calc_keys(&s, "C");
    calc_keys(&s, "N");
    CHECK(s.calc_typing && strcmp(s.calc_buf, "-0") == 0);
    calc_keys(&s, "C");
    calc_keys(&s, "8/0=");
    CHECK(s.calc_err);
    calc_keys(&s, "5");
    CHECK(s.calc_err);                     // only C leaves an error
    render_guarded(&s);
    calc_keys(&s, "C");
    CHECK(!s.calc_err && s.calc_val == 0);
    calc_keys(&s, "999999999999*10=");
    CHECK(s.calc_err);
    s.focus = 19;
    press(&s, KORTA_RIGHT);
    CHECK(s.focus == 19);
    press(&s, KORTA_DOWN);
    CHECK(s.focus == 19);
}

static void dice_coin_timer(void) {
    KortaState s;
    korta_init(&s);
    s.view = KORTA_DICE;
    press(&s, KORTA_RIGHT);
    CHECK(s.dice_kind == 1);
    press(&s, KORTA_A);
    CHECK(s.dice >= 1 && s.dice <= 6 && s.dice2 >= 1 && s.dice2 <= 6);
    render_guarded(&s);
    press(&s, KORTA_LEFT); press(&s, KORTA_LEFT);
    CHECK(s.dice_kind == 7 && s.dice == 0);
    int seen[101] = {0};
    for (int i = 0; i < 4000; i++) {
        press(&s, KORTA_A);
        CHECK(s.dice >= 1 && s.dice <= 100 && s.dice2 == 0);
        seen[s.dice]++;
    }
    int faces = 0;
    for (int i = 1; i <= 100; i++) faces += seen[i] > 0;
    CHECK(faces == 100);
    for (uint8_t k = 0; k < 8; k++) {
        s.dice_kind = k;
        press(&s, KORTA_A);
        render_guarded(&s);
        korta_service(&s, s.dice_anim_until + 1);
        render_guarded(&s);
    }

    korta_init(&s);
    s.view = KORTA_COIN;
    for (int i = 0; i < 200; i++) press(&s, KORTA_A);
    CHECK(s.heads + s.tails == 200 && s.heads > 60 && s.tails > 60);
    render_guarded(&s);
    press(&s, KORTA_DOWN);
    CHECK(s.heads == 0 && s.tails == 0);

    korta_init(&s);
    s.view = KORTA_TIMER;
    s.focus = 1;
    CHECK(s.timer_set == 60);
    press(&s, KORTA_RIGHT);
    CHECK(s.timer_set == 90);
    press(&s, KORTA_LEFT); press(&s, KORTA_LEFT);
    CHECK(s.timer_set == 55);
    s.timer_set = 570; press(&s, KORTA_RIGHT);
    CHECK(s.timer_set == 600);
    press(&s, KORTA_RIGHT);
    CHECK(s.timer_set == 660);
    s.timer_set = 3 * 3600; press(&s, KORTA_RIGHT);
    CHECK(s.timer_set == 3 * 3600);
    press(&s, KORTA_LEFT);
    CHECK(s.timer_set == 3 * 3600 - 300);
    s.timer_set = 5; press(&s, KORTA_LEFT);
    CHECK(s.timer_set == 5);
    render_guarded(&s);

    korta_init(&s);
    s.view = KORTA_COLOR;
    s.hw.palette = 2;
    s.hw.pal[0][0] = 0xFFFF;
    render_guarded(&s);
    s.hw.game_colors = true;
    render_guarded(&s);
    s.view = KORTA_INFO;
    render_guarded(&s);
}

/* One case per finding from the 2026-10-17 cross-check. */
static void review_fixes(void) {
    KortaState s;
    KortaAction a;

    // Clearing the coin tally mid-flip must not underflow the shown count.
    korta_init(&s);
    s.view = KORTA_COIN;
    press(&s, KORTA_A);
    press(&s, KORTA_DOWN);
    CHECK(s.heads == 0 && s.tails == 0 && (int32_t)(s.coin_anim_until - s.now_ms) <= 0);
    render_guarded(&s);

    // A note emptied from Files returns to Files and forgets the Files route.
    korta_init(&s);
    korta_load_note(&s, "ONE");
    korta_load_note(&s, "TWO");
    s.view = KORTA_FILES;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTE_OPEN && s.notes_from_files);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    while (s.note_lens[0]) press(&s, KORTA_B);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FILES && !s.notes_from_files && s.note_count == 1);
    press(&s, KORTA_B);
    open_app(&s, 1);
    s.focus = 1;
    press(&s, KORTA_A);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES);

    // A stale return path cannot open a note that no longer exists.
    korta_init(&s);
    s.view = KORTA_FIND;
    s.find_from = KORTA_NOTE_OPEN;
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_NOTES);
    s.view = KORTA_FIND;
    s.find_from = KORTA_CLEAR_CONFIRM;
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_NOTES);

    // Flash keeps its own level across an alert; the user's level returns at the end.
    korta_init(&s);
    s.hw.brightness = 5;
    open_app(&s, 6);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 14 && s.bright_keep == 5);
    s.timer_run = true; s.timer_deadline = s.now_ms + 1;
    korta_service(&s, s.now_ms + 1);
    CHECK(s.view == KORTA_TIMER_RING && s.hw.brightness == 16 && s.bright_keep == 5);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 14);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME && s.hw.brightness == 5);

    // The override is announced so the port can undo it after a power cut.
    korta_init(&s);
    drain(&s);
    open_app(&s, 6);
    int holds = 0;
    while (korta_take_action(&s, &a)) if (a.kind == KORTA_ACT_BRIGHT_HOLD) { CHECK(a.value == 7); holds++; }
    CHECK(holds == 1);
    korta_hidden(&s);
    CHECK(s.view == KORTA_HOME && s.hw.brightness == 7);
    bool cleared = false, restored = false;
    while (korta_take_action(&s, &a)) {
        if (a.kind == KORTA_ACT_BRIGHT_HOLD && a.value == 0) cleared = true;
        if (a.kind == KORTA_ACT_BRIGHTNESS && a.value == 7) restored = true;
    }
    CHECK(cleared && restored);

    // Closing the overlay on a ringing alarm dismisses it without re-ringing.
    korta_init(&s);
    s.time_known = true; s.alarms[0].on = true; s.hour = s.alarms[0].hour; s.min = s.alarms[0].minute;
    s.view = KORTA_NOTES;
    korta_service(&s, 1000);
    CHECK(s.view == KORTA_ALARM_RING);
    korta_hidden(&s);
    korta_service(&s, 2000);
    CHECK(s.view == KORTA_NOTES && s.hw.brightness == 7);
    korta_hidden(&s);
    CHECK(s.view == KORTA_NOTES);

    // Percent of a large total no longer overflows the intermediate product.
    korta_init(&s);
    s.view = KORTA_CALC;
    calc_keys(&s, "400000000000+100%=");
    CHECK(!s.calc_err && s.calc_val == 800000000000LL * 10000);
    calc_keys(&s, "C200+12.345%=");
    CHECK(!s.calc_err && s.calc_val == 2246900);  // 200 + 24.69, full precision

    // Holding B empties a new note and lands on Notes, not Home.
    korta_init(&s);
    open_app(&s, 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    uint32_t t = s.now_ms + 500;
    korta_frame(&s, 0, t);
    for (int i = 0; i < 40; i++) korta_frame(&s, 1u << KORTA_B, t += 30);
    CHECK(s.view == KORTA_NOTES && s.note_count == 0);
    korta_frame(&s, 0, t += 30);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);
}

static void settings_paths(void) {
    KortaState s;
    KortaAction a;
    korta_init(&s);
    open_app(&s, 5);
    // Date & Time opens clock settings and returns to Settings.
    s.focus = 6;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_CLOCK && s.via_settings);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETTINGS);
    // Display > GB Palette: L/R changes it in place, A opens the preview, B returns to Display.
    s.focus = 2;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_SETUP);
    press(&s, KORTA_DOWN); press(&s, KORTA_DOWN);
    drain(&s);
    press(&s, KORTA_RIGHT);
    CHECK(s.hw.palette == 1 && korta_take_action(&s, &a) && a.kind == KORTA_ACT_PALETTE && a.value == 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_COLOR);
    press(&s, KORTA_RIGHT);
    CHECK(s.hw.palette == 2 && s.focus == 0);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETUP);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_SETTINGS);
    // About: the player number row is adjustable, other rows are not.
    s.focus = 7;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_INFO);
    press(&s, KORTA_RIGHT);
    CHECK(s.hw.player == 1);
    for (int i = 0; i < 5; i++) press(&s, KORTA_DOWN);
    CHECK(s.focus == 5);
    drain(&s);
    press(&s, KORTA_RIGHT);
    CHECK(s.hw.player == 2 && korta_take_action(&s, &a) && a.kind == KORTA_ACT_PLAYER && s.focus == 5);
    press(&s, KORTA_LEFT); press(&s, KORTA_LEFT);
    CHECK(s.hw.player == 8);
    render_guarded(&s);
    // Controls has two settings and a shortcuts list.
    press(&s, KORTA_B);
    s.focus = 4;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_CONTROLS);
    press(&s, KORTA_DOWN); press(&s, KORTA_DOWN);
    CHECK(s.focus == 1);
    press(&s, KORTA_A);
    CHECK(!s.flow);
    render_guarded(&s);
    // Every search entry is reachable in the unfiltered list.
    korta_init(&s);
    press(&s, KORTA_SELECT);
    CHECK(s.view == KORTA_FIND);
    for (int i = 0; i < 60; i++) press(&s, KORTA_DOWN);
    CHECK(s.focus >= 30);
    render_guarded(&s);
    // Page 2 is reachable from every row of page 1, and back.
    // Page 2 has two rows (Count, Conv, Color / Phone): lower rows land on the last one.
    for (int row = 0; row < 3; row++) {
        int lands = row < 1 ? 0 : 1;
        korta_init(&s);
        s.selected = (uint8_t)(row * 3 + 2);
        press(&s, KORTA_RIGHT);
        CHECK(s.selected == 9 + lands * 3);
        press(&s, KORTA_LEFT);
        CHECK(s.selected == lands * 3 + 2);
    }
    // Home page 2 has three apps and no gaps to land in.
    korta_init(&s);
    s.selected = 9;
    press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT);
    CHECK(s.selected == 11);
    press(&s, KORTA_DOWN);
    CHECK(s.selected == 11);
}

/* Findings from the full UI audit (2026-10-17). */
static void audit_fixes(void) {
    KortaState s;
    uint32_t t;

    // Holding B empties a saved note's text but never deletes the note.
    korta_init(&s);
    korta_load_note(&s, "KEEP ME");
    open_app(&s, 1);
    s.focus = 1; press(&s, KORTA_A);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    t = s.now_ms + 500;
    korta_frame(&s, 0, t);
    for (int i = 0; i < 120; i++) korta_frame(&s, 1u << KORTA_B, t += 30);
    CHECK(s.view == KORTA_KEYBOARD && s.note_count == 1 && s.note_lens[0] == 0);
    korta_frame(&s, 0, t += 30);
    korta_frame(&s, 1u << KORTA_B, t += 30);  // a fresh press leaves (and drops the now-empty note)
    korta_frame(&s, 0, t += 30);
    CHECK(s.view == KORTA_NOTES);

    // A held A that opened the keyboard does not start typing.
    korta_init(&s);
    open_app(&s, 1);
    t = s.now_ms + 500;
    korta_frame(&s, 0, t);
    for (int i = 0; i < 20; i++) korta_frame(&s, 1u << KORTA_A, t += 30);
    CHECK(s.view == KORTA_KEYBOARD && s.pending_len == 0);
    korta_frame(&s, 0, t += 30);

    // A held button that changes nothing makes no sound.
    korta_init(&s);
    s.view = KORTA_INFO;
    t = s.now_ms + 500;
    korta_frame(&s, 0, t);
    korta_frame(&s, 1u << KORTA_LEFT, t += 30);
    (void)korta_take_sfx(&s);
    int sounds = 0;
    for (int i = 0; i < 30; i++) { korta_frame(&s, 1u << KORTA_LEFT, t += 30); sounds += korta_take_sfx(&s) != 0; }
    CHECK(sounds == 0);
    korta_frame(&s, 0, t += 30);

    // Holding B home forgets where Find or Files would have returned.
    korta_init(&s);
    korta_load_note(&s, "X");
    press(&s, KORTA_SELECT);
    s.focus = 1; press(&s, KORTA_A);  // NOTES via Find
    CHECK(s.view == KORTA_NOTES && s.find_dest_active);
    t = s.now_ms + 500;
    korta_frame(&s, 0, t);
    for (int i = 0; i < 20; i++) korta_frame(&s, 1u << KORTA_B, t += 30);
    korta_frame(&s, 0, t += 30);
    CHECK(s.view == KORTA_HOME && !s.find_dest_active);
    open_app(&s, 1);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME);

    // Quick and Find never send B round in a loop.
    korta_init(&s);
    open_app(&s, 2);
    CHECK(s.view == KORTA_TIME);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK);
    press(&s, KORTA_SELECT);
    CHECK(s.view == KORTA_FIND && s.find_from == KORTA_TIME);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_QUICK && s.quick_from == KORTA_TIME);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_TIME);

    // Time zone steps by half hours; changing it does not invent a time.
    korta_init(&s);
    s.time_known = false;
    s.view = KORTA_CLOCK;
    s.focus = 6;
    s.zone_minutes = 330;
    press(&s, KORTA_LEFT);
    CHECK(s.zone_minutes == 300 && !s.time_known);
    press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT);
    CHECK(s.zone_minutes == 360);
    s.zone_minutes = 345;  // Nepal, from a USB sync
    press(&s, KORTA_RIGHT);
    CHECK(s.zone_minutes == 360);
    s.zone_minutes = -210;  // Newfoundland
    press(&s, KORTA_LEFT);
    CHECK(s.zone_minutes == -240);
    s.zone_minutes = -210;
    press(&s, KORTA_RIGHT);
    CHECK(s.zone_minutes == -180);
    s.focus = 5; press(&s, KORTA_A);
    CHECK(!s.time_known);

    // An alarm that comes due under a timer ring rings right after it.
    korta_init(&s);
    s.time_known = true; s.alarms[0].on = true; s.hour = 6; s.min = 59; s.alarms[0].hour = 7; s.alarms[0].minute = 0;
    s.timer_run = true; s.timer_deadline = 10;
    s.view = KORTA_NOTES;
    korta_service(&s, 20);
    CHECK(s.view == KORTA_TIMER_RING);
    s.hour = 7; s.min = 0;  // the alarm minute arrives while the timer rings
    korta_service(&s, 30);
    CHECK(s.view == KORTA_TIMER_RING && s.alarm_pending);
    s.min = 1;  // dismissed a minute later
    korta_service(&s, 60000);
    press(&s, KORTA_A);
    korta_service(&s, s.now_ms + 1);
    CHECK(s.view == KORTA_ALARM_RING);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES && !s.alarm_pending);

    // The Wi-Fi cursor stays on the same network after a rescan reorders the list.
    korta_init(&s);
    KortaLink link = {0};
    link.phase = KORTA_WIFI_IDLE;
    link.count = 2;
    snprintf(link.nets[0].ssid, sizeof link.nets[0].ssid, "AAA");
    snprintf(link.nets[1].ssid, sizeof link.nets[1].ssid, "BBB");
    link.saved = true;
    korta_set_link(&s, &link);
    s.view = KORTA_LINKS;
    s.focus = 2;  // BBB
    snprintf(link.nets[0].ssid, sizeof link.nets[0].ssid, "BBB");
    snprintf(link.nets[1].ssid, sizeof link.nets[1].ssid, "CCC");
    link.count = 3;
    snprintf(link.nets[2].ssid, sizeof link.nets[2].ssid, "AAA");
    korta_set_link(&s, &link);
    CHECK(s.focus == 1);
    s.focus = 4;  // FORGET
    link.count = 1;
    korta_set_link(&s, &link);
    CHECK(s.focus == 2);

    // QWERTY notes have a FLOW key back to predictive typing; passwords do not.
    korta_init(&s);
    s.flow = false;
    open_app(&s, 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && !s.symbols);
    s.key_row = 3; s.key_col = 0;
    press(&s, KORTA_A);
    CHECK(s.flow);
    render_guarded(&s);
    s.flow = false;
    s.typing_pass = true;
    s.key_row = 3; s.key_col = 0;
    press(&s, KORTA_A);  // SPACE in the password keyboard
    CHECK(!s.flow && s.pass_len == 1);

    // Back to Find after an alert keeps the query and the cursor.
    korta_init(&s);
    press(&s, KORTA_SELECT);
    press(&s, KORTA_A);
    s.key_row = 0; s.key_col = 2; press(&s, KORTA_A);   // E
    press(&s, KORTA_START);
    press(&s, KORTA_DOWN);
    uint8_t at = s.focus;
    s.timer_run = true; s.timer_deadline = s.now_ms + 1;
    korta_service(&s, s.now_ms + 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FIND && strcmp(s.query, "E") == 0 && s.focus == at);
}

static void phone_link(void) {
    KortaState s;
    KortaAction a;
    char out[KORTA_NOTE_CAPACITY + 1];

    // Phone text becomes what the screen can draw.
    CHECK(korta_clean_text(out, sizeof out, "Caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D it\xE2\x80\x99s \xE2\x80\x94 ok") > 0);
    CHECK(strcmp(out, "Cafe \"quoted\" it's - ok") == 0);
    korta_clean_text(out, sizeof out, "a\nb\tc\x01" "d \xF0\x9F\x98\x80!");
    CHECK(strcmp(out, "a b c?d ?!") == 0);
    char big[400];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK(korta_clean_text(out, sizeof out, big) == KORTA_NOTE_CAPACITY && out[KORTA_NOTE_CAPACITY] == '\0');
    char tiny[4];
    CHECK(korta_clean_text(tiny, sizeof tiny, "\xE2\x80\x9C") == 1 && strcmp(tiny, "\"") == 0);

    // Replacing notes: revision checked, text cleaned, empties dropped.
    korta_init(&s);
    korta_load_note(&s, "OLD");
    uint16_t rev = s.note_rev;
    const char *list[3] = {"First \xE2\x80\x94 note", "", "Second"};
    CHECK(korta_web_notes(&s, (uint16_t)(rev + 1), list, 3, 0) == KORTA_WEB_CONFLICT && s.note_count == 1);
    drain(&s);
    CHECK(korta_web_notes(&s, rev, list, 3, 2) == KORTA_WEB_OK);
    CHECK(s.note_count == 2 && strcmp(s.notes[0], "First - note") == 0 && strcmp(s.notes[1], "Second") == 0);
    CHECK(s.note_pinned == 1);  // the empty pinned text went away with its pin
    CHECK(s.note_rev != rev && korta_take_action(&s, &a) && a.kind == KORTA_ACT_PREFS);
    CHECK(korta_web_notes(&s, s.note_rev, list, KORTA_NOTE_SLOTS + 1, 0) == KORTA_WEB_INVALID);
    CHECK(korta_web_notes(&s, s.note_rev, list, 2, 3) == KORTA_WEB_INVALID);
    CHECK(korta_web_notes(&s, s.note_rev, list, 3, 0) == KORTA_WEB_OK && s.note_pinned == 0);
    drain(&s);
    // Never while a note is being typed on the device, nor mid-delete.
    s.view = KORTA_NOTES; s.focus = 1;
    press(&s, KORTA_A); press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    CHECK(korta_web_notes(&s, s.note_rev, list, 1, 0) == KORTA_WEB_BUSY);
    press(&s, KORTA_START);
    // Viewing a note that disappears falls back to the list.
    CHECK(s.view == KORTA_NOTE_OPEN);
    CHECK(korta_web_notes(&s, s.note_rev, NULL, 0, 0) == KORTA_WEB_OK);
    CHECK(s.view == KORTA_NOTES && s.note_count == 0 && s.focus == 0);
    render_guarded(&s);

    // Alarm from the phone.
    CHECK(korta_web_alarm(&s, 0, true, 24, 0, KORTA_DAYS_ALL, NULL) == KORTA_WEB_INVALID);
    CHECK(korta_web_alarm(&s, 4, true, 6, 0, KORTA_DAYS_ALL, NULL) == KORTA_WEB_INVALID);
    CHECK(korta_web_alarm(&s, 0, true, 6, 0, 0x80, NULL) == KORTA_WEB_INVALID);
    CHECK(korta_web_alarm(&s, 0, true, 6, 45, KORTA_DAYS_ALL, NULL) == KORTA_WEB_OK && s.alarms[0].on && s.alarms[0].hour == 6 && s.alarms[0].minute == 45);
    CHECK(korta_web_alarm(&s, 2, true, 9, 15, 0x1F, "Gym \xE2\x80\x94 early, very long label") == KORTA_WEB_OK);
    CHECK(s.alarms[2].days == 0x1F && strcmp(s.alarms[2].label, "Gym - early,") == 0);
    korta_web_alarm(&s, 2, false, 9, 15, 0x1F, "");
    CHECK(s.alarms[2].label[0] == '\0');
    drain(&s);

    // Several alarms: repeat days, one-time alarms, back-to-back minutes, the editor.
    {
        KortaState t;
        korta_init(&t);
        korta_frame(&t, 0, 0);
        t.time_known = true;
        t.year = 2026; t.month = 10; t.day = 19;  // a Monday
        korta_web_alarm(&t, 1, true, 8, 0, 0x60, "Weekend");
        t.hour = 8; t.min = 0;
        korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_HOME);  // weekends only
        t.min = 1; korta_frame(&t, 0, t.now_ms + 50);
        t.day = 17; t.min = 0;        // a Saturday
        korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_ALARM_RING && t.alarm_ringing == 1 && t.alarms[1].on);
        render_guarded(&t);
        press(&t, KORTA_A);
        CHECK(t.view == KORTA_HOME);
        korta_web_alarm(&t, 2, true, 9, 0, 0, "Once");
        t.hour = 9; t.min = 0;
        korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_ALARM_RING && t.alarm_ringing == 2 && !t.alarms[2].on);  // spent
        press(&t, KORTA_B);
        t.min = 1; korta_frame(&t, 0, t.now_ms + 50);
        t.min = 0; t.day = 18; korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_HOME);  // never again
        korta_web_alarm(&t, 0, true, 7, 0, KORTA_DAYS_ALL, NULL);
        korta_web_alarm(&t, 3, true, 7, 1, KORTA_DAYS_ALL, NULL);
        t.hour = 7; t.min = 0;
        korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_ALARM_RING && t.alarm_ringing == 0);
        press(&t, KORTA_A);
        t.min = 1;
        korta_frame(&t, 0, t.now_ms + 50);
        CHECK(t.view == KORTA_ALARM_RING && t.alarm_ringing == 3);  // the next minute's alarm still rings
        press(&t, KORTA_A);
        drain(&t);

        t.view = KORTA_ALARM; t.focus = 0; t.alarm_cur = 0;
        press(&t, KORTA_RIGHT);
        CHECK(t.alarm_cur == 1);
        press(&t, KORTA_LEFT); press(&t, KORTA_LEFT);
        CHECK(t.alarm_cur == KORTA_ALARM_MAX - 1);
        press(&t, KORTA_DOWN);
        bool was = t.alarms[t.alarm_cur].on;
        press(&t, KORTA_A);
        CHECK(t.alarms[t.alarm_cur].on != was);
        press(&t, KORTA_DOWN); press(&t, KORTA_RIGHT);
        CHECK(t.alarms[t.alarm_cur].hour == 8);
        press(&t, KORTA_DOWN); press(&t, KORTA_LEFT);
        CHECK(t.alarms[t.alarm_cur].minute == 0);
        press(&t, KORTA_DOWN);
        CHECK(t.focus == 4);
        press(&t, KORTA_A);
        CHECK(t.alarms[t.alarm_cur].days == 0x1F);
        press(&t, KORTA_A); press(&t, KORTA_A);
        CHECK(t.alarms[t.alarm_cur].days == 0);
        t.alarms[t.alarm_cur].days = 0x15;  // custom, from the phone
        render_guarded(&t);
        press(&t, KORTA_RIGHT);
        CHECK(t.alarms[t.alarm_cur].days == 0);
        render_guarded(&t);
        drain(&t);
    }

    // The Phone app: on Home page 2; switching it on also brings up Wi-Fi.
    korta_init(&s);
    s.selected = 9;
    press(&s, KORTA_DOWN);
    CHECK(s.selected == 12);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_PHONE);
    render_guarded(&s);
    drain(&s);
    press(&s, KORTA_A);
    bool web = false, wifi = false;
    while (korta_take_action(&s, &a)) {
        if (a.kind == KORTA_ACT_WEB && a.value == 1) web = true;
        if (a.kind == KORTA_ACT_WIFI_ON) wifi = true;
    }
    CHECK(s.web_keep && s.wifi_keep && web && wifi);
    KortaLink link = {0};
    link.phase = KORTA_WIFI_ONLINE;
    link.ip = 0x7101A8C0u;
    link.web_ready = true;
    link.web_paired = 2;
    snprintf(link.web_code, sizeof link.web_code, "482913");
    snprintf(link.web_host, sizeof link.web_host, "chromatic-b7f8.local");
    korta_set_link(&s, &link);
    render_guarded(&s);
    press(&s, KORTA_DOWN);
    drain(&s);
    press(&s, KORTA_A);
    CHECK(korta_take_action(&s, &a) && a.kind == KORTA_ACT_WEB_FORGET);
    korta_apply_prefs(&s, 32, 0, 7, 0);
    CHECK(s.web_keep && !s.wifi_keep);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME && s.selected == 12);
}

static void remote_control(void) {
    KortaState s;
    KortaAction a;

    // Messages: shown over anything, dismissed back to where you were.
    korta_init(&s);
    open_app(&s, 1);
    drain(&s);
    CHECK(korta_web_message(&s, "Dinner \xE2\x80\x94 7pm", false) == KORTA_WEB_OK);
    CHECK(s.view == KORTA_MESSAGE && strcmp(s.msg, "Dinner - 7pm") == 0 && s.ring_from == KORTA_NOTES);
    bool alert = false;
    while (korta_take_action(&s, &a)) alert |= a.kind == KORTA_ACT_ALERT;
    CHECK(alert && s.hw.brightness == 7);  // a plain message does not take over the backlight
    render_guarded(&s);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES);
    CHECK(korta_web_message(&s, "", false) == KORTA_WEB_INVALID);
    // Find me: full brightness, flashing, returns the level when dismissed.
    s.hw.brightness = 4;
    CHECK(korta_web_message(&s, "", true) == KORTA_WEB_OK && s.hw.brightness == 16 && s.bright_keep == 4);
    render_guarded(&s);
    s.now_ms += 400; render_guarded(&s);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES && s.hw.brightness == 4);
    // A message expires on its own; a new one replaces the old.
    CHECK(korta_web_message(&s, "first", false) == KORTA_WEB_OK);
    CHECK(korta_web_message(&s, "second", false) == KORTA_WEB_OK && strcmp(s.msg, "second") == 0 && s.ring_from == KORTA_NOTES);
    korta_service(&s, s.msg_until + 1);
    CHECK(s.view == KORTA_NOTES);
    // An alarm wins over a message and still returns to the original screen.
    s.time_known = true; s.alarms[0].on = true; s.hour = 7; s.min = 0; s.alarms[0].hour = 7; s.alarms[0].minute = 0; s.alarm_latched = false;
    CHECK(korta_web_message(&s, "hello", true) == KORTA_WEB_OK);
    korta_service(&s, s.now_ms + 10);
    CHECK(s.view == KORTA_ALARM_RING && s.ring_from == KORTA_NOTES && s.hw.brightness == 16);
    CHECK(korta_web_message(&s, "later", false) == KORTA_WEB_BUSY);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES && s.hw.brightness == 4);
    // Over the keyboard: a new empty note is still cleaned up after.
    korta_init(&s);
    open_app(&s, 1);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && s.note_count == 1);
    korta_web_message(&s, "hi", false);
    korta_hidden(&s);
    CHECK(s.view == KORTA_KEYBOARD);
    korta_web_message(&s, "hi", false);
    press(&s, KORTA_A);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES && s.note_count == 0);
    char longer[260];
    memset(longer, 'm', sizeof longer - 1); longer[sizeof longer - 1] = '\0';
    korta_web_message(&s, longer, false);
    CHECK(strlen(s.msg) == KORTA_NOTE_CAPACITY);
    render_guarded(&s);

    // Settings: same actions as the device's own rows; out-of-range refused.
    korta_init(&s);
    drain(&s);
    CHECK(korta_web_setting(&s, KORTA_SET_BRIGHTNESS, 12) == KORTA_WEB_OK && s.hw.brightness == 12);
    CHECK(korta_take_action(&s, &a) && a.kind == KORTA_ACT_BRIGHTNESS && a.value == 12);
    CHECK(korta_web_setting(&s, KORTA_SET_BRIGHTNESS, 17) == KORTA_WEB_INVALID);
    CHECK(korta_web_setting(&s, KORTA_SET_SILENT, 1) == KORTA_WEB_OK && s.hw.silent);
    CHECK(korta_take_action(&s, &a) && a.kind == KORTA_ACT_SILENT && a.value == 1);
    CHECK(korta_web_setting(&s, KORTA_SET_PALETTE, 3) == KORTA_WEB_OK && s.hw.palette == 3);
    s.hw.gbc_game = true;
    CHECK(korta_web_setting(&s, KORTA_SET_PALETTE, 4) == KORTA_WEB_INVALID && s.hw.palette == 3);
    CHECK(korta_web_setting(&s, KORTA_SET_PLAYER, 9) == KORTA_WEB_INVALID);
    CHECK(korta_web_setting(&s, KORTA_SET_LOWBATT, 2) == KORTA_WEB_OK && s.hw.low_batt == 2);
    CHECK(korta_web_setting(&s, KORTA_SET_NIGHT, 1) == KORTA_WEB_OK && s.night);
    CHECK(korta_web_setting(&s, KORTA_SET_COUNT, 1) == KORTA_WEB_INVALID);
    // Brightness changed during Flash becomes the level Flash returns to.
    korta_init(&s);
    open_app(&s, 6);
    CHECK(korta_web_setting(&s, KORTA_SET_BRIGHTNESS, 9) == KORTA_WEB_OK && s.hw.brightness == 16);
    press(&s, KORTA_B);
    CHECK(s.hw.brightness == 9);

    // Timer from the phone.
    korta_init(&s);
    CHECK(korta_web_timer(&s, KORTA_TIMER_SET, true, 4) == KORTA_WEB_INVALID);
    CHECK(korta_web_timer(&s, KORTA_TIMER_SET, true, 90) == KORTA_WEB_OK && s.timer_set == 90 && s.timer_count);
    CHECK(korta_web_timer(&s, KORTA_TIMER_START, false, 0) == KORTA_WEB_OK && s.timer_run);
    CHECK(korta_web_timer(&s, KORTA_TIMER_SET, true, 60) == KORTA_WEB_BUSY);
    CHECK(korta_web_timer(&s, KORTA_TIMER_STOP, false, 0) == KORTA_WEB_OK && !s.timer_run);
    CHECK(korta_web_timer(&s, KORTA_TIMER_RESET, false, 0) == KORTA_WEB_OK && !s.timer_remaining);
    CHECK(korta_web_timer(&s, KORTA_TIMER_SET, false, 0) == KORTA_WEB_OK && !s.timer_count);
    CHECK(korta_web_timer(&s, 9, false, 0) == KORTA_WEB_INVALID);

    // Wi-Fi from the phone.
    korta_init(&s);
    drain(&s);
    CHECK(korta_web_wifi(&s, "", "password1") == KORTA_WEB_INVALID);
    CHECK(korta_web_wifi_check("Home", "password1") == KORTA_WEB_OK && korta_web_wifi_check("Home", "pw") == KORTA_WEB_INVALID);
    CHECK(korta_web_wifi_check(NULL, NULL) == KORTA_WEB_INVALID && korta_web_wifi_check("x", NULL) == KORTA_WEB_OK);
    CHECK(korta_web_wifi(&s, "Home", "short") == KORTA_WEB_INVALID);
    CHECK(korta_web_wifi(&s, "Cafe", "") == KORTA_WEB_OK && s.pass_len == 0);
    CHECK(korta_web_wifi(&s, "Home", "correct horse") == KORTA_WEB_OK && strcmp(s.join_ssid, "Home") == 0 && s.wifi_keep);
    bool join = false;
    while (korta_take_action(&s, &a)) join |= a.kind == KORTA_ACT_WIFI_JOIN;
    CHECK(join);
}

static void notes_list(void) {
    KortaState s;
    korta_init(&s);
    open_app(&s, 1);
    CHECK(s.view == KORTA_NOTES && s.note_count == 0);
    // An abandoned new note leaves nothing behind.
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && s.note_count == 1);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES && s.note_count == 0);

    // Fill every slot; newest first.
    for (int i = 0; i < KORTA_NOTE_SLOTS; i++) {
        s.focus = 0;
        press(&s, KORTA_A);
        CHECK(s.view == KORTA_KEYBOARD);
        press(&s, KORTA_A);  // ABC key, Flow mode
        press(&s, KORTA_START);
        CHECK(s.view == KORTA_NOTE_OPEN && s.note_count == i + 1 && s.note_cur == 0);
        s.notes[0][0] = (char)('A' + i);
        press(&s, KORTA_B);
    }
    CHECK(s.note_count == KORTA_NOTE_SLOTS && s.notes[0][0] == 'A' + KORTA_NOTE_SLOTS - 1 && s.notes[KORTA_NOTE_SLOTS - 1][0] == 'A');
    s.focus = 0;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTES && s.note_count == KORTA_NOTE_SLOTS);
    render_guarded(&s);

    // Browse, then delete the third note.
    s.focus = 1;
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_NOTE_OPEN && s.note_cur == 0);
    press(&s, KORTA_LEFT);
    CHECK(s.note_cur == 0);
    press(&s, KORTA_RIGHT); press(&s, KORTA_RIGHT);
    CHECK(s.note_cur == 2 && s.notes[2][0] == 'A' + KORTA_NOTE_SLOTS - 3);
    uint16_t rev = s.note_rev;
    press(&s, KORTA_DOWN); press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_CLEAR_CONFIRM);
    press(&s, KORTA_A);  // CANCEL
    CHECK(s.view == KORTA_NOTE_OPEN && s.note_count == KORTA_NOTE_SLOTS);
    press(&s, KORTA_DOWN); press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);  // DELETE
    CHECK(s.view == KORTA_NOTES && s.note_count == KORTA_NOTE_SLOTS - 1 && s.note_rev != rev);
    CHECK(s.notes[2][0] == 'A' + KORTA_NOTE_SLOTS - 4 && s.notes[KORTA_NOTE_SLOTS - 1][0] == '\0' && s.focus == 3);

    // Erasing every character of a note removes it.
    s.focus = 1;
    press(&s, KORTA_A);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);
    while (s.note_lens[0]) press(&s, KORTA_B);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_NOTES && s.note_count == KORTA_NOTE_SLOTS - 2 && s.notes[0][0] == 'A' + KORTA_NOTE_SLOTS - 2);

    // Pinning: a pinned note moves to the top and stays above new notes.
    {
        char last = s.notes[3][0];
        s.focus = 4;
        press(&s, KORTA_A);
        CHECK(s.view == KORTA_NOTE_OPEN && s.note_cur == 3);
        press(&s, KORTA_DOWN);
        rev = s.note_rev;
        press(&s, KORTA_A);  // PIN TO TOP
        CHECK(s.note_pinned == 1 && s.note_cur == 0 && s.notes[0][0] == last && s.note_rev != rev);
        CHECK(s.view == KORTA_NOTE_OPEN && s.focus == 1);
        render_guarded(&s);
        press(&s, KORTA_B);
        CHECK(s.view == KORTA_NOTES && s.focus == 1);
        s.focus = 0;
        press(&s, KORTA_A);
        CHECK(s.view == KORTA_KEYBOARD && s.note_cur == 1);
        press(&s, KORTA_A);
        press(&s, KORTA_START);
        CHECK(s.notes[0][0] == last && s.note_lens[1] > 0 && s.note_pinned == 1);
        press(&s, KORTA_B);
        // Deleting the pinned note shrinks the pinned block.
        s.focus = 1;
        press(&s, KORTA_A);
        press(&s, KORTA_DOWN);
        press(&s, KORTA_A);  // UNPIN
        CHECK(s.note_pinned == 0 && s.note_cur == 0 && s.notes[0][0] == last);
        press(&s, KORTA_A);  // PIN again
        press(&s, KORTA_DOWN);
        press(&s, KORTA_A);
        press(&s, KORTA_DOWN);
        press(&s, KORTA_A);  // DELETE
        CHECK(s.view == KORTA_NOTES && s.note_pinned == 0 && s.notes[0][0] != last);
    }

    // Loading clips long text and ignores empties and overflow.
    korta_init(&s);
    char longer[KORTA_NOTE_CAPACITY + 40];
    memset(longer, 'X', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    korta_load_note(&s, longer);
    korta_load_note(&s, "");
    CHECK(s.note_count == 1 && s.note_lens[0] == KORTA_NOTE_CAPACITY);
    for (int i = 0; i < KORTA_NOTE_SLOTS + 4; i++) korta_load_note(&s, "MORE");
    CHECK(s.note_count == KORTA_NOTE_SLOTS);
    // Word wrap and its hard-wrap fallback stay inside the frame.
    korta_init(&s);
    korta_load_note(&s, longer);
    korta_load_note(&s, "A BB CCC DDDD EEEEE FFFFFF GGGGGGG HHHHHHHH IIIIIIIII JJJJJJJJJJ KKKKKKKKKKK "
        "LLLLLLLLLLLL MMMMMMMMMMMMM NNNNNNNNNNNNNN OOOOOOOOOOOOOOO PPPPPPPPPPPPPPPP QQ");
    korta_load_note(&s, "                                                  SPACES");
    s.view = KORTA_NOTE_OPEN;
    for (s.note_cur = 0; s.note_cur < s.note_count; s.note_cur++) render_guarded(&s);
}

/* Swap converts the value in place, so it exposes the rounded result. */
static int conv_check(KortaState *s, uint8_t kind, int whole) {
    int16_t n = s->conv_n;
    s->conv_kind = kind;
    s->focus = 1;
    press(s, KORTA_A);
    int got = s->conv_n;
    s->conv_n = n;
    if (got != whole) fprintf(stderr, "conv %u: %d -> %d, want %d\n", kind, n, got, whole);
    return got == whole;
}

static void flash_and_tools(void) {
    KortaState s;
    KortaAction action;
    korta_init(&s);
    s.now_ms = 10000;
    open_app(&s, 6);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16 && s.flash_mode == 0);
    drain(&s);
    press(&s, KORTA_DOWN);
    CHECK(s.hw.brightness == 15);
    CHECK(korta_take_action(&s, &action) && action.kind == KORTA_ACT_BRIGHTNESS && action.value == 15);
    press(&s, KORTA_DOWN);
    CHECK(s.hw.brightness == 14);  // a quick second tap is not a list jump
    press(&s, KORTA_RIGHT);
    CHECK(s.flash_tint == 1);
    press(&s, KORTA_LEFT); press(&s, KORTA_LEFT);
    CHECK(s.flash_tint == 2);
    press(&s, KORTA_A);
    CHECK(s.flash_mode == 1);
    uint16_t px[KORTA_WIDTH * KORTA_HEIGHT];
    s.now_ms = s.flash_mark + 100;
    korta_render(&s, px);
    CHECK(px[0] == 0xF800);  // red, lit
    s.now_ms = s.flash_mark + 600;
    korta_render(&s, px);
    CHECK(px[0] == 0);  // blink off phase
    press(&s, KORTA_A);
    CHECK(s.flash_mode == 2);
    s.now_ms = s.flash_mark + 160 * 3;  // first letter gap
    korta_render(&s, px);
    CHECK(px[0] == 0);
    s.now_ms = s.flash_mark + 160 * 8;  // inside the first dash
    korta_render(&s, px);
    CHECK(px[0] == 0xF800);
    drain(&s);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_HOME && s.hw.brightness == 7);

    korta_init(&s);
    s.view = KORTA_COUNT;
    press(&s, KORTA_A); press(&s, KORTA_A); press(&s, KORTA_RIGHT); press(&s, KORTA_LEFT);
    CHECK(s.tally == 2);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_LEFT);
    CHECK(s.tally == 2);
    press(&s, KORTA_A);
    CHECK(s.tally == 0 && s.focus == 0);

    korta_init(&s);
    s.view = KORTA_CONV;
    s.conv_n = 100;
    CHECK(conv_check(&s, 0, 212));        // 100 C = 212 F
    CHECK(conv_check(&s, 1, 38));         // 100 F = 37.8 C
    CHECK(conv_check(&s, 2, 220));        // 100 KG = 220.46 LB
    CHECK(conv_check(&s, 5, 161));        // 100 MI = 160.93 KM
    CHECK(conv_check(&s, 6, 254));        // 100 IN = 254 CM
    s.conv_n = -40;
    CHECK(conv_check(&s, 0, -40));        // -40 C = -40 F
    s.conv_kind = 0;
    s.conv_n = 100;
    s.focus = 0;
    press(&s, KORTA_LEFT);
    CHECK(s.conv_kind == 7);
    press(&s, KORTA_RIGHT);
    CHECK(s.conv_kind == 0);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_RIGHT);
    CHECK(s.conv_n == 101);
    press(&s, KORTA_A);  // swap: 101 C -> 214 F
    CHECK(s.conv_kind == 1 && s.conv_n == 214);
    s.conv_n = 9999;
    press(&s, KORTA_RIGHT);
    CHECK(s.conv_n == 9999);
    render_guarded(&s);
}

int main(void) {
    type_hello();
    hold_and_double();
    apps();
    navigation_flows();
    regression_cases();
    notes_list();
    review_fixes();
    settings_paths();
    audit_fixes();
    phone_link();
    remote_control();
    calc_cases();
    dice_coin_timer();
    flash_and_tools();
    random_navigation();
    dump_frames();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    puts("KortaMod core: gestures, apps, Flow text, and framebuffer bounds passed");
    return 0;
}
