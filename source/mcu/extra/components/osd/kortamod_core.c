#include "kortamod_core.h"
#include "kortamod_assets.h"
#include "kortamod_icons.h"

#include <stdio.h>
#include <string.h>

enum { HOLD_MS = 220, REPEAT_MS = 110, REPEAT_MIN_MS = 36, DOUBLE_MS = 320 };

static const char *const APP_NAME[KORTA_APP_COUNT] = {
    "GAME", "NOTES", "CLOCK", "CALC", "FILES", "SETTINGS",
    "DICE", "COIN", "COUNT", "FLASH", "CONV", "COLOR", "PLAYER"
};
static const KortaView APP_VIEW[KORTA_APP_COUNT] = {
    KORTA_PLAY, KORTA_NOTES, KORTA_TIME, KORTA_CALC, KORTA_FILES, KORTA_SETTINGS,
    KORTA_DICE, KORTA_COIN, KORTA_COUNT, KORTA_FLASH, KORTA_CONV, KORTA_COLOR, KORTA_PLAYER
};
static const uint8_t APP_ICON[KORTA_APP_COUNT] = {
    9, 0, 1, 4, 6, 7, 12, 13, 14, 15, 16, 17, 19
};
static const int8_t HOME_APP[2][KORTA_PAGE_SIZE] = {
    {0, 1, 2, 3, 4, 5, -1, -1, -1},
    {6, 7, 8, 9, 10, 11, 12, -1, -1}
};
static int home_at(int page, int slot) {
    if (page < 0 || page > 1 || slot < 0 || slot >= KORTA_PAGE_SIZE) return -1;
    return HOME_APP[page][slot];
}
static int home_app(const KortaState *s) {
    return home_at(s->selected / KORTA_PAGE_SIZE, s->selected % KORTA_PAGE_SIZE);
}
typedef struct { const char *name; KortaView view; } FindItem;
static const FindItem FIND_ITEM[] = {
    {"NOTES", KORTA_NOTES}, {"TIME", KORTA_TIME}, {"CLOCK", KORTA_CLOCK},
    {"ALARM", KORTA_ALARM}, {"TIMER", KORTA_TIMER}, {"CALC", KORTA_CALC},
    {"FILES", KORTA_FILES}, {"WIFI", KORTA_LINKS}, {"GAME", KORTA_PLAY},
    {"SETTINGS", KORTA_SETTINGS},
    {"DISPLAY", KORTA_SETUP}, {"POWER", KORTA_POWER}, {"ABOUT", KORTA_INFO},
    {"CONTROLS", KORTA_CONTROLS},
    {"SOUND", KORTA_SOUND}, {"COLOR", KORTA_COLOR},
    {"PLAYER", KORTA_PLAYER}, {"DICE", KORTA_DICE}, {"COIN", KORTA_COIN},
    {"COUNT", KORTA_COUNT}, {"FLASH", KORTA_FLASH}, {"CONV", KORTA_CONV}
};
enum { FIND_COUNT = (int)(sizeof FIND_ITEM / sizeof FIND_ITEM[0]) };
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
static const char CALC_KEYS[] = "789/456*123-C0=+";

typedef struct { uint16_t bg, ink, muted, card, band, band_ink, accent, line, hi; } Palette;
typedef struct { uint16_t *pixels; Palette p; } Painter;

