#include "kortamod_core.h"
#include "kortamod_assets.h"
#include "kortamod_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HOLD_MS = 220, REPEAT_MS = 110, REPEAT_MIN_MS = 36 };

static const char *const APP_NAME[KORTA_APP_COUNT] = {
    "GAME", "NOTES", "CLOCK", "CALC", "FILES", "SETTINGS",
    "DICE", "COIN", "COUNT", "FLASH", "CONV", "COLOR", "PHONE"
};
static const KortaView APP_VIEW[KORTA_APP_COUNT] = {
    KORTA_PLAY, KORTA_NOTES, KORTA_TIME, KORTA_CALC, KORTA_FILES, KORTA_SETTINGS,
    KORTA_DICE, KORTA_COIN, KORTA_COUNT, KORTA_FLASH, KORTA_CONV, KORTA_COLOR, KORTA_PHONE
};
static const uint8_t APP_ICON[KORTA_APP_COUNT] = {
    9, 0, 1, 4, 6, 7, 12, 13, 14, 15, 16, 17, 20
};
static const int8_t HOME_APP[2][KORTA_PAGE_SIZE] = {
    {0, 1, 2, 3, 4, 5, 9, 6, 7},
    {8, 10, 11, 12, -1, -1, -1, -1, -1}
};
/* Installed games follow the built-in apps: the free tiles of page 2, then
 * further pages. A game's id is GAME_ID + its index. */
enum { GAME_ID = 100, FREE_ON_PAGE2 = 5 };
static int home_pages(const KortaState *s) {
    int extra = s->game_count > FREE_ON_PAGE2 ? s->game_count - FREE_ON_PAGE2 : 0;
    return 2 + (extra + KORTA_PAGE_SIZE - 1) / KORTA_PAGE_SIZE;
}
static int home_at(const KortaState *s, int page, int slot) {
    if (page < 0 || page >= home_pages(s) || slot < 0 || slot >= KORTA_PAGE_SIZE) return -1;
    int g = -1;
    if (page < 2) {
        if (HOME_APP[page][slot] >= 0) return HOME_APP[page][slot];
        if (page == 1) g = slot - (KORTA_PAGE_SIZE - FREE_ON_PAGE2);
    } else g = FREE_ON_PAGE2 + (page - 2) * KORTA_PAGE_SIZE + slot;
    return g >= 0 && g < s->game_count ? GAME_ID + g : -1;
}
static int home_app(const KortaState *s) {
    return home_at(s, s->selected / KORTA_PAGE_SIZE, s->selected % KORTA_PAGE_SIZE);
}
static bool is_game(int app) { return app >= GAME_ID; }
typedef struct { const char *name; KortaView view; } FindItem;
/* Apps and settings under the words people search for, not only screen titles. */
static const FindItem FIND_ITEM[] = {
    {"NOTES", KORTA_NOTES}, {"CLOCK", KORTA_TIME}, {"ALARM", KORTA_ALARM},
    {"TIMER", KORTA_TIMER}, {"STOPWATCH", KORTA_TIMER}, {"CALCULATOR", KORTA_CALC},
    {"FILES", KORTA_FILES}, {"GAME", KORTA_PLAY}, {"FLASHLIGHT", KORTA_FLASH},
    {"DICE", KORTA_DICE}, {"COIN", KORTA_COIN}, {"COUNTER", KORTA_COUNT},
    {"CONVERTER", KORTA_CONV}, {"GB PALETTE", KORTA_COLOR},
    {"SETTINGS", KORTA_SETTINGS}, {"WIFI", KORTA_LINKS}, {"DISPLAY", KORTA_SETUP},
    {"BRIGHTNESS", KORTA_SETUP}, {"THEME", KORTA_SETUP}, {"SOUND", KORTA_SOUND},
    {"VOLUME", KORTA_SOUND}, {"SILENT", KORTA_SOUND}, {"CONTROLS", KORTA_CONTROLS},
    {"HOT KEYS", KORTA_CONTROLS}, {"KEYBOARD", KORTA_CONTROLS}, {"POWER", KORTA_POWER},
    {"BATTERY", KORTA_POWER}, {"DATE AND TIME", KORTA_CLOCK}, {"TIME ZONE", KORTA_CLOCK},
    {"ABOUT", KORTA_INFO}, {"SERIAL NUMBER", KORTA_INFO}, {"PLAYER NUMBER", KORTA_INFO},
    {"VERSION", KORTA_INFO}, {"PHONE LINK", KORTA_PHONE}, {"COMPANION APP", KORTA_PHONE}
};
enum { FIND_COUNT = (int)(sizeof FIND_ITEM / sizeof FIND_ITEM[0]) };
/* Row counts of the Settings pages (Display, Controls, About). */
enum { DISPLAY_ROWS = 7, DISPLAY_PALETTE_ROW = 2, CONTROL_ROWS = 2, INFO_ROWS = 9, INFO_PLAYER_ROW = 5 };
static const char *const GROUPS[8] = {"ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"};
static const char *const WORDS[] = {
    "HELLO", "WORLD", "HOME", "NOTE", "GAME", "CHROMATIC", "KORTAMOD", "PLAY", "TIME", "GOOD",
    "THE", "AND", "YOU", "I", "A", "TO", "IN", "ON", "FOR", "WITH", "THIS", "THAT",
    "YES", "NO", "NOW", "LATER", "TODAY", "TOMORROW", "THANKS", "PLEASE", "MEET", "AT"
};
static const char *const PALETTE_NAME[15] = {
    "DEFAULT", "BROWN", "BLUE", "PASTEL", "GREEN", "RED", "DKBLUE", "ORANGE",
    "DKGREEN", "DKBROWN", "GRAY", "YELLOW", "NEGATIVE", "DMG 1", "DMG 2"
};
static const char *const MONTHS[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* mark: ink for symbols drawn on the pale hi surface (dice faces). */
typedef struct { uint16_t bg, ink, muted, card, band, band_ink, accent, line, hi, mark; } Palette;
typedef struct { uint16_t *pixels; Palette p; } Painter;

static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static Palette palette(bool night) {
    if (night) {
        return (Palette){
            rgb(18, 32, 34), rgb(236, 228, 204), rgb(150, 168, 156), rgb(42, 68, 66),
            rgb(10, 22, 24), rgb(236, 228, 204), rgb(232, 146, 92), rgb(70, 96, 92), rgb(255, 214, 160),
            rgb(150, 52, 36)
        };
    }
    return (Palette){
        rgb(236, 228, 204), rgb(28, 40, 38), rgb(96, 108, 100), rgb(255, 250, 236),
        rgb(28, 40, 38), rgb(244, 236, 210), rgb(168, 72, 48), rgb(196, 186, 160), rgb(255, 252, 244),
        rgb(168, 72, 48)
    };
}
static void fill(Painter *d, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > KORTA_WIDTH) w = KORTA_WIDTH - x;
    if (y + h > KORTA_HEIGHT) h = KORTA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = d->pixels + yy * KORTA_WIDTH + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static void border(Painter *d, int x, int y, int w, int h, uint16_t c, int t) {
    fill(d, x, y, w, t, c);
    fill(d, x, y + h - t, w, t, c);
    fill(d, x, y, t, h, c);
    fill(d, x + w - t, y, t, h, c);
}
static int text_width(const char *s) { return (s && s[0]) ? (int)strlen(s) * 6 - 1 : 0; }
static void draw_text(Painter *d, const char *s, int x, int y, uint16_t color) {
    if (!s) return;
    for (size_t i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 128) ch = '?';
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (korta_font[ch][row] & (1u << (4 - col))) fill(d, x + (int)i * 6 + col, y + row, 1, 1, color);
            }
        }
    }
}
static void draw_scaled(Painter *d, const char *s, int x, int y, uint16_t color, int scale) {
    if (!s || scale < 1) return;
    int step = 5 * scale + scale;
    for (size_t i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 128) ch = '?';
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (korta_font[ch][row] & (1u << (4 - col))) {
                    fill(d, x + (int)i * step + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}
static int scaled_width(const char *s, int scale) {
    if (!s || !s[0]) return 0;
    int step = 5 * scale + scale;
    return (int)strlen(s) * step - scale;
}
static void centered(Painter *d, const char *s, int cx, int y, uint16_t color) {
    draw_text(d, s, cx - text_width(s) / 2, y, color);
}
static int label_width(const char *s) { return (s && s[0]) ? (int)strlen(s) * 5 : 0; }
static void draw_label(Painter *d, const char *s, int x, int y, uint16_t color) {
    if (!s) return;
    for (size_t i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 128) ch = '?';
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (korta_font[ch][row] & (1u << (4 - col))) fill(d, x + (int)i * 5 + col, y + row, 1, 1, color);
            }
        }
    }
}
static void centered_label(Painter *d, const char *s, int cx, int y, uint16_t color) {
    draw_label(d, s, cx - label_width(s) / 2, y, color);
}
static void put_px(Painter *d, int x, int y, uint16_t c) {
    if ((unsigned)x >= KORTA_WIDTH || (unsigned)y >= KORTA_HEIGHT) return;
    d->pixels[y * KORTA_WIDTH + x] = c;
}
static uint16_t app_color(int app) {
    static const unsigned char tint[][3] = {
        {64, 130, 210}, {230, 120, 40}, {50, 160, 90}, {150, 90, 200},
        {20, 160, 170}, {210, 60, 70}, {80, 110, 190}, {90, 100, 120},
        {200, 140, 40}, {240, 180, 50}, {40, 140, 200}, {180, 80, 160},
        {220, 90, 50}, {70, 150, 180}, {120, 90, 70}, {50, 110, 190},
        {200, 70, 90}, {40, 150, 120},
    };
    if (app < 0 || app >= KORTA_APP_COUNT) return rgb(48, 128, 118);
    return rgb(tint[app][0], tint[app][1], tint[app][2]);
}
static void blit_app(Painter *d, int icon, int x, int y, uint16_t tile) {
    uint16_t mark = rgb(255, 252, 244);
    for (int yy = 0; yy < 24; yy++) {
        for (int xx = 0; xx < 24; xx++) {
            if ((xx == 0 || xx == 23) && (yy == 0 || yy == 23)) continue;
            put_px(d, x + xx, y + yy, tile);
        }
    }
    if (icon < 0 || icon >= KORTA_ICON_COUNT) return;
    for (int yy = 0; yy < 16; yy++) {
        uint32_t row = korta_icon_rows[icon][yy];
        for (int xx = 0; xx < 16; xx++) {
            if (((row >> (30 - 2 * xx)) & 3u) == 0) continue;
            put_px(d, x + 4 + xx, y + 4 + yy, mark);
        }
    }
}

static bool use_flow(const KortaState *s) { return s->flow && !s->typing_pass && !s->typing_find; }
static bool text_mode(const KortaState *s) { return s->view == KORTA_KEYBOARD; }

