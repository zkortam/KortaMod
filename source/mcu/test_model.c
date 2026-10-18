/* Exhaustive model checker for the KortaMod UI core.
 *
 * Breadth-first search over the UI states that taps (korta_press of each of
 * the eight buttons) and a few outside events reach from several seeded
 * devices. Actions are drained after every step, as the port drains them every
 * frame. States are deduplicated on a projection of the fields that decide
 * what is on screen and what the buttons do (values such as brightness level,
 * calculator digits or the dice result are left out to bound the search).
 *
 * Every state, transition and a sample of 1 s holds is checked against the
 * invariants listed in INVARIANTS below. Each distinct violation is printed once
 * with the shortest button sequence found that reproduces it, starting from
 * the named seed. Exit status is non-zero when anything is violated.
 *
 *   test_model [max_depth] [max_states_per_seed]
 *
 * The core is used through its public header only. A few probes poke a copy of
 * the state (focus = 0 before measuring a list, cart_in on a copy before asking
 * which app a Home tile opens), the way test_core.c sets s.selected.
 */
#include "kortamod_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* Invariants                                                                */
/* ------------------------------------------------------------------------ */

static const char *const INVARIANTS[] = {
    "HOME-APPS     every Home app (both pages) is reachable from Home by D-pad alone; every tile opens an app",
    "HOME-PAGES    Right from every tile of a page reaches the next page; Left reaches the previous one",
    "HOME-SCC      Home tiles are strongly connected under the D-pad",
    "B-HOME        B pressed at most N times reaches Home from every state, with no loop",
    ("HOLD-B        holding B 1 s reaches Home from every non-keyboard view; on an alarm/timer ring it only\n"
     "              dismisses it (same result as one B tap) and never erases note text"),
    "HOLD-START    holding Start 1 s opens Find outside the keyboard, games and alert screens",
    "HOLD-SELECT   holding Select 1 s toggles the theme exactly once outside the keyboard and games",
    "B-UNDO        B after an opening press (A, Start, Select, Left at Home) returns to the same screen and row",
    "LOCATIONS     every explored screen (and Home page/row) can reach every other one",
    "FOCUS         focus is within the rows the view shows; list-less views keep focus 0",
    "SELECTED      Home selection names a real app tile",
    "NOTES         note_count <= slots, pinned <= count, note_cur valid, lengths match terminators, no empty stored note",
    "BUFFERS       query, password, pending, calc and SSID buffers NUL-terminated within bounds",
    "KEYS          keyboard cursor on a real key; note keyboard has a note to edit",
    "TRANSIENT     restoring/repeating/find_restore clear and the action ring empty between frames",
    "RETURNS       quick_from/find_from/ring_from never point at themselves or at a loop",
    "RENDER        korta_render stays inside 160x144 (guard words) and draws a non-blank screen",
    "QUEUE         one press never fills the 7-entry action ring; action values in range",
    "BRIGHT        backlight returns to the user's level after Flash, alarm/timer rings and korta_hidden;",
    "              BRIGHT_HOLD is written before the override and erased after it; core level == device level",
    "VOLUME        vol_mode/vol_sel stay in range",
    "SETTINGS      every Settings row opens a different non-empty page; B returns with the cursor on that row",
    "FIND          every search result opens a screen and B returns to the list on the same result",
    "RESCAN        a Wi-Fi rescan keeps the cursor on the same network or Forget row",
    "HIDDEN        after korta_hidden no alert or Flash remains and the user's brightness is back",
};

/* ------------------------------------------------------------------------ */
/* Symbols: the eight buttons plus outside events the port feeds the core     */
/* ------------------------------------------------------------------------ */

enum {
    SYM_ALARM = 8,   /* clock reaches the armed alarm time (korta_set_status), one idle frame */
    SYM_WAIT,        /* the running countdown runs out (idle frame at its deadline) */
    SYM_HIDE,        /* Menu closes the overlay: korta_hidden, unprime, idle service */
    SYM_WHEEL,       /* volume wheel moves 5% (korta_note_volume) */
    SYM_RESCAN,      /* Wi-Fi rescan returns the networks in reverse order (korta_set_link) */
    SYM_GAME_OK,     /* the port finished loading a game (korta_arcade_status) */
    SYM_GAME_ERR,    /* the game stopped with an error */
    SYM_COUNT
};
static const char *const SYM_NAME[SYM_COUNT] = {
    "UP", "RIGHT", "DOWN", "LEFT", "A", "B", "START", "SELECT",
    "<alarm-due>", "<timer-due>", "<menu-hides>", "<wheel>", "<rescan>", "<game-loaded>", "<game-error>"
};

/* ------------------------------------------------------------------------ */
/* Model: the core state plus what the port and user would observe            */
/* ------------------------------------------------------------------------ */

typedef struct {
    KortaState s;
    uint8_t device_bright;  /* last KORTA_ACT_BRIGHTNESS value: what the backlight shows */
    uint8_t user_bright;    /* the user's level: hw.brightness whenever no override is up */
    bool hold_on;           /* a BRIGHT_HOLD record is open in NVS */
} Model;

typedef struct {
    KortaAction act[64];
    int n;
    int max_frame;          /* most actions drained after a single frame */
    int max_step;           /* most actions produced by one korta_press */
} StepLog;

/* Anomalies found while applying a step's actions to the model. */
typedef struct {
    bool bad_bright_value, bad_hold_value, hold_twice, hold_unpaired, hold_order;
    bool restore_wrong, device_mismatch, hold_mismatch, launch_no_cart;
    int restore_got, restore_want, hold_got, hold_want;
} Anomaly;

static bool override_view(KortaView v) {
    return v == KORTA_FLASH || v == KORTA_ALARM_RING || v == KORTA_TIMER_RING;
}

static const char *view_name(int v) {
    switch ((KortaView)v) {
        case KORTA_HOME: return "HOME";
        case KORTA_NOTES: return "NOTES";
        case KORTA_KEYBOARD: return "KEYBOARD";
        case KORTA_CLOCK: return "DATE&TIME";
        case KORTA_ALARM: return "ALARM";
        case KORTA_TIMER: return "TIMER";
        case KORTA_CALC: return "CALC";
        case KORTA_FIND: return "FIND";
        case KORTA_FILES: return "FILES";
        case KORTA_SETUP: return "DISPLAY";
        case KORTA_POWER: return "POWER";
        case KORTA_PLAY: return "GAME";
        case KORTA_LINKS: return "WIFI";
        case KORTA_INFO: return "ABOUT";
        case KORTA_QUICK: return "QUICK";
        case KORTA_CLEAR_CONFIRM: return "DELETE?";
        case KORTA_ALARM_RING: return "ALARM_RING";
        case KORTA_TIMER_RING: return "TIMER_RING";
        case KORTA_TIME: return "CLOCK";
        case KORTA_SETTINGS: return "SETTINGS";
        case KORTA_DICE: return "DICE";
        case KORTA_COIN: return "COIN";
        case KORTA_COUNT: return "COUNT";
        case KORTA_CONV: return "CONV";
        case KORTA_FLASH: return "FLASH";
        case KORTA_COLOR: return "COLOR";
        case KORTA_SOUND: return "SOUND";
        case KORTA_CONTROLS: return "CONTROLS";
        case KORTA_NOTE_OPEN: return "NOTE";
        case KORTA_ARCADE: return "GAME";
        case KORTA_PHONE: return "PHONE";
        case KORTA_MESSAGE: return "MESSAGE";
        default: return "VIEW?";
    }
}

/* ------------------------------------------------------------------------ */
/* Seeds                                                                      */
/* ------------------------------------------------------------------------ */

enum { MAX_PREFIX = 32 };
typedef struct {
    const char *name;
    void (*setup)(Model *m);
    bool alarm, wait, wifi;     /* which events make sense for this seed */
    uint8_t prefix[MAX_PREFIX]; /* presses made by setup, part of every repro */
    int prefix_n;
    Model root;
} Seed;

static Seed *setup_seed;        /* seed being built; step() records into its prefix */

/* ------------------------------------------------------------------------ */
/* Hash map: 64-bit key -> int32                                              */
/* ------------------------------------------------------------------------ */

typedef struct { uint64_t *key; int32_t *val; size_t mask, used; } Map;

static void map_init(Map *m, size_t cap) {
    m->key = calloc(cap, sizeof *m->key);
    m->val = calloc(cap, sizeof *m->val);
    if (!m->key || !m->val) { fprintf(stderr, "out of memory\n"); exit(2); }
    m->mask = cap - 1;
    m->used = 0;
}
static void map_free(Map *m) { free(m->key); free(m->val); m->key = NULL; m->val = NULL; }
static int32_t *map_get(Map *m, uint64_t k) {
    if (!k) k = 1;
    for (size_t i = (size_t)k & m->mask;; i = (i + 1) & m->mask) {
        if (m->key[i] == k) return &m->val[i];
        if (!m->key[i]) return NULL;
    }
}
static void map_put(Map *m, uint64_t k, int32_t v);
static void map_grow(Map *m) {
    Map bigger;
    map_init(&bigger, (m->mask + 1) * 2);
    for (size_t i = 0; i <= m->mask; i++) if (m->key[i]) map_put(&bigger, m->key[i], m->val[i]);
    map_free(m);
    *m = bigger;
}
static void map_put(Map *m, uint64_t k, int32_t v) {
    if (!k) k = 1;
    if ((m->used + 1) * 10 > (m->mask + 1) * 6) map_grow(m);
    for (size_t i = (size_t)k & m->mask;; i = (i + 1) & m->mask) {
        if (m->key[i] == k) { m->val[i] = v; return; }
        if (!m->key[i]) { m->key[i] = k; m->val[i] = v; m->used++; return; }
    }
}