static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static Palette palette(bool night) {
    if (night) {
        return (Palette){
            rgb(18, 32, 34), rgb(236, 228, 204), rgb(150, 168, 156), rgb(42, 68, 66),
            rgb(10, 22, 24), rgb(236, 228, 204), rgb(232, 146, 92), rgb(70, 96, 92), rgb(255, 214, 160)
        };
    }
    return (Palette){
        rgb(236, 228, 204), rgb(28, 40, 38), rgb(96, 108, 100), rgb(255, 250, 236),
        rgb(28, 40, 38), rgb(244, 236, 210), rgb(168, 72, 48), rgb(196, 186, 160), rgb(255, 252, 244)
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
static void set_view(KortaState *s, KortaView view) {
    if (s->view == KORTA_CLOCK && view != KORTA_CLOCK) s->clock_dirty = false;
    if (s->view == KORTA_FLASH && view != KORTA_FLASH) {
        uint8_t brightness = s->flash_saved ? s->flash_saved : 7;
        s->hw.brightness = brightness;
        push(s, KORTA_ACT_BRIGHTNESS, brightness);
    }
    if (s->view != KORTA_FLASH && view == KORTA_FLASH) {
        s->flash_saved = s->hw.brightness;
        s->hw.brightness = 16;
        push(s, KORTA_ACT_BRIGHTNESS, 16);
    }
    bool keep_query = view == KORTA_FIND && (s->find_restore || (s->view == KORTA_KEYBOARD && s->typing_find));
    if (s->view == KORTA_KEYBOARD && view != KORTA_KEYBOARD
        && view != KORTA_ALARM_RING && view != KORTA_TIMER_RING) {
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
    s->view = view;
    s->focus = 0;
    s->key_row = 0;
    s->key_col = 0;
}
static void append_note(KortaState *s, char c) {
    if (s->note_len < KORTA_NOTE_CAPACITY) {
        s->note[s->note_len++] = c;
        s->note[s->note_len] = '\0';
    }
}
static void erase_note(KortaState *s) {
    if (s->note_len) s->note[--s->note_len] = '\0';
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

static int exact_count(int row) { return row == 0 ? 10 : row == 1 ? 9 : row == 2 ? 9 : 3; }
static int key_x(const KortaState *s, int row, int col) {
    if (use_flow(s)) return 4 + col * 52;
    if (row == 0) return col * 16;
    if (row == 1) return 8 + col * 16;
    if (row == 2) return col == 0 ? 1 : col == 8 ? 138 : 25 + (col - 1) * 16;
    return col == 0 ? 2 : col == 1 ? 88 : 110;
}
static int key_w(const KortaState *s, int row, int col) {
    if (use_flow(s)) return 48;
    if (row < 2) return 15;
    if (row == 2) return col == 0 ? 22 : col == 8 ? 21 : 15;
    return col == 0 ? 84 : col == 1 ? 20 : 48;
}
static const char *key_label(const KortaState *s, int row, int col, char out[8]) {
    static const char *const flow_keys[4][3] = {
        {"ABC", "DEF", "GHI"}, {"JKL", "SPACE", "MNO"}, {"PQRS", "TUV", "WXYZ"}, {"NEXT", "EXACT", "DONE"}
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
    set_view(s, KORTA_NOTES);
}
static void finish_find(KortaState *s) {
    set_view(s, KORTA_FIND);
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
            if (s->pending_len < (int)sizeof s->pending - 1 && s->note_len + s->pending_len < KORTA_NOTE_CAPACITY) {
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
        if (strcmp(label, "EXACT") == 0) {
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
        int max = use_flow(s) ? 3 : exact_count(row);
        col += b == KORTA_LEFT ? -1 : 1;
        if (col < 0) col = max - 1;
        if (col >= max) col = 0;
    } else {
        int cx = key_x(s, row, col) + key_w(s, row, col) / 2;
        row += b == KORTA_UP ? -1 : 1;
        if (row < 0) row = 3;
        if (row > 3) row = 0;
        int best = 10000;
        int max = use_flow(s) ? 3 : exact_count(row);
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
    for (int i = 0; i < FIND_COUNT && n < 24; i++) {
        bool alias = FIND_ITEM[i].view == KORTA_PLAY && s->query_len
            && (strstr("CART", s->query) || strstr("PLAY", s->query));
        if (!s->query_len || strstr(FIND_ITEM[i].name, s->query) || alias) ids[n++] = (uint8_t)i;
    }
    return n;
}
static KortaView parent_of(const KortaState *s) {
    switch (s->view) {
        case KORTA_CLOCK:
        case KORTA_ALARM:
        case KORTA_TIMER:
        case KORTA_TIMER_RING:
            return KORTA_TIME;
        case KORTA_KEYBOARD:
            if (s->typing_pass) return KORTA_LINKS;
            if (s->typing_find) return KORTA_FIND;
            return KORTA_NOTES;
        case KORTA_CLEAR_CONFIRM:
            return KORTA_NOTES;
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
        case KORTA_COLOR:
        case KORTA_PLAYER:
        case KORTA_CONTROLS:
            return s->via_settings ? KORTA_SETTINGS : KORTA_HOME;
        default:
            return KORTA_HOME;
    }
}
static void open_find(KortaState *s, KortaView from) {
    if (s->view == KORTA_FIND) return;
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
    if (s->view == KORTA_NOTES && s->notes_from_files) {
        s->notes_from_files = false;
        set_view(s, KORTA_FILES);
        return;
    }
    set_view(s, parent_of(s));
}
static void launch_game(KortaState *s) {
    s->find_dest_active = false;
    s->notes_from_files = false;
    s->via_settings = false;
    set_view(s, KORTA_HOME);
    push(s, KORTA_ACT_LAUNCH, 0);
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
        case KORTA_NOTES: return 2;
        case KORTA_CLEAR_CONFIRM: return 2;
        case KORTA_TIME: return 3;
        case KORTA_SETTINGS: return 8;
        case KORTA_CONTROLS: return 3;
        case KORTA_CONV: return 2;
        case KORTA_COLOR: return 1;
        case KORTA_SOUND: return 3;
        case KORTA_PLAYER: return 1;
        case KORTA_FIND: {
            uint8_t ids[24];
            return 1 + matches(s, ids);
        }
        case KORTA_CLOCK: return 7;
        case KORTA_ALARM: return 3;
        case KORTA_TIMER: return 3;
        case KORTA_FILES: return 1;
        case KORTA_SETUP: return 7;
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
        if (col < 2 && home_at(page, row * 3 + col + 1) >= 0) col++;
        else if (home_at(page + 1, row * 3) >= 0) { page++; col = 0; }
        else return;
    } else if (b == KORTA_LEFT) {
        if (col > 0 && home_at(page, row * 3 + col - 1) >= 0) col--;
        else if (page > 0 && home_at(page - 1, row * 3 + 2) >= 0) { page--; col = 2; }
        else return;
    } else if (b == KORTA_DOWN) {
        if (row < 2 && home_at(page, (row + 1) * 3 + col) >= 0) row++;
        else return;
    } else if (b == KORTA_UP) {
        if (row > 0 && home_at(page, (row - 1) * 3 + col) >= 0) row--;
        else return;
    }
    s->selected = (uint8_t)(page * KORTA_PAGE_SIZE + row * 3 + col);
}
static void double_home(KortaState *s, KortaButton b) {
    if (b == KORTA_UP) { s->selected = 0; return; }
    if (b == KORTA_DOWN) { s->selected = KORTA_APP_COUNT - 1; return; }
    int pages = (KORTA_APP_COUNT + KORTA_PAGE_SIZE - 1) / KORTA_PAGE_SIZE;
    int page = s->selected / KORTA_PAGE_SIZE;
    int slot = s->selected % KORTA_PAGE_SIZE;
    int next = page + (b == KORTA_RIGHT ? 1 : -1);
    if (next < 0) next = 0;
    if (next >= pages) next = pages - 1;
    int index = next * KORTA_PAGE_SIZE + slot;
    if (index >= KORTA_APP_COUNT) index = KORTA_APP_COUNT - 1;
    s->selected = (uint8_t)index;
}
static void nudge_bright(KortaState *s, int dir) {
    int v = (int)s->hw.brightness + dir;
    if (v < 1) v = 1;
    if (v > 16) v = 16;
    s->hw.brightness = (uint8_t)v;
    push(s, KORTA_ACT_BRIGHTNESS, (uint16_t)v);
}
static void nudge_clock(KortaState *s, int dir) {
    if (!s->time_known) s->time_known = true;
    if (s->year < 2024) s->year = 2026;
    if (s->month < 1 || s->month > 12) s->month = 10;
    if (s->day < 1) s->day = 15;
    if (s->focus == 5) { s->hour12 = !s->hour12; push(s, KORTA_ACT_PREFS, 0); return; }
    if (s->focus == 6) {
        int z = s->zone + dir;
        if (z < -12) z = -12;
        if (z > 14) z = 14;
        s->zone = (int8_t)z;
        push(s, KORTA_ACT_ZONE, (uint16_t)(z + 12));
        push(s, KORTA_ACT_PREFS, 0);
        return;
    }
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
static void nudge_alarm(KortaState *s, int dir) {
    if (s->focus == 0) { s->alarm_on = !s->alarm_on; push(s, KORTA_ACT_PREFS, 0); return; }
    if (s->focus == 1) {
        int h = (int)s->alarm_h + dir;
        if (h < 0) h = 23;
        if (h > 23) h = 0;
        s->alarm_h = (uint8_t)h;
    } else {
        int m = (int)s->alarm_m + dir;
        if (m < 0) m = 59;
        if (m > 59) m = 0;
        s->alarm_m = (uint8_t)m;
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
            int p = (int)h->palette + dir;
            if (p < 0) p = 14;
            if (p > 14) p = 0;
            h->palette = (uint8_t)p;
            push(s, KORTA_ACT_PALETTE, h->palette);
            return;
        }
    }
}
enum { SET_HUB = 8 };
static const char *const SET_HUB_NAME[SET_HUB] = {
    "SEARCH", "WIFI", "DISPLAY", "SOUND", "CONTROLS", "POWER", "CLOCK", "ABOUT"
};
static const KortaView SET_HUB_VIEW[SET_HUB] = {
    KORTA_FIND, KORTA_LINKS, KORTA_SETUP, KORTA_SOUND, KORTA_CONTROLS, KORTA_POWER, KORTA_TIME, KORTA_INFO
};
static const uint8_t DISPLAY_MAP[7] = {0, 1, 4, 5, 6, 7, 11};
static const uint8_t CONTROL_MAP[3] = {8, 2, 10};
static const uint8_t POWER_LOW[1] = {9};
static void nudge_mapped(KortaState *s, const uint8_t *map, int count, int dir) {
    if (s->focus >= count) return;
    uint8_t keep = s->focus;
    s->focus = map[keep];
    nudge_setup(s, dir);
    s->focus = keep;
}
static uint8_t clamp100(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}
static bool calc_apply(KortaState *s) {
    int64_t result = s->calc_entry;
    int32_t a = s->calc_acc;
    int32_t b = s->calc_entry;
    if (s->calc_op == 1) result = (int64_t)a + b;
    else if (s->calc_op == 2) result = (int64_t)a - b;
    else if (s->calc_op == 3) result = (int64_t)a * b;
    else if (s->calc_op == 4) {
        if (b == 0) return false;
        result = (int64_t)a / b;
    }
    if (result > 2147483647LL || result < -2147483648LL) return false;
    s->calc_acc = (int32_t)result;
    return true;
}
static void calc_digit(KortaState *s, int digit) {
    if (s->calc_err) return;
    if (s->calc_fresh) { s->calc_entry = 0; s->calc_fresh = false; }
    int64_t next = (int64_t)s->calc_entry * 10 + digit;
    if (next > 99999999) return;
    s->calc_entry = (int32_t)next;
}
static void calc_operator(KortaState *s, int op) {
    if (s->calc_err) return;
    if (!s->calc_fresh && s->calc_op) {
        if (!calc_apply(s)) { s->calc_err = true; return; }
        s->calc_entry = s->calc_acc;
    } else {
        s->calc_acc = s->calc_entry;
    }
    s->calc_op = (uint8_t)op;
    s->calc_fresh = true;
}
static void calc_equals(KortaState *s) {
    if (s->calc_err) return;
    if (!calc_apply(s)) { s->calc_err = true; return; }
    s->calc_entry = s->calc_acc;
    s->calc_op = 0;
    s->calc_fresh = true;
}
static void calc_clear(KortaState *s) {
    s->calc_acc = 0;
    s->calc_entry = 0;
    s->calc_op = 0;
    s->calc_fresh = true;
    s->calc_err = false;
}
static void calc_key(KortaState *s) {
    char key = CALC_KEYS[s->focus];
    if (key >= '0' && key <= '9') calc_digit(s, key - '0');
    else if (key == '+') calc_operator(s, 1);
    else if (key == '-') calc_operator(s, 2);
    else if (key == '*') calc_operator(s, 3);
    else if (key == '/') calc_operator(s, 4);
    else if (key == '=') calc_equals(s);
    else calc_clear(s);
}
static void move_calc(KortaState *s, KortaButton b) {
    int col = s->focus % 4;
    int row = s->focus / 4;
    if (b == KORTA_LEFT && col > 0) s->focus--;
    if (b == KORTA_RIGHT && col < 3) s->focus++;
    if (b == KORTA_UP && row > 0) s->focus = (uint8_t)(s->focus - 4);
    if (b == KORTA_DOWN && row < 3) s->focus = (uint8_t)(s->focus + 4);
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
        int v = (int)s->timer_set + dir * 15;
        if (v < 15) v = 15;
        if (v > 3600) v = 3600;
        s->timer_set = (uint16_t)v;
        timer_reset(s);
    }
}
static void open_match(KortaState *s) {
    uint8_t ids[24];
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
static void on_tap(KortaState *s, KortaButton b, bool *leave) {
    (void)leave;
    if (s->vol_mode == 1 && b == KORTA_A) {
        s->vol_mode = 2;
        s->vol_sel = 0;
        s->vol_until = s->now_ms + 8000u;
        return;
    }
    if (s->vol_mode == 2 && (b == KORTA_UP || b == KORTA_DOWN || b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A || b == KORTA_B)) {
        s->vol_until = s->now_ms + 8000u;
        if (b == KORTA_B) { s->vol_mode = 0; return; }
        if (b == KORTA_UP && s->vol_sel) s->vol_sel = 0;
        else if (b == KORTA_DOWN && s->vol_sel < 2) s->vol_sel = 2;
        else if (b == KORTA_RIGHT || b == KORTA_A) nudge_vol(s, s->vol_sel, 1);
        else if (b == KORTA_LEFT) nudge_vol(s, s->vol_sel, -1);
        return;
    }
    if (s->view == KORTA_ALARM_RING) {
        if (b == KORTA_A || b == KORTA_B) { s->alarm_latched = true; set_view(s, s->ring_from); }
        return;
    }
    if (s->view == KORTA_TIMER_RING) {
        if (b == KORTA_A || b == KORTA_B) set_view(s, s->ring_from);
        return;
    }
    if (s->view == KORTA_KEYBOARD) {
        if (b == KORTA_B) {
            if (use_flow(s) && s->pending_len) {
                s->pending[--s->pending_len] = '\0';
                s->candidate = 0;
            } else if (s->typing_pass) {
                if (!s->pass_len) set_view(s, KORTA_LINKS);
                else erase_char(s);
            } else if (s->typing_find) {
                if (!s->query_len) set_view(s, KORTA_FIND);
                else erase_char(s);
            } else if (!s->note_len) set_view(s, KORTA_NOTES);
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
            if (APP_VIEW[app] == KORTA_PLAY) {
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
    if (b == KORTA_UP || b == KORTA_DOWN) { move_list(s, b); return; }
    if (b != KORTA_A && b != KORTA_LEFT && b != KORTA_RIGHT) return;
    int dir = (b == KORTA_LEFT) ? -1 : 1;
    if (s->view == KORTA_CLOCK && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) { nudge_clock(s, b == KORTA_LEFT ? -1 : 1); return; }
    if (s->view == KORTA_ALARM && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) { nudge_alarm(s, b == KORTA_LEFT ? -1 : 1); return; }
    if (s->view == KORTA_SETUP && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        nudge_mapped(s, DISPLAY_MAP, 7, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_CONTROLS && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        nudge_mapped(s, CONTROL_MAP, 3, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_POWER && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        if (s->focus == 1) nudge_mapped(s, POWER_LOW, 1, b == KORTA_LEFT ? -1 : 1);
        else nudge_bright(s, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_TIMER && (b == KORTA_LEFT || b == KORTA_RIGHT)) { nudge_timer(s, dir); return; }
    if (s->view == KORTA_COLOR && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        s->focus = 11;
        nudge_setup(s, b == KORTA_LEFT ? -1 : 1);
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
    if (s->view == KORTA_PLAYER && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        s->focus = 10;
        nudge_setup(s, b == KORTA_LEFT ? -1 : 1);
        return;
    }
    if (s->view == KORTA_CONV && (b == KORTA_LEFT || b == KORTA_RIGHT) && s->focus == 1) {
        s->conv_n = (int16_t)(s->conv_n + dir);
        return;
    }
    if (s->view == KORTA_COUNT && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_A)) {
        s->tally = (int16_t)(s->tally + (b == KORTA_LEFT ? -1 : 1));
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
        } else if (s->focus < SET_HUB) {
            s->via_settings = true;
            open_dest(s, SET_HUB_VIEW[s->focus]);
        }
    } else if (s->view == KORTA_NOTES) {
        if (s->focus == 0) {
            s->pending_len = 0;
            s->pending[0] = '\0';
            s->candidate = 0;
            set_view(s, KORTA_KEYBOARD);
        } else set_view(s, KORTA_CLEAR_CONFIRM);
    } else if (s->view == KORTA_CLEAR_CONFIRM) {
        if (s->focus == 1) {
            s->note_len = 0;
            s->note[0] = '\0';
            push(s, KORTA_ACT_PREFS, 0);
        }
        set_view(s, KORTA_NOTES);
    } else if (s->view == KORTA_FILES) {
        s->notes_from_files = true;
        set_view(s, KORTA_NOTES);
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
        s->roll_nonce++;
        s->dice = (uint8_t)((((s->now_ms >> 3) ^ (s->now_ms >> 9) ^ s->roll_nonce * 17u) % 6u) + 1u);
        s->dice_anim_until = s->now_ms + 640u;
    } else if (s->view == KORTA_COIN) {
        s->roll_nonce++;
        s->coin = (uint8_t)((((s->now_ms >> 4) ^ s->roll_nonce * 13u) & 1u) + 1u);
        s->coin_anim_until = s->now_ms + 640u;
    } else if (s->view == KORTA_CONV) {
        if (s->focus == 0) s->conv_kind = (uint8_t)((s->conv_kind + 1u) % 2u);
    } else if (s->view == KORTA_FLASH) {
        if (s->hw.brightness < 16) {
            s->flash_saved = s->hw.brightness;
            s->hw.brightness = 16;
        } else {
            s->hw.brightness = s->flash_saved ? s->flash_saved : 7;
        }
        push(s, KORTA_ACT_BRIGHTNESS, s->hw.brightness);
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
static void on_double(KortaState *s, KortaButton b) {
    if (s->view == KORTA_HOME) { double_home(s, b); return; }
    if (b == KORTA_UP) s->focus = 0;
    else if (b == KORTA_DOWN) {
        int n = list_count(s);
        s->focus = n > 0 ? (uint8_t)(n - 1) : 0;
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
            return s->focus != 0;
        case KORTA_TIMER:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus == 1 && (b == KORTA_LEFT || b == KORTA_RIGHT);
        case KORTA_SETUP:
        case KORTA_CONTROLS:
            return true;
        case KORTA_SETTINGS:
            return b == KORTA_UP || b == KORTA_DOWN;
        case KORTA_POWER:
            return true;
        case KORTA_QUICK:
            if (b == KORTA_UP || b == KORTA_DOWN) return true;
            return s->focus == 0 && (b == KORTA_LEFT || b == KORTA_RIGHT);
        case KORTA_COLOR:
        case KORTA_SOUND:
        case KORTA_PLAYER:
        case KORTA_COUNT:
        case KORTA_CONV:
            return true;
        case KORTA_FIND:
            return b == KORTA_UP || b == KORTA_DOWN;
        default:
            return b == KORTA_UP || b == KORTA_DOWN;
    }
}
static void on_repeat(KortaState *s, KortaButton b, bool *leave) {
    (void)leave;
    if (s->vol_mode == 2 && (b == KORTA_LEFT || b == KORTA_RIGHT || b == KORTA_UP || b == KORTA_DOWN)) {
        on_tap(s, b, leave);
        return;
    }
    if (b == KORTA_B && !text_mode(s)) {
        if (!s->held_home) {
            if (!s->b_from_home && s->view != KORTA_HOME) set_view(s, KORTA_HOME);
            s->held_home = true;
        }
        return;
    }
    if (!repeats(s, b)) return;
    on_tap(s, b, leave);
}
static void on_start_short(KortaState *s) {
    if (s->vol_mode == 2) { s->vol_mode = 0; return; }
    if (s->view == KORTA_KEYBOARD) {
        if (s->typing_pass) finish_pass(s);
        else if (s->typing_find) finish_find(s);
        else finish_note(s);
        return;
    }
    if (s->view == KORTA_QUICK) set_view(s, s->quick_from);
    else if (s->view != KORTA_ALARM_RING && s->view != KORTA_TIMER_RING) {
        s->quick_from = s->view;
        set_view(s, KORTA_QUICK);
    }
}
static void on_start_hold(KortaState *s) {
    if (s->vol_mode == 2) return;
    if (s->view == KORTA_KEYBOARD || s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING) return;
    open_find(s, s->view);
}
static void on_select_short(KortaState *s) {
    if (s->vol_mode == 2) return;
    if (s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING) return;
    open_find(s, s->view);
}
static void on_select_hold(KortaState *s) {
    if (s->vol_mode == 2) return;
    if (s->view == KORTA_KEYBOARD) return;
    s->night = !s->night;
    push(s, KORTA_ACT_PREFS, 0);
}
static void service_alarm(KortaState *s) {
    if (!s->time_known || !s->alarm_on || s->view == KORTA_ALARM_RING) {
        if (s->time_known && (s->hour != s->alarm_h || s->min != s->alarm_m)) s->alarm_latched = false;
        return;
    }
    /* Present one alert at a time; keep the timer's return destination intact. */
    if (s->view == KORTA_TIMER_RING) return;
    bool match = s->hour == s->alarm_h && s->min == s->alarm_m;
    if (!match) s->alarm_latched = false;
    else if (!s->alarm_latched) {
        s->alarm_latched = true;
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
        int dots_x = 76;
        for (int i = 0; i < 2; i++) {
            uint16_t c = i == page ? d->p.band_ink : d->p.muted;
            fill(d, dots_x + i * 7, 9, 4, 2, c);
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
        case 2: return s->flow ? "FLOW" : "EXACT";
        case 3: return h->silent ? "ON" : "OFF";
        case 4: return h->blend ? "ON" : "OFF";
        case 5: return h->cc_lcd ? "ON" : "OFF";
        case 6: return h->cc_usb ? "ON" : "OFF";
        case 7: return h->transit ? "ON" : "OFF";
        case 8: return h->dpad ? "CARD" : "DIAG";
        case 9: return h->low_batt == 0 ? "SHOW" : h->low_batt == 1 ? "FLASH" : "HIDE";
        case 10: snprintf(buf, n, "%u", h->player ? h->player : 1); return buf;
        default: {
            unsigned p = h->palette < 15 ? h->palette : 0;
            return PALETTE_NAME[p];
        }
    }
}
static const char *setup_label(int index) {
    static const char *const labels[12] = {
        "BRIGHT", "THEME", "KEYS", "SILENT", "BLEND", "LCD", "USB", "TRANSIT", "DPAD", "LOWBAT", "PLAYER", "COLOR"
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
        case KORTA_CLOCK: return "CLOCK";
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
        case KORTA_CLEAR_CONFIRM: return "CLEAR";
        case KORTA_DICE: return "DICE";
        case KORTA_COIN: return "COIN";
        case KORTA_COUNT: return "COUNT";
        case KORTA_CONV: return "CONV";
        case KORTA_FLASH: return "FLASH";
        case KORTA_COLOR: return "COLOR";
        case KORTA_SOUND: return "SOUND";
        case KORTA_PLAYER: return "PLAYER";
        case KORTA_CONTROLS: return "CONTROLS";
        default: return NULL;
    }
}
static void paint_home(Painter *d, const KortaState *s) {
    int page = s->selected / KORTA_PAGE_SIZE;
    int cur = home_app(s);
    for (int i = 0; i < KORTA_PAGE_SIZE; i++) {
        int app = home_at(page, i);
        if (app < 0) continue;
        int col = i % 3;
        int row = i / 3;
        int x = 2 + col * 53;
        int y = 14 + row * 42;
        if (app == cur) {
            fill(d, x, y, 50, 41, d->p.card);
            border(d, x, y, 50, 41, d->p.ink, 2);
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
        const char *tail = s->note_len > 23 ? s->note + s->note_len - 23 : s->note;
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
        const char *tail = s->note_len > 46 ? s->note + s->note_len - 46 : s->note;
        char line[24];
        size_t n = strlen(tail);
        size_t a = n > 23 ? 23 : n;
        memcpy(line, tail, a);
        line[a] = '\0';
        draw_text(d, line, 8, 16, d->p.ink);
        if (n > 23) draw_text(d, tail + 23, 8, 26, d->p.ink);
    }
    for (int r = 0; r < 4; r++) {
        int cols = use_flow(s) ? 3 : exact_count(r);
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
    draw_text(d, "HOLD", 132, 134, d->p.band_ink);
}
static void paint_notes(Painter *d, const KortaState *s) {
    fill(d, 4, 16, 152, 62, d->p.card);
    border(d, 4, 16, 152, 62, d->p.line, 1);
    if (!s->note_len) centered(d, "EMPTY", 80, 40, d->p.muted);
    else {
        char line[24];
        for (int i = 0; i < 6; i++) {
            int start = i * 23;
            if (start >= s->note_len) break;
            int n = s->note_len - start;
            if (n > 23) n = 23;
            memcpy(line, s->note + start, (size_t)n);
            line[n] = '\0';
            draw_text(d, line, 8, 20 + i * 10, d->p.ink);
        }
    }
    row_at(d, "EDIT", NULL, 80, s->focus == 0);
    row_at(d, "CLEAR", NULL, 98, s->focus == 1);
    if (!s->hw.storage_ok) centered(d, "SAVE FAILED", 80, 118, d->p.accent);
}
static void paint_clock(Painter *d, const KortaState *s) {
    char big[16];
    clock_text(s, big, sizeof big, false);
    big_center(d, s->time_known ? big : "SET", 16, d->p.ink);
    char date[20];
    int month = s->month >= 1 && s->month <= 12 ? s->month : 1;
    snprintf(date, sizeof date, "%02u %s %04u", s->day, MONTHS[month - 1], s->year);
    centered(d, date, 80, 36, d->p.muted);
    static const char *const labels[7] = {"HOUR", "MIN", "DAY", "MONTH", "YEAR", "CLOCK", "ZONE"};
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
        else if (s->zone == 0) right = "UTC";
        else snprintf(value, sizeof value, "UTC%+d", s->zone);
        row_at(d, labels[index], right, 48 + i * 15, index == s->focus);
    }
}
static void paint_alarm(Painter *d, const KortaState *s) {
    char big[12];
    int hour = s->alarm_h;
    if (s->hour12) {
        char mark = hour >= 12 ? 'P' : 'A';
        hour %= 12;
        if (!hour) hour = 12;
        snprintf(big, sizeof big, "%d:%02u%c", hour, s->alarm_m, mark);
    } else snprintf(big, sizeof big, "%02u:%02u", s->alarm_h, s->alarm_m);
    big_center(d, big, 28, d->p.ink);
    char hm[8], mm[8];
    snprintf(hm, sizeof hm, "%02u", s->alarm_h);
    snprintf(mm, sizeof mm, "%02u", s->alarm_m);
    row_at(d, "ALARM", s->alarm_on ? "ON" : "OFF", 64, s->focus == 0);
    row_at(d, "HOUR", hm, 82, s->focus == 1);
    row_at(d, "MIN", mm, 100, s->focus == 2);
}
static void paint_timer(Painter *d, const KortaState *s) {
    char big[16];
    if (s->timer_done) snprintf(big, sizeof big, "DONE");
    else fmt_ms(big, sizeof big, timer_shown(s));
    big_center(d, big, 22, s->timer_run ? d->p.accent : d->p.ink);
    char length[12];
    snprintf(length, sizeof length, "%u:%02u", s->timer_set / 60, s->timer_set % 60);
    row_at(d, "MODE", s->timer_count ? "COUNT" : "WATCH", 58, s->focus == 0);
    row_at(d, "LENGTH", s->timer_count ? length : "--", 76, s->focus == 1);
    row_at(d, "RESET", NULL, 94, s->focus == 2);
    centered(d, s->timer_run ? "A STOP" : "A GO", 80, 122, d->p.muted);
}
static void paint_calc(Painter *d, const KortaState *s) {
    char big[16];
    if (s->calc_err) snprintf(big, sizeof big, "ERR");
    else snprintf(big, sizeof big, "%ld", (long)s->calc_entry);
    int w = scaled_width(big, 2);
    if (w > 150) draw_text(d, big, 152 - text_width(big), 22, d->p.ink);
    else draw_scaled(d, big, 152 - w, 16, d->p.ink, 2);
    for (int i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = col * 40;
        int y = 52 + row * 23;
        bool on = i == s->focus;
        fill(d, x + 1, y, 38, 21, on ? d->p.ink : d->p.card);
        char label[2] = {CALC_KEYS[i], 0};
        int tw = text_width(label);
        draw_text(d, label, x + 1 + (38 - tw) / 2, y + 7, on ? d->p.bg : d->p.ink);
    }
}
static void paint_find(Painter *d, const KortaState *s) {
    uint8_t ids[24];
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
    char size[12];
    snprintf(size, sizeof size, "%uB", s->note_len);
    row_at(d, "NOTE", s->note_len ? size : "EMPTY", 20, s->focus == 0);
    draw_text(d, "SAVED IN NVS", 7, 48, d->p.muted);
    draw_text(d, "SD UNAVAILABLE", 7, 64, d->p.muted);
    if (!s->hw.storage_ok) draw_text(d, "SAVE FAILED", 7, 88, d->p.accent);
}
static void paint_setup(Painter *d, const KortaState *s) {
    int start, shown;
    list_window(s, 7, 8, &start, &shown);
    for (int i = 0; i < shown; i++) {
        int row = start + i;
        int index = DISPLAY_MAP[row];
        char buf[16];
        row_at(d, setup_label(index), setup_value(s, index, buf, sizeof buf), 14 + i * 16, row == s->focus);
    }
}
static void paint_controls(Painter *d, const KortaState *s) {
    for (int i = 0; i < 3; i++) {
        char buf[16];
        int index = CONTROL_MAP[i];
        row_at(d, setup_label(index), setup_value(s, index, buf, sizeof buf), 20 + i * 18, i == s->focus);
    }
}
static void paint_power(Painter *d, const KortaState *s) {
    char big[8];
    if (!s->hw.battery_known) snprintf(big, sizeof big, "--");
    else snprintf(big, sizeof big, "%u%%", s->hw.battery_pct);
    big_center(d, big, 16, d->p.ink);
    char line[24];
    if (s->hw.battery_known) {
        snprintf(line, sizeof line, "%u.%02uV %s", s->hw.battery_mv / 1000, (s->hw.battery_mv % 1000) / 10, s->hw.lipo ? "LIPO" : "AA");
        centered(d, line, 80, 38, d->p.ink);
        centered(d, s->hw.charging ? "CHARGING" : "IN USE", 80, 50, d->p.muted);
    } else centered(d, "WAITING", 80, 42, d->p.muted);
    char level[8], batt[8];
    snprintf(level, sizeof level, "%u", s->hw.brightness);
    const char *low = setup_value(s, 9, batt, sizeof batt);
    row_at(d, "BRIGHT", level, 72, s->focus == 0);
    row_at(d, "LOWBAT", low, 90, s->focus == 1);
    int bars = s->hw.brightness;
    int full = bars / 2;
    int x = 8;
    for (int i = 0; i < 8; i++) {
        int h = 4 + i;
        uint16_t c = i < full || (i == full && (bars % 2)) ? d->p.accent : d->p.line;
        fill(d, x + i * 8, 126 - h, 6, h, c);
    }
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
static void alarm_text(const KortaState *s, char *out, size_t n) {
    if (!s->alarm_on) { snprintf(out, n, "OFF"); return; }
    int hour = s->alarm_h;
    if (s->hour12) {
        char mark = hour >= 12 ? 'P' : 'A';
        hour %= 12;
        if (!hour) hour = 12;
        snprintf(out, n, "%d:%02u%c", hour, s->alarm_m, mark);
    } else snprintf(out, n, "%02u:%02u", s->alarm_h, s->alarm_m);
}
static void paint_time(Painter *d, const KortaState *s) {
    char clk[16], alarm[16], length[12];
    if (s->time_known) clock_text(s, clk, sizeof clk, false);
    else snprintf(clk, sizeof clk, "--:--");
    alarm_text(s, alarm, sizeof alarm);
    snprintf(length, sizeof length, "%u:%02u", s->timer_set / 60u, s->timer_set % 60u);
    row_at(d, "CLOCK", clk, 24, s->focus == 0);
    row_at(d, "ALARM", alarm, 44, s->focus == 1);
    row_at(d, "TIMER", length, 64, s->focus == 2);
    draw_text(d, "B BACK", 6, 132, d->p.muted);
}
static void paint_settings(Painter *d, const KortaState *s) {
    char buf[16];
    bool on_search = s->focus == 0;
    fill(d, 4, 14, 152, 18, d->p.card);
    border(d, 4, 14, 152, 18, on_search ? d->p.ink : d->p.line, on_search ? 2 : 1);
    draw_text(d, "SEARCH", 10, 19, on_search ? d->p.ink : d->p.muted);
    int cat_n = SET_HUB - 1;
    int cat_focus = s->focus == 0 ? 0 : (int)s->focus - 1;
    int start = 0;
    if (cat_n > 6 && cat_focus > 3) start = cat_focus - 3;
    if (start + 6 > cat_n) start = cat_n - 6;
    if (start < 0) start = 0;
    int shown = cat_n - start;
    if (shown > 6) shown = 6;
    for (int i = 0; i < shown; i++) {
        int index = start + i + 1;
        const char *right = NULL;
        if (index == 1) right = wifi_word(s->link.phase);
        else if (index == 2) right = s->night ? "NIGHT" : "PAPER";
        else if (index == 3) {
            if (s->hw.silent) right = "MUTE";
            else {
                snprintf(buf, sizeof buf, "%u", s->vol_sys);
                right = buf;
            }
        } else if (index == 5) {
            if (s->hw.battery_known) snprintf(buf, sizeof buf, "%u%%", s->hw.battery_pct);
            else snprintf(buf, sizeof buf, "--");
            right = buf;
        }
        row_at(d, SET_HUB_NAME[index], right, 36 + i * 16, index == s->focus);
    }
}
static void ellipse(Painter *d, int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx < 1 || ry < 1) return;
    int64_t limit = (int64_t)rx * rx * ry * ry;
    for (int y = -ry; y <= ry; y++) {
        int x = rx;
        while (x > 0 && (int64_t)x * x * ry * ry + (int64_t)y * y * rx * rx > limit) x--;
        fill(d, cx - x, cy + y, x * 2 + 1, 1, color);
    }
}
static void pip(Painter *d, int x, int y) {
    fill(d, x - 3, y - 3, 7, 7, d->p.accent);
    fill(d, x - 3, y - 3, 1, 1, d->p.hi);
    fill(d, x + 3, y - 3, 1, 1, d->p.hi);
    fill(d, x - 3, y + 3, 1, 1, d->p.hi);
    fill(d, x + 3, y + 3, 1, 1, d->p.hi);
}
static void paint_dice(Painter *d, const KortaState *s) {
    bool rolling = s->dice && (int32_t)(s->dice_anim_until - s->now_ms) > 0;
    uint32_t tick = rolling ? (640u - (s->dice_anim_until - s->now_ms)) / 64u : 0u;
    static const int8_t bounce[10] = {0, -3, -6, -4, -1, -5, -3, 0, -2, 0};
    int top = 31 + (rolling ? bounce[tick % 10u] : 0);
    int face = rolling ? (int)((tick + s->roll_nonce) % 6u) + 1 : s->dice;
    ellipse(d, 80, 95, 34, 5, d->p.line);
    fill(d, 57, top + 5, 52, 52, d->p.line);
    fill(d, 54, top, 52, 52, d->p.ink);
    fill(d, 57, top + 3, 46, 46, d->p.hi);
    fill(d, 57, top + 3, 46, 2, d->p.card);
    fill(d, 54, top, 3, 3, d->p.bg);
    fill(d, 103, top, 3, 3, d->p.bg);
    fill(d, 54, top + 49, 3, 3, d->p.bg);
    fill(d, 103, top + 49, 3, 3, d->p.bg);
    if (face > 0) {
        if (face == 1 || face == 3 || face == 5) pip(d, 80, top + 26);
        if (face >= 2) { pip(d, 68, top + 14); pip(d, 92, top + 38); }
        if (face >= 4) { pip(d, 92, top + 14); pip(d, 68, top + 38); }
        if (face == 6) { pip(d, 68, top + 26); pip(d, 92, top + 26); }
    } else centered(d, "?", 80, top + 23, d->p.accent);
    centered(d, rolling ? "ROLLING" : s->dice ? "RESULT" : "READY", 80, 105, d->p.ink);
    centered(d, "A ROLL   B BACK", 80, 126, d->p.muted);
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
    centered(d, flipping ? "FLIPPING" : s->coin == 1 ? "HEADS" : s->coin == 2 ? "TAILS" : "READY", 80, 105, d->p.ink);
    centered(d, "A FLIP   B BACK", 80, 126, d->p.muted);
}
static void paint_count(Painter *d, const KortaState *s) {
    char n[8];
    snprintf(n, sizeof n, "%d", s->tally);
    big_center(d, n, 40, d->p.ink);
    centered(d, "A +  LEFT -", 80, 88, d->p.muted);
}
static void paint_conv(Painter *d, const KortaState *s) {
    char left[16], right[16];
    if (s->conv_kind == 0) {
        snprintf(left, sizeof left, "%d C", s->conv_n);
        snprintf(right, sizeof right, "%d F", s->conv_n * 9 / 5 + 32);
    } else {
        snprintf(left, sizeof left, "%d IN", s->conv_n);
        snprintf(right, sizeof right, "%d CM", (s->conv_n * 254) / 100);
    }
    row_at(d, s->conv_kind == 0 ? "C TO F" : "IN TO CM", NULL, 24, s->focus == 0);
    row_at(d, left, right, 44, s->focus == 1);
    centered(d, "A SWITCH", 80, 88, d->p.muted);
}
static void paint_flash(Painter *d, const KortaState *s) {
    fill(d, 0, 12, 160, 132, d->p.hi);
    centered(d, s->hw.brightness >= 16 ? "MAX" : "DIM", 80, 64, d->p.ink);
    centered(d, "A TOGGLE", 80, 88, d->p.muted);
}
static void paint_color(Painter *d, const KortaState *s) {
    unsigned p = s->hw.palette < 15 ? s->hw.palette : 0;
    row_at(d, "PALETTE", PALETTE_NAME[p], 28, true);
    centered(d, "LEFT RIGHT", 80, 88, d->p.muted);
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
static void paint_player(Painter *d, const KortaState *s) {
    char n[4];
    snprintf(n, sizeof n, "%u", s->hw.player ? s->hw.player : 1);
    row_at(d, "PLAYER", n, 28, true);
    centered(d, "LEFT RIGHT", 80, 88, d->p.muted);
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
        char name[20];
        if (index > 0 && !(s->link.saved && index == rows - 1)) {
            int net = index - 1;
            if (net >= 0 && net < s->link.count) {
                snprintf(name, sizeof name, "%.16s", s->link.nets[net].ssid);
                label = name;
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
    char line[28];
    draw_text(d, "KORTAMOD", 7, 18, d->p.ink);
    snprintf(line, sizeof line, "MCU %.16s", s->hw.mcu[0] ? s->hw.mcu : "-");
    draw_text(d, line, 7, 34, d->p.ink);
    snprintf(line, sizeof line, "FPGA %s", s->hw.fpga[0] ? s->hw.fpga : "-");
    draw_text(d, line, 7, 48, d->p.ink);
    if (s->hw.battery_known) snprintf(line, sizeof line, "BAT %u.%02uV", s->hw.battery_mv / 1000, (s->hw.battery_mv % 1000) / 10);
    else snprintf(line, sizeof line, "BAT --");
    draw_text(d, line, 7, 62, d->p.ink);
    snprintf(line, sizeof line, "HEAP %uK", s->hw.heap_kb);
    draw_text(d, line, 7, 76, d->p.ink);
    snprintf(line, sizeof line, "WIFI %s", wifi_word(s->link.phase));
    draw_text(d, line, 7, 90, d->p.ink);
    if (s->link.ip) {
        fmt_ip(line, sizeof line, s->link.ip);
        draw_text(d, line, 7, 104, d->p.ink);
    }
}
static void paint_quick(Painter *d, const KortaState *s) {
    char level[8];
    snprintf(level, sizeof level, "%u", s->hw.brightness);
    row_at(d, "BRIGHT", level, 20, s->focus == 0);
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
static void paint_ring(Painter *d, const KortaState *s, const char *title) {
    bool flash = ((s->now_ms / 400u) & 1u) != 0;
    fill(d, 0, 0, 160, 144, flash ? d->p.accent : d->p.ink);
    int w = scaled_width(title, 2);
    draw_scaled(d, title, 80 - w / 2, 40, d->p.bg, 2);
    char big[12];
    if (s->view == KORTA_TIMER_RING) snprintf(big, sizeof big, "DONE");
    else clock_text(s, big, sizeof big, false);
    int tw = text_width(big);
    draw_text(d, big, 80 - tw / 2, 68, d->p.hi);
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
    s->alarm_h = 7;
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
    s->link = *link;
    if (s->link.count > KORTA_NET_MAX) s->link.count = KORTA_NET_MAX;
    s->link.ssid[sizeof s->link.ssid - 1] = '\0';
    for (int i = 0; i < KORTA_NET_MAX; i++) s->link.nets[i].ssid[32] = '\0';
    if (s->view == KORTA_LINKS) {
        int n = link_rows(s);
        if (n < 1) s->focus = 0;
        else if ((int)s->focus >= n) s->focus = (uint8_t)(n - 1);
    }
}
void korta_apply_prefs(KortaState *s, uint8_t flags, int8_t zone, uint8_t alarm_h, uint8_t alarm_m, const char *note) {
    if (!s) return;
    s->night = (flags & 1u) != 0;
    s->flow = (flags & 2u) != 0;
    s->hour12 = (flags & 4u) != 0;
    s->alarm_on = (flags & 8u) != 0;
    s->wifi_keep = (flags & 16u) != 0;
    if (zone < -12) zone = -12;
    if (zone > 14) zone = 14;
    s->zone = zone;
    s->alarm_h = (uint8_t)(alarm_h % 24);
    s->alarm_m = (uint8_t)(alarm_m % 60);
    if (!note) return;
    size_t n = strlen(note);
    if (n > KORTA_NOTE_CAPACITY) n = KORTA_NOTE_CAPACITY;
    memcpy(s->note, note, n);
    s->note[n] = '\0';
    s->note_len = (uint8_t)n;
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
    uint8_t kr0, uint8_t kc0, uint8_t sil0, uint8_t night0, uint8_t vm0, uint8_t nl0) {
    if (s->sfx == KORTA_SFX_ALERT) return;
    if (s->view == KORTA_ALARM_RING || s->view == KORTA_TIMER_RING) {
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
    if (s->view == KORTA_KEYBOARD && (s->note_len != nl0 || b == KORTA_A)) {
        s->sfx = KORTA_SFX_TYPE;
        return;
    }
    if (b == KORTA_A || b == KORTA_B) {
        bool play = (s->view == KORTA_HOME && home_app(s) >= 0 && APP_VIEW[home_app(s)] == KORTA_PLAY) || s->view == KORTA_PLAY;
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
    if (!s->primed) {
        s->primed = true;
        s->held = held;
        s->now_ms = time_ms;
        s->blocked_held = held;
        s->edge_seen = 0;
        memset(s->was_down, 0, sizeof s->was_down);
        memset(s->was_hold, 0, sizeof s->was_hold);
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
        uint8_t sil0 = s->hw.silent, night0 = s->night, vm0 = s->vol_mode, nl0 = s->note_len;
        if (down && !s->was_down[i]) {
            if (b == KORTA_B) s->b_from_home = (s->view == KORTA_HOME);
            bool seen = (s->edge_seen & (1u << i)) != 0;
            bool dbl = seen && time_ms - s->last_edge_ms[i] < DOUBLE_MS && !s->was_hold[i];
            s->edge_seen = (uint8_t)(s->edge_seen | (1u << i));
            s->last_edge_ms[i] = time_ms;
            s->edge_ms[i] = time_ms;
            s->long_fired[i] = false;
            s->was_hold[i] = false;
            s->next_repeat[i] = time_ms + HOLD_MS;
            s->repeat_gap[i] = REPEAT_MS;
            bool list_double = s->vol_mode != 2 && s->view != KORTA_KEYBOARD && s->view != KORTA_CALC && s->view != KORTA_ALARM_RING
                && s->view != KORTA_HOME;
            bool jump = list_double && (b == KORTA_UP || b == KORTA_DOWN);
            if (!defer && dbl && jump) on_double(s, b);
            else if (!defer) on_tap(s, b, &leave);
            if (!defer) infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, nl0);
        } else if (down && s->was_down[i]) {
            if (!s->long_fired[i] && time_ms - s->edge_ms[i] >= HOLD_MS) {
                s->long_fired[i] = true;
                s->was_hold[i] = true;
                if (b == KORTA_START) on_start_hold(s);
                else if (b == KORTA_SELECT && s->view != KORTA_KEYBOARD) on_select_hold(s);
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, nl0);
            }
            if (!defer && (int32_t)(time_ms - s->next_repeat[i]) >= 0) {
                on_repeat(s, b, &leave);
                uint16_t gap = (uint16_t)(s->repeat_gap[i] * 3 / 4);
                if (gap < REPEAT_MIN_MS) gap = REPEAT_MIN_MS;
                s->repeat_gap[i] = gap;
                s->next_repeat[i] = time_ms + gap;
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, nl0);
            }
        } else if (!down && s->was_down[i]) {
            if (defer && !s->long_fired[i]) {
                if (b == KORTA_START) on_start_short(s);
                else on_select_short(s);
                infer_sfx(s, b, v0, f0, sel0, kr0, kc0, sil0, night0, vm0, nl0);
            }
            s->was_hold[i] = s->long_fired[i];
            s->held_home = false;
        }
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
static void paint_vol(Painter *d, const KortaState *s) {
    if (!s->vol_mode) return;
    uint8_t levels[3] = {s->vol_sys, s->vol_media, s->vol_ring};
    if (s->vol_mode == 1) {
        int x = 146, y = 16, w = 12, h = 124;
        fill(d, x, y, w, h, d->p.bg);
        fill(d, x, y, 1, h, d->p.line);
        int inner = h - 4;
        int fh = inner * (int)levels[1] / 100;
        if (fh < 1 && levels[1]) fh = 1;
        fill(d, x + 2, y + h - 2 - fh, w - 3, fh, d->p.accent);
        return;
    }
    fill(d, 0, 0, 160, 144, d->p.bg);
    draw_text(d, "VOLUME", 8, 7, d->p.ink);
    fill(d, 8, 20, 144, 1, d->p.line);
    static const char *const names[3] = {"SYSTEM", "MEDIA", "RING"};
    for (int i = 0; i < 3; i++) {
        int row = 29 + i * 32;
        bool hot = s->vol_sel == i && i != 1;
        if (hot) fill(d, 4, row - 3, 152, 29, d->p.card);
        draw_text(d, names[i], 10, row, hot ? d->p.accent : d->p.ink);
        char n[8];
        snprintf(n, sizeof n, "%u", levels[i]);
        draw_text(d, n, 128, row, hot ? d->p.accent : d->p.muted);
        fill(d, 10, row + 15, 140, 5, d->p.line);
        int fw = 140 * (int)levels[i] / 100;
        if (fw < 1 && levels[i]) fw = 1;
        fill(d, 10, row + 15, fw, 5, hot ? d->p.accent : d->p.ink);
    }
    draw_text(d, "MEDIA: WHEEL", 10, 125, d->p.muted);
    draw_text(d, "UP/DOWN  LEFT/RIGHT  B", 10, 135, d->p.muted);
}
void korta_render(const KortaState *s, uint16_t *pixels) {
    if (!s || !pixels) return;
    Painter d = {pixels, palette(s->night)};
    if (s->view == KORTA_ALARM_RING) { paint_ring(&d, s, "ALARM"); paint_vol(&d, s); return; }
    if (s->view == KORTA_TIMER_RING) { paint_ring(&d, s, "TIMER"); paint_vol(&d, s); return; }
    fill(&d, 0, 0, 160, 144, d.p.bg);
    status_bar(&d, s, s->view == KORTA_HOME ? NULL : view_title(s));
    switch (s->view) {
        case KORTA_HOME: paint_home(&d, s); break;
        case KORTA_NOTES: paint_notes(&d, s); break;
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
        case KORTA_FLASH: paint_flash(&d, s); break;
        case KORTA_COLOR: paint_color(&d, s); break;
        case KORTA_SOUND: paint_sound(&d, s); break;
        case KORTA_PLAYER: paint_player(&d, s); break;
        case KORTA_CONTROLS: paint_controls(&d, s); break;
        default: break;
    }
    paint_vol(&d, s);
}
