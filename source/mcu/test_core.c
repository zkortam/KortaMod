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
    CHECK(strcmp(s.note, "HELLO ") == 0);
    press(&s, KORTA_START);
    CHECK(s.view == KORTA_NOTES);
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
    CHECK(s.view == KORTA_NOTES);
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
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FIND);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD && s.typing_find);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FIND);
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
    CHECK(s.view == KORTA_SETTINGS);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_DOWN);
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
    press(&s, KORTA_RIGHT);
    CHECK(s.vol_sys == 55 && s.vol_media == 50 && s.vol_ring == 80);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_RIGHT);
    CHECK(s.vol_ring == 85 && s.vol_media == 50);
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
    CHECK(s.view == KORTA_FIND && strcmp(s.query, "AL") == 0);
    press(&s, KORTA_DOWN);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_ALARM);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FIND && s.focus == 1 && strcmp(s.query, "AL") == 0);
    press(&s, KORTA_RIGHT);
    CHECK(s.view == KORTA_HOME);

    korta_init(&s);
    open_app(&s, 3);
    CHECK(s.view == KORTA_CALC);
    s.focus = 8; press(&s, KORTA_A);
    s.focus = 15; press(&s, KORTA_A);
    s.focus = 9; press(&s, KORTA_A);
    s.focus = 14; press(&s, KORTA_A);
    CHECK(s.calc_entry == 3 && !s.calc_err);
    s.focus = 3; press(&s, KORTA_A);
    s.focus = 13; press(&s, KORTA_A);
    s.focus = 14; press(&s, KORTA_A);
    CHECK(s.calc_err);

    korta_init(&s);
    s.view = KORTA_KEYBOARD;
    s.typing_pass = true;
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_LINKS);

    korta_init(&s);
    korta_frame(&s, 0, 0);
    s.time_known = true;
    s.alarm_on = true;
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
    CHECK(korta_take_action(&s, &alert));
    CHECK(alert.kind == KORTA_ACT_ALERT);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_TIMER);

    korta_init(&s);
    korta_apply_prefs(&s, 16, 0, 7, 0, NULL);
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
    CHECK(s.view == KORTA_NOTES);
    press(&s, KORTA_B);
    CHECK(s.view == KORTA_FILES);
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
    s.alarm_on = true;
    s.hour = s.alarm_h;
    s.min = s.alarm_m;
    korta_service(&s, s.now_ms + 100);
    CHECK(s.view == KORTA_ALARM_RING && s.ring_from == KORTA_KEYBOARD);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_KEYBOARD);

    korta_init(&s);
    s.time_known = true;
    s.alarm_on = true;
    s.hour = s.alarm_h;
    s.min = s.alarm_m;
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

    /* INT32_MIN / -1 must report overflow without undefined division. */
    korta_init(&s);
    s.view = KORTA_CALC;
    s.calc_acc = INT32_MIN;
    s.calc_entry = -1;
    s.calc_op = 4;
    s.focus = 14;
    press(&s, KORTA_A);
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
    s.time_known = s.alarm_on = true;
    s.hour = s.alarm_h; s.min = s.alarm_m;
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
    CHECK(s.view == KORTA_TIMER_RING && s.hw.brightness == 7);
    press(&s, KORTA_A);
    CHECK(s.view == KORTA_FLASH && s.hw.brightness == 16);
    press(&s, KORTA_B);
    CHECK(s.hw.brightness == 7);
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
        CHECK(s.note_len <= KORTA_NOTE_CAPACITY && s.note[s.note_len] == '\0');
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
    memcpy(s.note, "HELLO ", 6);
    s.note_len = 6;
    s.time_known = true;
    s.hour = 15;
    s.min = 4;

    KortaView shots[] = {
        KORTA_HOME, KORTA_NOTES, KORTA_KEYBOARD, KORTA_TIME, KORTA_CLOCK, KORTA_ALARM, KORTA_TIMER,
        KORTA_CALC, KORTA_FIND, KORTA_FILES, KORTA_SETTINGS, KORTA_SETUP, KORTA_POWER, KORTA_PLAY,
        KORTA_LINKS, KORTA_INFO, KORTA_QUICK, KORTA_CLEAR_CONFIRM, KORTA_ALARM_RING, KORTA_TIMER_RING,
        KORTA_SOUND, KORTA_CONTROLS
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

int main(void) {
    type_hello();
    hold_and_double();
    apps();
    navigation_flows();
    regression_cases();
    random_navigation();
    dump_frames();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    puts("KortaMod core: gestures, apps, Flow text, and framebuffer bounds passed");
    return 0;
}