typedef struct { uint64_t h; } Hash;
static void h_init(Hash *h) { h->h = 0xcbf29ce484222325ULL; }
static void h_bytes(Hash *h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h->h ^= b[i]; h->h *= 0x100000001b3ULL; }
}
static void h_int(Hash *h, int64_t v) { h_bytes(h, &v, sizeof v); }
static uint64_t h_done(const Hash *h) {
    uint64_t x = h->h;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x ? x : 1;
}

/* ------------------------------------------------------------------------ */
/* Search graph and violation log                                             */
/* ------------------------------------------------------------------------ */

typedef struct { int32_t parent; uint8_t sym, seed, depth; } Node;
static Node *nodes;
static int node_count, node_cap;
static Seed seeds[8];
static int seed_count;

enum { MAX_PATH = 64 };
static int node_path(int node, uint8_t *syms) {
    int n = 0;
    uint8_t rev[MAX_PATH];
    for (int at = node; at >= 0 && nodes[at].parent >= 0 && n < MAX_PATH; at = nodes[at].parent) rev[n++] = nodes[at].sym;
    for (int i = 0; i < n; i++) syms[i] = rev[n - 1 - i];
    return n;
}

typedef struct { char key[96]; char msg[400]; char seq[1200]; int len; long count; } Violation;
enum { MAX_VIOLATIONS = 400 };
static Violation viol[MAX_VIOLATIONS];
static int viol_n;
static long viol_total;

/* Records one violation, keyed for deduplication; keeps the shortest repro. */
static void report(const char *key, int seed, const uint8_t *syms, int n, const char *suffix, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
static void report(const char *key, int seed, const uint8_t *syms, int n, const char *suffix, const char *fmt, ...) {
    viol_total++;
    char seq[1200];
    int len = 0, at = 0;
    const Seed *sd = &seeds[seed];
    at += snprintf(seq + at, sizeof seq - (size_t)at, "[%s]", sd->name);
    for (int i = 0; i < sd->prefix_n && at < (int)sizeof seq - 16; i++)
        at += snprintf(seq + at, sizeof seq - (size_t)at, " %s", SYM_NAME[sd->prefix[i]]);
    if (sd->prefix_n) at += snprintf(seq + at, sizeof seq - (size_t)at, " |");
    for (int i = 0; i < n && at < (int)sizeof seq - 16; i++, len++)
        at += snprintf(seq + at, sizeof seq - (size_t)at, " %s", SYM_NAME[syms[i]]);
    if (suffix && suffix[0] && at < (int)sizeof seq - 4) {
        snprintf(seq + at, sizeof seq - (size_t)at, " %s", suffix);
        len++;
    }
    for (int i = 0; i < viol_n; i++) {
        if (strcmp(viol[i].key, key) != 0) continue;
        viol[i].count++;
        if (len < viol[i].len) {
            viol[i].len = len;
            snprintf(viol[i].seq, sizeof viol[i].seq, "%s", seq);
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(viol[i].msg, sizeof viol[i].msg, fmt, ap);
            va_end(ap);
        }
        return;
    }
    if (viol_n >= MAX_VIOLATIONS) return;
    Violation *v = &viol[viol_n++];
    snprintf(v->key, sizeof v->key, "%s", key);
    snprintf(v->seq, sizeof v->seq, "%s", seq);
    v->len = len;
    v->count = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->msg, sizeof v->msg, fmt, ap);
    va_end(ap);
}

/* Context for the check being run: where the state came from, either a search
 * node or an explicit scripted path. */
typedef struct { int seed; int node; const char *suffix; const uint8_t *path; int path_n; } Where;
#define REPORT(w, key, ...) do { \
        uint8_t path_[MAX_PATH]; \
        int n_ = (w)->path ? (w)->path_n : (w)->node >= 0 ? node_path((w)->node, path_) : 0; \
        report((key), (w)->seed, (w)->path ? (w)->path : path_, n_, (w)->suffix, __VA_ARGS__); \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Stepping                                                                   */
/* ------------------------------------------------------------------------ */

static void drain(Model *m, StepLog *log) {
    KortaAction a;
    int here = 0;
    while (korta_take_action(&m->s, &a)) {
        here++;
        if (a.kind == KORTA_ACT_BRIGHTNESS) m->device_bright = (uint8_t)a.value;  /* the port applies it now */
        if (log && log->n < (int)(sizeof log->act / sizeof log->act[0])) log->act[log->n++] = a;
    }
    if (log && here > log->max_frame) log->max_frame = here;
}

static void idle_frame(Model *m, uint32_t t, StepLog *log) {
    korta_frame(&m->s, 0, t);
    drain(m, log);
}

/* Reports the device's state back to the core, as the port does every poll. */
static void echo_status(Model *m, bool time_known, int hour, int min) {
    KortaStatus st = m->s.hw;
    st.brightness = m->device_bright;
    st.time_known = time_known;
    st.year = 2026; st.month = 10; st.day = 17;
    st.hour = (uint8_t)hour; st.min = (uint8_t)min; st.sec = 0;
    korta_set_status(&m->s, &st);
}

static bool applicable(const Model *m, const Seed *seed, int sym) {
    const KortaState *s = &m->s;
    switch (sym) {
        case SYM_ALARM: return seed->alarm && s->alarms[0].on && s->view != KORTA_ALARM_RING;
        case SYM_WAIT: return seed->wait && s->timer_run && s->timer_count;
        case SYM_RESCAN: return seed->wifi && s->link.count > 1;
        case SYM_GAME_OK: return s->view == KORTA_ARCADE && s->arcade_phase == KORTA_ARCADE_LOADING;
        case SYM_GAME_ERR: return s->view == KORTA_ARCADE && s->arcade_phase != KORTA_ARCADE_ERROR;
        /* The wheel bar is the same overlay everywhere; keep it out of the
         * keyboard, whose cursor states dominate the search. */
        case SYM_WHEEL: return s->vol_mode == 0 && s->view != KORTA_KEYBOARD;
        default: return true;
    }
}

/* Applies one symbol and updates the model from the actions it produced. */
static void model_after(Model *m, const StepLog *log, KortaView v0, Anomaly *an);
static void step(Model *m, int sym, StepLog *log, Anomaly *an) {
    memset(log, 0, sizeof *log);
    memset(an, 0, sizeof *an);
    KortaView v0 = m->s.view;
    KortaState *s = &m->s;
    if (sym < 8) {
        korta_press(s, (KortaButton)sym);
        drain(m, log);
        log->max_step = log->n;
    } else if (sym == SYM_ALARM) {
        echo_status(m, true, m->s.alarms[0].hour, m->s.alarms[0].minute);
        idle_frame(m, s->now_ms + 20, log);
        int h = m->s.alarms[0].hour, mi = m->s.alarms[0].minute - 1;
        if (mi < 0) { mi = 59; h = (h + 23) % 24; }
        echo_status(m, true, h, mi);
        idle_frame(m, s->now_ms + 20, log);
    } else if (sym == SYM_WAIT) {
        idle_frame(m, s->timer_deadline + 1, log);
    } else if (sym == SYM_HIDE) {
        korta_hidden(s);
        drain(m, log);
        s->primed = false;  /* the port unprimes while hidden */
        korta_service(s, s->now_ms + 20);
        drain(m, log);
    } else if (sym == SYM_WHEEL) {
        uint8_t v = s->vol_media <= 95 ? (uint8_t)(s->vol_media + 5) : (uint8_t)(s->vol_media - 5);
        korta_note_volume(s, v, s->now_ms + 20);
        drain(m, log);
    } else if (sym == SYM_GAME_OK || sym == SYM_GAME_ERR) {
        korta_arcade_status(s, sym == SYM_GAME_OK ? KORTA_ARCADE_RUNNING : KORTA_ARCADE_ERROR,
            sym == SYM_GAME_OK ? NULL : "line 12: score is not defined because of a long reason");
        drain(m, log);
    } else if (sym == SYM_RESCAN) {
        KortaLink link = s->link;
        for (int i = 0; i < link.count / 2; i++) {
            KortaNet t = link.nets[i];
            link.nets[i] = link.nets[link.count - 1 - i];
            link.nets[link.count - 1 - i] = t;
        }
        korta_set_link(s, &link);
        drain(m, log);
    }
    model_after(m, log, v0, an);
    if (setup_seed && setup_seed->prefix_n < MAX_PREFIX) setup_seed->prefix[setup_seed->prefix_n++] = (uint8_t)sym;
}