static void push(KortaState *s, KortaActionKind kind, uint16_t value) {
    uint8_t next = (uint8_t)((s->qt + 1) & 7);
    if (next == s->qh) return;
    s->queue[s->qt].kind = kind;
    s->queue[s->qt].value = value;
    s->qt = next;
}
static int list_count(const KortaState *s);
/* Views that interrupt whatever was on screen and return to it when dismissed. */
static bool is_alert(KortaView v) { return v == KORTA_ALARM_RING || v == KORTA_TIMER_RING || v == KORTA_MESSAGE; }
static char *cur_note(KortaState *s) { return s->notes[s->note_cur]; }
static uint8_t cur_len(const KortaState *s) { return s->note_count ? s->note_lens[s->note_cur] : 0; }
static void delete_note(KortaState *s, int index) {
    if (index < 0 || index >= s->note_count) return;
    for (int i = index; i + 1 < s->note_count; i++) {
        memcpy(s->notes[i], s->notes[i + 1], sizeof s->notes[i]);
        s->note_lens[i] = s->note_lens[i + 1];
    }
    if (index < s->note_pinned) s->note_pinned--;
    s->note_count--;
    memset(s->notes[s->note_count], 0, sizeof s->notes[0]);
    s->note_lens[s->note_count] = 0;
    if (s->note_cur >= s->note_count) s->note_cur = s->note_count ? (uint8_t)(s->note_count - 1) : 0;
    s->note_rev++;
}
/* Moves one note to another position, shifting those between. */
static void move_note(KortaState *s, int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= s->note_count || to >= s->note_count) return;
    char text[KORTA_NOTE_CAPACITY + 1];
    uint8_t len = s->note_lens[from];
    memcpy(text, s->notes[from], sizeof text);
    int step = to > from ? 1 : -1;
    for (int i = from; i != to; i += step) {
        memcpy(s->notes[i], s->notes[i + step], sizeof s->notes[i]);
        s->note_lens[i] = s->note_lens[i + step];
    }
    memcpy(s->notes[to], text, sizeof text);
    s->note_lens[to] = len;
}
/* Newest first, below the pinned notes. */
static bool new_note(KortaState *s) {
    if (s->note_count >= KORTA_NOTE_SLOTS) return false;
    int at = s->note_pinned;
    for (int i = s->note_count; i > at; i--) {
        memcpy(s->notes[i], s->notes[i - 1], sizeof s->notes[i]);
        s->note_lens[i] = s->note_lens[i - 1];
    }
    memset(s->notes[at], 0, sizeof s->notes[0]);
    s->note_lens[at] = 0;
    s->note_count++;
    s->note_cur = (uint8_t)at;
    s->note_rev++;
    return true;
}
static bool note_is_pinned(const KortaState *s) { return s->note_cur < s->note_pinned; }
/* Pinning moves a note to the end of the pinned block; unpinning to the top of the rest. */
static void toggle_pin(KortaState *s) {
    if (!s->note_count) return;
    if (note_is_pinned(s)) {
        move_note(s, s->note_cur, s->note_pinned - 1);
        s->note_pinned--;
        s->note_cur = s->note_pinned;
    } else {
        move_note(s, s->note_cur, s->note_pinned);
        s->note_cur = s->note_pinned;
        s->note_pinned++;
    }
    s->note_rev++;
}
static void set_view(KortaState *s, KortaView view) {
    if (s->view == KORTA_CLOCK && view != KORTA_CLOCK) s->clock_dirty = false;
    /* Return paths borrowed from Find, Files or Settings end where they lead. */
    if (view == KORTA_HOME) {
        s->find_dest_active = false;
        s->notes_from_files = false;
        s->via_settings = false;
        s->search_direct = false;
    }
    if (view == KORTA_NOTES) s->notes_from_files = false;
    if (view == KORTA_COLOR && s->view != KORTA_SETUP) s->color_back = KORTA_HOME;
    bool ring_over_keys = is_alert(s->view) && s->ring_from == KORTA_KEYBOARD;
    if ((s->view == KORTA_KEYBOARD || ring_over_keys) && view != KORTA_KEYBOARD && !is_alert(view)
        && !s->typing_pass && !s->typing_find && s->note_count && !cur_len(s)) {
        delete_note(s, s->note_cur);
        if (view == KORTA_NOTE_OPEN) view = s->notes_from_files ? KORTA_FILES : KORTA_NOTES;
        if (view == KORTA_FILES) s->notes_from_files = false;
    }
    /* A stale return path (Find, a ring) can point at a note that is gone. */
    if ((view == KORTA_NOTE_OPEN || view == KORTA_CLEAR_CONFIRM) && !s->note_count) view = KORTA_NOTES;
    /* Flash and ringing alerts override the backlight. The user's level is kept
     * once and returned when no overriding view remains. */
    bool boost_was = s->view == KORTA_FLASH || s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING
        || (s->view == KORTA_MESSAGE && s->msg_find);
    bool boost_now = view == KORTA_FLASH || view == KORTA_ALARM_RING || view == KORTA_TIMER_RING
        || (view == KORTA_MESSAGE && s->msg_find);
    if (boost_now && !boost_was) {
        s->bright_keep = s->hw.brightness;
        push(s, KORTA_ACT_BRIGHT_HOLD, s->bright_keep);
    }
    if (view == KORTA_FLASH && s->view != KORTA_FLASH) {
        /* Back from an alert, Flash resumes at its own level. */
        bool resume = boost_was || s->view == KORTA_QUICK || s->view == KORTA_FIND;
        if (!resume || !s->flash_level) s->flash_level = 16;
        s->flash_mark = s->now_ms;
        s->flash_hud_until = s->now_ms + 3000u;
    }
    uint8_t level = view == KORTA_FLASH ? s->flash_level : boost_now ? 16
        : boost_was ? (s->bright_keep ? s->bright_keep : 7) : s->hw.brightness;
    if (level != s->hw.brightness || (boost_now != boost_was)) {
        s->hw.brightness = level;
        push(s, KORTA_ACT_BRIGHTNESS, level);
    }
    if (boost_was && !boost_now) push(s, KORTA_ACT_BRIGHT_HOLD, 0);
    bool keep_query = view == KORTA_FIND
        && (s->find_restore || s->restoring || (s->view == KORTA_KEYBOARD && s->typing_find));
    if ((s->view == KORTA_KEYBOARD || ring_over_keys) && view != KORTA_KEYBOARD && !is_alert(view)) {
        if (s->typing_pass) s->caps = true;
        s->typing_pass = false;
        s->typing_find = false;
        s->pending_len = 0;
        s->pending[0] = '\0';
    }
    if (view == KORTA_FIND && !keep_query) {
        s->query_len = 0;
        s->query[0] = '\0';
        s->letter = 0;
    }
    if (s->view < KORTA_VIEW_COUNT) s->back_focus[s->view] = s->focus;
    bool restoring = s->restoring;
    s->restoring = false;
    KortaView from = s->view;
    s->view = view;
    /* The radio parks when idle; opening Wi-Fi asks for a fresh list. */
    if (view == KORTA_LINKS && from != KORTA_LINKS && s->link.phase != KORTA_WIFI_OFF
        && s->link.phase != KORTA_WIFI_JOIN && s->link.phase != KORTA_WIFI_ONLINE) push(s, KORTA_ACT_WIFI_SCAN, 0);
    uint8_t focus = 0;
    if (restoring && view < KORTA_VIEW_COUNT) {
        /* Back to a screen: the cursor returns to the row that was left. */
        int n = list_count(s);
        focus = s->back_focus[view];
        if (n > 0 && focus >= n) focus = (uint8_t)(n - 1);
        else if (n == 0 && view != KORTA_CALC) focus = 0;
    }
    if (from == KORTA_NOTE_OPEN && s->note_count) {
        if (view == KORTA_NOTES) focus = (uint8_t)(s->note_cur + 1);
        else if (view == KORTA_FILES) focus = s->note_cur;
    }
    s->focus = focus;
    s->key_row = 0;
    s->key_col = 0;
}
/* Navigation that returns somewhere rather than opening it fresh. */
static void return_to(KortaState *s, KortaView view) {
    s->restoring = true;
    set_view(s, view);
}
static void append_note(KortaState *s, char c) {
    if (!s->note_count) return;
    uint8_t *len = &s->note_lens[s->note_cur];
    if (*len < KORTA_NOTE_CAPACITY) {
        cur_note(s)[(*len)++] = c;
        cur_note(s)[*len] = '\0';
        s->note_rev++;
    }
}
static void erase_note(KortaState *s) {
    if (!s->note_count) return;
    uint8_t *len = &s->note_lens[s->note_cur];
    if (*len) {
        cur_note(s)[--(*len)] = '\0';
        s->note_rev++;
    }
}
static void append_char(KortaState *s, char c) {
    if (s->typing_pass) {
        if (s->pass_len < 63) {
            s->password[s->pass_len++] = c;
            s->password[s->pass_len] = '\0';
        }
        return;
    }
    if (s->typing_find) {
        if (c == ' ') return;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (s->query_len < 10) {
            s->query[s->query_len++] = c;
            s->query[s->query_len] = '\0';
        }
        return;
    }
    append_note(s, c);
}
static void erase_char(KortaState *s) {
    if (s->typing_find) {
        if (s->query_len) s->query[--s->query_len] = '\0';
        return;
    }
    if (s->typing_pass) {
        if (s->pass_len) s->password[--s->pass_len] = '\0';
        return;
    }
    erase_note(s);
}
static int group_of(char c) {
    for (int g = 0; g < 8; g++) if (strchr(GROUPS[g], c)) return g;
    return -1;
}
static bool code_matches(const char *word, const char *code, int len) {
    for (int i = 0; i < len; i++) if (!word[i] || group_of(word[i]) != code[i] - '0') return false;
    return true;
}
static const char *candidate(const KortaState *s) {
    if (!s->pending_len) return "";
    unsigned exact = 0;
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        if ((int)strlen(WORDS[i]) == s->pending_len && code_matches(WORDS[i], s->pending, s->pending_len)) {
            if (exact++ == s->candidate) return WORDS[i];
        }
    }
    return NULL;
}
static unsigned candidate_count(const KortaState *s) {
    unsigned count = 0;
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        if ((int)strlen(WORDS[i]) == s->pending_len && code_matches(WORDS[i], s->pending, s->pending_len)) count++;
    }
    return count;
}
static char apply_case(const KortaState *s, char c) {
    if (!s->caps && c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}
static void accept(KortaState *s, bool space) {
    if (s->pending_len) {
        const char *word = candidate(s);
        if (word) {
            for (int i = 0; word[i] && i < s->pending_len; i++) append_note(s, apply_case(s, word[i]));
        } else {
            for (int i = 0; i < s->pending_len; i++) append_note(s, apply_case(s, GROUPS[s->pending[i] - '0'][0]));
        }
        s->pending_len = 0;
        s->pending[0] = '\0';
        s->candidate = 0;
    }
    if (space) append_note(s, ' ');
}

/* Notes typed in QWERTY get a FLOW key back to predictive typing; passwords and
 * search are always QWERTY, so their bottom row keeps three keys. */
static bool flow_key(const KortaState *s) { return !s->flow && !s->typing_pass && !s->typing_find; }
static int exact_count(const KortaState *s, int row) {
    return row == 0 ? 10 : row == 1 ? 9 : row == 2 ? 9 : flow_key(s) ? 4 : 3;
}
static int key_x(const KortaState *s, int row, int col) {
    if (use_flow(s)) return 4 + col * 52;
    if (row == 0) return col * 16;
    if (row == 1) return 8 + col * 16;
    if (row == 2) return col == 0 ? 1 : col == 8 ? 138 : 25 + (col - 1) * 16;
    if (flow_key(s)) return col == 0 ? 2 : col == 1 ? 32 : col == 2 ? 88 : 110;
    return col == 0 ? 2 : col == 1 ? 88 : 110;
}
static int key_w(const KortaState *s, int row, int col) {
    if (use_flow(s)) return 48;
    if (row < 2) return 15;
    if (row == 2) return col == 0 ? 22 : col == 8 ? 21 : 15;
    if (flow_key(s)) return col == 0 ? 28 : col == 1 ? 54 : col == 2 ? 20 : 48;
    return col == 0 ? 84 : col == 1 ? 20 : 48;
}
static const char *key_label(const KortaState *s, int row, int col, char out[8]) {
    static const char *const flow_keys[4][3] = {
        {"ABC", "DEF", "GHI"}, {"JKL", "SPACE", "MNO"}, {"PQRS", "TUV", "WXYZ"}, {"NEXT", "QWERTY", "DONE"}
    };
    if (use_flow(s)) {
        if (row == 3 && col == 0 && !s->pending_len) return s->caps ? "AB" : "ab";
        return flow_keys[row][col];
    }
    const char *top = s->symbols ? "1234567890" : "QWERTYUIOP";
    const char *mid = s->symbols ? ",?!_-/+.*" : "ASDFGHJKL";
    const char *low = s->symbols ? "()@#$%&" : "ZXCVBNM";
    if (row < 2) { out[0] = (row == 0 ? top : mid)[col]; out[1] = '\0'; return out; }
    if (row == 2) {
        if (col == 0) return s->symbols ? "ABC" : "123";
        if (col == 8) return "BK";
        out[0] = low[col - 1];
        out[1] = '\0';
        return out;
    }
    if (flow_key(s)) {
        if (col == 0) return "FLOW";
        col--;
    }
    if (col == 1) return s->caps ? "AB" : "ab";
    return col == 0 ? "SPACE" : "DONE";
}
static void finish_pass(KortaState *s) {
    s->wifi_keep = true;
    push(s, KORTA_ACT_WIFI_JOIN, 0);
    push(s, KORTA_ACT_PREFS, 0);
    set_view(s, KORTA_LINKS);
}
static void finish_note(KortaState *s) {
    accept(s, false);
    push(s, KORTA_ACT_PREFS, 0);
    set_view(s, KORTA_NOTE_OPEN);
}
static int matches(const KortaState *s, uint8_t *ids);
static void finish_find(KortaState *s) {
    bool direct = s->search_direct;
    s->search_direct = false;
    if (direct && !s->query_len) { return_to(s, s->find_from); return; }
    set_view(s, KORTA_FIND);
    uint8_t ids[FIND_COUNT];
    if (s->query_len && matches(s, ids)) s->focus = 1;
}
static void open_search_keys(KortaState *s) {
    set_view(s, KORTA_KEYBOARD);
    s->typing_find = true;
    s->typing_pass = false;
    s->symbols = false;
    s->caps = true;
}
static void key_action(KortaState *s) {
    char one[8];
    const char *label = key_label(s, s->key_row, s->key_col, one);
    if (strcmp(label, "AB") == 0 || strcmp(label, "ab") == 0) {
        s->caps = !s->caps;
        return;
    }
    if (use_flow(s)) {
        for (int g = 0; g < 8; g++) if (strcmp(label, GROUPS[g]) == 0) {
            if (s->pending_len < (int)sizeof s->pending - 1 && cur_len(s) + s->pending_len < KORTA_NOTE_CAPACITY) {
                s->pending[s->pending_len++] = (char)('0' + g);
                s->pending[s->pending_len] = '\0';
                s->candidate = 0;
            }
            return;
        }
        if (strcmp(label, "NEXT") == 0) {
            unsigned n = candidate_count(s);
            if (n) s->candidate = (uint8_t)((s->candidate + 1) % n);
            return;
        }
        if (strcmp(label, "QWERTY") == 0) {
            if (s->pending_len) accept(s, false);
            s->flow = false;
            s->caps = false;
            s->key_row = s->key_col = 0;
            push(s, KORTA_ACT_PREFS, 0);
            return;
        }
        if (strcmp(label, "SPACE") == 0) { accept(s, true); return; }
        if (strcmp(label, "DONE") == 0) { finish_note(s); return; }
    }
    if (strcmp(label, "FLOW") == 0) {
        s->flow = true;
        s->caps = true;
        s->symbols = false;
        s->key_row = s->key_col = 0;
        push(s, KORTA_ACT_PREFS, 0);
        return;
    }
    if (strcmp(label, "123") == 0 || strcmp(label, "ABC") == 0) { s->symbols = !s->symbols; return; }
    if (strcmp(label, "BK") == 0) { erase_char(s); return; }
    if (strcmp(label, "SPACE") == 0) { append_char(s, ' '); return; }
    if (strcmp(label, "DONE") == 0) {
        if (s->typing_pass) finish_pass(s);
        else if (s->typing_find) finish_find(s);
        else finish_note(s);
        return;
    }
    if (label[0]) append_char(s, apply_case(s, label[0]));
}
static void move_key(KortaState *s, KortaButton b) {
    int row = s->key_row, col = s->key_col;
    if (b == KORTA_LEFT || b == KORTA_RIGHT) {
        int max = use_flow(s) ? 3 : exact_count(s, row);
        col += b == KORTA_LEFT ? -1 : 1;
        if (col < 0) col = max - 1;
        if (col >= max) col = 0;
    } else {
        int cx = key_x(s, row, col) + key_w(s, row, col) / 2;
        row += b == KORTA_UP ? -1 : 1;
        if (row < 0) row = 3;
        if (row > 3) row = 0;
        int best = 10000;
        int max = use_flow(s) ? 3 : exact_count(s, row);
        for (int i = 0; i < max; i++) {
            int center = key_x(s, row, i) + key_w(s, row, i) / 2;
            int dist = center > cx ? center - cx : cx - center;
            if (dist < best) { best = dist; col = i; }
        }
    }
    s->key_row = (uint8_t)row;
    s->key_col = (uint8_t)col;
}

static int days_in(int year, int month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    int n = days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) n = 29;
    return n;
}
static int matches(const KortaState *s, uint8_t *ids) {
    int n = 0;
    for (int i = 0; i < FIND_COUNT; i++) {
        bool alias = FIND_ITEM[i].view == KORTA_PLAY && s->query_len
            && (strstr("CART", s->query) || strstr("PLAY", s->query));
        if (!s->query_len || strstr(FIND_ITEM[i].name, s->query) || alias) ids[n++] = (uint8_t)i;
    }
    return n;
}
static KortaView parent_of(const KortaState *s) {
    switch (s->view) {
        case KORTA_CLOCK:
            return s->via_settings ? KORTA_SETTINGS : KORTA_TIME;
        case KORTA_ALARM:
        case KORTA_TIMER:
        case KORTA_TIMER_RING:
            return KORTA_TIME;
        case KORTA_MESSAGE:
            return s->ring_from;
        case KORTA_KEYBOARD:
            if (s->typing_pass) return KORTA_LINKS;
            if (s->typing_find) return KORTA_FIND;
            return KORTA_NOTE_OPEN;
        case KORTA_CLEAR_CONFIRM:
            return KORTA_NOTE_OPEN;
        case KORTA_NOTE_OPEN:
            return s->notes_from_files ? KORTA_FILES : KORTA_NOTES;
        case KORTA_QUICK:
            return s->quick_from;
        case KORTA_FIND:
            return s->find_from;
        case KORTA_LINKS:
        case KORTA_SETUP:
        case KORTA_POWER:
        case KORTA_INFO:
        case KORTA_TIME:
        case KORTA_SOUND:
        case KORTA_CONTROLS:
            return s->via_settings ? KORTA_SETTINGS : KORTA_HOME;
        case KORTA_COLOR:
            return s->color_back == KORTA_SETUP ? KORTA_SETUP : KORTA_HOME;
        default:
            return KORTA_HOME;
    }
}
static void open_find(KortaState *s, KortaView from) {
    if (s->view == KORTA_FIND) return;
    if (from == KORTA_QUICK) from = s->quick_from;  /* B from Find must not loop via Quick */
    s->find_from = from;
    s->find_dest_active = false;
    set_view(s, KORTA_FIND);
}
static void back_view(KortaState *s) {
    if (s->find_dest_active && s->view == s->find_dest) {
        s->find_restore = true;
        set_view(s, KORTA_FIND);
        s->find_restore = false;
        s->find_dest_active = false;
        s->focus = s->find_focus;
        return;
    }
    if (s->view == KORTA_NOTE_OPEN && s->notes_from_files) {
        s->notes_from_files = false;
        return_to(s, KORTA_FILES);
        return;
    }
    return_to(s, parent_of(s));
}
static void go_home(KortaState *s) {
    s->find_dest_active = false;
    s->notes_from_files = false;
    s->via_settings = false;
    s->search_direct = false;
    set_view(s, KORTA_HOME);
}
static void launch_game(KortaState *s) {
    s->find_dest_active = false;
    s->notes_from_files = false;
    s->via_settings = false;
    set_view(s, KORTA_HOME);
    push(s, KORTA_ACT_LAUNCH, 0);
}
/* Games: the port loads the game on KORTA_ACT_GAME_START and reports back with
 * korta_arcade_status; quitting stops it and returns to its Home tile. */
static void start_game(KortaState *s, int index) {
    if (index < 0 || index >= s->game_count) return;
    s->game_cur = (uint8_t)index;
    s->arcade_phase = KORTA_ARCADE_LOADING;
    s->arcade_paused = false;
    s->arcade_sel = 0;
    s->arcade_error[0] = '\0';
    set_view(s, KORTA_ARCADE);
    push(s, KORTA_ACT_GAME_START, (uint8_t)index);
}
static void quit_game(KortaState *s) {
    push(s, KORTA_ACT_GAME_STOP, 0);
    s->arcade_paused = false;
    go_home(s);
}
/* Buttons in a game: the game reads them itself; Start opens the pause menu. */
static void arcade_tap(KortaState *s, KortaButton b) {
    if (s->arcade_phase == KORTA_ARCADE_ERROR) {
        if (b == KORTA_A || b == KORTA_B) quit_game(s);
        return;
    }
    if (s->arcade_phase == KORTA_ARCADE_LOADING) {
        if (b == KORTA_B) quit_game(s);  /* never stuck behind a slow load */
        return;
    }
    if (!s->arcade_paused) return;
    if (b == KORTA_UP || b == KORTA_DOWN) s->arcade_sel ^= 1;
    else if (b == KORTA_B || (b == KORTA_A && s->arcade_sel == 0)) s->arcade_paused = false;
    else if (b == KORTA_A) quit_game(s);
}
static void open_dest(KortaState *s, KortaView next) {
    if (next == KORTA_HOME) s->via_settings = false;
    set_view(s, next);
}
static int link_rows(const KortaState *s) {
    return 1 + s->link.count + (s->link.saved ? 1 : 0);
}
static int list_count(const KortaState *s) {
    switch (s->view) {
        case KORTA_NOTES: return 1 + s->note_count;
        case KORTA_NOTE_OPEN: return 3;
        case KORTA_ARCADE: return s->arcade_paused ? 2 : 0;
        case KORTA_CLEAR_CONFIRM: return 2;
        case KORTA_COUNT: return 2;
        case KORTA_TIME: return 3;
        case KORTA_SETTINGS: return 8;
        case KORTA_CONTROLS: return CONTROL_ROWS;
        case KORTA_INFO: return INFO_ROWS;
        case KORTA_CONV: return 2;
        case KORTA_COLOR: return 1;
        case KORTA_SOUND: return 3;
        case KORTA_FIND: {
            uint8_t ids[FIND_COUNT];
            return 1 + matches(s, ids);
        }
        case KORTA_CLOCK: return 7;
        case KORTA_ALARM: return 5;
        case KORTA_TIMER: return 3;
        case KORTA_FILES: return s->note_count ? s->note_count : 1;
        case KORTA_SETUP: return DISPLAY_ROWS;
        case KORTA_PHONE: return 2;
        case KORTA_QUICK: return 5;
        case KORTA_LINKS: return link_rows(s);
        case KORTA_POWER: return 2;
        default: return 0;
    }
}
static void move_list(KortaState *s, KortaButton b) {
    int n = list_count(s);
    if (n < 1) return;
    if (b == KORTA_UP && s->focus > 0) s->focus--;
    if (b == KORTA_DOWN && (int)s->focus + 1 < n) s->focus++;
    if ((int)s->focus >= n) s->focus = (uint8_t)(n - 1);
}
static void move_home(KortaState *s, KortaButton b) {
    int page = s->selected / KORTA_PAGE_SIZE;
    int slot = s->selected % KORTA_PAGE_SIZE;
    int col = slot % 3;
    int row = slot / 3;
    if (b == KORTA_RIGHT) {
        if (col < 2 && home_at(s, page, row * 3 + col + 1) >= 0) col++;
        else {
            /* Cross to the next page from any row: land on its first column, in
             * the same row or the nearest row above it that has an app. */
            int r = row;
            while (r >= 0 && home_at(s, page + 1, r * 3) < 0) r--;
            if (r < 0) return;
            page++;
            row = r;
            col = 0;
        }
    } else if (b == KORTA_LEFT) {
        if (col > 0 && home_at(s, page, row * 3 + col - 1) >= 0) col--;
        else if (page > 0) {
            int r = row;
            while (r >= 0 && home_at(s, page - 1, r * 3 + 2) < 0) r--;
            if (r < 0) return;
            page--;
            row = r;
            col = 2;
        } else return;
    } else if (b == KORTA_DOWN) {
        if (row < 2 && home_at(s, page, (row + 1) * 3 + col) >= 0) row++;
        else return;
    } else if (b == KORTA_UP) {
        if (row > 0 && home_at(s, page, (row - 1) * 3 + col) >= 0) row--;
        else return;
    }
    s->selected = (uint8_t)(page * KORTA_PAGE_SIZE + row * 3 + col);
}
static void nudge_bright(KortaState *s, int dir) {
    int v = (int)s->hw.brightness + dir;
    if (v < 1) v = 1;
    if (v > 16) v = 16;
    s->hw.brightness = (uint8_t)v;
    push(s, KORTA_ACT_BRIGHTNESS, (uint16_t)v);
}
static void nudge_clock(KortaState *s, int dir) {
    if (s->focus == 5) { s->hour12 = !s->hour12; push(s, KORTA_ACT_PREFS, 0); return; }
    if (s->focus == 6) {
        /* Half-hour steps reach India, Iran, Newfoundland and the like; a
         * quarter-hour zone from a USB sync snaps to the next half hour. */
        int m = s->zone_minutes;
        int snapped = dir > 0 ? (m >= 0 ? (m / 30 + 1) * 30 : -((-m - 1) / 30) * 30)
                              : (m > 0 ? ((m - 1) / 30) * 30 : -((-m) / 30 + 1) * 30);
        if (snapped < -720) snapped = -720;
        if (snapped > 840) snapped = 840;
        s->zone_minutes = (int16_t)snapped;
        s->zone = (int8_t)(snapped / 60);
        push(s, KORTA_ACT_ZONE, 0);
        push(s, KORTA_ACT_PREFS, 0);
        return;
    }
    s->time_known = true;
    if (s->year < 2024) s->year = 2026;
    if (s->month < 1 || s->month > 12) s->month = 1;
    if (s->day < 1) s->day = 1;
    if (s->focus == 0) {
        int h = (int)s->hour + dir;
        if (h < 0) h = 23;
        if (h > 23) h = 0;
        s->hour = (uint8_t)h;
    } else if (s->focus == 1) {
        int m = (int)s->min + dir;
        if (m < 0) m = 59;
        if (m > 59) m = 0;
        s->min = (uint8_t)m;
    } else if (s->focus == 2) {
        int md = days_in(s->year, s->month);
        int day = (int)s->day + dir;
        if (day < 1) day = md;
        if (day > md) day = 1;
        s->day = (uint8_t)day;
    } else if (s->focus == 3) {
        int m = (int)s->month + dir;
        if (m < 1) m = 12;
        if (m > 12) m = 1;
        s->month = (uint8_t)m;
        int md = days_in(s->year, s->month);
        if (s->day > md) s->day = (uint8_t)md;
    } else {
        int y = (int)s->year + dir;
        if (y < 2024) y = 2024;
        if (y > 2099) y = 2099;
        s->year = (uint16_t)y;
        int md = days_in(s->year, s->month);
        if (s->day > md) s->day = (uint8_t)md;
    }
    s->clock_dirty = true;
    push(s, KORTA_ACT_CLOCK, 0);
}
/* Repeat choices on the device; other day sets come from the phone and show as letters. */
static const uint8_t REPEATS[] = {0, KORTA_DAYS_ALL, 0x1F, 0x60};
static void nudge_alarm(KortaState *s, int dir) {
    KortaAlarm *a = &s->alarms[s->alarm_cur % KORTA_ALARM_MAX];
    if (s->focus == 0) {
        /* Which alarm: Left/Right step through them, A goes to the next. */
        s->alarm_cur = (uint8_t)((s->alarm_cur + KORTA_ALARM_MAX + dir) % KORTA_ALARM_MAX);
        return;
    }
    if (s->focus == 1) a->on = !a->on;
    else if (s->focus == 2) a->hour = (uint8_t)((a->hour + 24 + dir) % 24);
    else if (s->focus == 3) a->minute = (uint8_t)((a->minute + 60 + dir) % 60);
    else {
        int at = -1, n = (int)sizeof REPEATS;
        for (int i = 0; i < n; i++) if (REPEATS[i] == a->days) at = i;
        /* A custom set steps to the nearest preset in that direction. */
        at = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + n + dir) % n;
        a->days = REPEATS[at];
    }
    push(s, KORTA_ACT_PREFS, 0);
}
static void nudge_setup(KortaState *s, int dir) {
    KortaStatus *h = &s->hw;
    switch (s->focus) {
        case 0: nudge_bright(s, dir); return;
        case 1: s->night = !s->night; push(s, KORTA_ACT_PREFS, 0); return;
        case 2: s->flow = !s->flow; push(s, KORTA_ACT_PREFS, 0); return;
        case 3: h->silent = !h->silent; push(s, KORTA_ACT_SILENT, h->silent); return;
        case 4: h->blend = !h->blend; push(s, KORTA_ACT_BLEND, h->blend); return;
        case 5: h->cc_lcd = !h->cc_lcd; push(s, KORTA_ACT_CC_LCD, h->cc_lcd); return;
        case 6: h->cc_usb = !h->cc_usb; push(s, KORTA_ACT_CC_USB, h->cc_usb); return;
        case 7: h->transit = !h->transit; push(s, KORTA_ACT_TRANSIT, h->transit); return;
        case 8: h->dpad = (uint8_t)(h->dpad ^ 1u); push(s, KORTA_ACT_DPAD, h->dpad); return;
        case 9:
            h->low_batt = (uint8_t)((h->low_batt + (dir < 0 ? 2 : 1)) % 3);
            push(s, KORTA_ACT_LOWBATT, h->low_batt);
            return;
        case 10: {
            int p = (int)h->player + dir;
            if (p < 1) p = 8;
            if (p > 8) p = 1;
            h->player = (uint8_t)p;
            push(s, KORTA_ACT_PLAYER, h->player);
            return;
        }
        default: {
            if (h->gbc_game) return;  /* a GBC title ignores DMG palettes */
            int p = (int)h->palette + dir;
            if (p < 0) p = 14;
            if (p > 14) p = 0;
            h->palette = (uint8_t)p;
            push(s, KORTA_ACT_PALETTE, h->palette);
            return;
        }
    }
}
/* Settings fits one screen: eight rows, no scrolling, Search first. */
enum { SET_HUB = 8 };
static const char *const SET_HUB_NAME[SET_HUB] = {
    "SEARCH", "WIFI", "DISPLAY", "SOUND", "CONTROLS", "POWER", "DATE & TIME", "ABOUT"
};
static const KortaView SET_HUB_VIEW[SET_HUB] = {
    KORTA_FIND, KORTA_LINKS, KORTA_SETUP, KORTA_SOUND, KORTA_CONTROLS, KORTA_POWER, KORTA_CLOCK, KORTA_INFO
};
/* Rows of each page as indices into nudge_setup/setup_label. */
static const uint8_t DISPLAY_MAP[DISPLAY_ROWS] = {0, 1, 11, 4, 5, 6, 7};
static const uint8_t CONTROL_MAP[CONTROL_ROWS] = {8, 2};
static const uint8_t POWER_LOW[1] = {9};
static const uint8_t PALETTE_ONLY[1] = {11};
static const uint8_t PLAYER_ONLY[1] = {10};
static void nudge_mapped(KortaState *s, const uint8_t *map, int count, int dir) {
    if (s->focus >= count) return;
    uint8_t keep = s->focus;
    s->focus = map[keep];
    nudge_setup(s, dir);
    s->focus = keep;
}
static int64_t round_div(int64_t a, int64_t b) { return (a >= 0 ? a + b / 2 : a - b / 2) / b; }
static uint8_t clamp100(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}
/* Fixed point with four decimals. Results beyond +/-1e12 are an error, which
 * keeps every intermediate product inside int64. */