static void model_after(Model *m, const StepLog *log, KortaView v0, Anomaly *an) {
    bool in0 = override_view(v0), in1 = override_view(m->s.view);
    int first_bright = -1, last_bright = -1, hold_set = -1, hold_clear = -1;
    for (int i = 0; i < log->n; i++) {
        const KortaAction *a = &log->act[i];
        if (a->kind == KORTA_ACT_BRIGHTNESS) {
            if (a->value < 1 || a->value > 16) an->bad_bright_value = true;
            m->device_bright = (uint8_t)a->value;
            if (first_bright < 0) first_bright = i;
            last_bright = i;
        } else if (a->kind == KORTA_ACT_BRIGHT_HOLD) {
            if (a->value) {
                if (m->hold_on) an->hold_twice = true;
                if (a->value > 16) an->bad_hold_value = true;
                if (a->value != m->user_bright) {
                    an->hold_mismatch = true;
                    an->hold_got = a->value;
                    an->hold_want = m->user_bright;
                }
                m->hold_on = true;
                if (hold_set < 0) hold_set = i;
            } else {
                if (!m->hold_on) an->hold_unpaired = true;
                m->hold_on = false;
                hold_clear = i;
            }
        } else if (a->kind == KORTA_ACT_LAUNCH && !m->s.hw.cart_in) {
            an->launch_no_cart = true;
        }
    }
    /* The record is written before the override and erased after the level is back. */
    if (hold_set >= 0 && first_bright >= 0 && first_bright < hold_set && !in0) an->hold_order = true;
    if (hold_clear >= 0 && last_bright > hold_clear) an->hold_order = true;
    if (!in1) {
        if (in0 && m->s.hw.brightness != m->user_bright) {
            an->restore_wrong = true;
            an->restore_got = m->s.hw.brightness;
            an->restore_want = m->user_bright;
        }
        m->user_bright = m->s.hw.brightness;
    }
    if (m->device_bright != m->s.hw.brightness) an->device_mismatch = true;
}

/* ------------------------------------------------------------------------ */
/* Projection                                                                 */
/* ------------------------------------------------------------------------ */

/* Text lengths only matter as empty / some / full for navigation. */
static int bucket(int len, int full) { return len == 0 ? 0 : len >= full ? 3 : len == 1 ? 1 : 2; }

/* What is on screen and what the buttons do.
 *   keys=false   drops the keyboard cursor (B does not depend on it)
 *   exact=true   hashes exact text lengths (B-chain distances depend on them);
 *                otherwise lengths are bucketed and only the query's first
 *                two letters count, which bounds the keyboard's state space
 *   returns      adds the return-path fields that can still be used: where
 *                Quick/Find/a ring go back to and the rows they restore */
static uint64_t project_ex(const Model *m, int seed, bool keys, bool exact, bool returns) {
    const KortaState *s = &m->s;
    Hash h;
    h_init(&h);
    h_int(&h, seed);
    h_int(&h, s->view);
    h_int(&h, s->focus);
    h_int(&h, s->selected);
    if (keys) { h_int(&h, s->key_row); h_int(&h, s->key_col); h_int(&h, s->candidate); }
    h_int(&h, s->flow | s->caps << 1 | s->symbols << 2 | s->typing_pass << 3 | s->typing_find << 4);
    if (exact) {
        h_int(&h, s->pending_len);
        h_int(&h, s->query_len);
        h_bytes(&h, s->query, s->query_len < sizeof s->query ? s->query_len : sizeof s->query);
        h_int(&h, s->pass_len);
        h_bytes(&h, s->note_lens, sizeof s->note_lens);
    } else {
        h_int(&h, bucket(s->pending_len, 31));
        h_int(&h, bucket(s->query_len, 10));
        h_int(&h, s->query_len ? s->query[0] : 0);
        h_int(&h, s->query_len > 1 ? s->query[1] : 0);
        h_int(&h, s->pass_len == 0 ? 0 : s->pass_len < 8 ? 1 : 2);
        for (int i = 0; i < KORTA_NOTE_SLOTS; i++) h_int(&h, bucket(s->note_lens[i], KORTA_NOTE_CAPACITY));
    }
    h_int(&h, s->note_count);
    h_int(&h, s->note_pinned);
    h_int(&h, s->note_cur);
    h_int(&h, s->vol_mode * 4 + s->vol_sel);
    h_int(&h, s->link.phase * 64 + s->link.count * 2 + s->link.saved);
    h_bytes(&h, s->link.nets[0].ssid, 4);
    h_int(&h, s->time_known | s->clock_dirty << 1 | s->alarms[0].on << 2 | s->alarm_latched << 3 | s->alarm_pending << 4);
    h_int(&h, s->alarm_cur | s->alarm_ringing << 4);
    for (int i = 0; i < KORTA_ALARM_MAX; i++) h_int(&h, s->alarms[i].on | s->alarms[i].days << 1);
    h_int(&h, s->timer_count | s->timer_run << 1 | s->timer_done << 2 | s->timer_alert_pending << 3);
    h_int(&h, s->calc_typing | s->calc_err << 1 | s->calc_fresh << 2 | s->calc_op << 3);
    h_int(&h, s->hw.cart_in | s->hw.gbc_game << 1);
    h_int(&h, override_view(s->view) | (s->hw.brightness == m->user_bright) << 1 | m->hold_on << 2);
    h_int(&h, s->restoring | s->repeating << 1 | s->find_restore << 2);
    if (returns) {
        bool finding = s->view == KORTA_FIND || s->find_dest_active || (s->view == KORTA_KEYBOARD && s->typing_find);
        h_int(&h, s->via_settings | s->notes_from_files << 1 | s->find_dest_active << 2 | s->search_direct << 3);
        if (finding) { h_int(&h, s->find_from); h_int(&h, s->back_focus[s->find_from]); }
        if (s->find_dest_active) { h_int(&h, s->find_dest); h_int(&h, s->find_focus); }
        if (s->view == KORTA_QUICK) { h_int(&h, s->quick_from); h_int(&h, s->back_focus[s->quick_from]); }
        if (override_view(s->view) && s->view != KORTA_FLASH) { h_int(&h, s->ring_from); h_int(&h, s->back_focus[s->ring_from]); }
        static const KortaView parents[] = {KORTA_SETTINGS, KORTA_TIME, KORTA_SETUP, KORTA_LINKS, KORTA_NOTE_OPEN, KORTA_NOTES, KORTA_FILES};
        for (size_t i = 0; i < sizeof parents / sizeof parents[0]; i++) h_int(&h, s->back_focus[parents[i]]);
    }
    return h_done(&h);
}
static uint64_t project(const Model *m, int seed) { return project_ex(m, seed, true, false, true); }

/* ------------------------------------------------------------------------ */
/* Probes (memoized)                                                          */
/* ------------------------------------------------------------------------ */

static Map focus_memo, bdist_memo, render_memo, hold_memo;
static int16_t app_memo[256];
static long renders, bchains, holds, edges;

/* Highest focus the view's list reaches by pressing Down from row 0. */
static int max_focus(const Model *m) {
    const KortaState *s = &m->s;
    Hash h;
    h_init(&h);
    h_int(&h, s->view);
    h_int(&h, s->note_count);
    h_int(&h, s->link.count * 2 + s->link.saved);
    h_bytes(&h, s->query, sizeof s->query);
    uint64_t k = h_done(&h);
    int32_t *hit = map_get(&focus_memo, k);
    if (hit) return *hit;
    KortaState c = s[0];
    KortaAction a;
    c.focus = 0;
    c.vol_mode = 0;
    int best = 0;
    if (c.view == KORTA_CALC) {
        for (int i = 0; i < 12; i++) { korta_press(&c, i < 6 ? KORTA_DOWN : KORTA_RIGHT); while (korta_take_action(&c, &a)) {} }
        best = c.focus;
    } else {
        for (int i = 0; i < 80; i++) {
            uint8_t before = c.focus;
            korta_press(&c, KORTA_DOWN);
            while (korta_take_action(&c, &a)) {}
            if (c.view != s->view) break;
            if (c.focus > best) best = c.focus;
            if (c.focus == before) break;
        }
    }
    map_put(&focus_memo, k, best);
    return best;
}

enum { OUT_NONE = -1, OUT_LAUNCH = 100 };
/* What A on this Home tile does: the view it opens, OUT_LAUNCH, or OUT_NONE. */
static int app_outcome(const KortaState *s) {
    if (app_memo[s->selected] != -2) return app_memo[s->selected];
    KortaState c = *s;
    KortaAction a;
    c.vol_mode = 0;
    c.hw.cart_in = true;
    korta_press(&c, KORTA_A);
    int out = c.view != KORTA_HOME ? (int)c.view : OUT_NONE;
    while (korta_take_action(&c, &a)) if (a.kind == KORTA_ACT_LAUNCH) out = OUT_LAUNCH;
    app_memo[s->selected] = (int16_t)out;
    return out;
}

/* Renders between guard words; returns distinct colors below the status bar. */
enum { GUARD = 64, PIXELS = KORTA_WIDTH * KORTA_HEIGHT };
static uint16_t *render_buf;
static int render_colors(const KortaState *s, bool *guard_ok, uint64_t *image) {
    for (int i = 0; i < GUARD; i++) { render_buf[i] = 0xA55A; render_buf[GUARD + PIXELS + i] = 0x5AA5; }
    korta_render(s, render_buf + GUARD);
    renders++;
    *guard_ok = true;
    for (int i = 0; i < GUARD; i++)
        if (render_buf[i] != 0xA55A || render_buf[GUARD + PIXELS + i] != 0x5AA5) *guard_ok = false;
    const uint16_t *px = render_buf + GUARD;
    uint16_t seen[4];
    int colors = 0;
    for (int i = KORTA_WIDTH * 12; i < PIXELS && colors < 4; i++) {
        bool known = false;
        for (int c = 0; c < colors; c++) if (seen[c] == px[i]) known = true;
        if (!known) seen[colors++] = px[i];
    }
    if (image) {
        Hash h;
        h_init(&h);
        h_bytes(&h, px, PIXELS * sizeof *px);
        *image = h_done(&h);
    }
    return colors;
}

/* Presses B until Home. Returns presses needed, -1 on a loop, -2 past limit. */
static int b_distance(const Model *start, int seed, int limit, KortaView *stuck) {
    Model c = *start;
    uint64_t chain[512];
    int n = 0, dist = -2;
    bchains++;
    for (int k = 0; k <= limit && n < 512; k++) {
        *stuck = c.s.view;
        if (c.s.view == KORTA_HOME) { dist = k; break; }
        uint64_t key = project_ex(&c, seed, false, true, true);
        int32_t *hit = map_get(&bdist_memo, key);
        if (hit) { dist = *hit < 0 ? *hit : k + *hit; break; }
        for (int i = 0; i < n; i++) if (chain[i] == key) { dist = -1; break; }
        if (dist == -1) break;
        chain[n++] = key;
        StepLog log;
        Anomaly an;
        step(&c, KORTA_B, &log, &an);
    }
    for (int i = 0; i < n; i++) map_put(&bdist_memo, chain[i], dist >= 0 ? dist - i : dist);
    return dist;
}

/* ------------------------------------------------------------------------ */
/* State invariants                                                           */
/* ------------------------------------------------------------------------ */

static bool nul_within(const char *buf, size_t n) { return memchr(buf, '\0', n) != NULL; }

static void check_state(const Model *m, const Where *w, bool render) {
    const KortaState *s = &m->s;
    const char *vn = view_name(s->view);
    char key[96];
    if ((unsigned)s->view >= KORTA_VIEW_COUNT) { REPORT(w, "TRANSIENT view out of range", "view=%d", (int)s->view); return; }

    /* TRANSIENT */
    if (s->restoring || s->repeating || s->find_restore) {
        snprintf(key, sizeof key, "TRANSIENT flag left set in %s", vn);
        REPORT(w, key, "restoring=%d repeating=%d find_restore=%d after the step", s->restoring, s->repeating, s->find_restore);
    }
    if (s->qh != s->qt) REPORT(w, "TRANSIENT queue not empty", "action ring not empty after draining");

    /* RETURNS */
    if (s->quick_from == KORTA_QUICK) REPORT(w, "RETURNS quick_from=QUICK", "Quick returns to itself");
    if (s->find_from == KORTA_FIND || s->find_from == KORTA_QUICK) {
        snprintf(key, sizeof key, "RETURNS find_from=%s", view_name(s->find_from));
        REPORT(w, key, "Find's B target is %s (loop)", view_name(s->find_from));
    }
    if (override_view(s->ring_from) && s->ring_from != KORTA_FLASH) REPORT(w, "RETURNS ring_from ring", "ring returns into a ring");

    /* NOTES */
    if (s->note_count > KORTA_NOTE_SLOTS) REPORT(w, "NOTES count", "note_count=%u > %u", s->note_count, KORTA_NOTE_SLOTS);
    else {
        if (s->alarm_cur >= KORTA_ALARM_MAX || s->alarm_ringing >= KORTA_ALARM_MAX) REPORT(w, "ALARMS index", "alarm_cur=%u ringing=%u", s->alarm_cur, s->alarm_ringing);
    for (int i = 0; i < KORTA_ALARM_MAX; i++)
        if (s->alarms[i].hour > 23 || s->alarms[i].minute > 59 || s->alarms[i].days > KORTA_DAYS_ALL || !nul_within(s->alarms[i].label, KORTA_ALARM_LABEL + 1))
            REPORT(w, "ALARMS range", "alarm %d out of range", i);
    if (s->note_pinned > s->note_count) REPORT(w, "NOTES pinned", "note_pinned=%u > note_count=%u", s->note_pinned, s->note_count);
        if (s->note_count ? s->note_cur >= s->note_count : s->note_cur != 0) {
            snprintf(key, sizeof key, "NOTES note_cur in %s", vn);
            REPORT(w, key, "note_cur=%u with note_count=%u", s->note_cur, s->note_count);
        }
        bool editing = (s->view == KORTA_KEYBOARD || ((s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING) && s->ring_from == KORTA_KEYBOARD))
            && !s->typing_pass && !s->typing_find;
        for (int i = 0; i < KORTA_NOTE_SLOTS; i++) {
            const char *t = s->notes[i];
            if (!nul_within(t, KORTA_NOTE_CAPACITY + 1)) { REPORT(w, "NOTES unterminated", "note %d has no terminator", i); continue; }
            size_t len = strlen(t);
            if (i < s->note_count) {
                if (len != s->note_lens[i] || len > KORTA_NOTE_CAPACITY) REPORT(w, "NOTES length", "note %d: strlen %zu, note_lens %u", i, len, s->note_lens[i]);
                if (!len && !(editing && i == s->note_cur)) {
                    snprintf(key, sizeof key, "NOTES empty note kept in %s", vn);
                    REPORT(w, key, "note %d is empty outside its editor (view %s)", i, vn);
                }
            } else if (len || s->note_lens[i]) REPORT(w, "NOTES stale slot", "slot %d past note_count holds %zu chars", i, len);
        }
    }

    /* BUFFERS */
    if (s->query_len >= sizeof s->query || s->query[s->query_len] || strlen(s->query) != s->query_len)
        REPORT(w, "BUFFERS query", "query_len=%u strlen=%zu", s->query_len, strnlen(s->query, sizeof s->query));
    if (s->pass_len >= sizeof s->password || s->password[s->pass_len] || strlen(s->password) != s->pass_len)
        REPORT(w, "BUFFERS password", "pass_len=%u", s->pass_len);
    if (s->pending_len >= sizeof s->pending || s->pending[s->pending_len] || strlen(s->pending) != s->pending_len)
        REPORT(w, "BUFFERS pending", "pending_len=%u", s->pending_len);
    if (!nul_within(s->calc_buf, sizeof s->calc_buf)) REPORT(w, "BUFFERS calc_buf", "calc_buf unterminated");
    if (!nul_within(s->join_ssid, sizeof s->join_ssid) || !nul_within(s->link.ssid, sizeof s->link.ssid))
        REPORT(w, "BUFFERS ssid", "SSID unterminated");

    /* KEYS */
    if (s->view == KORTA_KEYBOARD) {
        bool flow = s->flow && !s->typing_pass && !s->typing_find;
        int cols = flow ? 3 : s->key_row == 0 ? 10 : s->key_row < 3 ? 9 : (!s->flow && !s->typing_pass && !s->typing_find) ? 4 : 3;
        if (s->key_row > 3 || s->key_col >= cols) REPORT(w, "KEYS cursor", "key %u,%u outside the layout", s->key_row, s->key_col);
        if (!s->typing_pass && !s->typing_find && !s->note_count) REPORT(w, "KEYS no note", "note keyboard open with no note to edit");
    }

    /* VOLUME */
    if (s->vol_mode > 2 || (s->vol_sel != 0 && s->vol_sel != 2)) REPORT(w, "VOLUME range", "vol_mode=%u vol_sel=%u", s->vol_mode, s->vol_sel);

    /* BRIGHT: level in range, override record matches the screen */
    if (s->hw.brightness < 1 || s->hw.brightness > 16) REPORT(w, "BRIGHT level range", "hw.brightness=%u", s->hw.brightness);
    if (m->hold_on != override_view(s->view)) {
        snprintf(key, sizeof key, "BRIGHT hold record %s in %s", m->hold_on ? "open" : "missing", vn);
        REPORT(w, key, "BRIGHT_HOLD record %s while view is %s", m->hold_on ? "still open" : "missing", vn);
    }
    if (!override_view(s->view) && s->hw.brightness != m->user_bright) {
        snprintf(key, sizeof key, "BRIGHT user level lost in %s", vn);
        REPORT(w, key, "brightness %u, user level %u", s->hw.brightness, m->user_bright);
    }

    /* FOCUS */
    int top = max_focus(m);
    if (s->focus > top) {
        snprintf(key, sizeof key, "FOCUS out of range in %s", vn);
        REPORT(w, key, "focus=%u but the list ends at %d", s->focus, top);
    }

    /* SELECTED */
    if (s->view == KORTA_HOME) {
        if (app_outcome(s) == OUT_NONE) {
            REPORT(w, "SELECTED empty tile", "selected=%u does not open an app", s->selected);
        }
    }

    /* RENDER (once per distinct screen projection) */
    if (render) {
        /* Layout-relevant fields only: one render per screen arrangement. */
        Hash h;
        h_init(&h);
        h_int(&h, w->seed);
        h_int(&h, s->view);
        h_int(&h, s->focus);
        h_int(&h, s->view == KORTA_HOME ? s->selected : 0);
        /* every key is always drawn; the cursor only moves the highlight */
        h_int(&h, s->flow | s->caps << 1 | s->symbols << 2 | s->typing_pass << 3 | s->typing_find << 4 | s->night << 5);
        h_int(&h, s->vol_mode);
        h_int(&h, s->note_count * 16 + s->note_cur);
        h_int(&h, bucket(s->pending_len, 31) * 16 + bucket(s->query_len, 10) * 4 + (s->pass_len ? 1 : 0));
        h_int(&h, s->note_count ? bucket(s->note_lens[s->note_cur], KORTA_NOTE_CAPACITY) : 0);
        h_int(&h, s->timer_run | s->timer_done << 1 | s->calc_err << 2 | s->calc_typing << 3 | s->time_known << 4);
        uint64_t k = h_done(&h);
        if (!map_get(&render_memo, k)) {
            map_put(&render_memo, k, 1);
            bool ok;
            int colors = render_colors(s, &ok, NULL);
            if (!ok) { snprintf(key, sizeof key, "RENDER guard in %s", vn); REPORT(w, key, "korta_render wrote outside 160x144"); }
            if (colors < 2 && s->view != KORTA_FLASH) { snprintf(key, sizeof key, "RENDER blank %s", vn); REPORT(w, key, "screen is a single color"); }
        }
    }

    /* B-HOME */
    int limit = 12 + s->pending_len + s->query_len + s->pass_len + (s->note_count ? s->note_lens[s->note_cur] : 0);
    KortaView stuck;
    int d = b_distance(m, w->seed, limit, &stuck);
    if (d < 0) {
        snprintf(key, sizeof key, "B-HOME %s from %s", d == -1 ? "loop" : "no Home", vn);
        if (d == -1) REPORT(w, key, "pressing B cycles without reaching Home (around %s)", view_name(stuck));
        else REPORT(w, key, "B pressed %d times does not reach Home (ends in %s)", limit, view_name(stuck));
    }
}