enum { CALC_COLS = 4, CALC_ROWS = 5, CALC_KEY_COUNT = CALC_COLS * CALC_ROWS };
#define CALC_SCALE 10000LL
#define CALC_LIMIT (1000000000000LL * CALC_SCALE)
static const char *const CALC_KEY[CALC_KEY_COUNT] = {
    "C", "+-", "%", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", ".", "BK", "="
};
static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }
static bool calc_mul(int64_t a, int64_t b, int64_t *out) {
    int64_t ai = a / CALC_SCALE, af = a % CALC_SCALE;
    int64_t bi = b / CALC_SCALE, bf = b % CALC_SCALE;
    if (ai && abs64(b) > CALC_LIMIT / abs64(ai)) return false;
    /* (ai*S + af)(bi*S + bf) / S = ai*b + af*bi + af*bf/S */
    *out = ai * b + af * bi + round_div(af * bf, CALC_SCALE);
    return true;
}
static bool calc_div(int64_t a, int64_t b, int64_t *out) {
    if (b == 0) return false;
    bool neg = (a < 0) != (b < 0);
    uint64_t ua = (uint64_t)abs64(a), ub = (uint64_t)abs64(b);
    uint64_t q = ua / ub, r = ua % ub;
    if (q > (uint64_t)(CALC_LIMIT / CALC_SCALE)) return false;
    uint64_t frac = 0;
    for (int i = 0; i < 5; i++) {
        r *= 10;
        frac = frac * 10 + r / ub;
        r %= ub;
    }
    frac = (frac + 5) / 10;  /* round the fifth digit */
    int64_t v = (int64_t)(q * (uint64_t)CALC_SCALE + frac);
    *out = neg ? -v : v;
    return true;
}
static bool calc_apply(int64_t a, uint8_t op, int64_t b, int64_t *out) {
    int64_t r = b;
    if (op == 1) r = a + b;
    else if (op == 2) r = a - b;
    else if (op == 3) { if (!calc_mul(a, b, &r)) return false; }
    else if (op == 4) { if (!calc_div(a, b, &r)) return false; }
    if (abs64(r) >= CALC_LIMIT) return false;
    *out = r;
    return true;
}
static int64_t calc_parse(const char *buf) {
    bool neg = buf[0] == '-';
    int64_t whole = 0, frac = 0, scale = CALC_SCALE;
    const char *p = buf + (neg ? 1 : 0);
    for (; *p && *p != '.'; p++) whole = whole * 10 + (*p - '0');
    if (*p == '.') for (p++; *p && scale > 1; p++) { scale /= 10; frac += (*p - '0') * scale; }
    int64_t v = whole * CALC_SCALE + frac;
    return neg ? -v : v;
}
/* Newlib nano printf has no 64-bit conversions, so digits are written here. */
static void calc_format(int64_t v, char *out, size_t n) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t a = (uint64_t)abs64(v);
    int frac = (int)(a % CALC_SCALE), digits = 4;
    tmp[--i] = '\0';
    if (frac) {
        while (frac % 10 == 0) { frac /= 10; digits--; }
        while (digits--) { tmp[--i] = (char)('0' + frac % 10); frac /= 10; }
        tmp[--i] = '.';
    }
    uint64_t whole = a / CALC_SCALE;
    do { tmp[--i] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    if (v < 0) tmp[--i] = '-';
    snprintf(out, n, "%s", tmp + i);
}
static int64_t calc_current(const KortaState *s) {
    return s->calc_typing ? calc_parse(s->calc_buf) : s->calc_val;
}
static void calc_clear(KortaState *s) {
    s->calc_acc = 0;
    s->calc_val = 0;
    s->calc_buf[0] = '\0';
    s->calc_op = 0;
    s->calc_typing = false;
    s->calc_fresh = true;
    s->calc_err = false;
}
static void calc_begin(KortaState *s) {
    if (s->calc_typing) return;
    snprintf(s->calc_buf, sizeof s->calc_buf, "0");
    s->calc_typing = true;
    s->calc_fresh = false;
}
static void calc_digit(KortaState *s, char digit) {
    calc_begin(s);
    char *b = s->calc_buf;
    const char *dot = strchr(b, '.');
    int digits = 0;
    for (const char *p = b; *p; p++) if (*p >= '0' && *p <= '9') digits++;
    if (digits >= 12 || (dot && strlen(dot + 1) >= 4)) return;
    size_t len = strlen(b);
    if (strcmp(b, "0") == 0 || strcmp(b, "-0") == 0) len--;
    b[len] = digit;
    b[len + 1] = '\0';
}
static void calc_point(KortaState *s) {
    calc_begin(s);
    if (strchr(s->calc_buf, '.')) return;
    size_t len = strlen(s->calc_buf);
    if (len + 1 < sizeof s->calc_buf) { s->calc_buf[len] = '.'; s->calc_buf[len + 1] = '\0'; }
}
static void calc_erase(KortaState *s) {
    if (!s->calc_typing) return;
    size_t len = strlen(s->calc_buf);
    if (len) s->calc_buf[--len] = '\0';
    if (!len || strcmp(s->calc_buf, "-") == 0) snprintf(s->calc_buf, sizeof s->calc_buf, "0");
}
static void calc_negate(KortaState *s) {
    if (s->calc_typing) {
        char *b = s->calc_buf;
        if (b[0] == '-') memmove(b, b + 1, strlen(b));
        else if (strlen(b) + 1 < sizeof s->calc_buf) { memmove(b + 1, b, strlen(b) + 1); b[0] = '-'; }
        return;
    }
    if ((s->calc_op && s->calc_fresh) || s->calc_val == 0) {
        /* Starting a new number: the sign applies to what is typed next. */
        snprintf(s->calc_buf, sizeof s->calc_buf, "-0");
        s->calc_typing = true;
        s->calc_fresh = false;
        return;
    }
    s->calc_val = -s->calc_val;
    s->calc_fresh = false;
}
static void calc_percent(KortaState *s) {
    /* Like a phone calculator: 200 + 10% is 200 + 20; alone, 10% is 0.1. */
    int64_t x = calc_current(s), r = x;
    bool of_acc = s->calc_op == 1 || s->calc_op == 2;
    bool ok;
    if (!of_acc) ok = calc_div(x, 100 * CALC_SCALE, &r);
    else if (calc_mul(s->calc_acc, x, &r)) ok = calc_div(r, 100 * CALC_SCALE, &r);
    else ok = calc_div(x, 100 * CALC_SCALE, &r) && calc_mul(s->calc_acc, r, &r);
    if (!ok) {
        s->calc_err = true;
        return;
    }
    s->calc_val = r;
    s->calc_typing = false;
    s->calc_fresh = false;
}
static void calc_operator(KortaState *s, uint8_t op) {
    int64_t x = calc_current(s);
    if (s->calc_op && !s->calc_fresh) {
        if (!calc_apply(s->calc_acc, s->calc_op, x, &s->calc_acc)) { s->calc_err = true; return; }
    } else if (!s->calc_op) s->calc_acc = x;
    s->calc_op = op;
    s->calc_val = s->calc_acc;
    s->calc_typing = false;
    s->calc_fresh = true;
}
static void calc_equals(KortaState *s) {
    int64_t x = calc_current(s);
    if (s->calc_op) {
        if (!calc_apply(s->calc_acc, s->calc_op, x, &x)) { s->calc_err = true; return; }
    }
    s->calc_acc = x;
    s->calc_val = x;
    s->calc_op = 0;
    s->calc_typing = false;
    s->calc_fresh = true;
}
static void calc_key(KortaState *s) {
    const char *key = CALC_KEY[s->focus % CALC_KEY_COUNT];
    if (strcmp(key, "C") == 0) { calc_clear(s); return; }
    if (s->calc_err) return;
    if (key[0] >= '0' && key[0] <= '9') calc_digit(s, key[0]);
    else if (strcmp(key, ".") == 0) calc_point(s);
    else if (strcmp(key, "BK") == 0) calc_erase(s);
    else if (strcmp(key, "+-") == 0) calc_negate(s);
    else if (strcmp(key, "%") == 0) calc_percent(s);
    else if (strcmp(key, "=") == 0) calc_equals(s);
    else calc_operator(s, (uint8_t)(strchr("+-*/", key[0]) - "+-*/" + 1));
}
static void move_calc(KortaState *s, KortaButton b) {
    int col = s->focus % CALC_COLS;
    int row = s->focus / CALC_COLS;
    if (b == KORTA_LEFT && col > 0) s->focus--;
    if (b == KORTA_RIGHT && col < CALC_COLS - 1) s->focus++;
    if (b == KORTA_UP && row > 0) s->focus = (uint8_t)(s->focus - CALC_COLS);
    if (b == KORTA_DOWN && row < CALC_ROWS - 1) s->focus = (uint8_t)(s->focus + CALC_COLS);
}
static void timer_toggle(KortaState *s) {
    if (s->timer_run) {
        if (!s->timer_count) s->timer_acc += s->now_ms - s->timer_mark;
        else s->timer_remaining = (int32_t)(s->timer_deadline - s->now_ms) > 0
            ? s->timer_deadline - s->now_ms : 0;
        s->timer_run = false;
        return;
    }
    s->timer_done = false;
    s->timer_alert_pending = false;
    s->timer_run = true;
    s->timer_mark = s->now_ms;
    if (s->timer_count) {
        uint32_t remaining = s->timer_remaining ? s->timer_remaining : (uint32_t)s->timer_set * 1000u;
        s->timer_deadline = s->now_ms + remaining;
        s->timer_remaining = 0;
    }
}
static void timer_reset(KortaState *s) {
    s->timer_run = false;
    s->timer_done = false;
    s->timer_alert_pending = false;
    s->timer_acc = 0;
    s->timer_remaining = 0;
}
static void nudge_timer(KortaState *s, int dir) {
    if (s->timer_run) return;
    if (s->focus == 0) { s->timer_count = !s->timer_count; timer_reset(s); }
    else if (s->focus == 1 && s->timer_count) {
        /* Fine steps for short timers, coarse ones for long: 5 s, 30 s, 1 min, 5 min. */
        int v = s->timer_set;
        int at = dir > 0 ? v : v - 1;
        int step = at < 60 ? 5 : at < 600 ? 30 : at < 3600 ? 60 : 300;
        v += dir * step;
        if (v < 5) v = 5;
        if (v > 3 * 3600) v = 3 * 3600;
        s->timer_set = (uint16_t)v;
        timer_reset(s);
    }
}
static void open_match(KortaState *s) {
    uint8_t ids[FIND_COUNT];
    int n = matches(s, ids);
    int index = (int)s->focus - 1;
    if (index < 0 || index >= n) return;
    KortaView next = FIND_ITEM[ids[index]].view;
    s->find_dest = next;
    s->find_focus = s->focus;
    s->find_dest_active = true;
    s->via_settings = false;
    open_dest(s, next);
}
static void choose_net(KortaState *s, int index) {
    if (index < 0 || index >= s->link.count) return;
    snprintf(s->join_ssid, sizeof s->join_ssid, "%s", s->link.nets[index].ssid);
    s->pass_len = 0;
    s->password[0] = '\0';
    if (s->link.nets[index].locked) {
        set_view(s, KORTA_KEYBOARD);
        s->typing_pass = true;
        s->symbols = false;
        s->caps = false;
        return;
    }
    s->wifi_keep = true;
    push(s, KORTA_ACT_WIFI_JOIN, 0);
    push(s, KORTA_ACT_PREFS, 0);
}
static void nudge_vol(KortaState *s, uint8_t which, int dir) {
    int step = dir * 5;
    if (which == 0) s->vol_sys = clamp100((int)s->vol_sys + step);
    else if (which == 2) s->vol_ring = clamp100((int)s->vol_ring + step);
    else return;
    push(s, KORTA_ACT_PREFS, 0);
}
/* xorshift32, stirred with the press time so each roll is unpredictable. */
static uint32_t random_below(KortaState *s, uint32_t n) {
    uint32_t x = s->rng ^ (s->now_ms * 2654435761u) ^ ((uint32_t)s->roll_nonce << 16);
    if (!x) x = 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return n ? x % n : 0;
}
typedef struct { const char *name; uint8_t sides; bool two; } DiceKind;
static const DiceKind DICE[] = {
    {"D6", 6, false}, {"2D6", 6, true}, {"D4", 4, false}, {"D8", 8, false},
    {"D10", 10, false}, {"D12", 12, false}, {"D20", 20, false}, {"D100", 100, false}
};
enum { DICE_KINDS = (int)(sizeof DICE / sizeof DICE[0]) };
static void open_note_keys(KortaState *s) {
    s->pending_len = 0;
    s->pending[0] = '\0';
    s->candidate = 0;
    s->symbols = false;
    s->caps = s->flow;
    set_view(s, KORTA_KEYBOARD);
}
enum { FLASH_TINTS = 3, FLASH_MODES = 3 };
static void flash_input(KortaState *s, KortaButton b) {
    s->flash_hud_until = s->now_ms + 3000u;
    if (b == KORTA_UP || b == KORTA_DOWN) {
        nudge_bright(s, b == KORTA_UP ? 1 : -1);
        s->flash_level = s->hw.brightness;
    } else if (b == KORTA_LEFT || b == KORTA_RIGHT) {
        s->flash_tint = (uint8_t)((s->flash_tint + (b == KORTA_LEFT ? FLASH_TINTS - 1 : 1)) % FLASH_TINTS);
    } else if (b == KORTA_A) {
        s->flash_mode = (uint8_t)((s->flash_mode + 1) % FLASH_MODES);
        s->flash_mark = s->now_ms;
    }
}
/* Pairs are adjacent so kind ^ 1 is the reverse conversion. Result in tenths:
 * ((n + pre) * num) / den + post. */