/* ------------------------------------------------------------------------ */
/* Transition invariants                                                      */
/* ------------------------------------------------------------------------ */

static int max_actions_seen;

static void check_edge(const Model *from, const Model *to, int sym, const StepLog *log, const Anomaly *an, const Where *w) {
    const KortaState *a = &from->s, *b = &to->s;
    const char *vn = view_name(a->view);
    char key[96];
    if (log->max_step > max_actions_seen) max_actions_seen = log->max_step;
    if (log->max_frame > max_actions_seen) max_actions_seen = log->max_frame;

    /* QUEUE */
    if (sym < 8 && log->max_step >= 7) {
        snprintf(key, sizeof key, "QUEUE full on %s in %s", SYM_NAME[sym], vn);
        REPORT(w, key, "one press produced %d actions; the ring holds 7 so later ones are dropped", log->max_step);
    }
    if (an->bad_bright_value || an->bad_hold_value) REPORT(w, "QUEUE value range", "brightness action outside 1..16");
    if (an->launch_no_cart) REPORT(w, "QUEUE launch without cart", "LAUNCH sent with no cartridge");

    /* BRIGHT */
    if (an->restore_wrong) {
        snprintf(key, sizeof key, "BRIGHT not restored leaving %s by %s", vn, SYM_NAME[sym]);
        REPORT(w, key, "%s -> %s left brightness %d; the user's level was %d", vn, view_name(b->view), an->restore_got, an->restore_want);
    }
    if (an->device_mismatch) {
        snprintf(key, sizeof key, "BRIGHT core/device mismatch %s by %s", vn, SYM_NAME[sym]);
        REPORT(w, key, "core thinks %u, last level sent to the device %u", b->hw.brightness, to->device_bright);
    }
    if (an->hold_twice || an->hold_unpaired || an->hold_order || an->hold_mismatch) {
        snprintf(key, sizeof key, "BRIGHT hold record %s->%s", vn, view_name(b->view));
        REPORT(w, key, "BRIGHT_HOLD: twice=%d unpaired=%d order=%d recorded=%d user=%d",
            an->hold_twice, an->hold_unpaired, an->hold_order, an->hold_got, an->hold_want);
    }

    /* HIDDEN */
    if (sym == SYM_HIDE && (override_view(b->view) || b->hw.brightness != to->user_bright)) {
        snprintf(key, sizeof key, "HIDDEN leaves %s", view_name(b->view));
        REPORT(w, key, "after korta_hidden view %s, brightness %u, user level %u", view_name(b->view), b->hw.brightness, to->user_bright);
    }

    /* RESCAN */
    if (sym == SYM_RESCAN && a->view == KORTA_LINKS && a->focus > 0) {
        int rows_a = 1 + a->link.count + a->link.saved;
        bool forget = a->link.saved && a->focus == rows_a - 1;
        bool ok = forget ? (int)b->focus == 1 + b->link.count + b->link.saved - 1
                         : b->focus > 0 && b->focus - 1 < b->link.count
                           && strcmp(b->link.nets[b->focus - 1].ssid, a->link.nets[a->focus - 1].ssid) == 0;
        if (!ok) REPORT(w, "RESCAN cursor moved", "cursor left %s after a rescan", forget ? "FORGET" : a->link.nets[a->focus - 1].ssid);
    }

    /* B-UNDO: an opening press is undone by one B, back to the same row. */
    bool opener = sym == KORTA_A || (sym == KORTA_START && b->view == KORTA_QUICK)
        || (sym == KORTA_SELECT && b->view == KORTA_FIND) || (sym == KORTA_LEFT && a->view == KORTA_HOME);
    if (opener && b->view != a->view && b->view != KORTA_HOME && !override_view(b->view) && b->view != KORTA_ARCADE
        && a->view != KORTA_KEYBOARD && a->view != KORTA_CLEAR_CONFIRM && !override_view(a->view)
        && !(sym == KORTA_START && a->view == KORTA_FIND) && !(sym == KORTA_SELECT && a->view == KORTA_QUICK)
        && b->vol_mode != 2) {
        bool text = b->view == KORTA_KEYBOARD
            && (b->pending_len || (b->typing_pass ? b->pass_len : b->typing_find ? b->query_len : (b->note_count ? b->note_lens[b->note_cur] : 0)));
        if (!text) {
            Model c = *to;
            StepLog l2;
            Anomaly a2;
            step(&c, KORTA_B, &l2, &a2);
            bool same_row = a->view == KORTA_HOME ? c.s.selected == a->selected : c.s.focus == a->focus;
            if (c.s.view != a->view || !same_row) {
                snprintf(key, sizeof key, "B-UNDO %s -%s-> %s", vn, SYM_NAME[sym], view_name(b->view));
                char suf[64];
                snprintf(suf, sizeof suf, "%s B", SYM_NAME[sym]);
                Where w2 = {w->seed, w->node, suf, w->path, w->path_n};
                REPORT(&w2, key, "%s row %u, %s opens %s, B lands on %s row %u (expected %s row %u)", vn,
                    a->view == KORTA_HOME ? a->selected : a->focus, SYM_NAME[sym], view_name(b->view),
                    view_name(c.s.view), a->view == KORTA_HOME ? c.s.selected : c.s.focus, vn,
                    a->view == KORTA_HOME ? a->selected : a->focus);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Holds: each button held 1 s through korta_frame (20 ms frames)             */
/* ------------------------------------------------------------------------ */

static void hold_button(Model *m, int b, StepLog *log) {
    memset(log, 0, sizeof *log);
    Anomaly an;
    KortaView v0 = m->s.view;
    uint32_t t = m->s.now_ms + 400;
    idle_frame(m, t, log);
    for (int k = 1; k <= 50; k++) { korta_frame(&m->s, (uint16_t)(1u << b), t + 20u * (uint32_t)k); drain(m, log); }
    idle_frame(m, t + 1040, log);
    model_after(m, log, v0, &an);
}

static void check_holds(const Model *m, int seed, int node, bool all_buttons) {
    for (int b = 0; b < 8; b++) {
        if (!all_buttons && b != KORTA_B) continue;
        Model c = *m;
        StepLog log;
        hold_button(&c, b, &log);
        holds++;
        char suf[32], key[96];
        snprintf(suf, sizeof suf, "(hold %s 1 s)", SYM_NAME[b]);
        Where w = {seed, node, suf, NULL, 0};
        const KortaState *a = &m->s, *s = &c.s;
        const char *vn = view_name(a->view);
        if (log.max_frame >= 7) {
            snprintf(key, sizeof key, "QUEUE full holding %s in %s", SYM_NAME[b], vn);
            REPORT(&w, key, "%d actions in one frame", log.max_frame);
        }
        bool ring = a->view == KORTA_ALARM_RING || a->view == KORTA_TIMER_RING;
        if (b == KORTA_B && ring) {
            /* Dismissing an alert must stop at the dismissal. */
            Model tap = *m;
            korta_press(&tap.s, KORTA_B);
            int lost = (a->note_count && s->note_count) ? (int)a->note_lens[a->note_cur] - (int)s->note_lens[s->note_cur] : 0;
            if (s->view != tap.s.view || lost > 0) {
                snprintf(key, sizeof key, "HOLD-B on %s goes past the dismissal%s", vn, lost > 0 ? ", erasing note text" : "");
                REPORT(&w, key, "holding B on %s ends in %s; one tap ends in %s%s", vn, view_name(s->view),
                    view_name(tap.s.view), lost > 0 ? " and text was erased" : "");
            }
        } else if (b == KORTA_B && a->view != KORTA_KEYBOARD && a->view != KORTA_ARCADE && s->view != KORTA_HOME) {
            int lost = (a->note_count && s->note_count) ? (int)a->note_lens[a->note_cur] - (int)s->note_lens[s->note_cur] : 0;
            snprintf(key, sizeof key, "HOLD-B stays in %s from %s%s", view_name(s->view), vn, lost > 0 ? ", erasing note text" : "");
            REPORT(&w, key, "holding B from %s ends in %s%s", vn, view_name(s->view),
                lost > 0 ? " and erased typed note text" : "");
        }
        if (b == KORTA_START && a->view != KORTA_KEYBOARD && a->view != KORTA_ARCADE && !override_view(a->view) && a->view != KORTA_FLASH
            && a->vol_mode != 2 && s->view != KORTA_FIND) {
            snprintf(key, sizeof key, "HOLD-START from %s", vn);
            REPORT(&w, key, "holding Start from %s ends in %s, not FIND", vn, view_name(s->view));
        }
        if (b == KORTA_SELECT && a->view != KORTA_KEYBOARD && a->view != KORTA_ARCADE && a->vol_mode != 2 && s->night == a->night) {
            snprintf(key, sizeof key, "HOLD-SELECT from %s", vn);
            REPORT(&w, key, "holding Select from %s did not toggle the theme once", vn);
        }
        check_state(&c, &w, true);
    }
}

/* ------------------------------------------------------------------------ */
/* Home grid (D-pad only)                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    bool seen[256];
    int16_t next[256][4];       /* D-pad successor tile, -1 if the press leaves Home */
    int16_t parent[256];
    uint8_t via[256];
    int order[256], n;
} HomeGraph;

static void press_copy(KortaState *c, int b) {
    KortaAction a;
    korta_press(c, (KortaButton)b);
    while (korta_take_action(c, &a)) {}
}

static void home_graph(const KortaState *root, HomeGraph *g) {
    memset(g, 0, sizeof *g);
    int head = 0;
    g->seen[root->selected] = true;
    g->parent[root->selected] = -1;
    g->order[g->n++] = root->selected;
    while (head < g->n) {
        int sel = g->order[head++];
        for (int b = 0; b < 4; b++) {
            KortaState c = *root;
            c.selected = (uint8_t)sel;  /* only ever a tile already reached by D-pad */
            press_copy(&c, b);
            g->next[sel][b] = c.view == KORTA_HOME ? c.selected : -1;
            if (c.view == KORTA_HOME && !g->seen[c.selected]) {
                g->seen[c.selected] = true;
                g->parent[c.selected] = (int16_t)sel;
                g->via[c.selected] = (uint8_t)b;
                g->order[g->n++] = c.selected;
            }
        }
    }
}
static int home_route(const HomeGraph *g, int sel, uint8_t *syms) {
    uint8_t rev[64];
    int n = 0;
    for (int at = sel; g->parent[at] >= 0 && n < 64; at = g->parent[at]) rev[n++] = g->via[at];
    for (int i = 0; i < n; i++) syms[i] = rev[n - 1 - i];
    return n;
}
static void report_route(const char *key, int seed, const HomeGraph *g, int sel, const char *suffix, const char *msg) {
    uint8_t syms[64];
    int n = home_route(g, sel, syms);
    report(key, seed, syms, n, suffix, "%s", msg);
}

static void check_home_grid(int seed) {
    const KortaState *root = &seeds[seed].root.s;
    if (root->view != KORTA_HOME) return;
    HomeGraph g;
    home_graph(root, &g);
    bool app_seen[OUT_LAUNCH + 1] = {false};
    int extra = root->game_count > 5 ? root->game_count - 5 : 0;
    int apps = 0, pages_needed = 2 + (extra + KORTA_PAGE_SIZE - 1) / KORTA_PAGE_SIZE;
    bool page_seen[8] = {false};
    char key[96], msg[200];
    for (int i = 0; i < g.n; i++) {
        int sel = g.order[i];
        KortaState c = *root;
        c.selected = (uint8_t)sel;
        int out = app_outcome(&c);
        if (out == OUT_NONE) {
            snprintf(key, sizeof key, "HOME-APPS empty tile %d", sel);
            snprintf(msg, sizeof msg, "D-pad reaches tile %d, which opens nothing", sel);
            report_route(key, seed, &g, sel, "", msg);
        } else if (out != KORTA_ARCADE && !app_seen[out]) { app_seen[out] = true; apps++; }
        if (sel / KORTA_PAGE_SIZE < 8) page_seen[sel / KORTA_PAGE_SIZE] = true;
    }
    if (apps != KORTA_APP_COUNT) {
        snprintf(msg, sizeof msg, "D-pad from Home reaches %d distinct apps, KORTA_APP_COUNT is %d", apps, KORTA_APP_COUNT);
        report("HOME-APPS count", seed, NULL, 0, "", "%s", msg);
    }
    for (int p = 0; p < pages_needed; p++) if (!page_seen[p]) {
        snprintf(key, sizeof key, "HOME-APPS page %d unreachable", p + 1);
        report(key, seed, NULL, 0, "", "no D-pad sequence reaches Home page %d", p + 1);
    }
    /* Page crossing from every tile: Right walks forward, Left walks back. */
    for (int i = 0; i < g.n; i++) {
        int sel = g.order[i], page = sel / KORTA_PAGE_SIZE;
        for (int dir = 0; dir < 2; dir++) {
            int b = dir == 0 ? KORTA_RIGHT : KORTA_LEFT;
            int want = dir == 0 ? page + 1 : page - 1;
            if (want < 0 || want >= pages_needed) continue;
            int at = sel, k = 0;
            while (k < 8 && at >= 0 && at / KORTA_PAGE_SIZE != want) { at = g.next[at][b]; k++; }
            if (at < 0 || at / KORTA_PAGE_SIZE != want) {
                snprintf(key, sizeof key, "HOME-PAGES %s from page %d row %d", SYM_NAME[b], page + 1, (sel % KORTA_PAGE_SIZE) / 3 + 1);
                snprintf(msg, sizeof msg, "pressing %s from page %d row %d col %d never reaches page %d%s", SYM_NAME[b],
                    page + 1, (sel % KORTA_PAGE_SIZE) / 3 + 1, sel % 3 + 1, want + 1, at < 0 ? " (leaves Home)" : "");
                char suf[48];
                snprintf(suf, sizeof suf, "%s x%d", SYM_NAME[b], k);
                report_route(key, seed, &g, sel, suf, msg);
            }
        }
    }
    /* Strong connectivity: every tile reaches every other tile. */
    for (int i = 0; i < g.n; i++) {
        bool reach[256] = {false};
        int q[256], qh = 0, qt = 0;
        q[qt++] = g.order[i];
        reach[g.order[i]] = true;
        while (qh < qt) {
            int at = q[qh++];
            for (int b = 0; b < 4; b++) {
                int nx = g.next[at][b];
                if (nx >= 0 && !reach[nx]) { reach[nx] = true; q[qt++] = nx; }
            }
        }
        for (int j = 0; j < g.n; j++) if (!reach[g.order[j]]) {
            snprintf(key, sizeof key, "HOME-SCC %d->%d", g.order[i], g.order[j]);
            snprintf(msg, sizeof msg, "from tile %d (page %d) no D-pad path reaches tile %d (page %d)", g.order[i],
                g.order[i] / KORTA_PAGE_SIZE + 1, g.order[j], g.order[j] / KORTA_PAGE_SIZE + 1);
            report_route(key, seed, &g, g.order[i], "", msg);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Settings hub and Find results (scripted from each seed)                    */
/* ------------------------------------------------------------------------ */

/* Drives a model with a symbol list, appending to a repro path. */
typedef struct { Model m; uint8_t path[MAX_PATH]; int n; } Script;
static void script_press(Script *sc, int sym) {
    StepLog log;
    Anomaly an;
    step(&sc->m, sym, &log, &an);
    if (sc->n < MAX_PATH) sc->path[sc->n++] = (uint8_t)sym;
}
static bool script_open_app(Script *sc, KortaView target) {
    HomeGraph g;
    home_graph(&sc->m.s, &g);
    for (int i = 0; i < g.n; i++) {
        KortaState c = sc->m.s;
        c.selected = (uint8_t)g.order[i];
        if (app_outcome(&c) != (int)target) continue;
        uint8_t syms[64];
        int n = home_route(&g, g.order[i], syms);
        for (int k = 0; k < n; k++) script_press(sc, syms[k]);
        script_press(sc, KORTA_A);
        return sc->m.s.view == target;
    }
    return false;
}

static void check_settings_hub(int seed) {
    Script base = {seeds[seed].root, {0}, 0};
    if (base.m.s.view != KORTA_HOME || !script_open_app(&base, KORTA_SETTINGS)) {
        report("SETTINGS unreachable", seed, base.path, base.n, "", "Settings app not found from Home");
        return;
    }
    bool ok;
    uint64_t hub_image;
    render_colors(&base.m.s, &ok, &hub_image);
    int rows = max_focus(&base.m) + 1;
    uint64_t images[32];
    int opened[32];
    char key[96];
    for (int r = 0; r < rows && r < 32; r++) {
        Script sc = base;
        for (int k = 0; k < r; k++) script_press(&sc, KORTA_DOWN);
        script_press(&sc, KORTA_A);
        const KortaState *s = &sc.m.s;
        opened[r] = s->view * 4 + s->typing_find * 2 + s->typing_pass;
        int colors = render_colors(s, &ok, &images[r]);
        if (s->view == KORTA_SETTINGS || colors < 2 || images[r] == hub_image) {
            snprintf(key, sizeof key, "SETTINGS row %d opens nothing", r);
            report(key, seed, sc.path, sc.n, "", "Settings row %d: view %s, %d colors", r, view_name(s->view), colors);
        }
        for (int q = 0; q < r; q++) if (opened[q] == opened[r]) {
            snprintf(key, sizeof key, "SETTINGS rows %d and %d same page", q, r);
            report(key, seed, sc.path, sc.n, "", "Settings rows %d and %d both open %s", q, r, view_name(s->view));
        }
        script_press(&sc, KORTA_B);
        if (sc.m.s.view != KORTA_SETTINGS || sc.m.s.focus != r) {
            snprintf(key, sizeof key, "SETTINGS row %d B", r);
            report(key, seed, sc.path, sc.n, "", "Settings row %d, B lands on %s row %u", r, view_name(sc.m.s.view), sc.m.s.focus);
        }
    }
}

/* Search results can sit deeper than the search horizon. On a result screen,
 * check the screen itself and A on each of its rows (with B-UNDO). */
static void check_inside(int seed, const Script *at) {
    Where w = {seed, -1, "", at->path, at->n};
    check_state(&at->m, &w, true);
    int rows = max_focus(&at->m);
    for (int r = 0; r <= rows && r < 40; r++) {
        Script sc = *at;
        for (int k = 0; k < r; k++) script_press(&sc, KORTA_DOWN);
        Model before = sc.m;
        StepLog log;
        Anomaly an;
        step(&sc.m, KORTA_A, &log, &an);
        Where we = {seed, -1, "A", sc.path, sc.n};
        check_edge(&before, &sc.m, KORTA_A, &log, &an, &we);
        if (sc.n < MAX_PATH) sc.path[sc.n++] = KORTA_A;
        Where ws = {seed, -1, "", sc.path, sc.n};
        check_state(&sc.m, &ws, true);
    }
}

static void check_find_list(int seed, Script *base, const char *entry) {
    char key[96];
    if (base->m.s.view != KORTA_FIND) {
        snprintf(key, sizeof key, "FIND not opened via %s", entry);
        report(key, seed, base->path, base->n, "", "%s did not open Find (view %s)", entry, view_name(base->m.s.view));
        return;
    }
    KortaView from = base->m.s.find_from;
    int results = max_focus(&base->m);
    if (results < 1) report("FIND no results", seed, base->path, base->n, "", "empty query lists no results");
    for (int i = 1; i <= results; i++) {
        Script sc = *base;
        for (int k = 0; k < i; k++) script_press(&sc, KORTA_DOWN);
        if (sc.m.s.focus != i) break;
        script_press(&sc, KORTA_A);
        const KortaState *s = &sc.m.s;
        bool ok;
        int colors = render_colors(s, &ok, NULL);
        if (s->view == KORTA_FIND || colors < 2) {
            snprintf(key, sizeof key, "FIND result %d opens nothing (%s)", i, entry);
            report(key, seed, sc.path, sc.n, "", "result %d leaves view %s", i, view_name(s->view));
            continue;
        }
        KortaView dest = s->view;
        check_inside(seed, &sc);
        script_press(&sc, KORTA_B);
        if (sc.m.s.view != KORTA_FIND || sc.m.s.focus != i) {
            snprintf(key, sizeof key, "FIND result %s B (%s)", view_name(dest), entry);
            report(key, seed, sc.path, sc.n, "", "result %d opens %s; B lands on %s row %u", i, view_name(dest),
                view_name(sc.m.s.view), sc.m.s.focus);
            continue;
        }
        script_press(&sc, KORTA_B);
        if (sc.m.s.view != from) {
            snprintf(key, sizeof key, "FIND B after result (%s)", entry);
            report(key, seed, sc.path, sc.n, "", "B from Find lands on %s, opened from %s", view_name(sc.m.s.view), view_name(from));
        }
    }
}

static void check_find(int seed) {
    if (seeds[seed].root.s.view != KORTA_HOME) return;
    Script sc = {seeds[seed].root, {0}, 0};
    /* Left from the first tile of page 1. */
    if (sc.m.s.selected % 3 == 0 && sc.m.s.selected < KORTA_PAGE_SIZE) {
        Script a = sc;
        script_press(&a, KORTA_LEFT);
        check_find_list(seed, &a, "Home Left");
    }
    Script b = sc;
    if (script_open_app(&b, KORTA_SETTINGS)) {
        script_press(&b, KORTA_SELECT);
        check_find_list(seed, &b, "Settings Select");
    }
}

/* ------------------------------------------------------------------------ */
/* Locations: views, with Home split into page/row                            */
/* ------------------------------------------------------------------------ */

enum { LOC_COUNT = KORTA_VIEW_COUNT + 6 };
static bool loc_edge[LOC_COUNT][LOC_COUNT];
static bool loc_expanded[LOC_COUNT];
static int loc_node[LOC_COUNT];

static int loc_of(const KortaState *s) {
    if (s->view != KORTA_HOME) return (int)s->view < KORTA_VIEW_COUNT ? (int)s->view : 0;
    int page = s->selected / KORTA_PAGE_SIZE;
    return KORTA_VIEW_COUNT + (page > 1 ? 1 : page) * 3 + (s->selected % KORTA_PAGE_SIZE) / 3;
}
static void loc_label(int l, char *out, size_t n) {
    if (l < KORTA_VIEW_COUNT) snprintf(out, n, "%s", view_name(l));
    else snprintf(out, n, "HOME p%d r%d", (l - KORTA_VIEW_COUNT) / 3 + 1, (l - KORTA_VIEW_COUNT) % 3 + 1);
}

static void check_locations(int seed) {
    static bool reach[LOC_COUNT][LOC_COUNT];
    for (int i = 0; i < LOC_COUNT; i++) for (int j = 0; j < LOC_COUNT; j++) reach[i][j] = i == j || loc_edge[i][j];
    for (int k = 0; k < LOC_COUNT; k++)
        for (int i = 0; i < LOC_COUNT; i++) if (reach[i][k])
            for (int j = 0; j < LOC_COUNT; j++) if (reach[k][j]) reach[i][j] = true;
    for (int i = 0; i < LOC_COUNT; i++) {
        if (!loc_expanded[i]) continue;
        for (int j = 0; j < LOC_COUNT; j++) {
            if (!loc_expanded[j] || reach[i][j]) continue;
            char a[24], b[24], key[96];
            loc_label(i, a, sizeof a);
            loc_label(j, b, sizeof b);
            snprintf(key, sizeof key, "LOCATIONS %s -> %s", a, b);
            uint8_t path[MAX_PATH];
            int n = node_path(loc_node[i], path);
            report(key, seed, path, n, "", "no explored sequence leads from %s to %s", a, b);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Seeds                                                                      */
/* ------------------------------------------------------------------------ */

static void base_status(Model *m, uint8_t bright, bool time_known, int hour, int min) {
    KortaStatus st = m->s.hw;
    st.brightness = bright;
    st.battery_known = true;
    st.battery_pct = 64;
    st.battery_mv = 3900;
    st.lipo = true;
    st.heap_kb = 112;
    snprintf(st.mcu, sizeof st.mcu, "0.13.4-korta4");
    snprintf(st.fpga, sizeof st.fpga, "18.8");
    snprintf(st.chromatic, sizeof st.chromatic, "4.2");
    snprintf(st.serial, sizeof st.serial, "CHR0012345");
    st.time_known = time_known;
    st.year = 2026; st.month = 10; st.day = 17;
    st.hour = (uint8_t)hour; st.min = (uint8_t)min; st.sec = 0;
    korta_set_status(&m->s, &st);
    m->device_bright = m->s.hw.brightness;
    m->user_bright = m->s.hw.brightness;
}
static void model_init(Model *m) {
    memset(m, 0, sizeof *m);
    korta_init(&m->s);
    m->device_bright = m->user_bright = m->s.hw.brightness;
}

static void seed_fresh(Model *m) { model_init(m); }

static void seed_alarm(Model *m) {
    model_init(m);
    korta_apply_prefs(&m->s, 2u | 8u, 0, 7, 0);   /* Flow keyboard, alarm on at 07:00 */
    base_status(m, 9, true, 6, 59);
    korta_load_note(&m->s, "HELLO WORLD");
    korta_load_note(&m->s, "CAF\xC3\xA9 AT 9 ~ {X}");
    char full[KORTA_NOTE_CAPACITY + 1];
    for (int i = 0; i < KORTA_NOTE_CAPACITY; i++) full[i] = (char)(i % 7 == 6 ? ' ' : 'A' + i % 26);
    full[KORTA_NOTE_CAPACITY] = '\0';
    korta_load_note(&m->s, full);
}

static void seed_games(Model *m) {
    model_init(m);
    static const char *const names[7] = {"SNAKE", "PONG", "TETRIS", "BREAKOUT", "MAZE", "INVADERS", "FLAPPY"};
    korta_set_games(&m->s, names, 7);
    korta_load_note(&m->s, "HI");
}
static void seed_full(Model *m) {
    model_init(m);
    korta_apply_prefs(&m->s, 1u | 4u, -5, 23, 30); /* night, QWERTY, 12-hour */
    KortaStatus st = m->s.hw;
    st.cart_in = false;
    st.brightness = 16;
    korta_set_status(&m->s, &st);
    m->device_bright = m->user_bright = m->s.hw.brightness;
    static const char *const text[] = {
        "ONE", "TWO WORDS", "THREE", "A LONGER NOTE THAT WRAPS OVER LINES", "5", "SIX", "SEVEN", "EIGHT"
    };
    for (int i = 0; i < KORTA_NOTE_SLOTS; i++) {
        char note[24];
        snprintf(note, sizeof note, "%s %d", text[i % 8], i + 1);
        korta_load_note(&m->s, note);
    }
    korta_load_pins(&m->s, 2);
}

static void add_net(KortaLink *l, const char *ssid, int8_t rssi, bool locked) {
    KortaNet *n = &l->nets[l->count++];
    snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
    n->rssi = rssi;
    n->locked = locked;
}
static void seed_wifi(Model *m) {
    model_init(m);
    base_status(m, 1, false, 0, 0);
    korta_load_note(&m->s, "WIFI PASSWORD IS ON THE ROUTER");
    KortaLink l;
    memset(&l, 0, sizeof l);
    l.phase = KORTA_WIFI_IDLE;
    l.saved = true;
    snprintf(l.ssid, sizeof l.ssid, "HOMENET");
    add_net(&l, "HOMENET", -50, true);
    add_net(&l, "CAFE FREE", -70, false);
    add_net(&l, "A-THIRTY-TWO-CHARACTER-NETWORK!!", -85, true);
    korta_set_link(&m->s, &l);
}

static void seed_online(Model *m) {
    model_init(m);
    base_status(m, 12, true, 23, 59);
    KortaStatus st = m->s.hw;
    st.gbc_game = true;
    korta_set_status(&m->s, &st);
    korta_load_note(&m->s, "X");
    korta_load_note(&m->s, "Y");
    KortaLink l;
    memset(&l, 0, sizeof l);
    l.phase = KORTA_WIFI_ONLINE;
    l.saved = true;
    l.rssi = -60;
    l.ip = 0x0A01A8C0u;
    snprintf(l.ssid, sizeof l.ssid, "OFFICE");
    add_net(&l, "OFFICE", -60, true);
    add_net(&l, "GUEST", -75, false);
    korta_set_link(&m->s, &l);
}

/* Countdown started from the Clock app, then back Home. Cart out. */
static void seed_timer(Model *m) {
    model_init(m);
    KortaStatus st = m->s.hw;
    st.cart_in = false;
    korta_set_status(&m->s, &st);
    m->device_bright = m->user_bright = m->s.hw.brightness;
    Script sc = {*m, {0}, 0};
    Seed *keep = setup_seed;
    setup_seed = NULL;
    bool ok = script_open_app(&sc, KORTA_TIME);
    setup_seed = keep;
    if (keep) for (int i = 0; i < sc.n && keep->prefix_n < MAX_PREFIX; i++) keep->prefix[keep->prefix_n++] = sc.path[i];
    *m = sc.m;
    if (!ok) return;
    StepLog log;
    Anomaly an;
    static const uint8_t rest[] = {KORTA_DOWN, KORTA_DOWN, KORTA_A, KORTA_A, KORTA_B, KORTA_B};
    for (size_t i = 0; i < sizeof rest; i++) step(m, rest[i], &log, &an);
}

/* ------------------------------------------------------------------------ */
/* Search                                                                     */
/* ------------------------------------------------------------------------ */

static Map visited;
static int max_depth = 12;
static int max_states = 180000;
static int hold_every = 16;
static bool verbose;
static long view_states[KORTA_VIEW_COUNT];

/* The frontier holds node ids only; a node's state is rebuilt by replaying its
 * path from the seed (deterministic, and far cheaper than storing 2.6 KB each). */
typedef struct { int32_t *id; int n, cap; } Frontier;
static void frontier_push(Frontier *f, int id) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 1024;
        f->id = realloc(f->id, (size_t)f->cap * sizeof *f->id);
        if (!f->id) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    f->id[f->n++] = id;
}
static void rebuild(int node, Model *out) {
    uint8_t path[MAX_PATH];
    int n = node_path(node, path);
    *out = seeds[nodes[node].seed].root;
    StepLog log;
    Anomaly an;
    for (int i = 0; i < n; i++) step(out, path[i], &log, &an);
}

static int new_node(int parent, int sym, int seed, int depth) {
    if (node_count == node_cap) {
        node_cap = node_cap ? node_cap * 2 : 65536;
        nodes = realloc(nodes, (size_t)node_cap * sizeof *nodes);
        if (!nodes) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    nodes[node_count] = (Node){parent, (uint8_t)sym, (uint8_t)seed, (uint8_t)depth};
    return node_count++;
}

typedef struct { int states, depth; bool capped; } SeedStats;

static SeedStats explore(int seed) {
    for (int i = 0; i < 256; i++) app_memo[i] = -2;  /* tiles differ between seeds (games) */
    SeedStats st = {0, 0, false};
    const Seed *sd = &seeds[seed];
    memset(loc_edge, 0, sizeof loc_edge);
    memset(loc_expanded, 0, sizeof loc_expanded);
    for (int i = 0; i < LOC_COUNT; i++) loc_node[i] = -1;
    Frontier cur = {0}, next = {0};
    int root = new_node(-1, 0, seed, 0);
    map_put(&visited, project(&sd->root, seed), root);
    Where w0 = {seed, root, "", NULL, 0};
    check_state(&sd->root, &w0, true);
    check_holds(&sd->root, seed, root, true);
    frontier_push(&cur, root);
    st.states = 1;
    for (int depth = 0; depth < max_depth && cur.n; depth++) {
        next.n = 0;
        st.depth = depth + 1;
        for (int i = 0; i < cur.n; i++) {
            int id = cur.id[i];
            Model from_state;
            rebuild(id, &from_state);
            const Model *from = &from_state;
            int la = loc_of(&from->s);
            loc_expanded[la] = true;
            if (loc_node[la] < 0) loc_node[la] = id;
            for (int sym = 0; sym < SYM_COUNT; sym++) {
                if (!applicable(from, sd, sym)) continue;
                Model to = *from;
                StepLog log;
                Anomaly an;
                step(&to, sym, &log, &an);
                edges++;
                Where we = {seed, id, SYM_NAME[sym], NULL, 0};
                check_edge(from, &to, sym, &log, &an, &we);
                loc_edge[la][loc_of(&to.s)] = true;
                uint64_t key = project(&to, seed);
                if (map_get(&visited, key)) continue;
                if (st.states >= max_states) { st.capped = true; continue; }
                int nid = new_node(id, sym, seed, depth + 1);
                map_put(&visited, key, nid);
                st.states++;
                if ((unsigned)to.s.view < KORTA_VIEW_COUNT) view_states[to.s.view]++;
                Where wn = {seed, nid, "", NULL, 0};
                check_state(&to, &wn, true);
                /* Hold B on every distinct cursor-free state; all buttons on a sample. */
                uint64_t hk = project_ex(&to, seed, false, false, true);
                bool all = nid % hold_every == 0;
                if (all || !map_get(&hold_memo, hk)) {
                    map_put(&hold_memo, hk, 1);
                    check_holds(&to, seed, nid, all);
                }
                if (depth + 1 < max_depth) frontier_push(&next, nid);
            }
        }
        Frontier t = cur;
        cur = next;
        next = t;
    }
    free(cur.id);
    free(next.id);
    check_locations(seed);
    return st;
}

int main(int argc, char **argv) {
    for (int i = 1, pos = 0; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = true;
        else if (pos++ == 0) max_depth = atoi(argv[i]);
        else max_states = atoi(argv[i]);
    }
    if (max_depth < 1) max_depth = 1;
    if (max_depth > MAX_PATH - 8) max_depth = MAX_PATH - 8;
    clock_t t0 = clock();
    render_buf = malloc((GUARD + PIXELS + GUARD) * sizeof *render_buf);
    if (!render_buf) return 2;
    map_init(&visited, 1u << 20);
    map_init(&focus_memo, 1u << 12);
    map_init(&bdist_memo, 1u << 20);
    map_init(&render_memo, 1u << 18);
    map_init(&hold_memo, 1u << 18);
    for (int i = 0; i < 256; i++) app_memo[i] = -2;

    static const struct { const char *name; void (*fn)(Model *); bool alarm, wait, wifi; } defs[] = {
        {"fresh: 0 notes, time unknown, Wi-Fi off", seed_fresh, false, false, false},
        {"3 notes (one 200 chars), clock 06:59, alarm 07:00 armed, Flow, bright 9", seed_alarm, true, false, false},
        {"16 notes (full, 2 pinned), night, QWERTY, 12h, no cart, bright 16", seed_full, false, false, false},
        {"Wi-Fi idle: 3 networks + saved, 1 note, bright 1", seed_wifi, false, false, true},
        {"Wi-Fi online, GBC game, clock 23:59, 2 notes, bright 12", seed_online, false, false, true},
        {"countdown running, no cart", seed_timer, false, true, false},
        {"7 installed games (3 Home pages)", seed_games, false, false, false},
    };
    seed_count = (int)(sizeof defs / sizeof defs[0]);
    for (int i = 0; i < seed_count; i++) {
        Seed *sd = &seeds[i];
        sd->name = defs[i].name;
        sd->setup = defs[i].fn;
        sd->alarm = defs[i].alarm;
        sd->wait = defs[i].wait;
        sd->wifi = defs[i].wifi;
        setup_seed = sd;
        sd->setup(&sd->root);
        setup_seed = NULL;
        drain(&sd->root, NULL);
        sd->root.hold_on = false;
    }

    printf("KortaMod model check: depth %d, at most %d states per seed, %d symbols\n", max_depth, max_states, SYM_COUNT);
    int total = 0;
    for (int i = 0; i < seed_count; i++) {
        for (int k = 0; k < 256; k++) app_memo[k] = -2;
        clock_t ts = clock();
        check_home_grid(i);
        check_settings_hub(i);
        check_find(i);
        SeedStats st = explore(i);
        total += st.states;
        printf("  seed %d %-72s %7d states, depth %d%s, %.1f s\n", i, seeds[i].name, st.states, st.depth,
            st.capped ? " (state cap hit)" : "", (double)(clock() - ts) / CLOCKS_PER_SEC);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("explored %d states, %ld transitions, %ld renders, %ld B-chains, %ld holds; most actions per step %d; %.1f s\n",
        total, edges, renders, bchains, holds, max_actions_seen, secs);
    if (verbose) {
        printf("states per view:");
        for (int v = 0; v < KORTA_VIEW_COUNT; v++) if (view_states[v]) printf(" %s %ld", view_name(v), view_states[v]);
        printf("\n");
    }
    printf("\ninvariants:\n");
    for (size_t i = 0; i < sizeof INVARIANTS / sizeof INVARIANTS[0]; i++) printf("  %s\n", INVARIANTS[i]);
    if (!viol_n) {
        printf("\nno violations\n");
        return 0;
    }
    printf("\n%d distinct violations (%ld occurrences):\n", viol_n, viol_total);
    for (int i = 0; i < viol_n; i++) {
        printf("\nVIOLATION %s  (x%ld)\n  %s\n  repro: %s\n", viol[i].key, viol[i].count, viol[i].msg, viol[i].seq);
    }
    return 1;
}