typedef struct { const char *from, *to; int32_t pre, num, den, post; } Conv;
static const Conv CONV[] = {
    {"C", "F", 0, 18, 1, 320},             {"F", "C", -32, 50, 9, 0},
    {"KG", "LB", 0, 220462, 10000, 0},     {"LB", "KG", 0, 453592, 100000, 0},
    {"KM", "MI", 0, 621371, 100000, 0},    {"MI", "KM", 0, 1609344, 100000, 0},
    {"IN", "CM", 0, 254, 10, 0},           {"CM", "IN", 0, 393701, 100000, 0},
};
enum { CONV_COUNT = (int)(sizeof CONV / sizeof CONV[0]) };
static int32_t clamp_conv(int32_t n) { return n < -9999 ? -9999 : n > 9999 ? 9999 : n; }
static int32_t conv_tenths(uint8_t kind, int32_t n) {
    const Conv *c = &CONV[kind % CONV_COUNT];
    return (int32_t)(round_div(((int64_t)n + c->pre) * c->num, c->den) + c->post);
}
/* Rounded once from the exact value, so swapping never double-rounds. */
static int32_t conv_whole(uint8_t kind, int32_t n) {
    const Conv *c = &CONV[kind % CONV_COUNT];
    return (int32_t)round_div(((int64_t)n + c->pre) * c->num + (int64_t)c->post * c->den, (int64_t)c->den * 10);
}
static void on_tap(KortaState *s, KortaButton b, bool *leave) {
    (void)leave;
    if (s->view == KORTA_ARCADE) { arcade_tap(s, b); return; }
    if (s->vol_mode == 1 && (b == KORTA_A || b == KORTA_LEFT)) {
        s->vol_mode = 2;
        s->vol_sel = 0;
        s->vol_until = s->now_ms + 8000u;
        return;
    }
    if (s->vol_mode == 2 && (b == KORTA_UP || b == KORTA_DOWN || b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A || b == KORTA_B)) {
        /* Side panel of vertical sliders: Left/Right picks System or Ring (Media
         * follows the wheel), Up/Down sets the level, A or B closes. */
        s->vol_until = s->now_ms + 8000u;
        if (b == KORTA_B || b == KORTA_A) { s->vol_mode = 0; return; }
        if (b == KORTA_LEFT) s->vol_sel = 0;
        else if (b == KORTA_RIGHT) s->vol_sel = 2;
        else nudge_vol(s, s->vol_sel, b == KORTA_UP ? 1 : -1);
        return;
    }
    if (s->view == KORTA_ALARM_RING) {
        if (b == KORTA_A || b == KORTA_B) { s->alarm_latched = true; s->alarm_latch_min = (uint16_t)(s->hour * 60 + s->min); return_to(s, s->ring_from); }
        return;
    }
    if (s->view == KORTA_TIMER_RING || s->view == KORTA_MESSAGE) {
        if (b == KORTA_A || b == KORTA_B) return_to(s, s->ring_from);
        return;
    }
    if (s->view == KORTA_KEYBOARD) {
        if (b == KORTA_B) {
            bool empty = use_flow(s) && s->pending_len ? false
                : s->typing_pass ? !s->pass_len : s->typing_find ? !s->query_len : !cur_len(s);
            if (empty && s->repeating) return;
            if (use_flow(s) && s->pending_len) {
                s->pending[--s->pending_len] = '\0';
                s->candidate = 0;
            } else if (s->typing_pass) {
                if (!s->pass_len) return_to(s, KORTA_LINKS);
                else erase_char(s);
            } else if (s->typing_find) {
                if (!s->query_len) finish_find(s);
                else erase_char(s);
            } else if (!cur_len(s)) set_view(s, KORTA_NOTE_OPEN);
            else erase_char(s);
            return;
        }
        if (b == KORTA_A) { key_action(s); return; }
        if (b == KORTA_SELECT) {
            if (s->typing_pass || s->typing_find) s->symbols = !s->symbols;
            else if (!s->pending_len) {
                s->flow = !s->flow;
                s->caps = s->flow;
                s->key_row = s->key_col = 0;
                push(s, KORTA_ACT_PREFS, 0);
            }
            return;
        }
        if (b == KORTA_UP || b == KORTA_DOWN || b == KORTA_LEFT || b == KORTA_RIGHT) move_key(s, b);
        return;
    }
    if (s->view == KORTA_CALC) {
        if (b == KORTA_A) { calc_key(s); return; }
        if (b == KORTA_B) { back_view(s); return; }
        if (b == KORTA_UP || b == KORTA_DOWN || b == KORTA_LEFT || b == KORTA_RIGHT) move_calc(s, b);
        return;
    }
    if (b == KORTA_B) {
        if (s->view == KORTA_HOME) {
            if (s->hw.cart_in) launch_game(s);
            return;
        }
        back_view(s);
        return;
    }
    if (s->view == KORTA_HOME) {
        int app = home_app(s);
        int col = (s->selected % KORTA_PAGE_SIZE) % 3;
        int page = s->selected / KORTA_PAGE_SIZE;
        if (b == KORTA_A) {
            if (app < 0) return;
            if (is_game(app)) start_game(s, app - GAME_ID);
            else if (APP_VIEW[app] == KORTA_PLAY) {
                if (s->hw.cart_in) launch_game(s);
            } else {
                s->via_settings = false;
                open_dest(s, APP_VIEW[app]);
            }
        } else if (b == KORTA_LEFT && page == 0 && col == 0) {
            s->via_settings = false;
            open_find(s, KORTA_HOME);
        } else move_home(s, b);
        return;
    }
    if (s->view == KORTA_FIND && b == KORTA_RIGHT) {
        back_view(s);
        return;
    }
    if (s->view == KORTA_FIND && b == KORTA_LEFT && s->focus == 0) {
        open_search_keys(s);
        return;
    }
    if (s->view == KORTA_FLASH) { flash_input(s, b); return; }
    if (s->view == KORTA_NOTE_OPEN && (b == KORTA_LEFT || b == KORTA_RIGHT)) {
        if (b == KORTA_LEFT && s->note_cur > 0) s->note_cur--;
        else if (b == KORTA_RIGHT && s->note_cur + 1 < s->note_count) s->note_cur++;
        return;
    }
    if (s->view == KORTA_PHONE && (b == KORTA_A || b == KORTA_LEFT || b == KORTA_RIGHT)) {
        if (s->focus == 0) {
            s->web_keep = !s->web_keep;
            push(s, KORTA_ACT_WEB, s->web_keep);
            /* The link needs Wi-Fi; switching it on brings Wi-Fi up with it. */
            if (s->web_keep && s->link.phase == KORTA_WIFI_OFF) {
                s->wifi_keep = true;
                push(s, KORTA_ACT_WIFI_ON, 0);
            }
            push(s, KORTA_ACT_PREFS, 0);
        } else if (b == KORTA_A && s->link.web_paired) push(s, KORTA_ACT_WEB_FORGET, 0);
        return;
    }
    if (s->view == KORTA_COIN && b == KORTA_DOWN) {
        s->heads = s->tails = 0;
        s->coin_anim_until = s->now_ms;
        return;
    }
    if (b == KORTA_UP || b == KORTA_DOWN) { move_list(s, b); return; }
    if (b != KORTA_A && b != KORTA_LEFT && b != KORTA_RIGHT) return;
    int dir = (b == KORTA_LEFT) ? -1 : 1;
    if (s->view == KORTA_CLOCK && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) { nudge_clock(s, b == KORTA_LEFT ? -1 : 1); return; }
    if (s->view == KORTA_ALARM && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) { nudge_alarm(s, b == KORTA_LEFT ? -1 : 1); return; }
    if (s->view == KORTA_SETUP && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        if (b == KORTA_A && s->focus == DISPLAY_PALETTE_ROW) {
            s->color_back = KORTA_SETUP;
            set_view(s, KORTA_COLOR);
        }
        else nudge_mapped(s, DISPLAY_MAP, DISPLAY_ROWS, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_CONTROLS && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        nudge_mapped(s, CONTROL_MAP, CONTROL_ROWS, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_INFO && s->focus == INFO_PLAYER_ROW && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        uint8_t keep = s->focus;
        s->focus = 0;
        nudge_mapped(s, PLAYER_ONLY, 1, b == KORTA_LEFT ? -1 : 1);
        s->focus = keep;
        return;
    }
    if (s->view == KORTA_POWER && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        if (s->focus == 1) nudge_mapped(s, POWER_LOW, 1, b == KORTA_LEFT ? -1 : 1);
        else nudge_bright(s, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_TIMER && (b == KORTA_LEFT || b == KORTA_RIGHT)) { nudge_timer(s, dir); return; }
    if (s->view == KORTA_COLOR && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        s->focus = 0;
        nudge_mapped(s, PALETTE_ONLY, 1, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_SOUND && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        if (s->focus == 0) {
            s->hw.silent = !s->hw.silent;
            push(s, KORTA_ACT_SILENT, s->hw.silent);
        } else if (s->focus == 1) nudge_vol(s, 0, b == KORTA_LEFT ? -1 : 1);
        else if (s->focus == 2) nudge_vol(s, 2, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_CONV) {
        if (s->focus == 0) {
            s->conv_kind = (uint8_t)((s->conv_kind + (b == KORTA_LEFT ? CONV_COUNT - 1 : 1)) % CONV_COUNT);
        } else if (b == KORTA_A) {
            int32_t whole = conv_whole(s->conv_kind, s->conv_n);
            s->conv_kind ^= 1u;
            s->conv_n = (int16_t)clamp_conv(whole);
        } else s->conv_n = (int16_t)clamp_conv(s->conv_n + dir);
        return;
    }
    if (s->view == KORTA_DICE && (b == KORTA_LEFT || b == KORTA_RIGHT)) {
        s->dice_kind = (uint8_t)((s->dice_kind + (b == KORTA_LEFT ? DICE_KINDS - 1 : 1)) % DICE_KINDS);
        s->dice = s->dice2 = 0;
        return;
    }
    if (s->view == KORTA_COUNT) {
        if (s->focus == 1) {
            if (b == KORTA_A) { s->tally = 0; s->focus = 0; }
        } else if (b == KORTA_LEFT) {
            if (s->tally > -9999) s->tally--;
        } else if (s->tally < 9999) s->tally++;
        return;
    }
    if (s->view == KORTA_QUICK && (b == KORTA_LEFT || b == KORTA_RIGHT)) {
        if (s->focus == 0) nudge_bright(s, dir);
        else if (s->focus == 1) { s->night = !s->night; push(s, KORTA_ACT_PREFS, 0); }
        else if (s->focus == 2) { s->hw.silent = !s->hw.silent; push(s, KORTA_ACT_SILENT, s->hw.silent); }
        return;
    }
    if (b != KORTA_A) return;
    if (s->view == KORTA_PLAY) {
        if (s->hw.cart_in) launch_game(s);
        return;
    }
    if (s->view == KORTA_TIME) {
        if (s->focus == 0) set_view(s, KORTA_CLOCK);
        else if (s->focus == 1) set_view(s, KORTA_ALARM);
        else set_view(s, KORTA_TIMER);
    } else if (s->view == KORTA_SETTINGS) {
        if (s->focus == 0) {
            s->via_settings = true;
            open_find(s, KORTA_SETTINGS);
            open_search_keys(s);
            s->search_direct = true;
        } else if (s->focus < SET_HUB) {
            s->via_settings = true;
            open_dest(s, SET_HUB_VIEW[s->focus]);
        }
    } else if (s->view == KORTA_NOTES) {
        if (s->focus == 0) {
            if (new_note(s)) open_note_keys(s);
        } else {
            s->note_cur = (uint8_t)(s->focus - 1);
            set_view(s, KORTA_NOTE_OPEN);
        }
    } else if (s->view == KORTA_NOTE_OPEN) {
        if (!s->note_count) back_view(s);
        else if (s->focus == 0) open_note_keys(s);
        else if (s->focus == 1) {
            toggle_pin(s);
            push(s, KORTA_ACT_PREFS, 0);
        } else set_view(s, KORTA_CLEAR_CONFIRM);
    } else if (s->view == KORTA_CLEAR_CONFIRM) {
        if (s->focus == 1) {
            delete_note(s, s->note_cur);
            push(s, KORTA_ACT_PREFS, 0);
            bool files = s->notes_from_files;
            s->notes_from_files = false;
            set_view(s, files ? KORTA_FILES : KORTA_NOTES);
            if (s->note_count) s->focus = files ? s->note_cur : (uint8_t)(s->note_cur + 1);
        } else set_view(s, KORTA_NOTE_OPEN);
    } else if (s->view == KORTA_FILES) {
        if (s->note_count) {
            s->note_cur = s->focus;
            s->notes_from_files = true;
            set_view(s, KORTA_NOTE_OPEN);
        }
    }
    else if (s->view == KORTA_TIMER) {
        if (s->focus == 2) timer_reset(s);
        else timer_toggle(s);
    } else if (s->view == KORTA_FIND) {
        if (s->focus == 0) open_search_keys(s);
        else open_match(s);
    } else if (s->view == KORTA_QUICK) {
        if (s->focus == 0) nudge_bright(s, 1);
        else if (s->focus == 1) { s->night = !s->night; push(s, KORTA_ACT_PREFS, 0); }
        else if (s->focus == 2) { s->hw.silent = !s->hw.silent; push(s, KORTA_ACT_SILENT, s->hw.silent); }
        else if (s->focus == 3) { s->vol_mode = 2; s->vol_sel = 0; s->vol_until = s->now_ms + 8000u; }
        else if (s->hw.cart_in) launch_game(s);
    } else if (s->view == KORTA_DICE) {
        const DiceKind *k = &DICE[s->dice_kind % DICE_KINDS];
        s->roll_nonce++;
        s->dice = (uint8_t)(random_below(s, k->sides) + 1);
        s->dice2 = k->two ? (uint8_t)(random_below(s, k->sides) + 1) : 0;
        s->dice_anim_until = s->now_ms + 640u;
        s->sfx = KORTA_SFX_DICE;
    } else if (s->view == KORTA_COIN) {
        s->roll_nonce++;
        s->coin = (uint8_t)(random_below(s, 2) + 1);
        if (s->coin == 1) s->heads++;
        else s->tails++;
        s->coin_anim_until = s->now_ms + 640u;
        s->sfx = KORTA_SFX_COIN;
    } else if (s->view == KORTA_LINKS) {
        int rows = link_rows(s);
        if (s->focus == 0) {
            s->wifi_keep = s->link.phase == KORTA_WIFI_OFF;
            push(s, s->wifi_keep ? KORTA_ACT_WIFI_ON : KORTA_ACT_WIFI_OFF, 0);
            push(s, KORTA_ACT_PREFS, 0);
        } else if (s->link.saved && (int)s->focus == rows - 1) {
            s->wifi_keep = false;
            push(s, KORTA_ACT_WIFI_FORGET, 0);
            push(s, KORTA_ACT_PREFS, 0);
        } else choose_net(s, (int)s->focus - 1);
    }
}
static bool repeats(const KortaState *s, KortaButton b) {
    if (b == KORTA_A) return s->view == KORTA_KEYBOARD || s->view == KORTA_CALC;
    if (b == KORTA_B) return text_mode(s);
    if (b != KORTA_UP && b != KORTA_DOWN && b != KORTA_LEFT && b != KORTA_RIGHT) return false;
    switch (s->view) {
        case KORTA_KEYBOARD:
        case KORTA_HOME:
        case KORTA_CALC:
            return true;
        case KORTA_CLOCK:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus != 5;
        case KORTA_ALARM:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus == 2 || s->focus == 3;
        case KORTA_TIMER:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus == 1 && (b == KORTA_LEFT || b == KORTA_RIGHT);
        /* Holding Left/Right steps numbers; on/off and choices change once per press. */
        case KORTA_SETUP:
        case KORTA_POWER:
            return b == KORTA_UP || b == KORTA_DOWN || s->focus == 0;
        case KORTA_CONTROLS:
        case KORTA_SETTINGS:
        case KORTA_INFO:
            return b == KORTA_UP || b == KORTA_DOWN;
        case KORTA_QUICK:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus == 0 && (b == KORTA_LEFT || b == KORTA_RIGHT);
        case KORTA_SOUND:
            return b == KORTA_UP || b == KORTA_DOWN || s->focus == 1 || s->focus == 2;
        case KORTA_CONV:
            return b == KORTA_UP || b == KORTA_DOWN || s->focus == 1;
        case KORTA_FLASH:
        case KORTA_COUNT:
            return true;
        case KORTA_FIND:
            return b == KORTA_UP || b == KORTA_DOWN;
        default:
            return b == KORTA_UP || b == KORTA_DOWN;
    }
}
static void on_repeat(KortaState *s, KortaButton b, bool *leave) {
    (void)leave;
    if (s->view == KORTA_ARCADE) return;  /* held buttons belong to the game */
    if (s->vol_mode == 2 && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_UP || b == KORTA_DOWN)) {
        on_tap(s, b, leave);
        return;
    }
    if (b == KORTA_B && is_alert(s->view)) return;
    if (b == KORTA_B && !text_mode(s)) {
        if (!s->held_home) {
            if (!s->b_from_home && s->view != KORTA_HOME) go_home(s);
            s->held_home = true;
        }
        return;
    }
    if (!repeats(s, b)) return;
    s->repeating = true;
    on_tap(s, b, leave);
    s->repeating = false;
}
static void on_start_short(KortaState *s) {
    if (s->view == KORTA_ARCADE) {
        if (s->arcade_phase == KORTA_ARCADE_RUNNING) { s->arcade_paused = !s->arcade_paused; s->arcade_sel = 0; }
        else quit_game(s);
        return;
    }
    if (s->vol_mode == 2) { s->vol_mode = 0; return; }
    if (s->view == KORTA_KEYBOARD) {
        if (s->typing_pass) finish_pass(s);
        else if (s->typing_find) finish_find(s);
        else finish_note(s);
        return;
    }
    if (s->view == KORTA_QUICK) return_to(s, s->quick_from);
    else if (!is_alert(s->view)) {
        s->quick_from = s->view == KORTA_FIND ? s->find_from : s->view;
        if (s->view != KORTA_FIND) s->back_focus[s->view] = s->focus;
        set_view(s, KORTA_QUICK);
    }
}
static void on_start_hold(KortaState *s) {
    if (s->view == KORTA_ARCADE) return;
    if (s->vol_mode == 2) return;
    if (s->view == KORTA_KEYBOARD || is_alert(s->view)) return;
    open_find(s, s->view);
}
static void on_select_short(KortaState *s) {
    if (s->view == KORTA_ARCADE) return;
    if (s->vol_mode == 2) return;
    if (is_alert(s->view)) return;
    open_find(s, s->view);
}
static void on_select_hold(KortaState *s) {
    if (s->view == KORTA_ARCADE) return;
    if (s->vol_mode == 2) return;
    if (s->view == KORTA_KEYBOARD) return;
    s->night = !s->night;
    push(s, KORTA_ACT_PREFS, 0);
}
/* 0 Monday .. 6 Sunday (Sakamoto's method). */
static int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int sunday0 = (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
    return (sunday0 + 6) % 7;
}
/* The first armed alarm due this minute, or -1. */
static int due_alarm(const KortaState *s) {
    int today = 1 << weekday(s->year, s->month ? s->month : 1, s->day ? s->day : 1);
    for (int i = 0; i < KORTA_ALARM_MAX; i++) {
        const KortaAlarm *a = &s->alarms[i];
        if (a->on && a->hour == s->hour && a->minute == s->min && (!a->days || (a->days & today))) return i;
    }
    return -1;
}
static void service_alarm(KortaState *s) {
    uint16_t now_min = (uint16_t)(s->hour * 60 + s->min);
    /* A latch covers the minute it was set in. */
    if (s->time_known && s->alarm_latched && s->alarm_latch_min != now_min) s->alarm_latched = false;
    if (!s->time_known || s->view == KORTA_ALARM_RING) return;
    int due = due_alarm(s);
    bool fresh = due >= 0 && !s->alarm_latched;
    if (fresh) {
        s->alarm_latched = true;
        s->alarm_latch_min = now_min;
        s->alarm_ringing = (uint8_t)due;
        /* A one-time alarm is spent once it comes due. */
        if (!s->alarms[due].days) { s->alarms[due].on = false; push(s, KORTA_ACT_PREFS, 0); }
    }
    /* One alert at a time: an alarm due under a timer ring waits for it. */
    if (s->view == KORTA_TIMER_RING) {
        if (fresh) s->alarm_pending = true;
        return;
    }
    if (s->alarm_pending || fresh) {
        s->alarm_pending = false;
        if (s->view == KORTA_MESSAGE) return_to(s, s->ring_from);
        s->ring_from = s->view;
        set_view(s, KORTA_ALARM_RING);
        push(s, KORTA_ACT_ALERT, 0);
        s->sfx = KORTA_SFX_ALERT;
    }
}
static void service_timer(KortaState *s) {
    if (s->timer_run && s->timer_count && (int32_t)(s->now_ms - s->timer_deadline) >= 0) {
        s->timer_run = false;
        s->timer_done = true;
        s->timer_alert_pending = true;
    }
    if (s->timer_alert_pending && s->view != KORTA_ALARM_RING && s->view != KORTA_TIMER_RING) {
        s->timer_alert_pending = false;
        if (s->view == KORTA_MESSAGE) return_to(s, s->ring_from);
        s->ring_from = s->view;
        set_view(s, KORTA_TIMER_RING);
        push(s, KORTA_ACT_ALERT, 0);
        s->sfx = KORTA_SFX_ALERT;
    }
}

static void clock_text(const KortaState *s, char *out, size_t n, bool seconds) {
    int hour = s->hour;
    if (s->hour12) {
        char mark = hour >= 12 ? 'P' : 'A';
        hour %= 12;
        if (hour == 0) hour = 12;
        if (seconds) snprintf(out, n, "%d:%02u:%02u%c", hour, s->min, s->sec, mark);
        else snprintf(out, n, "%d:%02u%c", hour, s->min, mark);
    } else if (seconds) snprintf(out, n, "%02u:%02u:%02u", s->hour, s->min, s->sec);
    else snprintf(out, n, "%02u:%02u", s->hour, s->min);
}
static void status_bar(Painter *d, const KortaState *s, const char *title) {
    fill(d, 0, 0, 160, 12, d->p.band);
    fill(d, 0, 11, 160, 1, d->p.line);
    char clk[12];
    if (!s->time_known) snprintf(clk, sizeof clk, "--:--");
    else clock_text(s, clk, sizeof clk, false);
    draw_text(d, clk, 3, 2, d->p.band_ink);
    int icon_x = 145;
    int pct = s->hw.battery_known ? s->hw.battery_pct : -1;
    char num[4] = "";
    if (pct >= 0) snprintf(num, sizeof num, "%d", pct);
    int num_w = text_width(num);
    int num_x = icon_x - 2 - num_w;
    if (num[0]) draw_text(d, num, num_x, 2, d->p.band_ink);
    uint16_t body = d->p.band_ink;
    uint16_t juice = !s->hw.battery_known ? d->p.muted : s->hw.charging ? d->p.accent : pct <= 15 ? rgb(210, 64, 54) : pct <= 40 ? rgb(214, 164, 54) : rgb(78, 168, 86);
    fill(d, icon_x, 3, 12, 6, body);
    fill(d, icon_x + 1, 4, 10, 4, d->p.band);
    fill(d, icon_x + 12, 4, 2, 4, body);
    if (s->hw.battery_known) {
        int w = pct >= 100 ? 10 : pct / 10;
        if (pct > 0 && w < 1) w = 1;
        fill(d, icon_x + 1, 4, w, 4, juice);
    }
    bool wifi_glyph = s->link.phase == KORTA_WIFI_ONLINE || s->link.phase == KORTA_WIFI_JOIN;
    if (wifi_glyph) {
        int bx = num_x - 14;
        for (int i = 0; i < 4; i++) {
            int h = 2 + i * 2;
            bool on = s->link.phase == KORTA_WIFI_JOIN ? i == 0 : s->link.rssi > -90 + i * 12;
            fill(d, bx + i * 3, 9 - h, 2, h, on ? d->p.accent : d->p.muted);
        }
    }
    if (title && title[0]) {
        int tw = text_width(title);
        int tx = 80 - tw / 2;
        int left = 3 + text_width(clk) + 4;
        int right = (num[0] ? num_x : icon_x) - 4;
        if (wifi_glyph) right -= 14;
        if (tx >= left && tx + tw <= right) draw_text(d, title, tx, 2, d->p.band_ink);
    }
    if (s->view == KORTA_HOME) {
        int page = s->selected / KORTA_PAGE_SIZE;
        int pages = home_pages(s);
        int dots_x = 80 - (pages * 7 - 3) / 2;
        for (int i = 0; i < pages; i++) {
            uint16_t c = i == page ? d->p.band_ink : d->p.muted;
            fill(d, dots_x + i * 7, 5, 4, 2, c);  /* centred in the 12 px band */
        }
    }
}
static void row_at(Painter *d, const char *label, const char *right, int y, bool on) {
    if (on) {
        fill(d, 3, y, 154, 14, d->p.card);
        border(d, 3, y, 154, 14, d->p.ink, 1);
    } else fill(d, 8, y + 13, 144, 1, d->p.line);
    char clipped[26];
    int available = right && right[0] ? 153 - text_width(right) - 13 : 147;
    int chars = available / 6;
    if (chars < 0) chars = 0;
    if (chars > 25) chars = 25;
    snprintf(clipped, sizeof clipped, "%.*s", chars, label);
    if ((int)strlen(label) > chars && chars) clipped[chars - 1] = '.';
    draw_text(d, clipped, 7, y + 4, d->p.ink);
    if (right && right[0]) draw_text(d, right, 153 - text_width(right), y + 4, on ? d->p.ink : d->p.muted);
}
static void list_window(const KortaState *s, int count, int window, int *start, int *shown) {
    int focus = s->focus;
    if (focus >= count) focus = count - 1;
    if (focus < 0) focus = 0;
    *start = 0;
    if (count > window && focus > window / 2) *start = focus - window / 2;
    if (*start + window > count) *start = count - window;
    if (*start < 0) *start = 0;
    *shown = count - *start;
    if (*shown > window) *shown = window;
}
static const char *setup_value(const KortaState *s, int index, char *buf, size_t n) {
    const KortaStatus *h = &s->hw;
    switch (index) {
        case 0: snprintf(buf, n, "%u", h->brightness); return buf;
        case 1: return s->night ? "NIGHT" : "PAPER";
        case 2: return s->flow ? "FLOW" : "QWERTY";
        case 3: return h->silent ? "ON" : "OFF";
        case 4: return h->blend ? "ON" : "OFF";
        /* Stock option names: corrected/raw color, classic/smooth transition,
         * all directions/ignore diagonals, always show/blink/hide. */
        case 5: return h->cc_lcd ? "CORRECTED" : "RAW";
        case 6: return h->cc_usb ? "CORRECTED" : "RAW";
        case 7: return h->transit ? "SMOOTH" : "CLASSIC";
        case 8: return h->dpad ? "4-WAY" : "8-WAY";
        case 9: return h->low_batt == 0 ? "SHOW" : h->low_batt == 1 ? "BLINK" : "HIDE";
        case 10: snprintf(buf, n, "%u", h->player ? h->player : 1); return buf;
        default: {
            if (h->gbc_game) return "GBC GAME";
            unsigned p = h->palette < 15 ? h->palette : 0;
            return PALETTE_NAME[p];
        }
    }
}
static const char *setup_label(int index) {
    static const char *const labels[12] = {
        "BRIGHTNESS", "THEME", "KEYBOARD", "SILENT", "FRAME BLEND", "LCD COLOR", "USB COLOR", "TRANSITION",
        "D-PAD", "LOW BATT ICON", "PLAYER NO.", "GB PALETTE"
    };
    return labels[index];
}
static const char *wifi_word(uint8_t phase) {
    switch (phase) {
        case KORTA_WIFI_IDLE: return "IDLE";
        case KORTA_WIFI_SCAN: return "SCAN";
        case KORTA_WIFI_JOIN: return "JOINING";
        case KORTA_WIFI_ONLINE: return "ON";
        case KORTA_WIFI_FAIL: return "FAIL";
        case KORTA_WIFI_DENIED: return "DENY";
        default: return "OFF";
    }
}
static void fmt_ms(char *out, size_t n, uint32_t ms) {
    uint32_t sec = ms / 1000u;
    unsigned h = sec / 3600u;
    unsigned m = (sec / 60u) % 60u;
    unsigned s = sec % 60u;
    if (h) snprintf(out, n, "%u:%02u:%02u", h, m, s);
    else snprintf(out, n, "%02u:%02u", m, s);
}
static uint32_t timer_shown(const KortaState *s) {
    if (!s->timer_count) {
        uint32_t ms = s->timer_acc;
        if (s->timer_run) ms += s->now_ms - s->timer_mark;
        return ms;
    }
    if (s->timer_done) return 0;
    if (s->timer_run) return (int32_t)(s->timer_deadline - s->now_ms) <= 0 ? 0 : s->timer_deadline - s->now_ms;
    if (s->timer_remaining) return s->timer_remaining;
    return (uint32_t)s->timer_set * 1000u;
}
static void big_center(Painter *d, const char *text, int y, uint16_t color) {
    int w = scaled_width(text, 2);
    draw_scaled(d, text, 80 - w / 2, y, color, 2);
}
static const char *view_title(const KortaState *s) {
    switch (s->view) {
        case KORTA_NOTES: return "NOTES";
        case KORTA_KEYBOARD: return s->typing_find ? "SEARCH" : s->typing_pass ? "PASS" : use_flow(s) ? "FLOW" : s->symbols ? "123" : "ABC";
        case KORTA_TIME: return "CLOCK";
        case KORTA_CLOCK: return "DATE & TIME";
        case KORTA_ALARM: return "ALARM";
        case KORTA_TIMER: return "TIMER";
        case KORTA_TIMER_RING: return "TIMER";
        case KORTA_CALC: return "CALC";
        case KORTA_FIND: return "FIND";
        case KORTA_FILES: return "FILES";
        case KORTA_SETTINGS: return "SETTINGS";
        case KORTA_SETUP: return "DISPLAY";
        case KORTA_POWER: return "POWER";
        case KORTA_PLAY: return "GAME";
        case KORTA_LINKS: return "WIFI";
        case KORTA_INFO: return "ABOUT";
        case KORTA_QUICK: return "QUICK";
        case KORTA_CLEAR_CONFIRM: return "DELETE";
        case KORTA_DICE: return "DICE";
        case KORTA_COIN: return "COIN";
        case KORTA_COUNT: return "COUNT";
        case KORTA_CONV: return "CONV";
        case KORTA_FLASH: return "FLASH";
        case KORTA_COLOR: return "COLOR";
        case KORTA_SOUND: return "SOUND";
        case KORTA_CONTROLS: return "CONTROLS";
        case KORTA_PHONE: return "PHONE LINK";
        case KORTA_MESSAGE: return "MESSAGE";
        default: return NULL;
    }
}
static void paint_home(Painter *d, const KortaState *s) {
    int page = s->selected / KORTA_PAGE_SIZE;
    int cur = home_app(s);
    for (int i = 0; i < KORTA_PAGE_SIZE; i++) {
        int app = home_at(s, page, i);
        if (app < 0) continue;
        int col = i % 3;
        int row = i / 3;
        int x = 2 + col * 53;
        int y = 14 + row * 42;
        if (app == cur) {
            fill(d, x, y, 50, 41, d->p.card);
            border(d, x, y, 50, 41, d->p.ink, 2);
        }
        if (is_game(app)) {
            /* Installed games: the cartridge icon, each in its own colour. */
            blit_app(d, 9, x + 13, y + 2, app_color((app - GAME_ID + 3) % KORTA_APP_COUNT));
            centered_label(d, s->game_names[app - GAME_ID], x + 25, y + 29, d->p.ink);
            continue;
        }
        bool cart = APP_VIEW[app] == KORTA_PLAY;
        bool grey = cart && !s->hw.cart_in;
        uint16_t tile = grey ? d->p.muted : app_color(app);
        uint16_t label = grey ? d->p.muted : d->p.ink;
        blit_app(d, APP_ICON[app], x + 13, y + 2, tile);
        centered_label(d, APP_NAME[app], x + 25, y + 29, label);
    }
}
static void paint_keys(Painter *d, const KortaState *s) {
    int y0 = 36;
    fill(d, 4, 14, 152, 20, d->p.card);
    border(d, 4, 14, 152, 20, d->p.line, 1);
    if (s->typing_find) {
        draw_text(d, s->query_len ? s->query : "SEARCH", 8, 20, s->query_len ? d->p.ink : d->p.muted);
    } else if (s->typing_pass) {
        char shown[22];
        size_t n = s->pass_len;
        size_t start = n > 20 ? n - 20 : 0;
        size_t shown_n = n - start;
        for (size_t i = 0; i < shown_n; i++) {
            bool last = start + i + 1 == n;
            shown[i] = last ? s->password[start + i] : '*';
        }
        shown[shown_n] = '\0';
        draw_text(d, shown[0] ? shown : "PASSWORD", 8, 20, shown[0] ? d->p.ink : d->p.muted);
    } else if (use_flow(s)) {
        const char *note = s->notes[s->note_cur];
        uint8_t len = cur_len(s);
        const char *tail = len > 23 ? note + len - 23 : note;
        draw_text(d, tail, 8, 16, d->p.ink);
        char buf[24] = {0};
        if (s->pending_len) {
            const char *word = candidate(s);
            buf[0] = '>';
            buf[1] = ' ';
            if (word) {
                int k = 2;
                for (int i = 0; word[i] && k < (int)sizeof buf - 1; i++) buf[k++] = apply_case(s, word[i]);
                buf[k] = '\0';
            } else for (int i = 0; i < s->pending_len && i < 20; i++) buf[2 + i] = apply_case(s, GROUPS[s->pending[i] - '0'][0]);
            draw_text(d, buf, 8, 26, d->p.accent);
        }
    } else {
        const char *note = s->notes[s->note_cur];
        int len = cur_len(s);
        /* The last two 23-column lines, so text stays put while typing. */
        int first = len ? ((len - 1) / 23 - 1) * 23 : 0;
        if (first < 0) first = 0;
        char line[24];
        for (int row = 0; row < 2 && first + row * 23 < len; row++) {
            int at = first + row * 23;
            int n = len - at < 23 ? len - at : 23;
            memcpy(line, note + at, (size_t)n);
            line[n] = '\0';
            draw_text(d, line, 8, 16 + row * 10, d->p.ink);
        }
    }
    for (int r = 0; r < 4; r++) {
        int cols = use_flow(s) ? 3 : exact_count(s, r);
        for (int c = 0; c < cols; c++) {
            int x = key_x(s, r, c);
            int y = y0 + r * 22;
            int w = key_w(s, r, c);
            bool on = r == s->key_row && c == s->key_col;
            fill(d, x, y, w, 20, on ? d->p.ink : d->p.card);
            border(d, x, y, w, 20, on ? d->p.ink : d->p.line, 1);
            char one[8];
            const char *label = key_label(s, r, c, one);
            int tw = text_width(label);
            draw_text(d, label, x + (w - tw) / 2, y + 6, on ? d->p.bg : d->p.ink);
        }
    }
    fill(d, 0, 132, 160, 12, d->p.band);
    draw_text(d, "B DEL", 4, 134, d->p.band_ink);
    draw_text(d, "START DONE", 156 - text_width("START DONE"), 134, d->p.band_ink);
}
/* Breaks text into 23-column lines at spaces. Text that will not word-wrap into
 * max_lines falls back to a hard wrap, which always fits 23 * max_lines. */
static int wrap_lines(const char *text, int len, int max_lines, int *starts, int *counts) {
    int lines = 0, at = 0;
    while (at < len && lines <= max_lines) {
        while (lines && at < len && text[at] == ' ') at++;
        if (at >= len) break;
        int n = len - at < 23 ? len - at : 23;
        if (at + n < len && text[at + n] != ' ') {
            int cut = n;
            while (cut > 0 && text[at + cut - 1] != ' ') cut--;
            if (cut > 0) n = cut;
        }
        if (lines == max_lines) { lines++; break; }  /* would need one more line */
        starts[lines] = at;
        counts[lines++] = n;
        at += n;
    }
    if (lines > max_lines) {
        lines = 0;
        for (at = 0; at < len && lines < max_lines; at += 23, lines++) {
            starts[lines] = at;
            counts[lines] = len - at < 23 ? len - at : 23;
        }
    }
    return lines;
}
static void paint_message(Painter *d, const KortaState *s) {
    bool flash = s->msg_find && ((s->now_ms / 400u) & 1u) != 0;
    uint16_t bg = s->msg_find ? (flash ? d->p.accent : d->p.ink) : d->p.bg;
    uint16_t ink = s->msg_find ? d->p.bg : d->p.ink;
    fill(d, 0, 0, 160, 144, bg);
    const char *title = s->msg_find ? "HERE!" : "MESSAGE";
    draw_scaled(d, title, 80 - scaled_width(title, 2) / 2, 12, ink, 2);
    int len = (int)strlen(s->msg);
    int starts[9], counts[9];
    int lines = wrap_lines(s->msg, len, 9, starts, counts);  /* 9 x 23 holds 200 */
    int top = 34 + (9 - lines) * 5;
    char line[24];
    for (int i = 0; i < lines; i++) {
        memcpy(line, s->msg + starts[i], (size_t)counts[i]);
        line[counts[i]] = '\0';
        centered(d, line, 80, top + i * 10, ink);
    }
    centered(d, "A OK", 80, 132, s->msg_find ? d->p.bg : d->p.muted);
}
static void paint_notes(Painter *d, const KortaState *s) {
    int start, shown;
    list_window(s, 1 + s->note_count, 7, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int index = start + i;
        char right[8];
        if (index == 0) {
            snprintf(right, sizeof right, "%u/%u", s->note_count, KORTA_NOTE_SLOTS);
            row_at(d, s->note_count >= KORTA_NOTE_SLOTS ? "LIST FULL" : "NEW NOTE", right, 16 + i * 16, s->focus == 0);
        } else {
            if (index - 1 < s->note_pinned) snprintf(right, sizeof right, "PIN");
            else snprintf(right, sizeof right, "%u", s->note_lens[index - 1]);
            row_at(d, s->notes[index - 1], right, 16 + i * 16, s->focus == index);
        }
    }
    if (!s->note_count) centered(d, "NO NOTES YET", 80, 56, d->p.muted);
    if (!s->hw.storage_ok) centered(d, "SAVE FAILED", 80, 132, d->p.accent);
}
static void paint_note_open(Painter *d, const KortaState *s) {
    fill(d, 4, 14, 152, 86, d->p.card);
    border(d, 4, 14, 152, 86, d->p.line, 1);
    const char *note = s->notes[s->note_cur];
    int len = cur_len(s);
    int starts[9], counts[9];
    int lines = wrap_lines(note, len, 9, starts, counts);
    char line[24];
    for (int i = 0; i < lines; i++) {
        memcpy(line, note + starts[i], (size_t)counts[i]);
        line[counts[i]] = '\0';
        draw_text(d, line, 8, 17 + i * 9, d->p.ink);
    }
    row_at(d, "EDIT", NULL, 102, s->focus == 0);
    row_at(d, note_is_pinned(s) ? "UNPIN" : "PIN TO TOP", NULL, 116, s->focus == 1);
    row_at(d, "DELETE", NULL, 130, s->focus == 2);
}
static void paint_clock(Painter *d, const KortaState *s) {
    char big[16];
    clock_text(s, big, sizeof big, false);
    big_center(d, s->time_known ? big : "SET", 16, d->p.ink);
    char date[20];
    int month = s->month >= 1 && s->month <= 12 ? s->month : 1;
    if (s->time_known) snprintf(date, sizeof date, "%02u %s %04u", s->day, MONTHS[month - 1], s->year);
    else snprintf(date, sizeof date, "NOT SET");
    centered(d, date, 80, 36, d->p.muted);
    static const char *const labels[7] = {"HOUR", "MINUTE", "DAY", "MONTH", "YEAR", "FORMAT", "TIME ZONE"};
    int start, shown;
    list_window(s, 7, 6, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int index = start + i;
        char value[16];
        const char *right = value;
        if (index == 0) snprintf(value, sizeof value, "%02u", s->hour);
        else if (index == 1) snprintf(value, sizeof value, "%02u", s->min);
        else if (index == 2) snprintf(value, sizeof value, "%02u", s->day);
        else if (index == 3) snprintf(value, sizeof value, "%s", MONTHS[month - 1]);
        else if (index == 4) snprintf(value, sizeof value, "%04u", s->year);
        else if (index == 5) right = s->hour12 ? "12H" : "24H";
        else if (s->zone_minutes == 0) right = "UTC";
        else if (s->zone_minutes % 60 == 0) snprintf(value, sizeof value, "UTC%+d", s->zone_minutes / 60);
        else snprintf(value, sizeof value, "UTC%c%d:%02d", s->zone_minutes < 0 ? '-' : '+',
            abs(s->zone_minutes) / 60, abs(s->zone_minutes) % 60);
        row_at(d, labels[index], right, 48 + i * 15, index == s->focus);
    }
}
static void alarm_time(const KortaState *s, const KortaAlarm *a, char *out, size_t n) {
    int hour = a->hour;
    if (s->hour12) {
        char mark = hour >= 12 ? 'P' : 'A';
        hour %= 12;
        if (!hour) hour = 12;
        snprintf(out, n, "%d:%02u%c", hour, a->minute, mark);
    } else snprintf(out, n, "%02u:%02u", a->hour, a->minute);
}
static void repeat_text(uint8_t days, char *out, size_t n) {
    if (!days) snprintf(out, n, "ONCE");
    else if (days == KORTA_DAYS_ALL) snprintf(out, n, "DAILY");
    else if (days == 0x1F) snprintf(out, n, "WEEKDAYS");
    else if (days == 0x60) snprintf(out, n, "WEEKENDS");
    else {
        static const char letters[] = "MTWTFSS";
        for (int i = 0; i < 7 && i + 1 < (int)n; i++) out[i] = (days >> i & 1) ? letters[i] : '.';
        out[n < 8 ? n - 1 : 7] = '\0';
    }
}
static void paint_alarm(Painter *d, const KortaState *s) {
    const KortaAlarm *a = &s->alarms[s->alarm_cur % KORTA_ALARM_MAX];
    char big[12];
    alarm_time(s, a, big, sizeof big);
    big_center(d, big, 26, a->on ? d->p.ink : d->p.muted);
    char which[16], index[8], hm[8], mm[8], days[12];
    snprintf(which, sizeof which, "< ALARM %u >", s->alarm_cur + 1u);
    snprintf(index, sizeof index, "%u/%u", s->alarm_cur + 1u, KORTA_ALARM_MAX);
    snprintf(hm, sizeof hm, "%02u", a->hour);
    snprintf(mm, sizeof mm, "%02u", a->minute);
    repeat_text(a->days, days, sizeof days);
    row_at(d, a->label[0] ? a->label : which, index, 46, s->focus == 0);
    row_at(d, "ALARM", a->on ? "ON" : "OFF", 62, s->focus == 1);
    row_at(d, "HOUR", hm, 78, s->focus == 2);
    row_at(d, "MIN", mm, 94, s->focus == 3);
    row_at(d, "REPEAT", days, 110, s->focus == 4);
}
static void paint_timer(Painter *d, const KortaState *s) {
    char big[16];
    if (s->timer_done) snprintf(big, sizeof big, "DONE");
    else fmt_ms(big, sizeof big, timer_shown(s));
    big_center(d, big, 22, s->timer_run ? d->p.accent : d->p.ink);
    char length[12];
    fmt_ms(length, sizeof length, (uint32_t)s->timer_set * 1000u);
    row_at(d, "MODE", s->timer_count ? "COUNT" : "WATCH", 58, s->focus == 0);
    row_at(d, "LENGTH", s->timer_count ? length : "--", 76, s->focus == 1);
    row_at(d, "RESET", NULL, 94, s->focus == 2);
    centered(d, s->timer_run ? "A STOP" : "A GO", 80, 122, d->p.muted);
}
static void paint_calc(Painter *d, const KortaState *s) {
    char big[24];
    if (s->calc_err) snprintf(big, sizeof big, "ERR");
    else if (s->calc_typing) snprintf(big, sizeof big, "%s", s->calc_buf);
    else calc_format(s->calc_val, big, sizeof big);
    if (s->calc_op && !s->calc_err) {
        char pend[28];
        calc_format(s->calc_acc, pend, sizeof pend - 2);
        size_t n = strlen(pend);
        pend[n] = ' ';
        pend[n + 1] = "+-*/"[s->calc_op - 1];
        pend[n + 2] = '\0';
        draw_text(d, pend, 154 - text_width(pend), 15, d->p.muted);
    }
    int w = scaled_width(big, 2);
    if (w > 150) draw_text(d, big, 154 - text_width(big), 30, d->p.ink);
    else draw_scaled(d, big, 154 - w, 25, d->p.ink, 2);
    for (int i = 0; i < CALC_KEY_COUNT; i++) {
        int x = (i % CALC_COLS) * 40;
        int y = 44 + (i / CALC_COLS) * 20;
        bool on = i == s->focus;
        bool op = i % CALC_COLS == CALC_COLS - 1 || i < CALC_COLS;
        fill(d, x + 1, y, 38, 19, on ? d->p.ink : op ? d->p.line : d->p.card);
        const char *label = CALC_KEY[i];
        draw_text(d, label, x + 1 + (38 - text_width(label)) / 2, y + 6, on ? d->p.bg : d->p.ink);
    }
}
static void paint_find(Painter *d, const KortaState *s) {
    uint8_t ids[FIND_COUNT];
    int n = matches(s, ids);
    bool on_search = s->focus == 0;
    fill(d, 4, 14, 152, 18, d->p.card);
    border(d, 4, 14, 152, 18, on_search ? d->p.ink : d->p.line, on_search ? 2 : 1);
    draw_text(d, s->query_len ? s->query : "SEARCH", 10, 19, s->query_len ? d->p.ink : d->p.muted);
    int start = 0;
    if (s->focus > 5) start = (int)s->focus - 5;
    int shown = n - start;
    if (shown > 6) shown = 6;
    if (shown < 0) shown = 0;
    if (!n) draw_text(d, "NONE", 6, 38, d->p.muted);
    for (int i = 0; i < shown; i++) {
        int index = start + i;
        row_at(d, FIND_ITEM[ids[index]].name, NULL, 36 + i * 16, (int)s->focus == index + 1);
    }
}
static void paint_files(Painter *d, const KortaState *s) {
    if (!s->note_count) {
        row_at(d, "NO NOTES", NULL, 16, true);
        draw_text(d, "MAKE ONE IN NOTES", 7, 40, d->p.muted);
    } else {
        int start, shown;
        list_window(s, s->note_count, 5, &start, &shown);
        for (int i = 0; i < shown; i++) {
            int index = start + i;
            char size[8];
            snprintf(size, sizeof size, "%uB", s->note_lens[index]);
            row_at(d, s->notes[index], size, 16 + i * 16, s->focus == index);
        }
    }
    char used[24];
    snprintf(used, sizeof used, "NVS  %u/%u NOTES", s->note_count, KORTA_NOTE_SLOTS);
    draw_text(d, used, 7, 100, d->p.muted);
    draw_text(d, "SD UNAVAILABLE", 7, 112, d->p.muted);
    if (!s->hw.storage_ok) draw_text(d, "SAVE FAILED", 7, 124, d->p.accent);
}
static void paint_setup(Painter *d, const KortaState *s) {
    int start, shown;
    list_window(s, DISPLAY_ROWS, 8, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int row = start + i;
        int index = DISPLAY_MAP[row];
        char buf[16];
        row_at(d, setup_label(index), setup_value(s, index, buf, sizeof buf), 14 + i * 16, row == s->focus);
    }
}
static void paint_controls(Painter *d, const KortaState *s) {
    for (int i = 0; i < CONTROL_ROWS; i++) {
        char buf[16];
        int index = CONTROL_MAP[i];
        row_at(d, setup_label(index), setup_value(s, index, buf, sizeof buf), 14 + i * 16, i == s->focus);
    }
    static const char *const keys[6][2] = {
        {"START", "QUICK MENU"}, {"SELECT", "SEARCH"}, {"HOLD B", "HOME"},
        {"HOLD SELECT", "THEME"}, {"MENU+L/R", "BRIGHTNESS"}, {"MENU+A+B+ST+SE", "RESET GAME"}
    };
    draw_text(d, "SHORTCUTS", 7, 52, d->p.accent);
    for (int i = 0; i < 6; i++) {
        draw_text(d, keys[i][0], 7, 64 + i * 12, d->p.ink);
        draw_text(d, keys[i][1], 153 - text_width(keys[i][1]), 64 + i * 12, d->p.muted);
    }
}
static void paint_power(Painter *d, const KortaState *s) {
    char big[8];
    if (!s->hw.battery_known) snprintf(big, sizeof big, "--");
    else snprintf(big, sizeof big, "%u%%", s->hw.battery_pct);
    big_center(d, big, 18, d->p.ink);
    char line[24];
    if (s->hw.battery_known) {
        snprintf(line, sizeof line, "%u.%02uV %s", s->hw.battery_mv / 1000, (s->hw.battery_mv % 1000) / 10, s->hw.lipo ? "POWER CORE" : "AA CELLS");
        centered(d, line, 80, 40, d->p.ink);
        centered(d, s->hw.charging ? "CHARGING" : "ON BATTERY", 80, 52, d->p.muted);
    } else centered(d, "READING BATTERY", 80, 44, d->p.muted);
    char level[8], batt[8];
    snprintf(level, sizeof level, "%u", s->hw.brightness);
    const char *low = setup_value(s, 9, batt, sizeof batt);
    row_at(d, setup_label(0), level, 72, s->focus == 0);
    row_at(d, setup_label(9), low, 90, s->focus == 1);
}
static void paint_play(Painter *d, const KortaState *s) {
    blit_app(d, 9, 68, 22, s->hw.cart_in ? app_color(0) : d->p.muted);
    if (!s->hw.cart_in) {
        centered(d, "NO CART", 80, 56, d->p.muted);
        centered(d, "PLUG ONE IN", 80, 72, d->p.muted);
        return;
    }
    centered(d, "CART READY", 80, 54, d->p.ink);
    centered(d, "A PLAYS", 80, 78, d->p.ink);
    centered(d, "B BACK", 80, 96, d->p.muted);
}
static void fmt_ip(char *out, size_t n, uint32_t ip) {
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip & 255u), (unsigned)((ip >> 8) & 255u),
        (unsigned)((ip >> 16) & 255u), (unsigned)((ip >> 24) & 255u));
}
static const char *wifi_banner(const KortaState *s) {
    switch (s->link.phase) {
        case KORTA_WIFI_OFF: return "RADIO OFF";
        case KORTA_WIFI_SCAN: return "SEARCHING";
        case KORTA_WIFI_JOIN: return "CONNECTING";
        case KORTA_WIFI_ONLINE: return "CONNECTED";
        case KORTA_WIFI_FAIL: return "NO LINK";
        case KORTA_WIFI_DENIED: return (s->pass_len > 0 && s->pass_len < 8) ? "NEED 8" : "WRONG PASS";
        default: return s->link.count == 0 ? "NONE NEARBY" : "PICK A NETWORK";
    }
}
/* The Time screen's summary: the one armed alarm's time, or how many are on. */
static void alarm_text(const KortaState *s, char *out, size_t n) {
    int on = 0, first = -1;
    for (int i = 0; i < KORTA_ALARM_MAX; i++) if (s->alarms[i].on) { on++; if (first < 0) first = i; }
    if (!on) snprintf(out, n, "OFF");
    else if (on == 1) alarm_time(s, &s->alarms[first], out, n);
    else snprintf(out, n, "%d ON", on);
}
static void paint_time(Painter *d, const KortaState *s) {
    char clk[16], alarm[16], length[12];
    if (s->time_known) clock_text(s, clk, sizeof clk, false);
    else snprintf(clk, sizeof clk, "--:--");
    alarm_text(s, alarm, sizeof alarm);
    /* A running or paused timer shows what is left; otherwise its length. */
    bool active = s->timer_run || s->timer_remaining || s->timer_acc;
    fmt_ms(length, sizeof length, active ? timer_shown(s) : (uint32_t)s->timer_set * 1000u);
    row_at(d, "CLOCK", clk, 24, s->focus == 0);
    row_at(d, "ALARM", alarm, 44, s->focus == 1);
    row_at(d, "TIMER", length, 64, s->focus == 2);
}
static void paint_settings(Painter *d, const KortaState *s) {
    for (int i = 0; i < SET_HUB; i++) {
        char buf[16];
        const char *right = ">";
        if (i == 1) right = wifi_word(s->link.phase);
        else if (i == 3 && s->hw.silent) right = "SILENT";
        else if (i == 5) {
            if (s->hw.battery_known) snprintf(buf, sizeof buf, "%u%%", s->hw.battery_pct);
            else snprintf(buf, sizeof buf, "--");
            right = buf;
        } else if (i == 6) {
            if (s->time_known) clock_text(s, buf, sizeof buf, false);
            else snprintf(buf, sizeof buf, "SET");
            right = buf;
        }
        row_at(d, SET_HUB_NAME[i], right, 14 + i * 16, i == s->focus);
    }
}
static void ellipse(Painter *d, int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx < 1 || ry < 1) return;
    int64_t limit = (int64_t)rx * rx * ry * ry;
    for (int y = -ry; y <= ry; y++) {
        int x = rx;
        while (x > 0 && (int64_t)x * x * ry * ry + (int64_t)y * y * rx * rx > limit) x--;
        if (x == 0 && (y == -ry || y == ry)) continue;  /* no 1-pixel tip */
        fill(d, cx - x, cy + y, x * 2 + 1, 1, color);
    }
}
static void pip(Painter *d, int x, int y, int r) {
    fill(d, x - r, y - r, 2 * r + 1, 2 * r + 1, d->p.mark);
    fill(d, x - r, y - r, 1, 1, d->p.hi);
    fill(d, x + r, y - r, 1, 1, d->p.hi);
    fill(d, x - r, y + r, 1, 1, d->p.hi);
    fill(d, x + r, y + r, 1, 1, d->p.hi);
}
static void die_body(Painter *d, int x, int top, int size) {
    fill(d, x + 3, top + 5, size, size, d->p.line);
    fill(d, x, top, size, size, d->p.ink);
    fill(d, x + 3, top + 3, size - 6, size - 6, d->p.hi);
    fill(d, x + 3, top + 3, size - 6, 2, d->p.card);
    fill(d, x, top, 3, 3, d->p.bg);
    fill(d, x + size - 3, top, 3, 3, d->p.bg);
    fill(d, x, top + size - 3, 3, 3, d->p.bg);
    fill(d, x + size - 3, top + size - 3, 3, 3, d->p.bg);
}
static void die_pips(Painter *d, int x, int top, int size, int face) {
    int r = size >= 48 ? 3 : 2;
    int lo = size * 27 / 100, mid = size / 2, hi = size - lo;
    if (face == 1 || face == 3 || face == 5) pip(d, x + mid, top + mid, r);
    if (face >= 2) { pip(d, x + lo, top + lo, r); pip(d, x + hi, top + hi, r); }
    if (face >= 4) { pip(d, x + hi, top + lo, r); pip(d, x + lo, top + hi, r); }
    if (face == 6) { pip(d, x + lo, top + mid, r); pip(d, x + hi, top + mid, r); }
}
static void paint_dice(Painter *d, const KortaState *s) {
    const DiceKind *k = &DICE[s->dice_kind % DICE_KINDS];
    bool rolling = s->dice && (int32_t)(s->dice_anim_until - s->now_ms) > 0;
    uint32_t tick = rolling ? (640u - (s->dice_anim_until - s->now_ms)) / 64u : 0u;
    static const int8_t bounce[10] = {0, -3, -6, -4, -1, -5, -3, 0, -2, 0};
    int lift = rolling ? bounce[tick % 10u] : 0;
    int face = rolling ? (int)((tick * 7u + s->roll_nonce) % k->sides) + 1 : s->dice;
    int face2 = rolling ? (int)((tick * 5u + s->roll_nonce + 3u) % k->sides) + 1 : s->dice2;
    char label[12];
    snprintf(label, sizeof label, "< %s >", k->name);
    centered(d, label, 80, 16, d->p.ink);
    ellipse(d, 80, 96, 40, 5, d->p.line);
    if (k->two) {
        int top = 44 + lift;
        die_body(d, 34, top, 40);
        die_body(d, 86, top - lift / 2, 40);
        if (face) { die_pips(d, 34, top, 40, face); die_pips(d, 86, top - lift / 2, 40, face2); }
    } else if (k->sides == 6) {
        int top = 31 + lift;
        die_body(d, 54, top, 52);
        if (face) die_pips(d, 54, top, 52, face);
    } else {
        int top = 31 + lift;
        die_body(d, 50, top, 60);
        char n[8];
        snprintf(n, sizeof n, "%d", face);
        if (face) draw_scaled(d, n, 80 - scaled_width(n, 3) / 2, top + 20, d->p.mark, 3);
    }
    if (!face) centered(d, "?", 80, 60, d->p.accent);
    char result[20];
    if (rolling) snprintf(result, sizeof result, "ROLLING");
    else if (!s->dice) snprintf(result, sizeof result, "READY");
    else if (k->two) snprintf(result, sizeof result, "%u + %u = %u", s->dice, s->dice2, s->dice + s->dice2);
    else snprintf(result, sizeof result, "ROLLED %u", s->dice);
    centered(d, result, 80, 105, d->p.ink);
    centered(d, "A ROLL  LR DIE  B BACK", 80, 126, d->p.muted);
}
static void paint_coin(Painter *d, const KortaState *s) {
    bool flipping = s->coin && (int32_t)(s->coin_anim_until - s->now_ms) > 0;
    uint32_t tick = flipping ? (640u - (s->coin_anim_until - s->now_ms)) / 64u : 0u;
    static const uint8_t widths[10] = {27, 20, 10, 3, 11, 27, 18, 5, 18, 27};
    int rx = flipping ? widths[tick % 10u] : 27;
    int face = flipping ? ((tick & 1u) ? 1 : 2) : s->coin;
    ellipse(d, 80, 96, 34, 4, d->p.line);
    ellipse(d, 82, 64, rx, 28, d->p.ink);
    ellipse(d, 80, 61, rx, 28, d->p.accent);
    ellipse(d, 80, 61, rx > 4 ? rx - 4 : 1, 24, d->p.hi);
    if (rx > 12) {
        ellipse(d, 80, 61, rx - 8, 20, d->p.card);
        draw_scaled(d, face == 1 ? "H" : face == 2 ? "T" : "?", 73, 50, d->p.accent, 3);
        if (!flipping && face) {
            fill(d, 78, 37, 4, 2, d->p.accent);
            fill(d, 78, 83, 4, 2, d->p.accent);
        }
    }
    /* The tally already counts the flip in progress; hide it until it lands. */
    unsigned heads = s->heads, tails = s->tails;
    if (flipping && s->coin == 1 && heads) heads--;
    else if (flipping && s->coin == 2 && tails) tails--;
    centered(d, flipping ? "FLIPPING" : s->coin == 1 ? "HEADS" : s->coin == 2 ? "TAILS" : "READY", 80, 104, d->p.ink);
    char tally[24];
    snprintf(tally, sizeof tally, "H %u   T %u", heads, tails);
    centered(d, tally, 80, 115, d->p.muted);
    centered(d, "A FLIP  DOWN CLEAR", 80, 128, d->p.muted);
}
static void paint_count(Painter *d, const KortaState *s) {
    char n[8];
    snprintf(n, sizeof n, "%d", s->tally);
    if (s->focus == 0) {
        fill(d, 20, 22, 120, 40, d->p.card);
        border(d, 20, 22, 120, 40, d->p.ink, 2);
    }
    big_center(d, n, 35, d->p.ink);
    centered(d, "A/RIGHT +1  LEFT -1", 80, 70, d->p.muted);
    row_at(d, "RESET", NULL, 90, s->focus == 1);
}
static void fmt_tenths(char *out, size_t n, int32_t tenths, const char *unit) {
    int32_t a = tenths < 0 ? -tenths : tenths;
    snprintf(out, n, "%s%ld.%ld %s", tenths < 0 ? "-" : "", (long)(a / 10), (long)(a % 10), unit);
}
static void paint_conv(Painter *d, const KortaState *s) {
    const Conv *c = &CONV[s->conv_kind % CONV_COUNT];
    char kind[16], left[16], right[20];
    snprintf(kind, sizeof kind, "%s TO %s", c->from, c->to);
    snprintf(left, sizeof left, "%d %s", s->conv_n, c->from);
    fmt_tenths(right, sizeof right, conv_tenths(s->conv_kind, s->conv_n), c->to);
    row_at(d, kind, NULL, 24, s->focus == 0);
    row_at(d, left, right, 44, s->focus == 1);
    centered(d, s->focus == 0 ? "LEFT RIGHT  UNITS" : "LEFT RIGHT  VALUE", 80, 80, d->p.muted);
    if (s->focus == 1) centered(d, "A SWAP", 80, 92, d->p.muted);
}
static const char *const FLASH_TINT_NAME[FLASH_TINTS] = {"WHITE", "WARM", "RED"};
static const char *const FLASH_MODE_NAME[FLASH_MODES] = {"STEADY", "BLINK", "SOS"};
static bool flash_lit(const KortaState *s) {
    uint32_t t = s->now_ms - s->flash_mark;
    if (s->flash_mode == 1) return (t / 500u) % 2u == 0;
    if (s->flash_mode == 2) {
        /* Morse SOS in 160 ms units: dot 1, dash 3, gaps 1/3/7. */
        static const char SOS[] = "1010100011101110111000101010000000";
        return SOS[(t / 160u) % (sizeof SOS - 1)] == '1';
    }
    return true;
}
static void paint_flash(Painter *d, const KortaState *s) {
    static const unsigned char tint[FLASH_TINTS][3] = {{255, 255, 255}, {255, 196, 120}, {255, 0, 0}};
    const unsigned char *t = tint[s->flash_tint % FLASH_TINTS];
    fill(d, 0, 0, 160, 144, flash_lit(s) ? rgb(t[0], t[1], t[2]) : rgb(0, 0, 0));
    if ((int32_t)(s->flash_hud_until - s->now_ms) <= 0) return;
    fill(d, 0, 118, 160, 26, d->p.band);
    char line[28];
    snprintf(line, sizeof line, "%s  %s", FLASH_TINT_NAME[s->flash_tint % FLASH_TINTS],
        FLASH_MODE_NAME[s->flash_mode % FLASH_MODES]);
    draw_text(d, line, 4, 121, d->p.band_ink);
    snprintf(line, sizeof line, "%u/16", s->hw.brightness);
    draw_text(d, line, 156 - text_width(line), 121, d->p.band_ink);
    draw_text(d, "UD LEVEL LR TINT A MODE", 4, 133, d->p.band_ink);
}
static void paint_color(Painter *d, const KortaState *s) {
    unsigned p = s->hw.palette < 15 ? s->hw.palette : 0;
    char name[16];
    snprintf(name, sizeof name, "< %s >", PALETTE_NAME[p]);
    row_at(d, "PALETTE", name, 18, true);
    static const char *const part[3] = {"BG", "OBJ0", "OBJ1"};
    for (int r = 0; r < 3; r++) {
        int y = 40 + r * 18;
        draw_text(d, part[r], 7, y + 4, d->p.muted);
        for (int c = 0; c < 4; c++) {
            int x = 40 + c * 28;
            uint16_t col = s->hw.game_colors ? rgb((unsigned)(232 - c * 64), (unsigned)(232 - c * 64), (unsigned)(232 - c * 64))
                : s->hw.pal[r][c];
            fill(d, x, y, 26, 15, col);
            border(d, x, y, 26, 15, d->p.ink, 1);
        }
    }
    if (s->hw.gbc_game) {
        centered(d, "THIS GBC GAME USES", 80, 100, d->p.accent);
        centered(d, "ITS OWN COLORS", 80, 112, d->p.accent);
    } else {
        centered(d, s->hw.game_colors ? "GAME PICKS ITS COLORS" : "FOR ORIGINAL GB GAMES", 80, 100, d->p.muted);
        centered(d, "GBC GAMES KEEP THEIRS", 80, 112, d->p.muted);
    }
}
static void paint_sound(Painter *d, const KortaState *s) {
    char sys[8], med[8], rng[8];
    snprintf(sys, sizeof sys, "%u", s->vol_sys);
    snprintf(med, sizeof med, "%u", s->vol_media);
    snprintf(rng, sizeof rng, "%u", s->vol_ring);
    row_at(d, "SILENT", s->hw.silent ? "ON" : "OFF", 20, s->focus == 0);
    row_at(d, "SYSTEM", sys, 40, s->focus == 1);
    row_at(d, "RING", rng, 60, s->focus == 2);
    row_at(d, "MEDIA", med, 80, false);
    centered(d, "WHEEL SETS MEDIA", 80, 112, d->p.muted);
}
static void paint_phone(Painter *d, const KortaState *s) {
    char paired[12];
    snprintf(paired, sizeof paired, "%u PAIRED", s->link.web_paired);
    row_at(d, "PHONE LINK", s->web_keep ? "ON" : "OFF", 14, s->focus == 0);
    row_at(d, "FORGET PHONES", s->link.web_paired ? paired : "NONE", 30, s->focus == 1);
    if (!s->web_keep) {
        draw_text(d, "EDIT NOTES, ALARM AND", 7, 56, d->p.ink);
        draw_text(d, "CLOCK FROM ANY PHONE", 7, 68, d->p.ink);
        draw_text(d, "OR COMPUTER. NEEDS", 7, 80, d->p.ink);
        draw_text(d, "WI-FI. NO APP OR", 7, 92, d->p.ink);
        draw_text(d, "ACCOUNT NEEDED.", 7, 104, d->p.ink);
        centered(d, "A TURNS IT ON", 80, 126, d->p.muted);
        return;
    }
    if (s->link.phase != KORTA_WIFI_ONLINE) {
        draw_text(d, "WAITING FOR WI-FI", 7, 56, d->p.ink);
        draw_text(d, wifi_banner(s), 7, 70, d->p.muted);
        draw_text(d, "SETTINGS > WIFI TO", 7, 92, d->p.muted);
        draw_text(d, "PICK A NETWORK", 7, 104, d->p.muted);
        return;
    }
    if (!s->link.web_ready) {
        draw_text(d, "CONNECTING...", 7, 56, d->p.ink);
        draw_text(d, "NEEDS INTERNET ON", 7, 74, d->p.muted);
        draw_text(d, "THIS WI-FI", 7, 86, d->p.muted);
        return;
    }
    draw_text(d, "IN A BROWSER, OPEN", 7, 52, d->p.muted);
    draw_text(d, s->link.web_host, 7, 64, d->p.ink);
    draw_text(d, "PAIRING CODE", 7, 92, d->p.muted);
    char code[8];
    snprintf(code, sizeof code, "%.3s %.3s", s->link.web_code, s->link.web_code[0] ? s->link.web_code + 3 : "");
    draw_scaled(d, code, 7, 104, d->p.accent, 2);
}
static void paint_links(Painter *d, const KortaState *s) {
    const char *banner = wifi_banner(s);
    bool hot = s->link.phase == KORTA_WIFI_JOIN || s->link.phase == KORTA_WIFI_ONLINE;
    draw_text(d, banner, 6, 16, hot ? d->p.accent : d->p.ink);
    if (s->link.phase == KORTA_WIFI_ONLINE && s->link.ip) {
        char ip[16];
        fmt_ip(ip, sizeof ip, s->link.ip);
        draw_text(d, ip, 154 - text_width(ip), 16, d->p.ink);
    } else if (s->link.ssid[0] && (s->link.phase == KORTA_WIFI_JOIN || s->link.phase == KORTA_WIFI_FAIL || s->link.phase == KORTA_WIFI_DENIED)) {
        char name[12];
        snprintf(name, sizeof name, "%.10s", s->link.ssid);
        draw_text(d, name, 154 - text_width(name), 16, d->p.muted);
    }
    int rows = link_rows(s);
    int start, shown;
    list_window(s, rows, 7, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int index = start + i;
        const char *label = "WIFI";
        const char *right = wifi_word(s->link.phase);
        if (index > 0 && !(s->link.saved && index == rows - 1)) {
            int net = index - 1;
            if (net >= 0 && net < s->link.count) {
                label = s->link.nets[net].ssid;  /* row_at marks a cut name */
                right = s->link.nets[net].locked ? "LOCK" : "OPEN";
                if (s->link.phase == KORTA_WIFI_ONLINE && strcmp(s->link.nets[net].ssid, s->link.ssid) == 0) right = "ON";
            }
        } else if (index > 0) {
            label = "FORGET";
            right = NULL;
        }
        row_at(d, label, right, 28 + i * 16, index == s->focus);
    }
}
static void paint_info(Painter *d, const KortaState *s) {
    char v[INFO_ROWS][24];
    static const char *const label[INFO_ROWS] = {
        "KORTAMOD", "CHROMATIC", "MCU", "FPGA", "SERIAL", "PLAYER NO.", "NOTES", "FREE MEMORY", "WIFI"
    };
    const char *dash = strchr(s->hw.mcu, '-');
    snprintf(v[0], sizeof v[0], "%s", dash ? dash + 1 : "-");
    snprintf(v[1], sizeof v[1], "%s", s->hw.chromatic[0] ? s->hw.chromatic : "-");
    snprintf(v[2], sizeof v[2], "%.*s", dash ? (int)(dash - s->hw.mcu) : 19, s->hw.mcu[0] ? s->hw.mcu : "-");
    snprintf(v[3], sizeof v[3], "%s", s->hw.fpga[0] ? s->hw.fpga : "-");
    snprintf(v[4], sizeof v[4], "%s", s->hw.serial[0] ? s->hw.serial : "-");
    snprintf(v[5], sizeof v[5], "< %u >", s->hw.player ? s->hw.player : 1);
    snprintf(v[6], sizeof v[6], "%u OF %u", s->note_count, KORTA_NOTE_SLOTS);
    snprintf(v[7], sizeof v[7], "%uK", s->hw.heap_kb);
    if (s->link.phase == KORTA_WIFI_ONLINE && s->link.ip) fmt_ip(v[8], sizeof v[8], s->link.ip);
    else snprintf(v[8], sizeof v[8], "%s", wifi_word(s->link.phase));
    int start, shown;
    list_window(s, INFO_ROWS, 8, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int index = start + i;
        row_at(d, label[index], v[index], 14 + i * 16, index == s->focus);
    }
}
static void paint_quick(Painter *d, const KortaState *s) {
    char level[8];
    snprintf(level, sizeof level, "%u", s->hw.brightness);
    row_at(d, "BRIGHTNESS", level, 20, s->focus == 0);
    row_at(d, "THEME", s->night ? "NIGHT" : "PAPER", 38, s->focus == 1);
    row_at(d, "SILENT", s->hw.silent ? "ON" : "OFF", 56, s->focus == 2);
    row_at(d, "VOLUME", NULL, 74, s->focus == 3);
    row_at(d, "GAME", NULL, 92, s->focus == 4);
}
static void paint_clear(Painter *d, const KortaState *s) {
    draw_text(d, "DELETE NOTE?", 7, 28, d->p.ink);
    row_at(d, "CANCEL", NULL, 52, s->focus == 0);
    row_at(d, "DELETE", NULL, 70, s->focus == 1);
}
static void paint_update(Painter *d, const KortaState *s) {
    fill(d, 0, 0, 160, 144, d->p.bg);
    static const char *const words[] = {"", "UPDATING", "CHECKING", "RESTARTING", "FINISHING"};
    const char *w = words[s->update_phase < 5 ? s->update_phase : 0];
    centered(d, w, 80, 52, d->p.ink);
    bool bar = s->update_phase == KORTA_UPDATE_DOWNLOAD || s->update_phase == KORTA_UPDATE_FINISH;
    if (bar) {
        int pct = s->update_percent > 100 ? 100 : s->update_percent;
        fill(d, 30, 68, 100, 4, d->p.line);
        fill(d, 30, 68, pct, 4, d->p.accent);
        char num[8];
        snprintf(num, sizeof num, "%d%%", pct);
        centered(d, num, 80, 78, d->p.muted);
    }
    centered(d, "DON'T TURN OFF", 80, 120, d->p.muted);
}
/* While a game runs the port has already drawn it; only overlays are painted. */
static void paint_arcade(Painter *d, const KortaState *s) {
    const char *name = s->game_names[s->game_cur % KORTA_GAME_MAX];
    if (s->arcade_phase == KORTA_ARCADE_LOADING) {
        fill(d, 0, 0, 160, 144, d->p.bg);
        blit_app(d, 9, 68, 44, app_color((s->game_cur + 3) % KORTA_APP_COUNT));
        centered(d, name, 80, 76, d->p.ink);
        centered(d, "LOADING", 80, 90, d->p.muted);
        centered(d, "B CANCEL", 80, 132, d->p.muted);
        return;
    }
    if (s->arcade_phase == KORTA_ARCADE_ERROR) {
        fill(d, 0, 0, 160, 144, d->p.bg);
        centered(d, name, 80, 12, d->p.ink);
        centered(d, "GAME STOPPED", 80, 26, d->p.accent);
        int len = (int)strlen(s->arcade_error);
        int starts[9], counts[9];
        int lines = wrap_lines(s->arcade_error, len, 8, starts, counts);
        char line[24];
        for (int i = 0; i < lines; i++) {
            memcpy(line, s->arcade_error + starts[i], (size_t)counts[i]);
            line[counts[i]] = '\0';
            draw_text(d, line, 11, 44 + i * 10, d->p.ink);
        }
        centered(d, "A BACK", 80, 132, d->p.muted);
        return;
    }
    if (!s->arcade_paused) return;
    fill(d, 24, 40, 112, 64, d->p.bg);
    border(d, 24, 40, 112, 64, d->p.ink, 1);
    centered(d, name, 80, 47, d->p.muted);
    row_at(d, "RESUME", NULL, 62, s->arcade_sel == 0);
    row_at(d, "QUIT", NULL, 80, s->arcade_sel == 1);
}
static void paint_ring(Painter *d, const KortaState *s, const char *title) {
    bool flash = ((s->now_ms / 400u) & 1u) != 0;
    fill(d, 0, 0, 160, 144, flash ? d->p.accent : d->p.ink);
    int w = scaled_width(title, 2);
    draw_scaled(d, title, 80 - w / 2, 40, d->p.bg, 2);
    char big[12];
    if (s->view == KORTA_TIMER_RING) snprintf(big, sizeof big, "DONE");
    else clock_text(s, big, sizeof big, false);
    draw_scaled(d, big, 80 - scaled_width(big, 2) / 2, 66, d->p.bg, 2);
    const char *label = s->view == KORTA_ALARM_RING ? s->alarms[s->alarm_ringing % KORTA_ALARM_MAX].label : "";
    if (label[0]) centered(d, label, 80, 24, d->p.bg);
    draw_text(d, "A STOP", 62, 96, d->p.bg);
}

void korta_init(KortaState *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->view = KORTA_HOME;
    s->quick_from = KORTA_HOME;
    s->flow = true;
    s->caps = true;
    s->year = 2026;
    s->month = 10;
    s->day = 15;
    s->hour = 12;
    for (int i = 0; i < KORTA_ALARM_MAX; i++) {
        s->alarms[i].hour = 7;
        s->alarms[i].days = KORTA_DAYS_ALL;
    }
    s->timer_count = true;
    s->timer_set = 60;
    s->calc_fresh = true;
    s->hw.storage_ok = true;
    s->hw.brightness = 7;
    s->hw.blend = true;
    s->hw.cc_usb = true;
    s->hw.player = 1;
    s->hw.cart_in = true;
    s->vol_sys = 50;
    s->vol_media = 50;
    s->vol_ring = 80;
}
void korta_set_status(KortaState *s, const KortaStatus *status) {
    if (!s || !status) return;
    if (status->time_known && !s->clock_dirty) {
        s->time_known = true;
        s->year = status->year;
        s->month = status->month;
        s->day = status->day;
        s->hour = status->hour;
        s->min = status->min;
        s->sec = status->sec;
    }
    s->hw = *status;
    if (s->hw.brightness < 1) s->hw.brightness = 1;
    if (s->hw.brightness > 16) s->hw.brightness = 16;
    s->hw.volume = s->vol_media;
}
void korta_set_link(KortaState *s, const KortaLink *link) {
    if (!s || !link) return;
    /* A rescan reorders the list; keep the cursor on the same row by meaning. */
    int role = 0;  /* 0 the Wi-Fi toggle, 1 Forget, 2 a network */
    char focused[33] = "";
    if (s->view == KORTA_LINKS && s->focus > 0) {
        int rows = link_rows(s);
        if (s->link.saved && s->focus == rows - 1) role = 1;
        else if (s->focus - 1 < s->link.count) {
            role = 2;
            memcpy(focused, s->link.nets[s->focus - 1].ssid, sizeof focused);
            focused[32] = '\0';
        }
    }
    s->link = *link;
    if (s->link.count > KORTA_NET_MAX) s->link.count = KORTA_NET_MAX;
    s->link.ssid[sizeof s->link.ssid - 1] = '\0';
    for (int i = 0; i < KORTA_NET_MAX; i++) s->link.nets[i].ssid[32] = '\0';
    if (s->view == KORTA_LINKS) {
        int n = link_rows(s);
        if (role == 1 && s->link.saved) s->focus = (uint8_t)(n - 1);
        else if (role == 2) {
            for (int i = 0; i < s->link.count; i++) {
                if (strncmp(s->link.nets[i].ssid, focused, 32) == 0) { s->focus = (uint8_t)(i + 1); break; }
            }
        }
        if (n < 1) s->focus = 0;
        else if ((int)s->focus >= n) s->focus = (uint8_t)(n - 1);
    }
}
void korta_apply_prefs(KortaState *s, uint8_t flags, int8_t zone, uint8_t alarm_h, uint8_t alarm_m) {
    if (!s) return;
    s->night = (flags & 1u) != 0;
    s->flow = (flags & 2u) != 0;
    s->hour12 = (flags & 4u) != 0;
    s->alarms[0].on = (flags & 8u) != 0;
    s->wifi_keep = (flags & 16u) != 0;
    s->web_keep = (flags & 32u) != 0;
    if (zone < -12) zone = -12;
    if (zone > 14) zone = 14;
    s->zone = zone;
    s->zone_minutes = (int16_t)(zone * 60);
    s->alarms[0].hour = (uint8_t)(alarm_h % 24);
    s->alarms[0].minute = (uint8_t)(alarm_m % 60);
}
void korta_load_note(KortaState *s, const char *text) {
    if (!s || !text || !text[0] || s->note_count >= KORTA_NOTE_SLOTS) return;
    size_t n = strlen(text);
    if (n > KORTA_NOTE_CAPACITY) n = KORTA_NOTE_CAPACITY;
    char *dst = s->notes[s->note_count];
    memcpy(dst, text, n);
    dst[n] = '\0';
    s->note_lens[s->note_count++] = (uint8_t)n;
    s->note_rev++;
}
void korta_load_pins(KortaState *s, int n) {
    if (!s) return;
    s->note_pinned = (uint8_t)(n < 0 ? 0 : n > s->note_count ? s->note_count : n);
}
void korta_hidden(KortaState *s) {
    if (!s) return;
    if (s->view == KORTA_ALARM_RING) { s->alarm_latched = true; s->alarm_latch_min = (uint16_t)(s->hour * 60 + s->min); }
    if (is_alert(s->view)) return_to(s, s->ring_from);
    if (s->view == KORTA_FLASH) go_home(s);
    if (s->view == KORTA_ARCADE && s->arcade_phase == KORTA_ARCADE_RUNNING) { s->arcade_paused = true; s->arcade_sel = 0; }
}
void korta_set_games(KortaState *s, const char *const *names, int count) {
    if (!s) return;
    if (count < 0) count = 0;
    if (count > KORTA_GAME_MAX) count = KORTA_GAME_MAX;
    memset(s->game_names, 0, sizeof s->game_names);
    for (int i = 0; i < count; i++) {
        if (names && names[i]) korta_clean_text(s->game_names[i], sizeof s->game_names[i], names[i]);
        if (!s->game_names[i][0]) snprintf(s->game_names[i], sizeof s->game_names[i], "GAME %d", i + 1);
    }
    s->game_count = (uint8_t)count;
    /* The list changed under a running game: stop it. */
    if (s->view == KORTA_ARCADE && s->game_cur >= count) quit_game(s);
    /* Keep the Home cursor on a real tile. */
    if (home_app(s) < 0) s->selected = 0;
}
void korta_set_update(KortaState *s, int phase, int percent) {
    if (!s) return;
    s->update_phase = (uint8_t)(phase >= KORTA_UPDATE_DOWNLOAD && phase <= KORTA_UPDATE_FINISH ? phase : KORTA_UPDATE_NONE);
    s->update_percent = (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent);
}
void korta_open_game(KortaState *s, int index) {
    if (!s || index < 0 || index >= s->game_count || s->view == KORTA_ARCADE || is_alert(s->view)) return;
    start_game(s, index);
}
void korta_arcade_status(KortaState *s, int phase, const char *error) {
    if (!s || s->view != KORTA_ARCADE) return;
    s->arcade_phase = (uint8_t)(phase == KORTA_ARCADE_RUNNING ? KORTA_ARCADE_RUNNING : KORTA_ARCADE_ERROR);
    s->arcade_paused = false;
    snprintf(s->arcade_error, sizeof s->arcade_error, "%s", error ? error : "");
}
int korta_clean_text(char *dst, int size, const char *src) {
    /* Latin-1 letters (UTF-8 C3 80..BF) folded to their base letters. */
    static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int n = 0;
    if (!dst || size < 2) return 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && n < size - 1;) {
        unsigned c = *p;
        if (c < 0x80) {
            dst[n++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (c < 32 || c == 127) ? '?' : (char)c;
            p++;
            continue;
        }
        char out = '?';
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) out = latin[p[1] - 0x80];
        else if (c == 0xC2 && p[1] == 0xA0) out = ' ';
        else if (c == 0xE2 && p[1] == 0x80) {
            unsigned k = p[2];
            if (k == 0x98 || k == 0x99) out = '\'';
            else if (k == 0x9C || k == 0x9D) out = '"';
            else if (k == 0x93 || k == 0x94) out = '-';
            else if (k == 0xA2) out = '*';
            else if (k == 0xA6) out = '.';
        }
        dst[n++] = out;
        p++;
        while ((*p & 0xC0) == 0x80) p++;  /* skip the rest of the sequence */
    }
    dst[n] = '\0';
    return n;
}
int korta_web_notes(KortaState *s, uint16_t rev, const char *const *texts, int count, int pinned) {
    if (!s || count < 0 || count > KORTA_NOTE_SLOTS || (count && !texts) || pinned < 0 || pinned > count) return KORTA_WEB_INVALID;
    /* Never swap the list under someone typing or confirming a delete on the device. */
    if ((s->view == KORTA_KEYBOARD && !s->typing_pass && !s->typing_find) || s->view == KORTA_CLEAR_CONFIRM)
        return KORTA_WEB_BUSY;
    if (rev != s->note_rev) return KORTA_WEB_CONFLICT;
    memset(s->notes, 0, sizeof s->notes);
    memset(s->note_lens, 0, sizeof s->note_lens);
    s->note_count = 0;
    s->note_pinned = 0;
    for (int i = 0; i < count; i++) {
        char clean[KORTA_NOTE_CAPACITY + 1];
        korta_clean_text(clean, sizeof clean, texts[i]);
        korta_load_note(s, clean);
        /* An empty text is dropped; a pinned one shrinks the pinned block with it. */
        if (i < pinned && s->note_count == s->note_pinned + 1) s->note_pinned++;
    }
    s->note_rev++;
    if (s->note_cur >= s->note_count) s->note_cur = s->note_count ? (uint8_t)(s->note_count - 1) : 0;
    if (s->view == KORTA_NOTE_OPEN && !s->note_count) set_view(s, KORTA_NOTES);
    int n = list_count(s);
    if (n > 0 && s->focus >= n) s->focus = (uint8_t)(n - 1);
    push(s, KORTA_ACT_PREFS, 0);
    return KORTA_WEB_OK;
}
int korta_web_alarm(KortaState *s, int index, bool on, int hour, int minute, int days, const char *label) {
    if (!s || index < 0 || index >= KORTA_ALARM_MAX || hour < 0 || hour > 23 || minute < 0 || minute > 59
        || days < 0 || days > KORTA_DAYS_ALL) return KORTA_WEB_INVALID;
    KortaAlarm *a = &s->alarms[index];
    a->on = on;
    a->hour = (uint8_t)hour;
    a->minute = (uint8_t)minute;
    a->days = (uint8_t)days;
    if (label) korta_clean_text(a->label, sizeof a->label, label);
    push(s, KORTA_ACT_PREFS, 0);
    return KORTA_WEB_OK;
}
uint32_t korta_timer_ms(const KortaState *s) { return s ? timer_shown(s) : 0; }
int korta_web_setting(KortaState *s, int key, int value) {
    if (!s) return KORTA_WEB_INVALID;
    KortaStatus *h = &s->hw;
    bool on = value != 0;
    switch (key) {
        case KORTA_SET_BRIGHTNESS: {
            if (value < 1 || value > 16) return KORTA_WEB_INVALID;
            bool boosted = s->view == KORTA_FLASH || s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING
                || (s->view == KORTA_MESSAGE && s->msg_find);
            if (boosted) {  /* the user's level, returned when the override ends */
                s->bright_keep = (uint8_t)value;
                push(s, KORTA_ACT_BRIGHT_HOLD, (uint16_t)value);
            } else {
                h->brightness = (uint8_t)value;
                push(s, KORTA_ACT_BRIGHTNESS, (uint16_t)value);
            }
            return KORTA_WEB_OK;
        }
        case KORTA_SET_NIGHT: s->night = on; push(s, KORTA_ACT_PREFS, 0); return KORTA_WEB_OK;
        case KORTA_SET_FLOW: s->flow = on; push(s, KORTA_ACT_PREFS, 0); return KORTA_WEB_OK;
        case KORTA_SET_HOUR12: s->hour12 = on; push(s, KORTA_ACT_PREFS, 0); return KORTA_WEB_OK;
        case KORTA_SET_SILENT: h->silent = on; push(s, KORTA_ACT_SILENT, on); return KORTA_WEB_OK;
        case KORTA_SET_BLEND: h->blend = on; push(s, KORTA_ACT_BLEND, on); return KORTA_WEB_OK;
        case KORTA_SET_CC_LCD: h->cc_lcd = on; push(s, KORTA_ACT_CC_LCD, on); return KORTA_WEB_OK;
        case KORTA_SET_CC_USB: h->cc_usb = on; push(s, KORTA_ACT_CC_USB, on); return KORTA_WEB_OK;
        case KORTA_SET_TRANSIT: h->transit = on; push(s, KORTA_ACT_TRANSIT, on); return KORTA_WEB_OK;
        case KORTA_SET_DPAD: h->dpad = on ? 1 : 0; push(s, KORTA_ACT_DPAD, h->dpad); return KORTA_WEB_OK;
        case KORTA_SET_LOWBATT:
            if (value < 0 || value > 2) return KORTA_WEB_INVALID;
            h->low_batt = (uint8_t)value;
            push(s, KORTA_ACT_LOWBATT, h->low_batt);
            return KORTA_WEB_OK;
        case KORTA_SET_PLAYER:
            if (value < 1 || value > 8) return KORTA_WEB_INVALID;
            h->player = (uint8_t)value;
            push(s, KORTA_ACT_PLAYER, h->player);
            return KORTA_WEB_OK;
        case KORTA_SET_PALETTE:
            if (value < 0 || value > 14 || h->gbc_game) return KORTA_WEB_INVALID;
            h->palette = (uint8_t)value;
            push(s, KORTA_ACT_PALETTE, h->palette);
            return KORTA_WEB_OK;
        default:
            return KORTA_WEB_INVALID;
    }
}
int korta_web_message(KortaState *s, const char *text, bool find) {
    if (!s) return KORTA_WEB_INVALID;
    /* Alarms and timers are not covered up; the sender is told to try later. */
    if (s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING) return KORTA_WEB_BUSY;
    char clean[KORTA_NOTE_CAPACITY + 1];
    korta_clean_text(clean, sizeof clean, text);
    if (!clean[0] && !find) return KORTA_WEB_INVALID;
    if (s->view == KORTA_MESSAGE) return_to(s, s->ring_from);  /* the new one replaces it */
    memcpy(s->msg, clean, sizeof s->msg);
    s->msg_find = find;
    s->msg_until = s->now_ms + (find ? 120000u : 600000u);
    s->ring_from = s->view;
    set_view(s, KORTA_MESSAGE);
    push(s, KORTA_ACT_ALERT, 0);
    s->sfx = KORTA_SFX_ALERT;
    return KORTA_WEB_OK;
}
int korta_web_timer(KortaState *s, int action, bool countdown, int seconds) {
    if (!s) return KORTA_WEB_INVALID;
    if (action == KORTA_TIMER_SET) {
        if (s->timer_run) return KORTA_WEB_BUSY;
        if (countdown && (seconds < 5 || seconds > 3 * 3600)) return KORTA_WEB_INVALID;
        s->timer_count = countdown;
        if (countdown) s->timer_set = (uint16_t)seconds;
        timer_reset(s);
        return KORTA_WEB_OK;
    }
    if (action == KORTA_TIMER_START) { if (!s->timer_run) timer_toggle(s); return KORTA_WEB_OK; }
    if (action == KORTA_TIMER_STOP) { if (s->timer_run) timer_toggle(s); return KORTA_WEB_OK; }
    if (action == KORTA_TIMER_RESET) { timer_reset(s); return KORTA_WEB_OK; }
    return KORTA_WEB_INVALID;
}
int korta_web_wifi_check(const char *ssid, const char *password) {
    if (!ssid) return KORTA_WEB_INVALID;
    size_t n = strlen(ssid), p = password ? strlen(password) : 0;
    return (!n || n > 32 || (p && (p < 8 || p > 63))) ? KORTA_WEB_INVALID : KORTA_WEB_OK;
}
int korta_web_wifi(KortaState *s, const char *ssid, const char *password) {
    if (!s || korta_web_wifi_check(ssid, password) != KORTA_WEB_OK) return KORTA_WEB_INVALID;
    size_t p = password ? strlen(password) : 0;
    snprintf(s->join_ssid, sizeof s->join_ssid, "%s", ssid);
    snprintf(s->password, sizeof s->password, "%s", password ? password : "");
    s->pass_len = (uint8_t)p;
    s->wifi_keep = true;
    push(s, KORTA_ACT_WIFI_JOIN, 0);
    push(s, KORTA_ACT_PREFS, 0);
    return KORTA_WEB_OK;
}
bool korta_take_action(KortaState *s, KortaAction *out) {
    if (!s || !out || s->qh == s->qt) return false;
    *out = s->queue[s->qh];
    s->qh = (uint8_t)((s->qh + 1) & 7);
    return true;
}
void korta_service(KortaState *s, uint32_t time_ms) {
    if (!s) return;
    if (s->now_ms && time_ms < s->now_ms && (s->now_ms - time_ms) < 120000u) time_ms = s->now_ms;
    s->now_ms = time_ms;
    if (s->vol_mode && (int32_t)(time_ms - s->vol_until) >= 0) {
        s->vol_mode = 0;
        s->vol_sel = 0;
    }
    if (s->view == KORTA_MESSAGE && (int32_t)(time_ms - s->msg_until) >= 0) return_to(s, s->ring_from);
    service_alarm(s);
    service_timer(s);
}
void korta_sync_volume(KortaState *s, uint8_t percent) {
    if (!s) return;
    s->vol_media = clamp100(percent);
    s->hw.volume = s->vol_media;
}
void korta_note_volume(KortaState *s, uint8_t percent, uint32_t time_ms) {
    if (!s) return;
    uint8_t next = clamp100(percent);
    if (next == s->vol_media) return;
    s->vol_media = next;
    s->hw.volume = next;
    if (s->vol_mode != 2) s->vol_mode = 1;
    s->vol_until = time_ms + (s->vol_mode == 2 ? 8000u : 3000u);
    s->now_ms = time_ms;
    s->sfx = KORTA_SFX_TICK;
}
uint8_t korta_take_sfx(KortaState *s) {
    if (!s) return KORTA_SFX_NONE;
    uint8_t kind = s->sfx;
    s->sfx = KORTA_SFX_NONE;
    return kind;
}
static void infer_sfx(KortaState *s, KortaButton b, KortaView v0, uint8_t f0, uint8_t sel0,
    uint8_t kr0, uint8_t kc0, uint8_t sil0, uint8_t night0, uint8_t vm0, uint8_t rev0, bool held) {
    if (s->sfx == KORTA_SFX_ALERT || s->sfx == KORTA_SFX_DICE || s->sfx == KORTA_SFX_COIN) return;
    if (s->view == KORTA_ARCADE && v0 == KORTA_ARCADE && !s->arcade_paused) return;  /* the game's own input */
    if (is_alert(s->view)) {
        if (s->view != v0) s->sfx = KORTA_SFX_ALERT;
        return;
    }
    if (s->hw.silent != sil0 || s->night != night0) { s->sfx = KORTA_SFX_TOGGLE; return; }
    if (s->view != v0) {
        s->sfx = (b == KORTA_B) ? KORTA_SFX_BACK : KORTA_SFX_OPEN;
        return;
    }
    if (s->vol_mode != vm0) { s->sfx = KORTA_SFX_OPEN; return; }
    if (s->focus != f0 || s->selected != sel0 || s->key_row != kr0 || s->key_col != kc0) {
        s->sfx = KORTA_SFX_MOVE;
        return;
    }
    if (s->view == KORTA_KEYBOARD && (s->note_rev != rev0 || b == KORTA_A)) {
        s->sfx = KORTA_SFX_TYPE;
        return;
    }
    if (held) return;  /* a repeat that changed nothing stays quiet */
    if (b == KORTA_A || b == KORTA_B) {
        int app = s->view == KORTA_HOME ? home_app(s) : -1;
        bool play = (app >= 0 && !is_game(app) && APP_VIEW[app] == KORTA_PLAY) || s->view == KORTA_PLAY;
        if (play) s->sfx = s->hw.cart_in ? KORTA_SFX_OPEN : KORTA_SFX_DENY;
        else if (b == KORTA_A) s->sfx = KORTA_SFX_OK;
        else s->sfx = KORTA_SFX_BACK;
        return;
    }
    if (b == KORTA_LEFT || b == KORTA_RIGHT) s->sfx = KORTA_SFX_TOGGLE;
}
bool korta_frame(KortaState *s, uint16_t held, uint32_t time_ms) {
    if (!s) return false;
    bool leave = false;
    korta_service(s, time_ms);
    if (s->update_phase) { s->primed = false; return false; }  /* inputs wait until the update ends */
    if (!s->primed) {
        s->primed = true;
        s->held = held;
        s->now_ms = time_ms;
        s->blocked_held = held;
        memset(s->was_down, 0, sizeof s->was_down);
        memset(s->long_fired, 0, sizeof s->long_fired);
        return false;
    }
    s->blocked_held &= held;
    held &= (uint16_t)~s->blocked_held;
    s->held = held;
    for (int i = 0; i < 8; i++) {
        bool down = (held & (1u << i)) != 0;
        KortaButton b = (KortaButton)i;
        bool defer = b == KORTA_START || (b == KORTA_SELECT && s->view != KORTA_KEYBOARD);
        KortaView v0 = s->view;
        uint8_t f0 = s->focus, sel0 = s->selected, kr0 = s->key_row, kc0 = s->key_col;
        uint8_t sil0 = s->hw.silent, night0 = s->night, vm0 = s->vol_mode, rev0 = s->note_rev;
        if (down && !s->was_down[i]) {
            if (b == KORTA_B) s->b_from_home = (s->view == KORTA_HOME);
            s->edge_ms[i] = time_ms;
            s->long_fired[i] = false;
            s->next_repeat[i] = time_ms + HOLD_MS;
            s->repeat_gap[i] = REPEAT_MS;
            if (!defer) on_tap(s, b, &leave);
            if (!defer) infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, rev0, false);
        } else if (down && s->was_down[i]) {
            if (!s->long_fired[i] && time_ms - s->edge_ms[i] >= HOLD_MS) {
                s->long_fired[i] = true;
                if (b == KORTA_START) on_start_hold(s);
                else if (b == KORTA_SELECT && s->view != KORTA_KEYBOARD) on_select_hold(s);
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, rev0, true);
            }
            if (!defer && (int32_t)(time_ms - s->next_repeat[i]) >= 0) {
                on_repeat(s, b, &leave);
                uint16_t gap = (uint16_t)(s->repeat_gap[i] * 3 / 4);
                if (gap < REPEAT_MIN_MS) gap = REPEAT_MIN_MS;
                s->repeat_gap[i] = gap;
                s->next_repeat[i] = time_ms + gap;
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, rev0, true);
            }
        } else if (!down && s->was_down[i]) {
            if (defer && !s->long_fired[i]) {
                if (b == KORTA_START) on_start_short(s);
                else on_select_short(s);
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, rev0, false);
            }
            s->held_home = false;
        }
        /* A press that opened another screen must not keep acting there. B is
         * the exception outside the keyboard: holding it on means "go Home". */
        bool hold_home = b == KORTA_B && v0 != KORTA_KEYBOARD && !is_alert(v0)
            && s->view != KORTA_KEYBOARD;
        if (down && !defer && s->view != v0 && !hold_home) s->blocked_held |= (uint16_t)(1u << i);
        s->was_down[i] = down;
    }
    return leave;
}
bool korta_press(KortaState *s, KortaButton button) {
    if (!s || (unsigned)button > KORTA_SELECT) return false;
    uint32_t t = s->now_ms + 400;
    korta_frame(s, 0, t);
    bool leave = korta_frame(s, (uint16_t)(1u << button), t + 30);
    leave = korta_frame(s, 0, t + 60) || leave;
    return leave;
}
/* The wheel shows Media as a slim bar on the right edge. Opening the panel grows
 * that edge into three vertical sliders and leaves the app visible beside it. */
enum { VOL_X = 97, VOL_COL = 21, VOL_TOP = 28, VOL_H = 92 };
static void vol_slider(Painter *d, int x, int top, int h, int w, uint8_t level, uint16_t c) {
    fill(d, x, top, w, h, d->p.line);
    int fh = h * (int)level / 100;
    if (fh < 1 && level) fh = 1;
    fill(d, x, top + h - fh, w, fh, c);
}
static void paint_vol(Painter *d, const KortaState *s) {
    if (!s->vol_mode) return;
    if (s->vol_mode == 1) {
        fill(d, 146, 14, 14, 128, d->p.bg);
        fill(d, 146, 14, 1, 128, d->p.line);
        vol_slider(d, 150, 18, 120, 7, s->vol_media, d->p.accent);
        return;
    }
    fill(d, VOL_X, 12, 160 - VOL_X, 132, d->p.bg);
    fill(d, VOL_X, 12, 1, 132, d->p.ink);
    /* Display order System, Ring, Media; vol_sel uses 0 System, 1 Media, 2 Ring. */
    static const uint8_t which[3] = {0, 2, 1};
    static const char *const name[3] = {"SYS", "RNG", "MED"};
    for (int c = 0; c < 3; c++) {
        int x = VOL_X + 1 + c * VOL_COL;
        uint8_t level = which[c] == 0 ? s->vol_sys : which[c] == 2 ? s->vol_ring : s->vol_media;
        bool hot = s->vol_sel == which[c];
        bool media = which[c] == 1;
        if (hot) fill(d, x, 13, VOL_COL - 1, 130, d->p.card);
        char n[8];
        snprintf(n, sizeof n, "%u", level);
        int cx = x + (VOL_COL - 1) / 2;
        centered_label(d, n, cx + 1, 17, hot ? d->p.accent : d->p.muted);
        vol_slider(d, cx - 3, VOL_TOP, VOL_H, 7, level, hot ? d->p.accent : media ? d->p.muted : d->p.ink);
        centered_label(d, name[c], cx + 1, 126, hot ? d->p.accent : media ? d->p.muted : d->p.ink);
    }
    if (s->hw.silent) {
        fill(d, VOL_X + 1, 135, 160 - VOL_X - 1, 9, d->p.accent);
        centered(d, "SILENT", VOL_X + (160 - VOL_X) / 2, 136, d->p.bg);
    }
}
void korta_render(const KortaState *s, uint16_t *pixels) {
    if (!s || !pixels) return;
    Painter d = {pixels, palette(s->night)};
    if (s->view == KORTA_ALARM_RING) { paint_ring(&d, s, "ALARM"); paint_vol(&d, s); return; }
    if (s->view == KORTA_TIMER_RING) { paint_ring(&d, s, "TIMER"); paint_vol(&d, s); return; }
    if (s->view == KORTA_MESSAGE) { paint_message(&d, s); paint_vol(&d, s); return; }
    if (s->view == KORTA_FLASH) { paint_flash(&d, s); paint_vol(&d, s); return; }
    if (s->update_phase) { paint_update(&d, s); return; }
    if (s->view == KORTA_ARCADE) { paint_arcade(&d, s); paint_vol(&d, s); return; }
    fill(&d, 0, 0, 160, 144, d.p.bg);
    char note_title[24];
    const char *title = s->view == KORTA_HOME ? NULL : view_title(s);
    if (s->view == KORTA_NOTE_OPEN) {
        snprintf(note_title, sizeof note_title, "%s NOTE %u/%u %s", s->note_cur > 0 ? "<" : " ",
            s->note_cur + 1u, s->note_count, s->note_cur + 1u < s->note_count ? ">" : " ");
        title = note_title;
    }
    status_bar(&d, s, title);
    switch (s->view) {
        case KORTA_HOME: paint_home(&d, s); break;
        case KORTA_NOTES: paint_notes(&d, s); break;
        case KORTA_NOTE_OPEN: paint_note_open(&d, s); break;
        case KORTA_KEYBOARD: paint_keys(&d, s); break;
        case KORTA_TIME: paint_time(&d, s); break;
        case KORTA_CLOCK: paint_clock(&d, s); break;
        case KORTA_ALARM: paint_alarm(&d, s); break;
        case KORTA_TIMER: paint_timer(&d, s); break;
        case KORTA_CALC: paint_calc(&d, s); break;
        case KORTA_FIND: paint_find(&d, s); break;
        case KORTA_FILES: paint_files(&d, s); break;
        case KORTA_SETTINGS: paint_settings(&d, s); break;
        case KORTA_SETUP: paint_setup(&d, s); break;
        case KORTA_POWER: paint_power(&d, s); break;
        case KORTA_PLAY: paint_play(&d, s); break;
        case KORTA_LINKS: paint_links(&d, s); break;
        case KORTA_INFO: paint_info(&d, s); break;
        case KORTA_QUICK: paint_quick(&d, s); break;
        case KORTA_CLEAR_CONFIRM: paint_clear(&d, s); break;
        case KORTA_DICE: paint_dice(&d, s); break;
        case KORTA_COIN: paint_coin(&d, s); break;
        case KORTA_COUNT: paint_count(&d, s); break;
        case KORTA_CONV: paint_conv(&d, s); break;
        case KORTA_COLOR: paint_color(&d, s); break;
        case KORTA_SOUND: paint_sound(&d, s); break;
        case KORTA_CONTROLS: paint_controls(&d, s); break;
        case KORTA_PHONE: paint_phone(&d, s); break;
        default: break;
    }
    paint_vol(&d, s);
}
