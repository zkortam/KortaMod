/* KortaMod games engine: compiles a JavaScript subset (docs/games.md) to
 * bytecode and runs it on a stack VM with a mark-sweep heap, all inside one
 * caller-provided block of memory. No libc calls, so the same file builds for
 * the ESP32, the host tests and WebAssembly.
 *
 * Safety: every game is untrusted text. Compilation has a nesting limit; the VM
 * checks every stack, frame, heap and index bound, and each frame has an
 * instruction budget. Any failure stops the game with a line-numbered message. */
#include "kortamod_game.h"
#include "kortamod_assets.h"

#include <stdbool.h>

/* ---------- small libc stand-ins ---------- */
static void mcpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mset(void *d, int v, uint32_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)v; }
static int mcmp(const void *x, const void *y, uint32_t n) {
    const uint8_t *a = x, *b = y;
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* ---------- math without libm (identical results everywhere) ---------- */
#define KPI 3.14159265358979f
static float kfloor(float x) {
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    return t > x ? t - 1.0f : t;
}
static float kceil(float x) { float f = kfloor(x); return f < x ? f + 1.0f : f; }
static float ktrunc(float x) { return x < 0 ? -kfloor(-x) : kfloor(x); }
static float kabs(float x) { return x < 0 ? -x : x; }
static bool kisnan(float x) { return x != x; }
static float ksqrt(float x) {
    if (!(x > 0)) return x == 0 ? 0 : 0.0f / 0.0f;
    if (x > 3.0e38f) return x;
    /* Newton from a bit-level guess: exact enough for games, same on every target. */
    union { float f; uint32_t i; } u = {x};
    u.i = 0x1fbd1df5u + (u.i >> 1);
    float y = u.f;
    for (int i = 0; i < 4; i++) y = 0.5f * (y + x / y);
    return y;
}
static float ksin(float x) {
    if (kisnan(x) || kabs(x) > 1.0e6f) return 0.0f / 0.0f;
    x = x - 2 * KPI * kfloor((x + KPI) / (2 * KPI));      /* [-pi, pi) */
    if (x > KPI / 2) x = KPI - x;
    else if (x < -KPI / 2) x = -KPI - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float kcos(float x) { return ksin(x + KPI / 2); }
static float katan(float x) {
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv = x > 1;
    if (inv) x = 1 / x;
    /* Minimax-style polynomial on [0, 1]. */
    float x2 = x * x;
    float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
    if (inv) r = KPI / 2 - r;
    return neg ? -r : r;
}
static float katan2(float y, float x) {
    if (x > 0) return katan(y / x);
    if (x < 0) return y >= 0 ? katan(y / x) + KPI : katan(y / x) - KPI;
    if (y > 0) return KPI / 2;
    if (y < 0) return -KPI / 2;
    return 0;
}
static float kexp(float x) {
    if (x > 88) return 1.0f / 0.0f;
    if (x < -87) return 0;
    /* e^x = 2^k * e^r with |r| <= ln2/2 */
    float k = kfloor(x * 1.44269504f + 0.5f);
    float r = x - k * 0.69314718f;
    float e = 1 + r * (1 + r / 2 * (1 + r / 3 * (1 + r / 4 * (1 + r / 5 * (1 + r / 6)))));
    union { float f; uint32_t i; } u;
    u.i = (uint32_t)((int32_t)k + 127) << 23;
    return e * u.f;
}
static float klog(float x) {
    if (!(x > 0)) return x == 0 ? -1.0f / 0.0f : 0.0f / 0.0f;
    union { float f; uint32_t i; } u = {x};
    int e = (int)((u.i >> 23) & 0xff) - 127;
    u.i = (u.i & 0x007fffffu) | 0x3f800000u;     /* m in [1, 2) */
    float m = u.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    float t = (m - 1) / (m + 1), t2 = t * t;
    float l = 2 * t * (1 + t2 / 3 + t2 * t2 / 5 + t2 * t2 * t2 / 7 + t2 * t2 * t2 * t2 / 9);
    return l + (float)e * 0.69314718f;
}
static float kpow(float b, float e) {
    if (e == 0) return 1;
    if (e == kfloor(e) && kabs(e) <= 64) {
        float r = 1, p = b;
        int n = (int)kabs(e);
        while (n) { if (n & 1) r *= p; p *= p; n >>= 1; }
        return e < 0 ? 1 / r : r;
    }
    if (b < 0) return 0.0f / 0.0f;
    if (b == 0) return 0;
    return kexp(e * klog(b));
}
static float kmod(float a, float b) {
    if (b == 0 || kisnan(a) || kisnan(b)) return 0.0f / 0.0f;
    float q = ktrunc(a / b);
    return a - q * b;
}
static int32_t to_i32(float x) {
    if (kisnan(x) || x >= 2147483648.0f || x <= -2147483649.0f) {
        if (kisnan(x)) return 0;
        /* JS wraps modulo 2^32; games only need something defined. */
        return 0;
    }
    return (int32_t)x;
}

/* ---------- values and heap ---------- */
/* The heap is a run of blocks from offset 8 up to `bump`, each starting with a
 * header that records its size, so a sweep can walk it. Collection is mark and
 * sweep without moving anything: dead neighbours merge into free runs, and
 * allocation takes the first run that fits, splitting off the rest. Offsets
 * (not pointers) refer to blocks, so the same code runs on 32- and 64-bit hosts. */
enum { T_UNDEF, T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ, T_FN, T_NAT, T_ENV, T_RAW, T_FREE };
typedef struct { uint32_t t; union { float n; uint32_t r; } u; } Val;
typedef struct { uint32_t next; uint8_t type, flags; uint16_t units; } Hdr;   /* units of 8 bytes */
enum { F_MARK = 1, F_PERM = 2 };
typedef struct { Hdr h; uint32_t len, hash; char s[]; } Str;
typedef struct { Hdr h; uint32_t len, cap, data; } Arr;  /* also objects: data holds Props */
typedef struct { uint32_t key; Val v; } Prop;
typedef struct { Hdr h; uint32_t proto, env; } Fn;
typedef struct { Hdr h; uint32_t parent, count; Val v[]; } Env;

#define BLOCK_MAX 8192
#define ARR_MAX ((BLOCK_MAX - 8) / 8)
#define OBJ_MAX ((BLOCK_MAX - 8) / 12)
#define STR_MAX (BLOCK_MAX - 17)

#define STACK_MAX 256
#define FRAME_MAX 64
#define GLOBAL_MAX 192
#define CONST_MAX 384
#define PROTO_MAX 128
#define LINES_MAX 512
#define NESTED_MAX 6
#define BUDGET 150000
#define LOG_MAX 512

typedef struct { uint16_t code; uint8_t params, locals, env, pad; } Proto;
typedef struct { uint32_t pc, env; uint16_t proto, base; } Frame;
typedef struct { uint16_t pc, line; } LineEntry;

struct KGame {
    uint8_t *heap;
    uint32_t heap_size, bump, high, free_head;
    bool perm_mode, failed, loaded;
    char err[100];
    /* code */
    uint8_t *code;
    uint32_t code_len, code_cap;
    Proto protos[PROTO_MAX];
    int nprotos;
    Val consts[CONST_MAX];
    int nconsts;
    LineEntry lines[LINES_MAX];
    int nlines;
    /* globals */
    uint32_t gname[GLOBAL_MAX];
    Val gval[GLOBAL_MAX];
    uint8_t gdef[GLOBAL_MAX];
    int nglobals;
    /* vm */
    Val stack[STACK_MAX];
    int sp;
    Frame frames[FRAME_MAX];
    int nframes;
    int nested, join_depth;
    uintptr_t stack_base;   /* address near the top of the C stack when load/frame began */
    uint32_t stack_limit;   /* bytes of C stack the engine may use below it; 0 = no check */
    int32_t budget;
    uint32_t err_pc;
    uint32_t rng;
    uint32_t frame_no;
    uint8_t held, pressed;
    float saved;
    bool saved_dirty;
    int g_init, g_update, g_draw;
    char log[LOG_MAX];
    uint32_t log_len;
    uint8_t *fb;            /* KGAME_FB_BYTES, its own block */
};

#define REF(g, r) ((void *)((g)->heap + (r)))
#define HDR(g, r) ((Hdr *)REF(g, r))
#define STR(g, r) ((Str *)REF(g, r))
#define ARR(g, r) ((Arr *)REF(g, r))
#define FN(g, r) ((Fn *)REF(g, r))
#define ENV(g, r) ((Env *)REF(g, r))

static Val vundef(void) { Val v = {T_UNDEF, {.r = 0}}; return v; }
static Val vnull(void) { Val v = {T_NULL, {.r = 0}}; return v; }
static Val vbool(bool b) { Val v = {T_BOOL, {.r = b ? 1u : 0u}}; return v; }
static Val vnum(float n) { Val v = {T_NUM, {.n = n}}; return v; }
static Val vref(uint32_t t, uint32_t r) { Val v = {t, {.r = r}}; return v; }

static int fail(KGame *g, const char *msg) {
    if (g->failed) return -1;
    g->failed = true;
    uint32_t n = slen(msg);
    if (n >= sizeof g->err) n = sizeof g->err - 1;
    mcpy(g->err, msg, n);
    g->err[n] = '\0';
    return -1;
}

/* Joins pieces into the error text: fail3(g, "x ", name, n, " is not defined"). */
static int fail3(KGame *g, const char *a, const char *b, uint32_t blen, const char *c) {
    char m[100];
    uint32_t n = 0;
    for (uint32_t i = 0; a[i] && n < 90; i++) m[n++] = a[i];
    for (uint32_t i = 0; i < blen && n < 90; i++) m[n++] = b[i];
    for (uint32_t i = 0; c[i] && n < 98; i++) m[n++] = c[i];
    m[n] = '\0';
    return fail(g, m);
}

static void gc(KGame *g);
/* Safety net for small task stacks: true once the C stack has grown past the limit. */
static bool stack_low(const KGame *g) {
    if (!g->stack_limit) return false;
    volatile char probe;
    uintptr_t here = (uintptr_t)&probe;
    return here < g->stack_base && g->stack_base - here > g->stack_limit;
}

/* First fit from the free runs (in address order), else from the top. */
static uint32_t take(KGame *g, uint32_t size) {
    uint32_t need = size / 8;
    for (uint32_t *link = &g->free_head; *link; link = &HDR(g, *link)->next) {
        uint32_t f = *link;
        Hdr *h = HDR(g, f);
        if (h->units < need) continue;
        if (h->units - need >= 2) {
            /* Split: the remainder stays in the list in f's place. */
            uint32_t rest = f + size;
            Hdr *r = HDR(g, rest);
            r->type = T_FREE;
            r->flags = 0;
            r->units = (uint16_t)(h->units - need);
            r->next = h->next;
            *link = rest;
            h->units = (uint16_t)need;
        } else *link = h->next;
        return f;
    }
    if (g->bump + size <= g->heap_size) {
        uint32_t r = g->bump;
        g->bump += size;
        if (g->bump > g->high) g->high = g->bump;
        HDR(g, r)->units = (uint16_t)need;
        return r;
    }
    return 0;
}
static uint32_t alloc(KGame *g, uint8_t type, uint32_t bytes) {
    if (g->failed) return 0;
    if (bytes > BLOCK_MAX) { fail(g, "value too large"); return 0; }
    uint32_t size = (bytes + 7) & ~7u;
    if (size < 16) size = 16;
    uint32_t r = take(g, size);
    if (!r && !g->perm_mode) { gc(g); r = take(g, size); }
    if (!r) { fail(g, "out of memory"); return 0; }
    Hdr *h = HDR(g, r);
    uint16_t units = h->units;
    mset(h, 0, (uint32_t)units * 8);
    h->units = units;
    h->type = type;
    h->flags = g->perm_mode ? F_PERM : 0;
    return r;
}

static uint32_t hash_bytes(const char *s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}
static uint32_t new_str(KGame *g, const char *s, uint32_t n) {
    if (n > STR_MAX) { fail(g, "string too long"); return 0; }
    uint32_t r = alloc(g, T_STR, (uint32_t)sizeof(Str) + n + 1);
    if (!r) return 0;
    Str *st = STR(g, r);
    st->len = n;
    st->hash = hash_bytes(s, n);
    mcpy(st->s, s, n);
    st->s[n] = '\0';
    return r;
}

/* ---------- garbage collection (explicit mark stack, overflow rescans) ---------- */
#define MARK_MAX 192
typedef struct { uint32_t stack[MARK_MAX]; int n; bool overflow; } Marker;

static void mark_ref(KGame *g, Marker *m, uint32_t r) {
    if (!r) return;
    Hdr *h = HDR(g, r);
    if (h->flags & (F_MARK | F_PERM)) return;
    h->flags |= F_MARK;
    if (h->type == T_STR || h->type == T_RAW) return;
    if (m->n < MARK_MAX) m->stack[m->n++] = r;
    else m->overflow = true;
}
static void mark_val(KGame *g, Marker *m, Val v) {
    if (v.t == T_STR || v.t == T_ARR || v.t == T_OBJ || v.t == T_FN) mark_ref(g, m, v.u.r);
}
static void mark_children(KGame *g, Marker *m, uint32_t r) {
    Hdr *h = HDR(g, r);
    if (h->type == T_ARR) {
        Arr *a = ARR(g, r);
        mark_ref(g, m, a->data);
        Val *v = a->data ? (Val *)((uint8_t *)REF(g, a->data) + sizeof(Hdr)) : 0;
        for (uint32_t i = 0; v && i < a->len; i++) mark_val(g, m, v[i]);
    } else if (h->type == T_OBJ) {
        Arr *a = ARR(g, r);
        mark_ref(g, m, a->data);
        Prop *p = a->data ? (Prop *)((uint8_t *)REF(g, a->data) + sizeof(Hdr)) : 0;
        for (uint32_t i = 0; p && i < a->len; i++) { mark_ref(g, m, p[i].key); mark_val(g, m, p[i].v); }
    } else if (h->type == T_FN) {
        mark_ref(g, m, FN(g, r)->env);
    } else if (h->type == T_ENV) {
        Env *e = ENV(g, r);
        mark_ref(g, m, e->parent);
        for (uint32_t i = 0; i < e->count; i++) mark_val(g, m, e->v[i]);
    }
}
static void gc(KGame *g) {
    Marker m;
    m.n = 0;
    m.overflow = false;
    for (int i = 0; i < g->sp; i++) mark_val(g, &m, g->stack[i]);
    for (int i = 0; i < g->nglobals; i++) mark_val(g, &m, g->gval[i]);
    for (int i = 0; i < g->nframes; i++) mark_ref(g, &m, g->frames[i].env);
    for (;;) {
        while (m.n) mark_children(g, &m, m.stack[--m.n]);
        if (!m.overflow) break;
        /* The mark stack filled up: rescan marked blocks for unmarked children. */
        m.overflow = false;
        for (uint32_t r = 8; r < g->bump; r += (uint32_t)HDR(g, r)->units * 8)
            if (HDR(g, r)->type != T_FREE && (HDR(g, r)->flags & F_MARK)) mark_children(g, &m, r);
    }
    /* Sweep in address order, merging every dead or free neighbour into one run. */
    g->free_head = 0;
    uint32_t *tail = &g->free_head;
    uint32_t run = 0;
    for (uint32_t r = 8; r < g->bump;) {
        Hdr *h = HDR(g, r);
        uint32_t size = (uint32_t)h->units * 8;
        bool live = h->type != T_FREE && (h->flags & (F_MARK | F_PERM));
        if (live) {
            h->flags &= (uint8_t)~F_MARK;
            run = 0;
        } else if (run && HDR(g, run)->units + h->units <= 0xffff) {
            HDR(g, run)->units = (uint16_t)(HDR(g, run)->units + h->units);
        } else {
            h->type = T_FREE;
            h->flags = 0;
            h->next = 0;
            *tail = r;
            tail = &h->next;
            run = r;
        }
        r += size;
    }
    /* A free run at the top goes back to the bump area. */
    if (run && run + (uint32_t)HDR(g, run)->units * 8 == g->bump) {
        uint32_t *link = &g->free_head;
        while (*link && *link != run) link = &HDR(g, *link)->next;
        if (*link == run) { *link = 0; g->bump = run; }
    }
}

/* Raw backing store for arrays and objects. */
static uint8_t *raw_ptr(KGame *g, uint32_t r) { return (uint8_t *)REF(g, r) + sizeof(Hdr); }
static Val *arr_items(KGame *g, uint32_t a) { return ARR(g, a)->data ? (Val *)raw_ptr(g, ARR(g, a)->data) : 0; }
static Prop *obj_props(KGame *g, uint32_t o) { return ARR(g, o)->data ? (Prop *)raw_ptr(g, ARR(g, o)->data) : 0; }

/* Grows the store of a rooted array or object to hold `need` items. */
static bool grow(KGame *g, uint32_t a, uint32_t need, uint32_t item) {
    Arr *x = ARR(g, a);
    if (need <= x->cap) return true;
    uint32_t max = item == sizeof(Val) ? ARR_MAX : OBJ_MAX;
    if (need > max) { fail(g, "array or object too large"); return false; }
    uint32_t cap = x->cap ? x->cap * 2 : 4;
    if (cap < need) cap = need;
    if (cap > max) cap = max;
    uint32_t raw = alloc(g, T_RAW, (uint32_t)sizeof(Hdr) + cap * item);
    if (!raw) return false;
    x = ARR(g, a);  /* the heap does not move, but keep the habit */
    if (x->data) mcpy(raw_ptr(g, raw), raw_ptr(g, x->data), x->len * item);
    x->data = raw;
    x->cap = cap;
    return true;
}
static uint32_t new_arr(KGame *g, uint32_t cap) {
    uint32_t a = alloc(g, T_ARR, sizeof(Arr));
    return a;
    (void)cap;
}
static uint32_t new_obj(KGame *g) { return alloc(g, T_OBJ, sizeof(Arr)); }

static bool str_eq(KGame *g, uint32_t a, uint32_t b) {
    if (a == b) return true;
    Str *x = STR(g, a), *y = STR(g, b);
    return x->len == y->len && x->hash == y->hash && mcmp(x->s, y->s, x->len) == 0;
}
static int obj_find(KGame *g, uint32_t o, uint32_t key) {
    Prop *p = obj_props(g, o);
    for (uint32_t i = 0; p && i < ARR(g, o)->len; i++) if (str_eq(g, p[i].key, key)) return (int)i;
    return -1;
}
static Val obj_get(KGame *g, uint32_t o, uint32_t key) {
    int i = obj_find(g, o, key);
    return i < 0 ? vundef() : obj_props(g, o)[i].v;
}
/* o, key and v must be reachable from the stack: growing may collect. */
static bool obj_set(KGame *g, uint32_t o, uint32_t key, Val v) {
    int i = obj_find(g, o, key);
    if (i >= 0) { obj_props(g, o)[i].v = v; return true; }
    Arr *x = ARR(g, o);
    if (!grow(g, o, x->len + 1, sizeof(Prop))) return false;
    x = ARR(g, o);
    Prop *p = obj_props(g, o);
    p[x->len].key = key;
    p[x->len].v = v;
    x->len++;
    return true;
}
static bool arr_push(KGame *g, uint32_t a, Val v) {
    Arr *x = ARR(g, a);
    if (!grow(g, a, x->len + 1, sizeof(Val))) return false;
    x = ARR(g, a);
    arr_items(g, a)[x->len++] = v;
    return true;
}

/* ---------- conversions ---------- */
static bool truthy(KGame *g, Val v) {
    switch (v.t) {
        case T_UNDEF: case T_NULL: return false;
        case T_BOOL: return v.u.r != 0;
        case T_NUM: return v.u.n != 0 && !kisnan(v.u.n);
        case T_STR: return STR(g, v.u.r)->len != 0;
        default: return true;
    }
}
/* Formats a number like JavaScript does for ordinary values. */
static uint32_t fmt_num(float x, char *out) {
    uint32_t n = 0;
    if (kisnan(x)) { mcpy(out, "NaN", 3); return 3; }
    if (x > 3.0e38f) { mcpy(out, "Infinity", 8); return 8; }
    if (x < -3.0e38f) { mcpy(out, "-Infinity", 9); return 9; }
    if (x < 0) { out[n++] = '-'; x = -x; }
    if (x >= 1.0e9f) {
        /* Large: an integer-ish rendering is enough for games. */
        char tmp[24]; int k = 0; float y = kfloor(x);
        int digits = 0; while (y >= 1 && digits < 39) { tmp[k++] = (char)('0' + (int)kmod(y, 10)); y = kfloor(y / 10); digits++; }
        while (k) out[n++] = tmp[--k];
        return n;
    }
    uint32_t ip = (uint32_t)x;
    float frac = x - (float)ip;
    /* Round to 6 significant fractional digits, then trim zeros. */
    uint32_t scale = ip >= 100000 ? 10 : ip >= 1000 ? 1000 : 100000;
    uint32_t fp = (uint32_t)(frac * (float)scale + 0.5f);
    if (fp >= scale) { ip++; fp = 0; }
    char tmp[12]; int k = 0;
    do { tmp[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (k) out[n++] = tmp[--k];
    if (fp) {
        out[n++] = '.';
        char f[8]; int d = 0;
        for (uint32_t s = scale / 10; s; s /= 10) { f[d++] = (char)('0' + (fp / s) % 10); }
        while (d && f[d - 1] == '0') d--;
        for (int i = 0; i < d; i++) out[n++] = f[i];
    }
    return n;
}
static float parse_num(const char *s, uint32_t n, bool *ok) {
    uint32_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n')) n--;
    if (i == n) { *ok = true; return 0; }
    bool neg = false;
    if (s[i] == '-' || s[i] == '+') { neg = s[i] == '-'; i++; }
    float v = 0; bool any = false;
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        while (i < n) {
            char c = s[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (float)d; any = true; i++;
        }
    } else {
        while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (float)(s[i] - '0'); i++; any = true; }
        if (i < n && s[i] == '.') {
            i++; float p = 0.1f;
            while (i < n && s[i] >= '0' && s[i] <= '9') { v += (float)(s[i] - '0') * p; p *= 0.1f; i++; any = true; }
        }
        if (any && i < n && (s[i] == 'e' || s[i] == 'E')) {
            i++; bool en = false; int e = 0;
            if (i < n && (s[i] == '-' || s[i] == '+')) { en = s[i] == '-'; i++; }
            while (i < n && s[i] >= '0' && s[i] <= '9') { if (e < 99) e = e * 10 + (s[i] - '0'); i++; }
            v *= kpow(10, (float)(en ? -e : e));
        }
    }
    *ok = any && i == n;
    return neg ? -v : v;
}
static float to_num(KGame *g, Val v) {
    switch (v.t) {
        case T_NUM: return v.u.n;
        case T_BOOL: return (float)v.u.r;
        case T_NULL: return 0;
        case T_STR: { bool ok; float x = parse_num(STR(g, v.u.r)->s, STR(g, v.u.r)->len, &ok); return ok ? x : 0.0f / 0.0f; }
        default: return 0.0f / 0.0f;
    }
}
/* Writes a short text form of v (numbers, booleans, null) or returns false for refs. */
static uint32_t prim_text(KGame *g, Val v, char *buf) {
    switch (v.t) {
        case T_UNDEF: mcpy(buf, "undefined", 9); return 9;
        case T_NULL: mcpy(buf, "null", 4); return 4;
        case T_BOOL: if (v.u.r) { mcpy(buf, "true", 4); return 4; } mcpy(buf, "false", 5); return 5;
        case T_NUM: return fmt_num(v.u.n, buf);
        case T_FN: case T_NAT: mcpy(buf, "function", 8); return 8;
        case T_OBJ: mcpy(buf, "[object Object]", 15); return 15;
        default: (void)g; return 0;
    }
}
/* String form of v as a heap string, written to *out (rooting is the caller's). */
static uint32_t to_str(KGame *g, Val v);
static uint32_t join_arr(KGame *g, uint32_t a, const char *sep, uint32_t seplen);
static uint32_t join_inner(KGame *g, uint32_t a, const char *sep, uint32_t seplen);
static uint32_t to_str(KGame *g, Val v) {
    if (v.t == T_STR) return v.u.r;
    if (v.t == T_ARR) return join_arr(g, v.u.r, ",", 1);
    char buf[24];
    uint32_t n = prim_text(g, v, buf);
    return new_str(g, buf, n);
}

/* ---------- framebuffer ---------- */
const uint32_t kgame_palette_rgb[16] = {
    0x000000, 0x1d2b53, 0x7e2553, 0x008751, 0xab5236, 0x5f574f, 0xc2c3c7, 0xfff1e8,
    0xff004d, 0xffa300, 0xffec27, 0x00e436, 0x29adff, 0x83769c, 0xff77a8, 0xffccaa,
};
#define RGB565(c) ((uint16_t)((((c) >> 19) & 0x1f) << 11 | (((c) >> 10) & 0x3f) << 5 | (((c) >> 3) & 0x1f)))
const uint16_t kgame_palette565[16] = {
    RGB565(0x000000), RGB565(0x1d2b53), RGB565(0x7e2553), RGB565(0x008751),
    RGB565(0xab5236), RGB565(0x5f574f), RGB565(0xc2c3c7), RGB565(0xfff1e8),
    RGB565(0xff004d), RGB565(0xffa300), RGB565(0xffec27), RGB565(0x00e436),
    RGB565(0x29adff), RGB565(0x83769c), RGB565(0xff77a8), RGB565(0xffccaa),
};
static void put(KGame *g, int x, int y, int c) {
    if ((unsigned)x >= KGAME_W || (unsigned)y >= KGAME_H) return;
    uint8_t *p = &g->fb[(y * KGAME_W + x) >> 1];
    c &= 15;
    if (x & 1) *p = (uint8_t)((*p & 0xf0) | c);
    else *p = (uint8_t)((*p & 0x0f) | (c << 4));
}
static int get(KGame *g, int x, int y) {
    if ((unsigned)x >= KGAME_W || (unsigned)y >= KGAME_H) return 0;
    uint8_t p = g->fb[(y * KGAME_W + x) >> 1];
    return (x & 1) ? (p & 15) : (p >> 4);
}
static void hspan(KGame *g, int x0, int x1, int y, int c) {
    if ((unsigned)y >= KGAME_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= KGAME_W) x1 = KGAME_W - 1;
    for (int x = x0; x <= x1; x++) put(g, x, y, c);
}
static void fill_rect(KGame *g, int x, int y, int w, int h, int c) {
    if (w <= 0 || h <= 0) return;
    int y0 = y < 0 ? 0 : y, y1 = y + h - 1 >= KGAME_H ? KGAME_H - 1 : y + h - 1;
    for (int yy = y0; yy <= y1; yy++) hspan(g, x, x + w - 1, yy, c);
}
static void draw_line(KGame *g, int x0, int y0, int x1, int y1, int c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int guard = 0; guard < 2048; guard++) {
        put(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void draw_circle(KGame *g, int cx, int cy, int r, int c, bool filled) {
    if (r < 0) return;
    if (r > 512) r = 512;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (filled) {
            hspan(g, cx - x, cx + x, cy + y, c); hspan(g, cx - x, cx + x, cy - y, c);
            hspan(g, cx - y, cx + y, cy + x, c); hspan(g, cx - y, cx + y, cy - x, c);
        } else {
            put(g, cx + x, cy + y, c); put(g, cx - x, cy + y, c); put(g, cx + x, cy - y, c); put(g, cx - x, cy - y, c);
            put(g, cx + y, cy + x, c); put(g, cx - y, cy + x, c); put(g, cx + y, cy - x, c); put(g, cx - y, cy - x, c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}
static int draw_text_g(KGame *g, const char *s, uint32_t n, int x, int y, int c, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int cx = x;
    for (uint32_t i = 0; i < n; i++) {
        unsigned ch = (uint8_t)s[i];
        if (ch == '\n') { cx = x; y += 8 * scale; continue; }
        if (ch >= 128) ch = '?';
        if (cx < KGAME_W && cx + 6 * scale > 0 && y < KGAME_H && y + 7 * scale > 0) {
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (korta_font[ch][row] & (1u << (4 - col))) fill_rect(g, cx + col * scale, y + row * scale, scale, scale, c);
        }
        cx += 6 * scale;
    }
    return cx - x - (n ? scale : 0);
}

/* ---------- lexer ---------- */
enum {
    TK_EOF = 0, TK_NUM = 256, TK_STR, TK_TPL, TK_ID,
    TK_LET, TK_CONST, TK_VAR, TK_FUNCTION, TK_RETURN, TK_IF, TK_ELSE, TK_FOR, TK_WHILE, TK_DO,
    TK_BREAK, TK_CONTINUE, TK_TRUE, TK_FALSE, TK_NULL, TK_UNDEFINED, TK_TYPEOF, TK_NEW, TK_SWITCH,
    TK_CASE, TK_DEFAULT,
    TK_EQ3, TK_NE3, TK_EQ2, TK_NE2, TK_LE, TK_GE, TK_AND, TK_OR, TK_INC, TK_DEC,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ, TK_ANDEQ, TK_OREQ, TK_XOREQ, TK_SHLEQ, TK_SHREQ,
    TK_ARROW, TK_SHL, TK_SHR, TK_USHR, TK_POW, TK_NULLISH,
};
static const struct { const char *w; int k; } KEYWORDS[] = {
    {"let", TK_LET}, {"const", TK_CONST}, {"var", TK_VAR}, {"function", TK_FUNCTION}, {"return", TK_RETURN},
    {"if", TK_IF}, {"else", TK_ELSE}, {"for", TK_FOR}, {"while", TK_WHILE}, {"do", TK_DO}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"true", TK_TRUE}, {"false", TK_FALSE}, {"null", TK_NULL},
    {"undefined", TK_UNDEFINED}, {"typeof", TK_TYPEOF}, {"new", TK_NEW}, {"switch", TK_SWITCH},
    {"case", TK_CASE}, {"default", TK_DEFAULT},
};
typedef struct { int kind; uint32_t start, len; float num; uint16_t line; bool nl; } Tok;
typedef struct Loop Loop;
typedef struct FS FS;
typedef struct {
    KGame *g;
    const char *src;
    uint32_t len, pos;
    uint16_t line;
    Tok tok;
    FS *fs;
    int depth;
} P;

static bool is_id0(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static bool is_dig(char c) { return c >= '0' && c <= '9'; }

static void perr(P *p, const char *msg) {
    if (p->g->failed) return;
    char m[100];
    uint32_t n = 0;
    const char *pre = "line ";
    for (uint32_t i = 0; pre[i]; i++) m[n++] = pre[i];
    char num[8]; int k = 0; uint32_t l = p->tok.line ? p->tok.line : p->line;
    do { num[k++] = (char)('0' + l % 10); l /= 10; } while (l && k < 7);
    while (k) m[n++] = num[--k];
    m[n++] = ':'; m[n++] = ' ';
    for (uint32_t i = 0; msg[i] && n < 98; i++) m[n++] = msg[i];
    m[n] = '\0';
    fail(p->g, m);
}

/* Skips whitespace and comments; returns whether a newline was crossed. */
static bool skip_space(P *p) {
    bool nl = false;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '\n') { nl = true; p->line++; p->pos++; }
        else if (c == ' ' || c == '\t' || c == '\r') p->pos++;
        else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) {
                if (p->src[p->pos] == '\n') { nl = true; p->line++; }
                p->pos++;
            }
            p->pos = p->pos + 2 <= p->len ? p->pos + 2 : p->len;
        } else break;
    }
    return nl;
}
static void next(P *p) {
    Tok t = {TK_EOF, 0, 0, 0, 0, false};
    if (p->g->failed) { p->tok = t; return; }
    t.nl = skip_space(p);
    t.line = p->line;
    t.start = p->pos;
    if (p->pos >= p->len) { p->tok = t; return; }
    const char *s = p->src;
    char c = s[p->pos];
    if (is_id0(c)) {
        uint32_t b = p->pos;
        while (p->pos < p->len && (is_id0(s[p->pos]) || is_dig(s[p->pos]))) p->pos++;
        t.kind = TK_ID;
        t.len = p->pos - b;
        for (unsigned i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++)
            if (slen(KEYWORDS[i].w) == t.len && mcmp(KEYWORDS[i].w, s + b, t.len) == 0) t.kind = KEYWORDS[i].k;
        p->tok = t;
        return;
    }
    if (is_dig(c) || (c == '.' && p->pos + 1 < p->len && is_dig(s[p->pos + 1]))) {
        uint32_t b = p->pos;
        if (c == '0' && p->pos + 1 < p->len && (s[p->pos + 1] == 'x' || s[p->pos + 1] == 'X')) {
            p->pos += 2;
            while (p->pos < p->len && (is_dig(s[p->pos]) || ((s[p->pos] | 32) >= 'a' && (s[p->pos] | 32) <= 'f'))) p->pos++;
        } else {
            while (p->pos < p->len && (is_dig(s[p->pos]) || s[p->pos] == '.' || s[p->pos] == '_')) p->pos++;
            if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
                p->pos++;
                if (p->pos < p->len && (s[p->pos] == '-' || s[p->pos] == '+')) p->pos++;
                while (p->pos < p->len && is_dig(s[p->pos])) p->pos++;
            }
        }
        char buf[40]; uint32_t n = 0;
        for (uint32_t i = b; i < p->pos && n < sizeof buf; i++) if (s[i] != '_') buf[n++] = s[i];
        bool ok;
        t.num = parse_num(buf, n, &ok);
        if (!ok) { p->tok = t; perr(p, "bad number"); return; }
        t.kind = TK_NUM;
        t.len = p->pos - b;
        p->tok = t;
        return;
    }
    if (c == '"' || c == '\'') {
        uint32_t b = ++p->pos;
        while (p->pos < p->len && s[p->pos] != c) {
            if (s[p->pos] == '\\') p->pos++;
            else if (s[p->pos] == '\n') break;
            p->pos++;
        }
        if (p->pos >= p->len || s[p->pos] != c) { p->tok = t; perr(p, "unterminated string"); return; }
        t.kind = TK_STR; t.start = b; t.len = p->pos - b;
        p->pos++;
        p->tok = t;
        return;
    }
    if (c == '`') {
        uint32_t b = ++p->pos;
        int depth = 0;
        while (p->pos < p->len) {
            char d = s[p->pos];
            if (d == '\\') { p->pos += 2; continue; }
            if (d == '\n') p->line++;
            if (depth == 0 && d == '`') break;
            if (d == '$' && p->pos + 1 < p->len && s[p->pos + 1] == '{') { depth++; p->pos += 2; continue; }
            if (depth && d == '{') depth++;
            if (depth && d == '}') depth--;
            p->pos++;
        }
        if (p->pos >= p->len) { p->tok = t; perr(p, "unterminated template"); return; }
        t.kind = TK_TPL; t.start = b; t.len = p->pos - b;
        p->pos++;
        p->tok = t;
        return;
    }
    static const struct { const char *op; int k; } OPS[] = {
        {">>>", TK_USHR}, {"===", TK_EQ3}, {"!==", TK_NE3}, {"<<=", TK_SHLEQ}, {">>=", TK_SHREQ},
        {"==", TK_EQ2}, {"!=", TK_NE2}, {"<=", TK_LE}, {">=", TK_GE}, {"&&", TK_AND}, {"||", TK_OR},
        {"++", TK_INC}, {"--", TK_DEC}, {"+=", TK_ADDEQ}, {"-=", TK_SUBEQ}, {"*=", TK_MULEQ}, {"/=", TK_DIVEQ},
        {"%=", TK_MODEQ}, {"&=", TK_ANDEQ}, {"|=", TK_OREQ}, {"^=", TK_XOREQ}, {"=>", TK_ARROW},
        {"<<", TK_SHL}, {">>", TK_SHR}, {"**", TK_POW}, {"??", TK_NULLISH},
    };
    for (unsigned i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        uint32_t n = slen(OPS[i].op);
        if (p->pos + n <= p->len && mcmp(OPS[i].op, s + p->pos, n) == 0) {
            t.kind = OPS[i].k; t.len = n; p->pos += n; p->tok = t; return;
        }
    }
    if (c == '?' || c == ':' || c == ';' || c == ',' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '<' || c == '>'
        || c == '=' || c == '!' || c == '&' || c == '|' || c == '^' || c == '~') {
        t.kind = c; t.len = 1; p->pos++; p->tok = t; return;
    }
    p->tok = t;
    perr(p, "unexpected character");
}
static bool accept(P *p, int k) { if (p->tok.kind == k) { next(p); return true; } return false; }
static void expect(P *p, int k, const char *what) {
    if (p->tok.kind == k) next(p);
    else {
        char m[48]; uint32_t n = 0;
        const char *a = "expected ";
        for (uint32_t i = 0; a[i]; i++) m[n++] = a[i];
        for (uint32_t i = 0; what[i] && n < 46; i++) m[n++] = what[i];
        m[n] = '\0';
        perr(p, m);
    }
}

/* ---------- code emission ---------- */
enum {
    OP_CONST, OP_UNDEF, OP_NULL, OP_TRUE, OP_FALSE, OP_POP, OP_DUP, OP_DUP2,
    OP_GETL, OP_SETL, OP_GETE, OP_SETE, OP_GETG, OP_SETG, OP_GETP, OP_SETP, OP_GETI, OP_SETI,
    OP_ARR, OP_OBJ, OP_CLOSURE, OP_CALL, OP_CALLM, OP_RET, OP_JMP, OP_JF, OP_JT, OP_JFK, OP_JTK, OP_JNNK,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG, OP_PLUS, OP_NOT, OP_BNOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_USHR, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_TYPEOF,
    OP_ITER, OP_LEN, OP_GETGS,
};

static void emit(P *p, uint8_t b) {
    KGame *g = p->g;
    if (g->failed) return;
    if (g->code_len >= g->code_cap) { perr(p, "game too large"); return; }
    if (g->nlines == 0 || g->lines[g->nlines - 1].line != p->tok.line) {
        if (g->nlines < LINES_MAX) { g->lines[g->nlines].pc = (uint16_t)g->code_len; g->lines[g->nlines].line = p->tok.line; g->nlines++; }
    }
    g->code[g->code_len++] = b;
}
static void emit16(P *p, uint16_t v) { emit(p, (uint8_t)v); emit(p, (uint8_t)(v >> 8)); }
static void emit_op16(P *p, uint8_t op, uint16_t v) { emit(p, op); emit16(p, v); }
static uint32_t here(P *p) { return p->g->code_len; }
static uint32_t emit_jump(P *p, uint8_t op) { emit(p, op); uint32_t at = here(p); emit16(p, 0); return at; }
static void patch(P *p, uint32_t at, uint32_t target) {
    if (p->g->failed) return;
    if (target > 0xffff) { perr(p, "game too large"); return; }
    p->g->code[at] = (uint8_t)target;
    p->g->code[at + 1] = (uint8_t)(target >> 8);
}
static void emit_jump_to(P *p, uint8_t op, uint32_t target) { emit(p, op); emit16(p, (uint16_t)target); }

static int add_const(P *p, Val v) {
    KGame *g = p->g;
    for (int i = 0; i < g->nconsts; i++) {
        Val c = g->consts[i];
        if (c.t != v.t) continue;
        if (v.t == T_NUM && c.u.n == v.u.n && !kisnan(v.u.n)) return i;
        if (v.t == T_STR && str_eq(g, c.u.r, v.u.r)) return i;
    }
    if (g->nconsts >= CONST_MAX) { perr(p, "too many constants"); return 0; }
    g->consts[g->nconsts] = v;
    return g->nconsts++;
}
static int const_str(P *p, const char *s, uint32_t n) {
    KGame *g = p->g;
    for (int i = 0; i < g->nconsts; i++) {
        Val c = g->consts[i];
        if (c.t == T_STR && STR(g, c.u.r)->len == n && mcmp(STR(g, c.u.r)->s, s, n) == 0) return i;
    }
    g->perm_mode = true;
    uint32_t r = new_str(g, s, n);
    g->perm_mode = false;
    if (!r) { perr(p, "out of memory"); return 0; }
    return add_const(p, vref(T_STR, r));
}
static int global_index(KGame *g, const char *s, uint32_t n) {
    for (int i = 0; i < g->nglobals; i++) {
        Str *x = STR(g, g->gname[i]);
        if (x->len == n && mcmp(x->s, s, n) == 0) return i;
    }
    if (g->nglobals >= GLOBAL_MAX) { fail(g, "too many global names"); return 0; }
    bool was = g->perm_mode;
    g->perm_mode = true;
    uint32_t r = new_str(g, s, n);
    g->perm_mode = was;
    if (!r) return 0;
    g->gname[g->nglobals] = r;
    g->gval[g->nglobals] = vundef();
    g->gdef[g->nglobals] = 0;
    return g->nglobals++;
}

/* ---------- scopes ---------- */
#define VARS_MAX 96
typedef struct { uint8_t kind; uint8_t a, b; uint16_t idx; } Ref;
enum { R_VAL, R_LOCAL, R_ENV, R_GLOBAL, R_MEMBER, R_INDEX };
struct Loop {
    Loop *up;
    bool is_switch;
    uint32_t brk[24]; int nbrk;
    uint32_t cont[24]; int ncont;
};
struct FS {
    FS *up;
    int proto;
    bool env, top;
    int nlocals;
    struct { uint32_t start, len; uint8_t slot; } vars[VARS_MAX];
    int nvars;
    Loop *loop;
    int stmt_depth;                       /* 1 for statements directly in the body */
    struct { uint16_t proto; Ref ref; } hoisted[32];
    int nhoisted;
};

static bool same_name(P *p, uint32_t s1, uint32_t l1, uint32_t s2, uint32_t l2) {
    return l1 == l2 && mcmp(p->src + s1, p->src + s2, l1) == 0;
}
static int declare_local(P *p, uint32_t start, uint32_t len) {
    FS *fs = p->fs;
    if (fs->nvars >= VARS_MAX || fs->nlocals >= 250) { perr(p, "too many variables in one function"); return 0; }
    int slot = fs->nlocals++;
    fs->vars[fs->nvars].start = start;
    fs->vars[fs->nvars].len = len;
    fs->vars[fs->nvars].slot = (uint8_t)slot;
    fs->nvars++;
    return slot;
}
static int hidden_local(P *p) { return declare_local(p, 0, 0); }
/* Locals of the current function are stack slots, or env slots when it has
 * closures. An enclosing function's locals are `depth` env hops away: the hop
 * count starts at 1 if this function made its own env, and grows by one per
 * enclosing function passed (each of those has an env, since it encloses us). */
static Ref resolve(P *p, uint32_t start, uint32_t len) {
    Ref r = {R_GLOBAL, 0, 0, 0};
    int depth = p->fs->env ? 1 : 0;
    for (FS *fs = p->fs; fs && !fs->top; fs = fs->up) {
        for (int i = fs->nvars - 1; i >= 0; i--) {
            if (!fs->vars[i].len || !same_name(p, fs->vars[i].start, fs->vars[i].len, start, len)) continue;
            if (fs == p->fs) {
                if (fs->env) { r.kind = R_ENV; r.a = 0; }
                else { r.kind = R_LOCAL; r.a = fs->vars[i].slot; return r; }
            } else { r.kind = R_ENV; r.a = (uint8_t)depth; }
            r.b = fs->vars[i].slot;
            return r;
        }
        if (fs != p->fs) depth++;
    }
    r.idx = (uint16_t)global_index(p->g, p->src + start, len);
    return r;
}
static void load(P *p, Ref r) {
    switch (r.kind) {
        case R_LOCAL: emit(p, OP_GETL); emit(p, r.a); break;
        case R_ENV: emit(p, OP_GETE); emit(p, r.a); emit(p, r.b); break;
        case R_GLOBAL: emit_op16(p, OP_GETG, r.idx); break;
        case R_MEMBER: emit_op16(p, OP_GETP, r.idx); break;
        case R_INDEX: emit(p, OP_GETI); break;
        default: break;
    }
}
static void store(P *p, Ref r) {
    switch (r.kind) {
        case R_LOCAL: emit(p, OP_SETL); emit(p, r.a); break;
        case R_ENV: emit(p, OP_SETE); emit(p, r.a); emit(p, r.b); break;
        case R_GLOBAL: emit_op16(p, OP_SETG, r.idx); break;
        case R_MEMBER: emit_op16(p, OP_SETP, r.idx); break;
        case R_INDEX: emit(p, OP_SETI); break;
        default: perr(p, "cannot assign to this"); break;
    }
}
/* The declaration target for a new name in the current function. */
static Ref declare(P *p, uint32_t start, uint32_t len) {
    Ref r = {R_GLOBAL, 0, 0, 0};
    if (p->fs->top) { r.idx = (uint16_t)global_index(p->g, p->src + start, len); return r; }
    int slot = declare_local(p, start, len);
    if (p->fs->env) { r.kind = R_ENV; r.a = 0; r.b = (uint8_t)slot; }
    else { r.kind = R_LOCAL; r.a = (uint8_t)slot; }
    return r;
}

/* ---------- raw source scanning (for arrows and closures) ---------- */
/* From an opening bracket at `pos`, the position just after its match. */
static uint32_t skip_balanced(P *p, uint32_t pos) {
    int depth = 0;
    const char *s = p->src;
    while (pos < p->len) {
        char c = s[pos];
        if (c == '"' || c == '\'' || c == '`') {
            char q = c; pos++;
            while (pos < p->len && s[pos] != q) { if (s[pos] == '\\') pos++; pos++; }
            pos++;
            continue;
        }
        if (c == '/' && pos + 1 < p->len && s[pos + 1] == '/') { while (pos < p->len && s[pos] != '\n') pos++; continue; }
        if (c == '/' && pos + 1 < p->len && s[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < p->len && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
            pos += 2;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') { depth--; if (depth == 0) return pos + 1; }
        pos++;
    }
    return p->len;
}
static bool arrow_after(P *p, uint32_t pos) {
    while (pos < p->len && (p->src[pos] == ' ' || p->src[pos] == '\t' || p->src[pos] == '\n' || p->src[pos] == '\r')) pos++;
    return pos + 1 < p->len && p->src[pos] == '=' && p->src[pos + 1] == '>';
}
/* Whether [from, to) contains a nested function (so locals must live in an env). */
static bool has_inner(P *p, uint32_t from, uint32_t to) {
    const char *s = p->src;
    for (uint32_t i = from; i + 1 < to; i++) {
        char c = s[i];
        if (c == '"' || c == '\'' || c == '`') {
            char q = c; i++;
            while (i < to && s[i] != q) { if (s[i] == '\\') i++; i++; }
            continue;
        }
        if (c == '/' && s[i + 1] == '/') { while (i < to && s[i] != '\n') i++; continue; }
        if (c == '=' && s[i + 1] == '>') return true;
        if (c == 'f' && i + 8 <= to && mcmp(s + i, "function", 8) == 0 && (i == 0 || !is_id0(s[i - 1]))
            && (i + 8 == to || !(is_id0(s[i + 8]) || is_dig(s[i + 8])))) return true;
    }
    return false;
}

/* ---------- expressions ---------- */
static void expr(P *p);
static Ref assign_expr(P *p);
static void statement(P *p);
static void emit_hoisted(P *p, uint32_t jump_in, uint32_t body);
static void block_body(P *p, bool function_body);
static int function_literal(P *p, uint32_t name_start, uint32_t name_len, bool arrow, bool arrow_single);

static void value(P *p) { load(p, assign_expr(p)); }
static void expr(P *p) { value(p); }

static void emit_string_lit(P *p, uint32_t start, uint32_t len) {
    char buf[512];
    uint32_t n = 0;
    const char *s = p->src + start;
    for (uint32_t i = 0; i < len; i++) {
        if (n >= sizeof buf - 4) { perr(p, "string literal too long"); return; }
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char e = s[++i];
            if (e == 'n') buf[n++] = '\n';
            else if (e == 't') buf[n++] = '\t';
            else if (e == 'r') buf[n++] = '\r';
            else if (e == '0') buf[n++] = '\0';
            else if (e == 'x' && i + 2 < len) {
                char hx[2] = {s[i + 1], s[i + 2]}; bool ok;
                char tmp[4] = {'0', 'x', hx[0], hx[1]};
                float v = parse_num(tmp, 4, &ok);
                buf[n++] = ok ? (char)(int)v : '?';
                i += 2;
            } else if (e == 'u') {
                /* \uXXXX: keep printable ASCII, else '?' (the font is ASCII) */
                uint32_t j = i + 1, v = 0, k = 0;
                if (j < len && s[j] == '{') { j++; while (j < len && s[j] != '}' && k < 6) { char d = s[j]; v = v * 16 + (uint32_t)(is_dig(d) ? d - '0' : ((d | 32) - 'a' + 10)); j++; k++; } }
                else { while (k < 4 && j < len) { char d = s[j]; v = v * 16 + (uint32_t)(is_dig(d) ? d - '0' : ((d | 32) - 'a' + 10)); j++; k++; } j--; }
                buf[n++] = v >= 32 && v < 127 ? (char)v : '?';
                i = j;
            } else if (e == '\n') { /* line continuation */ }
            else buf[n++] = e;
        } else buf[n++] = c;
    }
    emit_op16(p, OP_CONST, (uint16_t)const_str(p, buf, n));
}
/* `a ${b} c` compiles to "" + "a " + b + " c". */
static void template_lit(P *p, Tok t) {
    emit_op16(p, OP_CONST, (uint16_t)const_str(p, "", 0));
    uint32_t i = t.start, end = t.start + t.len, lit = i;
    while (i <= end && !p->g->failed) {
        bool at_end = i == end;
        bool hole = !at_end && p->src[i] == '$' && i + 1 < end && p->src[i + 1] == '{';
        if (!at_end && !hole) { if (p->src[i] == '\\') i++; i++; continue; }
        if (i > lit) { emit_string_lit(p, lit, i - lit); emit(p, OP_ADD); }
        if (at_end) break;
        /* Compile the hole with the lexer moved there, then come back. */
        uint32_t save_pos = p->pos; uint16_t save_line = p->line; Tok save_tok = p->tok;
        p->pos = i + 2;
        next(p);
        expr(p);
        if (p->tok.kind != '}') { perr(p, "expected } in template"); return; }
        uint32_t after = p->tok.start + 1;
        p->pos = save_pos; p->line = save_line; p->tok = save_tok;
        emit(p, OP_ADD);
        i = lit = after;
    }
}
static int args(P *p) {
    int n = 0;
    if (!accept(p, ')')) {
        do {
            if (p->tok.kind == ')') break;  /* trailing comma */
            value(p);
            n++;
            if (n > 32) { perr(p, "too many arguments"); return n; }
        } while (accept(p, ','));
        expect(p, ')', ")");
    }
    return n;
}
static Ref primary(P *p) {
    Ref r = {R_VAL, 0, 0, 0};
    Tok t = p->tok;
    if (++p->depth > 40 || stack_low(p->g)) { perr(p, "expression nested too deeply"); return r; }
    switch (t.kind) {
        case TK_NUM: next(p); emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(t.num))); break;
        case TK_STR: next(p); emit_string_lit(p, t.start, t.len); break;
        case TK_TPL: next(p); template_lit(p, t); break;
        case TK_TRUE: next(p); emit(p, OP_TRUE); break;
        case TK_FALSE: next(p); emit(p, OP_FALSE); break;
        case TK_NULL: next(p); emit(p, OP_NULL); break;
        case TK_UNDEFINED: next(p); emit(p, OP_UNDEF); break;
        case TK_FUNCTION: {
            next(p);
            uint32_t ns = 0, nl = 0;
            if (p->tok.kind == TK_ID) { ns = p->tok.start; nl = p->tok.len; next(p); }
            function_literal(p, ns, nl, false, false);
            break;
        }
        case TK_ID:
            if (arrow_after(p, p->pos)) { function_literal(p, 0, 0, true, true); break; }
            next(p);
            r = resolve(p, t.start, t.len);
            break;
        case TK_NEW: {
            next(p);
            if (p->tok.kind != TK_ID || p->tok.len != 5 || mcmp(p->src + p->tok.start, "Array", 5) != 0) { perr(p, "only new Array(n) is supported"); break; }
            r = resolve(p, p->tok.start, p->tok.len);
            next(p);
            load(p, r);
            r.kind = R_VAL;
            if (accept(p, '(')) { int n = args(p); emit(p, OP_CALL); emit(p, (uint8_t)n); }
            else { emit(p, OP_CALL); emit(p, 0); }
            break;
        }
        case '(':
            if (arrow_after(p, skip_balanced(p, t.start))) { function_literal(p, 0, 0, true, false); break; }
            next(p);
            expr(p);
            expect(p, ')', ")");
            break;
        case '[': {
            next(p);
            int n = 0;
            while (p->tok.kind != ']' && !p->g->failed) {
                value(p);
                if (++n > 1000) { perr(p, "array literal too long"); break; }
                if (!accept(p, ',')) break;
            }
            expect(p, ']', "]");
            emit_op16(p, OP_ARR, (uint16_t)n);
            break;
        }
        case '{': {
            next(p);
            int n = 0;
            while (p->tok.kind != '}' && !p->g->failed) {
                Tok k = p->tok;
                if (k.kind == TK_STR) { next(p); emit_string_lit(p, k.start, k.len); }
                else if (k.kind == TK_NUM) {
                    next(p);
                    char buf[24]; uint32_t m = fmt_num(k.num, buf);
                    emit_op16(p, OP_CONST, (uint16_t)const_str(p, buf, m));
                } else if (k.kind >= TK_ID && k.kind <= TK_DEFAULT) {
                    next(p);
                    emit_op16(p, OP_CONST, (uint16_t)const_str(p, p->src + k.start, k.len));
                    if (p->tok.kind != ':' && k.kind == TK_ID) {
                        /* {x} shorthand */
                        load(p, resolve(p, k.start, k.len));
                        n++;
                        if (!accept(p, ',')) break;
                        continue;
                    }
                } else { perr(p, "bad object key"); break; }
                expect(p, ':', ":");
                value(p);
                if (++n > 500) { perr(p, "object literal too long"); break; }
                if (!accept(p, ',')) break;
            }
            expect(p, '}', "}");
            emit_op16(p, OP_OBJ, (uint16_t)n);
            break;
        }
        default:
            if (t.kind == TK_EOF) perr(p, "unexpected end of game");
            else if (t.kind >= TK_LET && t.kind <= TK_DEFAULT) perr(p, "unexpected keyword");
            else perr(p, "unexpected symbol");
            break;
    }
    p->depth--;
    return r;
}
static Ref postfix(P *p) {
    Ref r = primary(p);
    for (;;) {
        if (p->g->failed) return r;
        int k = p->tok.kind;
        if (k == '.') {
            next(p);
            if (!(p->tok.kind >= TK_ID && p->tok.kind <= TK_DEFAULT)) { perr(p, "expected a name after ."); return r; }
            load(p, r);
            r.kind = R_MEMBER;
            r.idx = (uint16_t)const_str(p, p->src + p->tok.start, p->tok.len);
            next(p);
        } else if (k == '[') {
            next(p);
            load(p, r);
            expr(p);
            expect(p, ']', "]");
            r.kind = R_INDEX;
        } else if (k == '(') {
            next(p);
            if (r.kind == R_MEMBER) {
                int n = args(p);
                emit_op16(p, OP_CALLM, r.idx);
                emit(p, (uint8_t)n);
            } else {
                load(p, r);
                int n = args(p);
                emit(p, OP_CALL);
                emit(p, (uint8_t)n);
            }
            r.kind = R_VAL;
        } else if ((k == TK_INC || k == TK_DEC) && !p->tok.nl && r.kind != R_VAL) {
            next(p);
            /* x++ is (++x) - 1, which keeps the stack shape simple for every target. */
            if (r.kind == R_MEMBER) emit(p, OP_DUP);
            else if (r.kind == R_INDEX) emit(p, OP_DUP2);
            load(p, r);
            emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(1)));
            emit(p, k == TK_INC ? OP_ADD : OP_SUB);
            store(p, r);
            emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(1)));
            emit(p, k == TK_INC ? OP_SUB : OP_ADD);
            r.kind = R_VAL;
        } else return r;
    }
}
static Ref unary(P *p) {
    int k = p->tok.kind;
    Ref v = {R_VAL, 0, 0, 0};
    if (++p->depth > 40) { perr(p, "expression nested too deeply"); return v; }
    if (k == TK_TYPEOF) {
        next(p);
        Ref r = unary(p);
        /* typeof of an undeclared name is "undefined", not an error. */
        if (r.kind == R_GLOBAL) emit_op16(p, OP_GETGS, r.idx);
        else load(p, r);
        emit(p, OP_TYPEOF);
    } else if (k == '!' || k == '-' || k == '+' || k == '~') {
        next(p);
        load(p, unary(p));
        emit(p, k == '!' ? OP_NOT : k == '-' ? OP_NEG : k == '+' ? OP_PLUS : OP_BNOT);
    } else if (k == TK_INC || k == TK_DEC) {
        next(p);
        Ref r = unary(p);
        if (r.kind == R_VAL) { perr(p, "cannot increment this"); p->depth--; return v; }
        if (r.kind == R_MEMBER) emit(p, OP_DUP);
        else if (r.kind == R_INDEX) emit(p, OP_DUP2);
        load(p, r);
        emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(1)));
        emit(p, k == TK_INC ? OP_ADD : OP_SUB);
        store(p, r);
    } else { v = postfix(p); }
    p->depth--;
    return v;
}
static int binary_prec(int k) {
    switch (k) {
        case TK_OR: case TK_NULLISH: return 1;
        case TK_AND: return 2;
        case '|': return 3;
        case '^': return 4;
        case '&': return 5;
        case TK_EQ2: case TK_NE2: case TK_EQ3: case TK_NE3: return 6;
        case '<': case '>': case TK_LE: case TK_GE: return 7;
        case TK_SHL: case TK_SHR: case TK_USHR: return 8;
        case '+': case '-': return 9;
        case '*': case '/': case '%': return 10;
        case TK_POW: return 11;
        default: return 0;
    }
}
static uint8_t binary_op(int k) {
    switch (k) {
        case '|': return OP_BOR; case '^': return OP_BXOR; case '&': return OP_BAND;
        case TK_EQ2: case TK_EQ3: return OP_EQ; case TK_NE2: case TK_NE3: return OP_NE;
        case '<': return OP_LT; case '>': return OP_GT; case TK_LE: return OP_LE; case TK_GE: return OP_GE;
        case TK_SHL: return OP_SHL; case TK_SHR: return OP_SHR; case TK_USHR: return OP_USHR;
        case '+': return OP_ADD; case '-': return OP_SUB; case '*': return OP_MUL; case '/': return OP_DIV;
        case '%': return OP_MOD; case TK_POW: return OP_POW;
        default: return OP_ADD;
    }
}
/* Continues a binary expression whose left operand is already on the stack. */
static void binary_rest(P *p, int min) {
    for (;;) {
        int k = p->tok.kind, prec = binary_prec(k);
        if (!prec || prec < min || p->g->failed) return;
        next(p);
        if (k == TK_AND || k == TK_OR || k == TK_NULLISH) {
            uint32_t j = emit_jump(p, k == TK_AND ? OP_JFK : k == TK_OR ? OP_JTK : OP_JNNK);
            load(p, unary(p));
            binary_rest(p, prec + 1);
            patch(p, j, here(p));
            continue;
        }
        load(p, unary(p));
        binary_rest(p, k == TK_POW ? prec : prec + 1);
        emit(p, binary_op(k));
    }
}
static Ref conditional(P *p) {
    Ref r = unary(p);
    if (!binary_prec(p->tok.kind) && p->tok.kind != '?') return r;
    load(p, r);
    binary_rest(p, 1);
    if (accept(p, '?')) {
        uint32_t jf = emit_jump(p, OP_JF);
        value(p);
        uint32_t je = emit_jump(p, OP_JMP);
        expect(p, ':', ":");
        patch(p, jf, here(p));
        value(p);
        patch(p, je, here(p));
    }
    Ref v = {R_VAL, 0, 0, 0};
    return v;
}
static Ref assign_expr(P *p) {
    Ref r = conditional(p);
    int k = p->tok.kind;
    uint8_t op = 0;
    switch (k) {
        case '=': break;
        case TK_ADDEQ: op = OP_ADD; break; case TK_SUBEQ: op = OP_SUB; break;
        case TK_MULEQ: op = OP_MUL; break; case TK_DIVEQ: op = OP_DIV; break;
        case TK_MODEQ: op = OP_MOD; break; case TK_ANDEQ: op = OP_BAND; break;
        case TK_OREQ: op = OP_BOR; break; case TK_XOREQ: op = OP_BXOR; break;
        case TK_SHLEQ: op = OP_SHL; break; case TK_SHREQ: op = OP_SHR; break;
        default: return r;
    }
    if (r.kind == R_VAL) { perr(p, "cannot assign to this"); return r; }
    next(p);
    if (op) {
        if (r.kind == R_MEMBER) emit(p, OP_DUP);
        else if (r.kind == R_INDEX) emit(p, OP_DUP2);
        load(p, r);
        value(p);
        emit(p, op);
    } else value(p);
    store(p, r);
    Ref v = {R_VAL, 0, 0, 0};
    return v;
}

/* ---------- functions ---------- */
static int function_literal(P *p, uint32_t ns, uint32_t nl, bool arrow, bool single) {
    KGame *g = p->g;
    (void)ns; (void)nl;
    if (g->nprotos >= PROTO_MAX) { perr(p, "too many functions"); return 0; }
    if (stack_low(g)) { perr(p, "functions nested too deeply"); return 0; }
    uint32_t over = emit_jump(p, OP_JMP);
    int pi = g->nprotos++;
    FS fs;
    mset(&fs, 0, sizeof fs);
    fs.up = p->fs;
    fs.proto = pi;
    /* Parameters */
    uint32_t pstart[16], plen[16];
    int np = 0;
    if (single) { pstart[0] = p->tok.start; plen[0] = p->tok.len; np = 1; next(p); }
    else {
        expect(p, '(', "(");
        while (p->tok.kind == TK_ID && np < 16) {
            pstart[np] = p->tok.start; plen[np] = p->tok.len; np++;
            next(p);
            if (p->tok.kind == '=') { perr(p, "default parameters are not supported"); return 0; }
            if (!accept(p, ',')) break;
        }
        expect(p, ')', ")");
    }
    if (arrow) expect(p, TK_ARROW, "=>");
    bool block = p->tok.kind == '{';
    uint32_t body_start = p->tok.start;
    uint32_t body_end = block ? skip_balanced(p, body_start) : body_start;
    if (!block) {
        /* An expression body ends at a , ) ] } or ; outside brackets. */
        uint32_t i = body_start; int d = 0;
        while (i < p->len) {
            char c = p->src[i];
            if (c == '(' || c == '[' || c == '{') { i = skip_balanced(p, i); continue; }
            if (c == '"' || c == '\'' || c == '`') { char q = c; i++; while (i < p->len && p->src[i] != q) { if (p->src[i] == '\\') i++; i++; } i++; continue; }
            if (d == 0 && (c == ',' || c == ')' || c == ']' || c == '}' || c == ';')) break;
            i++;
        }
        body_end = i;
    }
    fs.env = has_inner(p, body_start, body_end);
    g->protos[pi].code = (uint16_t)here(p);
    g->protos[pi].params = (uint8_t)np;
    g->protos[pi].env = fs.env;
    FS *outer = p->fs;
    p->fs = &fs;
    for (int i = 0; i < np; i++) declare_local(p, pstart[i], plen[i]);
    if (block) {
        next(p);
        uint32_t jump_in = emit_jump(p, OP_JMP);
        uint32_t body = here(p);
        block_body(p, true);
        expect(p, '}', "}");
        emit(p, OP_UNDEF);
        emit(p, OP_RET);
        emit_hoisted(p, jump_in, body);
    } else {
        value(p);
        emit(p, OP_RET);
    }
    g->protos[pi].locals = (uint8_t)fs.nlocals;
    p->fs = outer;
    patch(p, over, here(p));
    emit_op16(p, OP_CLOSURE, (uint16_t)pi);
    return pi;
}

/* ---------- statements ---------- */
static void end_statement(P *p) {
    if (accept(p, ';')) return;
    if (p->tok.kind == '}' || p->tok.kind == TK_EOF || p->tok.nl) return;
    perr(p, "expected ;");
}
static void var_decl(P *p) {
    do {
        if (p->tok.kind != TK_ID) { perr(p, p->tok.kind == '{' || p->tok.kind == '[' ? "destructuring is not supported" : "expected a variable name"); return; }
        Tok name = p->tok;
        next(p);
        Ref r = declare(p, name.start, name.len);
        if (accept(p, '=')) value(p);
        else emit(p, OP_UNDEF);
        store(p, r);
        emit(p, OP_POP);
    } while (accept(p, ','));
}
static void loop_push(P *p, Loop *l, bool sw) { mset(l, 0, sizeof *l); l->up = p->fs->loop; l->is_switch = sw; p->fs->loop = l; }
static void loop_pop(P *p, Loop *l, uint32_t brk, uint32_t cont) {
    for (int i = 0; i < l->nbrk; i++) patch(p, l->brk[i], brk);
    for (int i = 0; i < l->ncont; i++) patch(p, l->cont[i], cont);
    p->fs->loop = l->up;
}
static void jump_out(P *p, bool brk) {
    Loop *l = p->fs->loop;
    /* break leaves the innermost loop or switch; continue the innermost loop.
     * Each switch crossed has its value on the stack. */
    while (l && !brk && l->is_switch) { emit(p, OP_POP); l = l->up; }
    if (!l) { perr(p, brk ? "break outside a loop" : "continue outside a loop"); return; }
    if (brk) { if (l->nbrk >= 24) { perr(p, "too many breaks"); return; } l->brk[l->nbrk++] = emit_jump(p, OP_JMP); }
    else { if (l->ncont >= 24) { perr(p, "too many continues"); return; } l->cont[l->ncont++] = emit_jump(p, OP_JMP); }
}
static void for_statement(P *p) {
    expect(p, '(', "(");
    int saved_vars = p->fs->nvars;
    /* for (const x of arr) */
    if ((p->tok.kind == TK_LET || p->tok.kind == TK_CONST || p->tok.kind == TK_VAR || p->tok.kind == TK_ID)) {
        uint32_t save_pos = p->pos; uint16_t save_line = p->line; Tok save = p->tok;
        bool decl = p->tok.kind != TK_ID;
        if (decl) next(p);
        if (p->tok.kind == TK_ID) {
            Tok name = p->tok;
            next(p);
            if (p->tok.kind == TK_ID && p->tok.len == 2 && mcmp(p->src + p->tok.start, "of", 2) == 0) {
                next(p);
                int arr = hidden_local(p), idx = hidden_local(p);
                Ref ra = {R_LOCAL, (uint8_t)arr, 0, 0}, ri = {R_LOCAL, (uint8_t)idx, 0, 0};
                if (p->fs->env) { ra.kind = ri.kind = R_ENV; ra.a = ri.a = 0; ra.b = (uint8_t)arr; ri.b = (uint8_t)idx; }
                value(p);
                emit(p, OP_ITER);  /* checks it is an array or string */
                store(p, ra); emit(p, OP_POP);
                emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(0)));
                store(p, ri); emit(p, OP_POP);
                expect(p, ')', ")");
                Ref rx = decl ? declare(p, name.start, name.len) : resolve(p, name.start, name.len);
                uint32_t top = here(p);
                load(p, ri); load(p, ra); emit(p, OP_LEN); emit(p, OP_LT);
                uint32_t exit = emit_jump(p, OP_JF);
                load(p, ra); load(p, ri); emit(p, OP_GETI); store(p, rx); emit(p, OP_POP);
                Loop l; loop_push(p, &l, false);
                statement(p);
                uint32_t cont = here(p);
                load(p, ri); emit_op16(p, OP_CONST, (uint16_t)add_const(p, vnum(1))); emit(p, OP_ADD); store(p, ri); emit(p, OP_POP);
                emit_jump_to(p, OP_JMP, top);
                patch(p, exit, here(p));
                loop_pop(p, &l, here(p), cont);
                p->fs->nvars = saved_vars;
                return;
            }
            if (p->tok.kind == TK_ID && p->tok.len == 2 && mcmp(p->src + p->tok.start, "in", 2) == 0) { perr(p, "for...in is not supported; use for...of"); return; }
        }
        p->pos = save_pos; p->line = save_line; p->tok = save;
    }
    /* for (init; cond; update) body */
    if (p->tok.kind == TK_LET || p->tok.kind == TK_CONST || p->tok.kind == TK_VAR) { next(p); var_decl(p); }
    else if (p->tok.kind != ';') { expr(p); emit(p, OP_POP); }
    expect(p, ';', ";");
    uint32_t cond = here(p);
    uint32_t exit = 0; bool has_cond = p->tok.kind != ';';
    if (has_cond) { expr(p); exit = emit_jump(p, OP_JF); }
    expect(p, ';', ";");
    uint32_t to_body = emit_jump(p, OP_JMP);
    uint32_t update = here(p);
    if (p->tok.kind != ')') { expr(p); emit(p, OP_POP); }
    emit_jump_to(p, OP_JMP, cond);
    expect(p, ')', ")");
    patch(p, to_body, here(p));
    Loop l; loop_push(p, &l, false);
    statement(p);
    emit_jump_to(p, OP_JMP, update);
    if (has_cond) patch(p, exit, here(p));
    loop_pop(p, &l, here(p), update);
    p->fs->nvars = saved_vars;
}
static void switch_statement(P *p) {
    expect(p, '(', "(");
    expr(p);
    expect(p, ')', ")");
    expect(p, '{', "{");
    Loop l; loop_push(p, &l, true);
    uint32_t next_test = emit_jump(p, OP_JMP);
    uint32_t fall = 0; bool has_fall = false;
    uint32_t default_body = 0; bool has_default = false;
    while (p->tok.kind != '}' && !p->g->failed) {
        bool is_default = p->tok.kind == TK_DEFAULT;
        if (!is_default && p->tok.kind != TK_CASE) { perr(p, "expected case"); break; }
        next(p);
        patch(p, next_test, here(p));
        uint32_t body;
        if (is_default) {
            expect(p, ':', ":");
            next_test = emit_jump(p, OP_JMP);   /* tests skip the default body */
            body = here(p);
            default_body = body; has_default = true;
        } else {
            emit(p, OP_DUP);
            expr(p);
            expect(p, ':', ":");
            emit(p, OP_EQ);
            next_test = emit_jump(p, OP_JF);
            body = here(p);
        }
        if (has_fall) patch(p, fall, body);
        while (p->tok.kind != TK_CASE && p->tok.kind != TK_DEFAULT && p->tok.kind != '}' && !p->g->failed) statement(p);
        fall = emit_jump(p, OP_JMP);
        has_fall = true;
    }
    expect(p, '}', "}");
    /* No case matched: the default body, or out. */
    patch(p, next_test, here(p));
    if (has_default) emit_jump_to(p, OP_JMP, default_body);
    uint32_t end = here(p);
    if (has_fall) patch(p, fall, end);
    loop_pop(p, &l, end, end);
    emit(p, OP_POP);
}
static void statement(P *p) {
    KGame *g = p->g;
    if (g->failed) return;
    if (++p->depth > 40 || stack_low(g)) { perr(p, "blocks nested too deeply"); return; }
    p->fs->stmt_depth++;
    Tok t = p->tok;
    switch (t.kind) {
        case ';': next(p); break;
        case '{': {
            next(p);
            int saved = p->fs->nvars;
            block_body(p, false);
            expect(p, '}', "}");
            p->fs->nvars = saved;
            break;
        }
        case TK_LET: case TK_CONST: case TK_VAR: next(p); var_decl(p); end_statement(p); break;
        case TK_FUNCTION: {
            next(p);
            if (p->tok.kind != TK_ID) { perr(p, "expected a function name"); break; }
            Tok name = p->tok;
            next(p);
            /* Names in a body were declared up front by hoist(); find the slot again. */
            Ref r = resolve(p, name.start, name.len);
            if (p->fs->stmt_depth == 1 && p->fs->nhoisted < 32) {
                /* Only the body is emitted here; the closure is made before any statement runs. */
                int pi = function_literal(p, name.start, name.len, false, false);
                if (!p->g->failed) {
                    p->g->code_len -= 3;  /* drop the OP_CLOSURE just emitted */
                    p->fs->hoisted[p->fs->nhoisted].proto = (uint16_t)pi;
                    p->fs->hoisted[p->fs->nhoisted].ref = r;
                    p->fs->nhoisted++;
                }
                break;
            }
            function_literal(p, name.start, name.len, false, false);
            store(p, r);
            emit(p, OP_POP);
            break;
        }
        case TK_IF: {
            next(p);
            expect(p, '(', "(");
            expr(p);
            expect(p, ')', ")");
            uint32_t jf = emit_jump(p, OP_JF);
            statement(p);
            if (accept(p, TK_ELSE)) {
                uint32_t je = emit_jump(p, OP_JMP);
                patch(p, jf, here(p));
                statement(p);
                patch(p, je, here(p));
            } else patch(p, jf, here(p));
            break;
        }
        case TK_WHILE: {
            next(p);
            uint32_t top = here(p);
            expect(p, '(', "(");
            expr(p);
            expect(p, ')', ")");
            uint32_t exit = emit_jump(p, OP_JF);
            Loop l; loop_push(p, &l, false);
            statement(p);
            emit_jump_to(p, OP_JMP, top);
            patch(p, exit, here(p));
            loop_pop(p, &l, here(p), top);
            break;
        }
        case TK_DO: {
            next(p);
            uint32_t top = here(p);
            Loop l; loop_push(p, &l, false);
            statement(p);
            uint32_t cont = here(p);
            expect(p, TK_WHILE, "while");
            expect(p, '(', "(");
            expr(p);
            expect(p, ')', ")");
            emit_jump_to(p, OP_JT, top);
            loop_pop(p, &l, here(p), cont);
            accept(p, ';');
            break;
        }
        case TK_FOR: next(p); for_statement(p); break;
        case TK_SWITCH: next(p); switch_statement(p); break;
        case TK_BREAK: next(p); jump_out(p, true); end_statement(p); break;
        case TK_CONTINUE: next(p); jump_out(p, false); end_statement(p); break;
        case TK_RETURN:
            next(p);
            if (p->fs->top) { perr(p, "return outside a function"); break; }
            if (p->tok.kind == ';' || p->tok.kind == '}' || p->tok.nl || p->tok.kind == TK_EOF) emit(p, OP_UNDEF);
            else expr(p);
            emit(p, OP_RET);
            end_statement(p);
            break;
        default:
            if (t.kind == TK_ID && t.len == 5 && mcmp(p->src + t.start, "class", 5) == 0) { perr(p, "classes are not supported; use objects and functions"); break; }
            if (t.kind == TK_ID && t.len == 3 && mcmp(p->src + t.start, "try", 3) == 0) { perr(p, "try/catch is not supported"); break; }
            expr(p);
            emit(p, OP_POP);
            end_statement(p);
            break;
    }
    p->fs->stmt_depth--;
    p->depth--;
}
/* The section a body jumps to first: it makes the body's declared functions,
 * then jumps back to the first statement. */
static void emit_hoisted(P *p, uint32_t jump_in, uint32_t body) {
    patch(p, jump_in, here(p));
    for (int i = 0; i < p->fs->nhoisted; i++) {
        emit_op16(p, OP_CLOSURE, p->fs->hoisted[i].proto);
        store(p, p->fs->hoisted[i].ref);
        emit(p, OP_POP);
    }
    emit_jump_to(p, OP_JMP, body);
}
/* Function declarations at the top of a body are hoisted: their names are
 * declared before any statement, so earlier code can call them. */
static void hoist(P *p) {
    const char *s = p->src;
    uint32_t i = p->tok.start;
    int depth = 0;
    while (i < p->len && !p->g->failed) {
        char c = s[i];
        if (c == '"' || c == '\'' || c == '`') { char q = c; i++; while (i < p->len && s[i] != q) { if (s[i] == '\\') i++; i++; } i++; continue; }
        if (c == '/' && i + 1 < p->len && s[i + 1] == '/') { while (i < p->len && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < p->len && s[i + 1] == '*') { i += 2; while (i + 1 < p->len && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; continue; }
        if (c == '{' || c == '(' || c == '[') depth++;
        else if (c == '}' || c == ')' || c == ']') { if (depth == 0) break; depth--; }
        else if (depth == 0 && c == 'f' && i + 8 < p->len && mcmp(s + i, "function", 8) == 0 && (i == 0 || !(is_id0(s[i - 1]) || is_dig(s[i - 1]) || s[i - 1] == '.'))) {
            uint32_t j = i + 8;
            if (j < p->len && (is_id0(s[j]) || is_dig(s[j]))) { i++; continue; }
            while (j < p->len && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            uint32_t b = j;
            while (j < p->len && (is_id0(s[j]) || is_dig(s[j]))) j++;
            if (j > b && !p->fs->top) {
                bool known = false;
                for (int v = 0; v < p->fs->nvars; v++) if (same_name(p, p->fs->vars[v].start, p->fs->vars[v].len, b, j - b)) known = true;
                if (!known) declare_local(p, b, j - b);
            }
            i = j;
            continue;
        }
        i++;
    }
}
static void block_body(P *p, bool function_body) {
    if (function_body) hoist(p);
    while (p->tok.kind != '}' && p->tok.kind != TK_EOF && !p->g->failed) statement(p);
}

/* ---------- natives ---------- */
typedef int (*Native)(KGame *g, Val *self, Val *a, int n, Val *out);
static float argn(KGame *g, Val *a, int n, int i, float dflt) { return i < n && a[i].t != T_UNDEF ? to_num(g, a[i]) : dflt; }
static int argi(KGame *g, Val *a, int n, int i, int dflt) {
    float f = argn(g, a, n, i, (float)dflt);
    if (kisnan(f)) return dflt;
    f = kfloor(f);
    return f > 100000 ? 100000 : f < -100000 ? -100000 : (int)f;
}

static int n_cls(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)o; int c = argi(g, a, n, 0, 0) & 15; mset(g->fb, (uint8_t)(c << 4 | c), KGAME_FB_BYTES); return 0; }
static int n_pset(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)o; put(g, argi(g, a, n, 0, 0), argi(g, a, n, 1, 0), argi(g, a, n, 2, 7)); return 0; }
static int n_pget(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum((float)get(g, argi(g, a, n, 0, 0), argi(g, a, n, 1, 0))); return 0; }
static int n_rect(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)o; fill_rect(g, argi(g, a, n, 0, 0), argi(g, a, n, 1, 0), argi(g, a, n, 2, 0), argi(g, a, n, 3, 0), argi(g, a, n, 4, 7)); return 0; }
static int n_rectb(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; (void)o;
    int x = argi(g, a, n, 0, 0), y = argi(g, a, n, 1, 0), w = argi(g, a, n, 2, 0), h = argi(g, a, n, 3, 0), c = argi(g, a, n, 4, 7);
    if (w <= 0 || h <= 0) return 0;
    fill_rect(g, x, y, w, 1, c); fill_rect(g, x, y + h - 1, w, 1, c);
    fill_rect(g, x, y, 1, h, c); fill_rect(g, x + w - 1, y, 1, h, c);
    return 0;
}
static int n_circ(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)o; draw_circle(g, argi(g, a, n, 0, 0), argi(g, a, n, 1, 0), argi(g, a, n, 2, 1), argi(g, a, n, 3, 7), true); return 0; }
static int n_circb(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)o; draw_circle(g, argi(g, a, n, 0, 0), argi(g, a, n, 1, 0), argi(g, a, n, 2, 1), argi(g, a, n, 3, 7), false); return 0; }
static int n_line(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; (void)o;
    int x0 = argi(g, a, n, 0, 0), y0 = argi(g, a, n, 1, 0), x1 = argi(g, a, n, 2, 0), y1 = argi(g, a, n, 3, 0);
    /* Clip to a generous box so the step guard always covers the line. */
    if (x0 < -500 || x0 > 700 || x1 < -500 || x1 > 700 || y0 < -500 || y0 > 700 || y1 < -500 || y1 > 700) return 0;
    draw_line(g, x0, y0, x1, y1, argi(g, a, n, 4, 7));
    return 0;
}
static int n_text(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s;
    if (n < 1) return 0;
    uint32_t r = to_str(g, a[0]);
    if (!r) return -1;
    Str *st = STR(g, r);
    *o = vnum((float)draw_text_g(g, st->s, st->len, argi(g, a, n, 1, 0), argi(g, a, n, 2, 0), argi(g, a, n, 3, 7), argi(g, a, n, 4, 1)));
    return 0;
}
static int button_bit(KGame *g, Val v) {
    if (v.t == T_NUM) { int i = (int)v.u.n; return i >= 0 && i < 6 ? 1 << i : 0; }
    if (v.t != T_STR) return 0;
    Str *s = STR(g, v.u.r);
    static const char *const names[] = {"left", "right", "up", "down", "a", "b"};
    for (int i = 0; i < 6; i++) {
        uint32_t l = slen(names[i]);
        if (s->len != l) continue;
        bool same = true;
        for (uint32_t k = 0; k < l; k++) if ((s->s[k] | 32) != names[i][k]) same = false;
        if (same) return 1 << i;
    }
    return 0;
}
static int n_btn(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vbool(n > 0 && (g->held & button_bit(g, a[0]))); return 0; }
static int n_btnp(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vbool(n > 0 && (g->pressed & button_bit(g, a[0]))); return 0; }
static float rnd01(KGame *g) {
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x ? x : 0x9e3779b9u;
    return (float)(x >> 8) / 16777216.0f;
}
static int n_rnd(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(rnd01(g) * argn(g, a, n, 0, 1)); return 0; }
static int n_time(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)a; (void)n; *o = vnum((float)g->frame_no / KGAME_FPS); return 0; }
static int n_save(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; (void)o;
    float v = argn(g, a, n, 0, 0);
    if (kisnan(v)) v = 0;
    if (v != g->saved) { g->saved = v; g->saved_dirty = true; }
    return 0;
}
static int n_load(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)a; (void)n; *o = vnum(g->saved); return 0; }
static void log_text(KGame *g, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n && g->log_len + 1 < LOG_MAX; i++) g->log[g->log_len++] = s[i];
    g->log[g->log_len] = '\0';
}
static int n_log(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; (void)o;
    for (int i = 0; i < n; i++) {
        if (i) log_text(g, " ", 1);
        uint32_t r = to_str(g, a[i]);
        if (!r) return -1;
        log_text(g, STR(g, r)->s, STR(g, r)->len);
    }
    log_text(g, "\n", 1);
    return 0;
}
static int n_floor(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kfloor(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_ceil(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kceil(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_round(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kfloor(argn(g, a, n, 0, 0.0f / 0.0f) + 0.5f)); return 0; }
static int n_trunc(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(ktrunc(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_abs(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kabs(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_sign(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; float x = argn(g, a, n, 0, 0.0f / 0.0f); *o = vnum(x > 0 ? 1.0f : x < 0 ? -1.0f : x); return 0; }
static int n_sqrt(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(ksqrt(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_sin(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(ksin(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_cos(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kcos(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_tan(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; float x = argn(g, a, n, 0, 0.0f / 0.0f); *o = vnum(ksin(x) / kcos(x)); return 0; }
static int n_atan(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(katan(argn(g, a, n, 0, 0.0f / 0.0f))); return 0; }
static int n_atan2(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(katan2(argn(g, a, n, 0, 0), argn(g, a, n, 1, 0))); return 0; }
static int n_pow(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(kpow(argn(g, a, n, 0, 0.0f / 0.0f), argn(g, a, n, 1, 0.0f / 0.0f))); return 0; }
static int n_hypot(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; float x = argn(g, a, n, 0, 0), y = argn(g, a, n, 1, 0); *o = vnum(ksqrt(x * x + y * y)); return 0; }
static int n_random(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; (void)a; (void)n; *o = vnum(rnd01(g)); return 0; }
static int n_min(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; float m = 1.0f / 0.0f;
    for (int i = 0; i < n; i++) { float x = to_num(g, a[i]); if (kisnan(x)) { m = x; break; } if (x < m) m = x; }
    *o = vnum(m); return 0;
}
static int n_max(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s; float m = -1.0f / 0.0f;
    for (int i = 0; i < n; i++) { float x = to_num(g, a[i]); if (kisnan(x)) { m = x; break; } if (x > m) m = x; }
    *o = vnum(m); return 0;
}
static int n_string(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; uint32_t r = n ? to_str(g, a[0]) : new_str(g, "", 0); if (!r) return -1; *o = vref(T_STR, r); return 0; }
static int n_number(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vnum(n ? to_num(g, a[0]) : 0); return 0; }
static int n_parseint(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s;
    if (!n) { *o = vnum(0.0f / 0.0f); return 0; }
    if (a[0].t == T_NUM) { *o = vnum(ktrunc(a[0].u.n)); return 0; }
    uint32_t r = to_str(g, a[0]); if (!r) return -1;
    Str *st = STR(g, r);
    uint32_t i = 0; bool neg = false; float v = 0; bool any = false;
    while (i < st->len && st->s[i] == ' ') i++;
    if (i < st->len && (st->s[i] == '-' || st->s[i] == '+')) { neg = st->s[i] == '-'; i++; }
    while (i < st->len && is_dig(st->s[i])) { v = v * 10 + (float)(st->s[i] - '0'); i++; any = true; }
    *o = vnum(any ? (neg ? -v : v) : 0.0f / 0.0f);
    return 0;
}
static int n_isnan(KGame *g, Val *s, Val *a, int n, Val *o) { (void)s; *o = vbool(kisnan(n ? to_num(g, a[0]) : 0.0f / 0.0f)); return 0; }
static int n_array(KGame *g, Val *s, Val *a, int n, Val *o) {
    (void)s;
    int len = n == 1 && a[0].t == T_NUM ? argi(g, a, n, 0, 0) : 0;
    if (len < 0 || len > ARR_MAX) return fail(g, "bad array length");
    uint32_t r = new_arr(g, 0);
    if (!r) return -1;
    *o = vref(T_ARR, r);  /* rooted through *o (a stack slot) */
    if (len && !grow(g, r, (uint32_t)len, sizeof(Val))) return -1;
    Val *it = arr_items(g, r);
    for (int i = 0; i < len; i++) it[i] = vundef();
    ARR(g, r)->len = (uint32_t)len;
    if (n > 1) for (int i = 0; i < n; i++) if (!arr_push(g, r, a[i])) return -1;
    return 0;
}

typedef struct { const char *name; Native fn; } NativeDef;
static const NativeDef GLOBAL_NATIVES[] = {
    {"cls", n_cls}, {"pset", n_pset}, {"pget", n_pget}, {"rect", n_rect}, {"rectb", n_rectb},
    {"circ", n_circ}, {"circb", n_circb}, {"line", n_line}, {"text", n_text}, {"btn", n_btn},
    {"btnp", n_btnp}, {"rnd", n_rnd}, {"time", n_time}, {"save", n_save}, {"load", n_load},
    {"String", n_string}, {"Number", n_number}, {"parseInt", n_parseint}, {"parseFloat", n_number},
    {"isNaN", n_isnan}, {"Array", n_array},
};
static const NativeDef MATH_NATIVES[] = {
    {"floor", n_floor}, {"ceil", n_ceil}, {"round", n_round}, {"trunc", n_trunc}, {"abs", n_abs},
    {"sign", n_sign}, {"min", n_min}, {"max", n_max}, {"sqrt", n_sqrt}, {"pow", n_pow}, {"sin", n_sin},
    {"cos", n_cos}, {"tan", n_tan}, {"atan", n_atan}, {"atan2", n_atan2}, {"hypot", n_hypot}, {"random", n_random},
};
/* Natives are numbered: globals first, then Math, then log. */
#define NAT_GLOBALS ((int)(sizeof GLOBAL_NATIVES / sizeof GLOBAL_NATIVES[0]))
#define NAT_MATH ((int)(sizeof MATH_NATIVES / sizeof MATH_NATIVES[0]))
#define NAT_LOG (NAT_GLOBALS + NAT_MATH)
static Native native_at(int i) {
    if (i < NAT_GLOBALS) return GLOBAL_NATIVES[i].fn;
    if (i < NAT_LOG) return MATH_NATIVES[i - NAT_GLOBALS].fn;
    if (i == NAT_LOG) return n_log;
    return 0;
}

/* ---------- VM ---------- */
static int run(KGame *g, int stop);
static int line_of(KGame *g, uint32_t pc) {
    int line = 0;
    for (int i = 0; i < g->nlines && g->lines[i].pc <= pc; i++) line = g->lines[i].line;
    return line;
}
static int rt_fail(KGame *g, const char *a, const char *b, uint32_t bl, const char *c) {
    if (g->failed) return -1;
    char m[100]; uint32_t n = 0;
    const char *pre = "line ";
    for (uint32_t i = 0; pre[i]; i++) m[n++] = pre[i];
    char num[8]; int k = 0; uint32_t l = (uint32_t)line_of(g, g->err_pc);
    do { num[k++] = (char)('0' + l % 10); l /= 10; } while (l && k < 7);
    while (k) m[n++] = num[--k];
    m[n++] = ':'; m[n++] = ' ';
    m[n] = '\0';
    char full[100]; uint32_t f = 0;
    for (uint32_t i = 0; i < n; i++) full[f++] = m[i];
    for (uint32_t i = 0; a[i] && f < 96; i++) full[f++] = a[i];
    full[f] = '\0';
    return fail3(g, full, b, bl, c);
}
#define PUSH(v) do { if (g->sp >= STACK_MAX) return rt_fail(g, "too much recursion or data on the stack", "", 0, ""); g->stack[g->sp++] = (v); } while (0)

static bool push_val(KGame *g, Val v) { if (g->sp >= STACK_MAX) { rt_fail(g, "stack overflow", "", 0, ""); return false; } g->stack[g->sp++] = v; return true; }

/* Calls the function at stack[sp-argc-1] with argc arguments; leaves the result there. */
static int call_value(KGame *g, int argc, Val self) {
    int at = g->sp - argc - 1;
    Val f = g->stack[at];
    if (f.t == T_NAT) {
        Native fn = native_at((int)f.u.r);
        Val out = vundef();
        g->stack[at] = vundef();  /* the result slot; arguments above stay rooted */
        if (!fn || fn(g, &self, &g->stack[at + 1], argc, &g->stack[at]) < 0) return -1;
        (void)out;
        g->sp = at + 1;
        return 0;
    }
    if (f.t != T_FN) return rt_fail(g, "that is not a function", "", 0, "");
    Fn *fnp = FN(g, f.u.r);
    Proto *pr = &g->protos[fnp->proto];
    if (g->nframes >= FRAME_MAX) return rt_fail(g, "too much recursion", "", 0, "");
    while (argc < pr->params) { if (!push_val(g, vundef())) return -1; argc++; }
    while (argc > pr->params) { g->sp--; argc--; }
    int base = at + 1;
    for (int i = argc; i < pr->locals; i++) if (!push_val(g, vundef())) return -1;
    uint32_t env = fnp->env;
    if (pr->env) {
        uint32_t e = alloc(g, T_ENV, (uint32_t)sizeof(Env) + (uint32_t)pr->locals * sizeof(Val));
        if (!e) return -1;
        Env *ep = ENV(g, e);
        ep->parent = FN(g, g->stack[at].u.r)->env;
        ep->count = pr->locals;
        for (int i = 0; i < pr->locals; i++) ep->v[i] = g->stack[base + i];
        env = e;
    }
    Frame *fr = &g->frames[g->nframes++];
    fr->proto = (uint16_t)FN(g, g->stack[at].u.r)->proto;
    fr->pc = pr->code;
    fr->base = (uint16_t)base;
    fr->env = env;
    return 1;  /* a frame was pushed: run it */
}
/* Calls fn(args...) from a native and returns its result (fn and args rooted by the caller). */
static int reenter(KGame *g, Val fn, Val *argv, int argc, Val *out) {
    if (g->nested >= NESTED_MAX || stack_low(g)) return rt_fail(g, "callbacks nested too deeply", "", 0, "");
    int at = g->sp;
    if (!push_val(g, fn)) return -1;
    for (int i = 0; i < argc; i++) if (!push_val(g, argv[i])) return -1;
    int depth = g->nframes;
    int r = call_value(g, argc, vundef());
    if (r < 0) return -1;
    if (r == 1) {
        g->nested++;
        r = run(g, depth);
        g->nested--;
        if (r < 0) return -1;
    }
    *out = g->stack[at];
    g->sp = at;
    return 0;
}

static bool loose_eq(KGame *g, Val a, Val b) {
    if ((a.t == T_UNDEF || a.t == T_NULL) && (b.t == T_UNDEF || b.t == T_NULL)) return true;
    if (a.t != b.t) return false;
    switch (a.t) {
        case T_NUM: return a.u.n == b.u.n;
        case T_STR: return str_eq(g, a.u.r, b.u.r);
        default: return a.u.r == b.u.r;
    }
}
static int compare(KGame *g, Val a, Val b, int op) {
    if (a.t == T_STR && b.t == T_STR) {
        Str *x = STR(g, a.u.r), *y = STR(g, b.u.r);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int c = mcmp(x->s, y->s, n);
        if (!c) c = x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
        return op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0;
    }
    float x = to_num(g, a), y = to_num(g, b);
    return op == OP_LT ? x < y : op == OP_LE ? x <= y : op == OP_GT ? x > y : x >= y;
}
static const char *type_name(Val v) {
    switch (v.t) {
        case T_UNDEF: return "undefined";
        case T_NULL: case T_ARR: case T_OBJ: return "object";
        case T_BOOL: return "boolean";
        case T_NUM: return "number";
        case T_STR: return "string";
        default: return "function";
    }
}
/* stack: a b -> a+b (string concatenation if either is a string). */
static int op_add(KGame *g) {
    Val a = g->stack[g->sp - 2], b = g->stack[g->sp - 1];
    if (a.t != T_STR && b.t != T_STR && a.t != T_ARR && b.t != T_ARR && a.t != T_OBJ && b.t != T_OBJ) {
        g->sp--;
        g->stack[g->sp - 1] = vnum(to_num(g, a) + to_num(g, b));
        return 0;
    }
    uint32_t sa = to_str(g, a);
    if (!sa) return -1;
    g->stack[g->sp - 2] = vref(T_STR, sa);
    uint32_t sb = to_str(g, g->stack[g->sp - 1]);
    if (!sb) return -1;
    g->stack[g->sp - 1] = vref(T_STR, sb);
    Str *x = STR(g, sa), *y = STR(g, sb);
    uint32_t n = x->len + y->len;
    if (n > STR_MAX) return rt_fail(g, "string too long", "", 0, "");
    uint32_t r = alloc(g, T_STR, (uint32_t)sizeof(Str) + n + 1);
    if (!r) return -1;
    x = STR(g, g->stack[g->sp - 2].u.r); y = STR(g, g->stack[g->sp - 1].u.r);
    Str *z = STR(g, r);
    z->len = n;
    mcpy(z->s, x->s, x->len);
    mcpy(z->s + x->len, y->s, y->len);
    z->s[n] = '\0';
    z->hash = hash_bytes(z->s, n);
    g->sp--;
    g->stack[g->sp - 1] = vref(T_STR, r);
    return 0;
}
static uint32_t join_arr(KGame *g, uint32_t a, const char *sep, uint32_t seplen) {
    /* Nested (or self-containing) arrays: stop at a fixed depth. */
    if (g->join_depth >= 3) return new_str(g, "...", 3);
    g->join_depth++;
    uint32_t r = join_inner(g, a, sep, seplen);
    g->join_depth--;
    return r;
}
static uint32_t join_inner(KGame *g, uint32_t a, const char *sep, uint32_t seplen) {
    /* Builds the result on the stack so pieces stay rooted. */
    int at = g->sp;
    uint32_t empty = new_str(g, "", 0);
    if (!empty || !push_val(g, vref(T_STR, empty))) return 0;
    if (!push_val(g, vref(T_ARR, a))) return 0;  /* keep the array alive */
    uint32_t len = ARR(g, a)->len;
    for (uint32_t i = 0; i < len && !g->failed; i++) {
        if (i) {
            uint32_t s = new_str(g, sep, seplen);
            if (!s || !push_val(g, g->stack[at])) return 0;
            if (!push_val(g, vref(T_STR, s)) || op_add(g) < 0) return 0;
            g->stack[at] = g->stack[--g->sp];
        }
        Val v = i < ARR(g, a)->len ? arr_items(g, a)[i] : vundef();
        if (v.t == T_UNDEF || v.t == T_NULL) continue;
        if (!push_val(g, g->stack[at]) || !push_val(g, v)) return 0;
        if (v.t != T_STR) {
            uint32_t s = to_str(g, v);
            if (!s) return 0;
            g->stack[g->sp - 1] = vref(T_STR, s);
        }
        if (op_add(g) < 0) return 0;
        g->stack[at] = g->stack[--g->sp];
    }
    uint32_t r = g->stack[at].u.r;
    g->sp = at;
    return r;
}

/* Reads v[key] into *out. */
static int get_index(KGame *g, Val obj, Val key, Val *out) {
    if (obj.t == T_ARR || obj.t == T_STR) {
        if (key.t == T_NUM) {
            float f = key.u.n;
            if (f >= 0 && f == kfloor(f)) {
                uint32_t i = (uint32_t)f;
                if (obj.t == T_ARR) { *out = i < ARR(g, obj.u.r)->len ? arr_items(g, obj.u.r)[i] : vundef(); return 0; }
                Str *s = STR(g, obj.u.r);
                if (i < s->len) { uint32_t r = new_str(g, s->s + i, 1); if (!r) return -1; *out = vref(T_STR, r); }
                else *out = vundef();
                return 0;
            }
            *out = vundef();
            return 0;
        }
        if (key.t == T_STR && STR(g, key.u.r)->len == 6 && mcmp(STR(g, key.u.r)->s, "length", 6) == 0) {
            *out = vnum((float)(obj.t == T_ARR ? ARR(g, obj.u.r)->len : STR(g, obj.u.r)->len));
            return 0;
        }
        *out = vundef();
        return 0;
    }
    if (obj.t == T_OBJ) {
        uint32_t k = key.t == T_STR ? key.u.r : to_str(g, key);
        if (!k) return -1;
        *out = obj_get(g, obj.u.r, k);
        return 0;
    }
    if (obj.t == T_UNDEF || obj.t == T_NULL) {
        char kb[24]; uint32_t kl = 0; const char *ks = kb;
        if (key.t == T_STR) { ks = STR(g, key.u.r)->s; kl = STR(g, key.u.r)->len; }
        else kl = prim_text(g, key, kb);
        return rt_fail(g, "cannot read '", ks, kl, obj.t == T_NULL ? "' of null" : "' of undefined");
    }
    *out = vundef();
    return 0;
}
/* stack: obj key val -> val */
static int set_index(KGame *g) {
    Val obj = g->stack[g->sp - 3], key = g->stack[g->sp - 2], v = g->stack[g->sp - 1];
    if (obj.t == T_ARR) {
        float f = key.t == T_NUM ? key.u.n : -1;
        if (key.t == T_STR) {
            if (STR(g, key.u.r)->len == 6 && mcmp(STR(g, key.u.r)->s, "length", 6) == 0) {
                float nl = to_num(g, v);
                if (!(nl >= 0 && nl <= ARR_MAX)) return rt_fail(g, "bad array length", "", 0, "");
                uint32_t n = (uint32_t)nl;
                if (n > ARR(g, obj.u.r)->len) { if (!grow(g, obj.u.r, n, sizeof(Val))) return -1; for (uint32_t i = ARR(g, obj.u.r)->len; i < n; i++) arr_items(g, obj.u.r)[i] = vundef(); }
                ARR(g, obj.u.r)->len = n;
                goto done;
            }
            return rt_fail(g, "arrays take number indexes", "", 0, "");
        }
        if (!(f >= 0 && f == kfloor(f) && f < ARR_MAX)) return rt_fail(g, "bad array index", "", 0, "");
        uint32_t i = (uint32_t)f;
        Arr *a = ARR(g, obj.u.r);
        if (i >= a->len) {
            if (!grow(g, obj.u.r, i + 1, sizeof(Val))) return -1;
            a = ARR(g, obj.u.r);
            for (uint32_t j = a->len; j <= i; j++) arr_items(g, obj.u.r)[j] = vundef();
            a->len = i + 1;
        }
        arr_items(g, obj.u.r)[i] = g->stack[g->sp - 1];
    } else if (obj.t == T_OBJ) {
        if (key.t != T_STR) {
            uint32_t k = to_str(g, key);
            if (!k) return -1;
            g->stack[g->sp - 2] = vref(T_STR, k);
        }
        if (!obj_set(g, obj.u.r, g->stack[g->sp - 2].u.r, g->stack[g->sp - 1])) return -1;
    } else if (obj.t == T_UNDEF || obj.t == T_NULL) {
        return rt_fail(g, "cannot set a property of ", obj.t == T_NULL ? "null" : "undefined", obj.t == T_NULL ? 4 : 9, "");
    } else return rt_fail(g, "cannot set a property on a ", type_name(obj), slen(type_name(obj)), "");
done:
    g->stack[g->sp - 3] = g->stack[g->sp - 1];
    g->sp -= 2;
    return 0;
}

/* ---------- methods on arrays, strings and numbers ---------- */
static bool name_is(Str *k, const char *s) { uint32_t n = slen(s); return k->len == n && mcmp(k->s, s, n) == 0; }
static int callback(KGame *g, Val fn, Val *argv, int argc, Val *out) {
    if (fn.t != T_FN && fn.t != T_NAT) return rt_fail(g, "expected a function", "", 0, "");
    return reenter(g, fn, argv, argc, out);
}
/* Sort: insertion sort (stable, few allocations); fine for game-sized arrays. */
static int sort_cmp(KGame *g, Val fn, Val a, Val b, int *res) {
    if (fn.t == T_UNDEF) {
        uint32_t sa = to_str(g, a); if (!sa) return -1;
        if (!push_val(g, vref(T_STR, sa))) return -1;
        uint32_t sb = to_str(g, b); if (!sb) return -1;
        Str *x = STR(g, sa), *y = STR(g, sb);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int c = mcmp(x->s, y->s, n);
        *res = c ? c : (int)x->len - (int)y->len;
        g->sp--;
        return 0;
    }
    Val argv[2] = {a, b}, out;
    if (callback(g, fn, argv, 2, &out) < 0) return -1;
    float f = to_num(g, out);
    *res = f > 0 ? 1 : f < 0 ? -1 : 0;
    return 0;
}
/* stack: [obj args...] at `at`; result goes to stack[at]. */
static int array_method(KGame *g, Str *k, int at, int argc) {
    Val *a = &g->stack[at + 1];
    uint32_t arr = g->stack[at].u.r;
    Val out = vundef();
#define LEN (ARR(g, arr)->len)
#define ITEMS (arr_items(g, arr))
    if (name_is(k, "push")) { for (int i = 0; i < argc; i++) if (!arr_push(g, arr, a[i])) return -1; out = vnum((float)LEN); }
    else if (name_is(k, "pop")) { if (LEN) out = ITEMS[--ARR(g, arr)->len]; }
    else if (name_is(k, "shift")) { if (LEN) { out = ITEMS[0]; for (uint32_t i = 1; i < LEN; i++) ITEMS[i - 1] = ITEMS[i]; ARR(g, arr)->len--; } }
    else if (name_is(k, "unshift")) {
        uint32_t n = (uint32_t)argc;
        if (!grow(g, arr, LEN + n, sizeof(Val))) return -1;
        for (uint32_t i = LEN; i > 0; i--) ITEMS[i - 1 + n] = ITEMS[i - 1];
        for (uint32_t i = 0; i < n; i++) ITEMS[i] = a[i];
        ARR(g, arr)->len += n;
        out = vnum((float)LEN);
    } else if (name_is(k, "indexOf") || name_is(k, "includes")) {
        int found = -1;
        for (uint32_t i = 0; argc && i < LEN; i++) if (loose_eq(g, ITEMS[i], a[0]) && ITEMS[i].t == a[0].t) { found = (int)i; break; }
        out = name_is(k, "includes") ? vbool(found >= 0) : vnum((float)found);
    } else if (name_is(k, "splice")) {
        int len = (int)LEN, s = argi(g, a, argc, 0, 0);
        if (s < 0) s += len;
        if (s < 0) s = 0;
        if (s > len) s = len;
        int del = argc < 2 ? len - s : argi(g, a, argc, 1, 0);
        if (del < 0) del = 0;
        if (del > len - s) del = len - s;
        int ins = argc > 2 ? argc - 2 : 0;
        uint32_t removed = new_arr(g, 0);
        if (!removed) return -1;
        if (!push_val(g, vref(T_ARR, removed))) return -1;
        for (int i = 0; i < del; i++) if (!arr_push(g, removed, ITEMS[s + i])) return -1;
        int nl = len - del + ins;
        if (nl > len && !grow(g, arr, (uint32_t)nl, sizeof(Val))) return -1;
        if (ins != del) {
            if (ins > del) for (int i = len - 1; i >= s + del; i--) ITEMS[i + ins - del] = ITEMS[i];
            else for (int i = s + del; i < len; i++) ITEMS[i - del + ins] = ITEMS[i];
        }
        for (int i = 0; i < ins; i++) ITEMS[s + i] = a[2 + i];
        ARR(g, arr)->len = (uint32_t)nl;
        out = vref(T_ARR, removed);
        g->sp--;
    } else if (name_is(k, "slice")) {
        int len = (int)LEN, s = argi(g, a, argc, 0, 0), e = argi(g, a, argc, 1, len);
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (s < 0) s = 0;
        if (e > len) e = len;
        uint32_t r = new_arr(g, 0);
        if (!r || !push_val(g, vref(T_ARR, r))) return -1;
        for (int i = s; i < e; i++) if (!arr_push(g, r, ITEMS[i])) return -1;
        out = vref(T_ARR, r);
        g->sp--;
    } else if (name_is(k, "join")) {
        const char *sep = ","; uint32_t sl = 1;
        if (argc && a[0].t == T_STR) { sep = STR(g, a[0].u.r)->s; sl = STR(g, a[0].u.r)->len; }
        char sepbuf[32];
        if (sl > sizeof sepbuf) sl = sizeof sepbuf;
        mcpy(sepbuf, sep, sl);
        uint32_t r = join_arr(g, arr, sepbuf, sl);
        if (!r) return -1;
        out = vref(T_STR, r);
    } else if (name_is(k, "reverse")) {
        for (uint32_t i = 0, j = LEN ? LEN - 1 : 0; i < j; i++, j--) { Val t = ITEMS[i]; ITEMS[i] = ITEMS[j]; ITEMS[j] = t; }
        out = g->stack[at];
    } else if (name_is(k, "fill")) {
        int len = (int)LEN, s = argi(g, a, argc, 1, 0), e = argi(g, a, argc, 2, len);
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (s < 0) s = 0;
        if (e > len) e = len;
        for (int i = s; i < e; i++) ITEMS[i] = argc ? a[0] : vundef();
        out = g->stack[at];
    } else if (name_is(k, "forEach") || name_is(k, "map") || name_is(k, "filter") || name_is(k, "find")
               || name_is(k, "findIndex") || name_is(k, "some") || name_is(k, "every")) {
        Val fn = argc ? a[0] : vundef();
        bool map = name_is(k, "map"), filter = name_is(k, "filter"), find = name_is(k, "find"), findi = name_is(k, "findIndex");
        bool some = name_is(k, "some"), every = name_is(k, "every");
        uint32_t r = 0;
        if (map || filter) { r = new_arr(g, 0); if (!r || !push_val(g, vref(T_ARR, r))) return -1; }
        out = some ? vbool(false) : every ? vbool(true) : find ? vundef() : findi ? vnum(-1) : vundef();
        for (uint32_t i = 0; i < LEN; i++) {
            Val argv[3] = {ITEMS[i], vnum((float)i), g->stack[at]}, res;
            if (callback(g, fn, argv, 3, &res) < 0) return -1;
            if (map) { if (!arr_push(g, r, res)) return -1; }
            else if (filter) { if (truthy(g, res) && i < LEN && !arr_push(g, r, ITEMS[i])) return -1; }
            else if (find && truthy(g, res)) { out = i < LEN ? ITEMS[i] : vundef(); break; }
            else if (findi && truthy(g, res)) { out = vnum((float)i); break; }
            else if (some && truthy(g, res)) { out = vbool(true); break; }
            else if (every && !truthy(g, res)) { out = vbool(false); break; }
        }
        if (map || filter) { out = vref(T_ARR, r); g->sp--; }
    } else if (name_is(k, "reduce")) {
        Val fn = argc ? a[0] : vundef();
        uint32_t i = 0;
        Val acc;
        if (argc > 1) acc = a[1];
        else { if (!LEN) return rt_fail(g, "reduce of empty array with no initial value", "", 0, ""); acc = ITEMS[0]; i = 1; }
        if (!push_val(g, acc)) return -1;
        int slot = g->sp - 1;
        for (; i < LEN; i++) {
            Val argv[4] = {g->stack[slot], ITEMS[i], vnum((float)i), g->stack[at]}, res;
            if (callback(g, fn, argv, 4, &res) < 0) return -1;
            g->stack[slot] = res;
        }
        out = g->stack[slot];
        g->sp--;
    } else if (name_is(k, "sort")) {
        Val fn = argc ? a[0] : vundef();
        for (uint32_t i = 1; i < LEN; i++) {
            Val x = ITEMS[i];
            if (!push_val(g, x)) return -1;
            uint32_t j = i;
            while (j > 0 && j - 1 < LEN) {
                int c;
                if (sort_cmp(g, fn, ITEMS[j - 1], g->stack[g->sp - 1], &c) < 0) return -1;
                if (c <= 0) break;
                if (j < LEN) ITEMS[j] = ITEMS[j - 1];
                j--;
            }
            if (j < LEN) ITEMS[j] = g->stack[g->sp - 1];
            g->sp--;
        }
        out = g->stack[at];
    } else if (name_is(k, "toString")) {
        uint32_t r = join_arr(g, arr, ",", 1);
        if (!r) return -1;
        out = vref(T_STR, r);
    } else return rt_fail(g, "arrays have no method '", k->s, k->len, "'");
#undef LEN
#undef ITEMS
    g->stack[at] = out;
    g->sp = at + 1;
    return 0;
}
static int string_method(KGame *g, Str *k, int at, int argc) {
    Val *a = &g->stack[at + 1];
    Str *s = STR(g, g->stack[at].u.r);
    int len = (int)s->len;
    Val out = vundef();
    char buf[256];
    if (name_is(k, "charAt") || name_is(k, "charCodeAt")) {
        int i = argi(g, a, argc, 0, 0);
        if (name_is(k, "charCodeAt")) out = vnum(i >= 0 && i < len ? (float)(uint8_t)s->s[i] : 0.0f / 0.0f);
        else { uint32_t r = new_str(g, i >= 0 && i < len ? s->s + i : "", i >= 0 && i < len ? 1 : 0); if (!r) return -1; out = vref(T_STR, r); }
    } else if (name_is(k, "indexOf") || name_is(k, "includes")) {
        int found = -1;
        if (argc && a[0].t == T_STR) {
            Str *n = STR(g, a[0].u.r);
            for (int i = 0; i + (int)n->len <= len; i++) if (mcmp(s->s + i, n->s, n->len) == 0) { found = i; break; }
        }
        out = name_is(k, "includes") ? vbool(found >= 0) : vnum((float)found);
    } else if (name_is(k, "slice") || name_is(k, "substring") || name_is(k, "substr")) {
        int st = argi(g, a, argc, 0, 0), e = argi(g, a, argc, 1, len);
        if (name_is(k, "substr")) e = argc > 1 ? st + argi(g, a, argc, 1, 0) : len;
        if (name_is(k, "slice")) { if (st < 0) st += len; if (e < 0) e += len; }
        if (st < 0) st = 0;
        if (e > len) e = len;
        if (e < 0) e = 0;
        if (name_is(k, "substring") && st > e) { int t = st; st = e; e = t; }
        if (e < st) e = st;
        if (st > len) st = len;
        uint32_t r = new_str(g, s->s + st, (uint32_t)(e - st));
        if (!r) return -1;
        out = vref(T_STR, r);
    } else if (name_is(k, "toUpperCase") || name_is(k, "toLowerCase")) {
        bool up = name_is(k, "toUpperCase");
        uint32_t r = new_str(g, s->s, s->len);
        if (!r) return -1;
        Str *z = STR(g, r);
        for (uint32_t i = 0; i < z->len; i++) {
            char c = z->s[i];
            if (up && c >= 'a' && c <= 'z') z->s[i] = (char)(c - 32);
            if (!up && c >= 'A' && c <= 'Z') z->s[i] = (char)(c + 32);
        }
        z->hash = hash_bytes(z->s, z->len);
        out = vref(T_STR, r);
    } else if (name_is(k, "padStart") || name_is(k, "padEnd") || name_is(k, "repeat")) {
        bool rep = name_is(k, "repeat");
        int want = argi(g, a, argc, 0, 0);
        const char *pad = " "; uint32_t pl = 1;
        if (!rep && argc > 1 && a[1].t == T_STR && STR(g, a[1].u.r)->len) { pad = STR(g, a[1].u.r)->s; pl = STR(g, a[1].u.r)->len; }
        uint32_t n = 0;
        if (rep) {
            if (want < 0 || (int64_t)want * len > (int)sizeof buf) return rt_fail(g, "repeat count too large for this engine", "", 0, "");
            for (int i = 0; i < want; i++) { mcpy(buf + n, s->s, s->len); n += s->len; }
        } else {
            if (want > (int)sizeof buf) want = sizeof buf;
            if (want <= len) { out = g->stack[at]; goto done; }
            int fill = want - len;
            if (name_is(k, "padEnd")) { mcpy(buf, s->s, s->len); n = s->len; }
            for (int i = 0; i < fill; i++) buf[n++] = pad[i % pl];
            if (name_is(k, "padStart")) { mcpy(buf + n, s->s, s->len); n += s->len; }
        }
        uint32_t r = new_str(g, buf, n);
        if (!r) return -1;
        out = vref(T_STR, r);
    } else if (name_is(k, "trim")) {
        int b = 0, e = len;
        while (b < e && (s->s[b] == ' ' || s->s[b] == '\t' || s->s[b] == '\n')) b++;
        while (e > b && (s->s[e - 1] == ' ' || s->s[e - 1] == '\t' || s->s[e - 1] == '\n')) e--;
        uint32_t r = new_str(g, s->s + b, (uint32_t)(e - b));
        if (!r) return -1;
        out = vref(T_STR, r);
    } else if (name_is(k, "split")) {
        uint32_t r = new_arr(g, 0);
        if (!r || !push_val(g, vref(T_ARR, r))) return -1;
        s = STR(g, g->stack[at].u.r);
        uint32_t sl = argc && a[0].t == T_STR ? STR(g, a[0].u.r)->len : 0;
        uint32_t st = 0;
        for (uint32_t i = 0; i <= s->len && !g->failed; i++) {
            bool cut = i == s->len;
            if (!cut && sl && i + sl <= s->len) cut = mcmp(s->s + i, STR(g, a[0].u.r)->s, sl) == 0;
            if (!cut && !sl && argc && i > st) cut = true;
            if (!cut) continue;
            if (!sl && argc && i == s->len && st == i) break;
            uint32_t piece = new_str(g, s->s + st, i - st);
            if (!piece || !arr_push(g, r, vref(T_STR, piece))) return -1;
            s = STR(g, g->stack[at].u.r);
            st = sl ? i + sl : i;
            if (sl) i += sl - 1;
        }
        out = vref(T_ARR, r);
        g->sp--;
    } else if (name_is(k, "toString")) out = g->stack[at];
    else return rt_fail(g, "strings have no method '", k->s, k->len, "'");
done:
    g->stack[at] = out;
    g->sp = at + 1;
    return 0;
}
static int number_method(KGame *g, Str *k, int at, int argc) {
    Val *a = &g->stack[at + 1];
    float x = g->stack[at].u.n;
    char buf[48]; uint32_t n = 0;
    if (name_is(k, "toFixed")) {
        int d = argi(g, a, argc, 0, 0);
        if (d < 0) d = 0;
        if (d > 6) d = 6;
        if (kisnan(x) || kabs(x) > 1e9f) n = fmt_num(x, buf);
        else {
            float m = 1; for (int i = 0; i < d; i++) m *= 10;
            bool neg = x < 0; if (neg) x = -x;
            float r = kfloor(x * m + 0.5f);
            uint32_t ip = (uint32_t)(r / m), fp = (uint32_t)(r - (float)ip * m + 0.5f);
            if (neg && (ip || fp)) buf[n++] = '-';
            char t[12]; int c = 0;
            do { t[c++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (c) buf[n++] = t[--c];
            if (d) {
                buf[n++] = '.';
                for (int i = d - 1; i >= 0; i--) { uint32_t p10 = 1; for (int j = 0; j < i; j++) p10 *= 10; buf[n++] = (char)('0' + (fp / p10) % 10); }
            }
        }
    } else if (name_is(k, "toString")) n = fmt_num(x, buf);
    else return rt_fail(g, "numbers have no method '", k->s, k->len, "'");
    uint32_t r = new_str(g, buf, n);
    if (!r) return -1;
    g->stack[at] = vref(T_STR, r);
    g->sp = at + 1;
    return 0;
}

static int run(KGame *g, int stop) {
    while (g->nframes > stop) {
        Frame *fr = &g->frames[g->nframes - 1];
        const uint8_t *code = g->code;
        uint32_t pc = fr->pc;
        if (pc >= g->code_len) return rt_fail(g, "internal error: bad jump", "", 0, "");
        if (--g->budget < 0) { g->err_pc = pc; return rt_fail(g, "too slow: a frame took too long (endless loop?)", "", 0, ""); }
        g->err_pc = pc;
        uint8_t op = code[pc++];
#define U8() (code[pc++])
#define U16() (pc += 2, (uint16_t)(code[pc - 2] | code[pc - 1] << 8))
        switch (op) {
            case OP_CONST: { uint16_t i = U16(); PUSH(g->consts[i < g->nconsts ? i : 0]); break; }
            case OP_UNDEF: PUSH(vundef()); break;
            case OP_NULL: PUSH(vnull()); break;
            case OP_TRUE: PUSH(vbool(true)); break;
            case OP_FALSE: PUSH(vbool(false)); break;
            case OP_POP: if (g->sp > 0) g->sp--; break;
            case OP_DUP: { Val v = g->stack[g->sp - 1]; PUSH(v); break; }
            case OP_DUP2: { Val a = g->stack[g->sp - 2], b = g->stack[g->sp - 1]; PUSH(a); PUSH(b); break; }
            case OP_GETL: { uint8_t s = U8(); PUSH(g->stack[fr->base + s]); break; }
            case OP_SETL: { uint8_t s = U8(); g->stack[fr->base + s] = g->stack[g->sp - 1]; break; }
            case OP_GETE: case OP_SETE: {
                uint8_t d = U8(), s = U8();
                uint32_t e = fr->env;
                for (int i = 0; i < d && e; i++) e = ENV(g, e)->parent;
                if (!e || s >= ENV(g, e)->count) return rt_fail(g, "internal error: scope", "", 0, "");
                if (op == OP_GETE) PUSH(ENV(g, e)->v[s]);
                else ENV(g, e)->v[s] = g->stack[g->sp - 1];
                break;
            }
            case OP_GETG: {
                uint16_t i = U16();
                if (i >= g->nglobals || !g->gdef[i]) {
                    Str *n = STR(g, g->gname[i < g->nglobals ? i : 0]);
                    fr->pc = pc;
                    return rt_fail(g, "", n->s, n->len, " is not defined");
                }
                PUSH(g->gval[i]);
                break;
            }
            case OP_GETGS: { uint16_t i = U16(); PUSH(i < g->nglobals && g->gdef[i] ? g->gval[i] : vundef()); break; }
            case OP_SETG: { uint16_t i = U16(); if (i < g->nglobals) { g->gval[i] = g->stack[g->sp - 1]; g->gdef[i] = 1; } break; }
            case OP_GETP: {
                uint16_t ki = U16();
                fr->pc = pc;
                Val out;
                if (get_index(g, g->stack[g->sp - 1], g->consts[ki], &out) < 0) return -1;
                g->stack[g->sp - 1] = out;
                break;
            }
            case OP_SETP: {
                uint16_t ki = U16();
                fr->pc = pc;
                /* obj val -> obj key val */
                Val v = g->stack[g->sp - 1];
                g->stack[g->sp - 1] = g->consts[ki];
                PUSH(v);
                if (set_index(g) < 0) return -1;
                break;
            }
            case OP_GETI: {
                fr->pc = pc;
                Val out;
                if (get_index(g, g->stack[g->sp - 2], g->stack[g->sp - 1], &out) < 0) return -1;
                g->sp--;
                g->stack[g->sp - 1] = out;
                break;
            }
            case OP_SETI: fr->pc = pc; if (set_index(g) < 0) return -1; break;
            case OP_ARR: {
                uint16_t n = U16();
                fr->pc = pc;
                uint32_t r = new_arr(g, n);
                if (!r || !push_val(g, vref(T_ARR, r))) return -1;
                if (n && !grow(g, r, n, sizeof(Val))) return -1;
                Val *it = arr_items(g, r);
                for (int i = 0; i < n; i++) it[i] = g->stack[g->sp - 1 - n + i];
                ARR(g, r)->len = n;
                g->stack[g->sp - 1 - n] = g->stack[g->sp - 1];
                g->sp -= n;
                break;
            }
            case OP_OBJ: {
                uint16_t n = U16();
                fr->pc = pc;
                uint32_t r = new_obj(g);
                if (!r || !push_val(g, vref(T_OBJ, r))) return -1;
                if (n && !grow(g, r, n, sizeof(Prop))) return -1;   /* exactly its literal's keys */
                int base = g->sp - 1 - 2 * n;
                for (int i = 0; i < n; i++) {
                    Val k = g->stack[base + 2 * i];
                    if (k.t != T_STR) return rt_fail(g, "internal error: key", "", 0, "");
                    if (!obj_set(g, r, k.u.r, g->stack[base + 2 * i + 1])) return -1;
                }
                g->stack[base] = g->stack[g->sp - 1];
                g->sp = base + 1;
                break;
            }
            case OP_CLOSURE: {
                uint16_t pi = U16();
                fr->pc = pc;
                uint32_t r = alloc(g, T_FN, sizeof(Fn));
                if (!r) return -1;
                FN(g, r)->proto = pi;
                FN(g, r)->env = fr->env;
                PUSH(vref(T_FN, r));
                break;
            }
            case OP_CALL: case OP_CALLM: {
                uint16_t ki = op == OP_CALLM ? U16() : 0;
                uint8_t argc = U8();
                fr->pc = pc;
                if (g->sp - argc - 1 < 0) return rt_fail(g, "internal error: call", "", 0, "");
                if (op == OP_CALLM) {
                    int at = g->sp - argc - 1;
                    Val obj = g->stack[at];
                    Str *k = STR(g, g->consts[ki].u.r);
                    if (obj.t == T_ARR) { if (array_method(g, k, at, argc) < 0) return -1; break; }
                    if (obj.t == T_STR) { if (string_method(g, k, at, argc) < 0) return -1; break; }
                    if (obj.t == T_NUM) { if (number_method(g, k, at, argc) < 0) return -1; break; }
                    Val m;
                    if (get_index(g, obj, g->consts[ki], &m) < 0) return -1;
                    if (m.t != T_FN && m.t != T_NAT) return rt_fail(g, "", k->s, k->len, " is not a function");
                    g->stack[at] = m;
                }
                int r = call_value(g, argc, vundef());
                if (r < 0) return -1;
                break;
            }
            case OP_RET: {
                Val v = g->stack[g->sp - 1];
                g->sp = fr->base - 1;
                g->stack[g->sp++] = v;
                g->nframes--;
                continue;
            }
            case OP_JMP: { uint16_t t = U16(); pc = t; break; }
            case OP_JF: { uint16_t t = U16(); if (!truthy(g, g->stack[--g->sp])) pc = t; break; }
            case OP_JT: { uint16_t t = U16(); if (truthy(g, g->stack[--g->sp])) pc = t; break; }
            case OP_JFK: { uint16_t t = U16(); if (!truthy(g, g->stack[g->sp - 1])) pc = t; else g->sp--; break; }
            case OP_JTK: { uint16_t t = U16(); if (truthy(g, g->stack[g->sp - 1])) pc = t; else g->sp--; break; }
            case OP_JNNK: { uint16_t t = U16(); Val v = g->stack[g->sp - 1]; if (v.t != T_UNDEF && v.t != T_NULL) pc = t; else g->sp--; break; }
            case OP_ADD: fr->pc = pc; if (op_add(g) < 0) return -1; break;
            case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: {
                float b = to_num(g, g->stack[g->sp - 1]), a = to_num(g, g->stack[g->sp - 2]);
                float r = op == OP_SUB ? a - b : op == OP_MUL ? a * b : op == OP_DIV ? a / b : op == OP_MOD ? kmod(a, b) : kpow(a, b);
                g->sp--;
                g->stack[g->sp - 1] = vnum(r);
                break;
            }
            case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR: {
                int32_t b = to_i32(to_num(g, g->stack[g->sp - 1])), a = to_i32(to_num(g, g->stack[g->sp - 2]));
                float r;
                switch (op) {
                    case OP_BAND: r = (float)(a & b); break;
                    case OP_BOR: r = (float)(a | b); break;
                    case OP_BXOR: r = (float)(a ^ b); break;
                    case OP_SHL: r = (float)(int32_t)((uint32_t)a << (b & 31)); break;
                    case OP_SHR: r = (float)(a >> (b & 31)); break;
                    default: r = (float)((uint32_t)a >> (b & 31)); break;
                }
                g->sp--;
                g->stack[g->sp - 1] = vnum(r);
                break;
            }
            case OP_NEG: g->stack[g->sp - 1] = vnum(-to_num(g, g->stack[g->sp - 1])); break;
            case OP_PLUS: g->stack[g->sp - 1] = vnum(to_num(g, g->stack[g->sp - 1])); break;
            case OP_NOT: g->stack[g->sp - 1] = vbool(!truthy(g, g->stack[g->sp - 1])); break;
            case OP_BNOT: g->stack[g->sp - 1] = vnum((float)~to_i32(to_num(g, g->stack[g->sp - 1]))); break;
            case OP_EQ: case OP_NE: {
                bool e = loose_eq(g, g->stack[g->sp - 2], g->stack[g->sp - 1]);
                g->sp--;
                g->stack[g->sp - 1] = vbool(op == OP_EQ ? e : !e);
                break;
            }
            case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
                int c = compare(g, g->stack[g->sp - 2], g->stack[g->sp - 1], op);
                g->sp--;
                g->stack[g->sp - 1] = vbool(c != 0);
                break;
            }
            case OP_TYPEOF: {
                fr->pc = pc;
                const char *t = type_name(g->stack[g->sp - 1]);
                uint32_t r = new_str(g, t, slen(t));
                if (!r) return -1;
                g->stack[g->sp - 1] = vref(T_STR, r);
                break;
            }
            case OP_ITER: {
                Val v = g->stack[g->sp - 1];
                if (v.t != T_ARR && v.t != T_STR) { fr->pc = pc; return rt_fail(g, "for...of needs an array or string", "", 0, ""); }
                break;
            }
            case OP_LEN: {
                Val v = g->stack[g->sp - 1];
                g->stack[g->sp - 1] = vnum((float)(v.t == T_ARR ? ARR(g, v.u.r)->len : v.t == T_STR ? STR(g, v.u.r)->len : 0));
                break;
            }
            default: return rt_fail(g, "internal error: bad instruction", "", 0, "");
        }
        if (g->failed) return -1;
        /* The frame may have changed (a call pushed one); save the pc in ours. */
        fr->pc = pc;
#undef U8
#undef U16
    }
    return 0;
}

/* ---------- public API ---------- */
size_t kgame_state_size(void) { return sizeof(KGame) + 8; }

KGame *kgame_create(void *memory, size_t size, uint32_t seed) {
    if (!memory || size < KGAME_MEMORY_MIN) return 0;
    uint8_t *m = memory;
    size_t state = kgame_state_size();
    return kgame_create_parts(m, state, m + state, m + state + KGAME_FB_BYTES, size - state - KGAME_FB_BYTES, seed);
}

KGame *kgame_create_parts(void *state, size_t state_size, void *pixels, void *arena, size_t arena_size, uint32_t seed) {
    if (!state || state_size < kgame_state_size() || !pixels || !arena || arena_size < KGAME_ARENA_MIN) return 0;
    KGame *g = (KGame *)(((uintptr_t)state + 7) & ~(uintptr_t)7);
    mset(g, 0, sizeof *g);
    g->fb = pixels;
    mset(g->fb, 0, KGAME_FB_BYTES);
    uintptr_t base = ((uintptr_t)arena + 7) & ~(uintptr_t)7;
    uint8_t *rest = (uint8_t *)base;
    size_t left = (arena_size - (base - (uintptr_t)arena)) & ~(size_t)7;
    /* While compiling, the heap (constants only) has the lower part and the
     * bytecode the upper; kgame_load then moves the bytecode to the very top
     * and gives everything below it to the heap. */
    g->heap = rest;
    g->heap_size = (uint32_t)(left * 2 / 5) & ~7u;
    if (g->heap_size > 0xfffff0) g->heap_size = 0xfffff0;
    g->code_cap = (uint32_t)(left - g->heap_size);
    if (g->code_cap > 0xfff0) g->code_cap = 0xfff0;
    g->code = rest + left - g->code_cap;
    g->bump = g->high = 8;
    g->rng = seed ? seed : 0x2545f491u;
    g->g_init = g->g_update = g->g_draw = -1;
    /* Natives and constants as globals. */
    for (int i = 0; i < NAT_GLOBALS; i++) {
        int gi = global_index(g, GLOBAL_NATIVES[i].name, slen(GLOBAL_NATIVES[i].name));
        g->gval[gi] = vref(T_NAT, (uint32_t)i);
        g->gdef[gi] = 1;
    }
    /* Math and console are ordinary objects rooted by their globals (a game may
     * add properties to them); only their key strings are permanent. */
    int gm = global_index(g, "Math", 4), gc_ = global_index(g, "console", 7);
    uint32_t math = new_obj(g);
    if (!math) return 0;
    g->gval[gm] = vref(T_OBJ, math); g->gdef[gm] = 1;
    uint32_t console = new_obj(g);
    if (!console) return 0;
    g->gval[gc_] = vref(T_OBJ, console); g->gdef[gc_] = 1;
    for (int i = 0; i <= NAT_MATH + 1 && !g->failed; i++) {
        const char *name = i < NAT_MATH ? MATH_NATIVES[i].name : i == NAT_MATH ? "PI" : "log";
        g->perm_mode = true;
        uint32_t k = new_str(g, name, slen(name));
        g->perm_mode = false;
        if (!k) return 0;
        Val v = i < NAT_MATH ? vref(T_NAT, (uint32_t)(NAT_GLOBALS + i)) : i == NAT_MATH ? vnum(KPI) : vref(T_NAT, NAT_LOG);
        if (!obj_set(g, i <= NAT_MATH ? math : console, k, v)) return 0;
    }
    int gw = global_index(g, "WIDTH", 5), gh = global_index(g, "HEIGHT", 6);
    g->gval[gw] = vnum(KGAME_W); g->gdef[gw] = 1;
    g->gval[gh] = vnum(KGAME_H); g->gdef[gh] = 1;
    if (g->failed) return 0;
    return g;
}

static int call_global(KGame *g, int gi) {
    if (gi < 0 || !g->gdef[gi]) return 0;
    Val f = g->gval[gi];
    if (f.t != T_FN && f.t != T_NAT) return 0;
    g->budget = BUDGET;
    Val out;
    g->sp = 0;
    return reenter(g, f, 0, 0, &out);
}

void kgame_set_stack_limit(KGame *g, uint32_t bytes) { if (g) g->stack_limit = bytes; }

int kgame_load(KGame *g, const char *src, size_t len) {
    if (!g || g->loaded || g->failed) return -1;
    volatile char stack_top;
    g->stack_base = (uintptr_t)&stack_top;
    if (!src || len > KGAME_SOURCE_MAX) return fail(g, "the game is too long");
    P p;
    mset(&p, 0, sizeof p);
    p.g = g;
    p.src = src;
    p.len = (uint32_t)len;
    p.line = 1;
    FS top;
    mset(&top, 0, sizeof top);
    top.top = true;
    top.proto = 0;
    p.fs = &top;
    g->nprotos = 1;
    g->protos[0].code = 0;
    next(&p);
    uint32_t jump_in = emit_jump(&p, OP_JMP);
    uint32_t body = here(&p);
    while (p.tok.kind != TK_EOF && !g->failed) statement(&p);
    emit(&p, OP_UNDEF);
    emit(&p, OP_RET);
    emit_hoisted(&p, jump_in, body);
    g->protos[0].locals = (uint8_t)top.nlocals;
    if (g->failed) return -1;
    /* Bytecode to the top of the block; the heap grows into what it did not use. */
    uint8_t *end = g->code + g->code_cap;
    uint8_t *code = end - g->code_len;
    for (uint32_t i = g->code_len; i-- > 0;) code[i] = g->code[i];   /* upward, overlapping */
    g->code = code;
    g->code_cap = g->code_len;
    uintptr_t top_of_heap = ((uintptr_t)code - (uintptr_t)g->heap) & ~(uintptr_t)7;
    g->heap_size = top_of_heap > 0xfffff0 ? 0xfffff0 : (uint32_t)top_of_heap;
    g->loaded = true;
    /* Run the top level as a function with no arguments. */
    g->budget = BUDGET * 4;
    g->sp = 0;
    uint32_t fr = alloc(g, T_FN, sizeof(Fn));
    if (!fr) return -1;
    FN(g, fr)->proto = 0;
    FN(g, fr)->env = 0;
    Val out;
    if (reenter(g, vref(T_FN, fr), 0, 0, &out) < 0) return -1;
    g->g_init = global_index(g, "init", 4);
    g->g_update = global_index(g, "update", 6);
    g->g_draw = global_index(g, "draw", 4);
    if (g->failed) return -1;
    if (call_global(g, g->g_init) < 0) return -1;
    return 0;
}

int kgame_frame(KGame *g, unsigned buttons) {
    if (!g || !g->loaded || g->failed) return -1;
    volatile char stack_top;
    g->stack_base = (uintptr_t)&stack_top;
    buttons &= 63;
    g->pressed = (uint8_t)(buttons & ~g->held);
    g->held = (uint8_t)buttons;
    if (call_global(g, g->g_update) < 0) return -1;
    if (call_global(g, g->g_draw) < 0) return -1;
    g->frame_no++;
    return 0;
}

const char *kgame_error(const KGame *g) { return g ? g->err : "no game"; }
const uint8_t *kgame_pixels(const KGame *g) { return g ? g->fb : 0; }
void kgame_set_saved(KGame *g, float v) { if (g) { g->saved = v; g->saved_dirty = false; } }
int kgame_saved(KGame *g, float *v) {
    if (!g || !g->saved_dirty) return 0;
    g->saved_dirty = false;
    if (v) *v = g->saved;
    return 1;
}
const char *kgame_log(KGame *g) {
    static char empty[1];
    if (!g) return empty;
    /* Returned text stays valid until the next frame; the next call starts fresh. */
    static char out[LOG_MAX];
    mcpy(out, g->log, g->log_len + 1);
    g->log_len = 0;
    g->log[0] = '\0';
    return out;
}
void kgame_usage(const KGame *g, KGameUsage *u) {
    if (!g || !u) return;
    u->code = g->code_len;
    u->code_cap = g->code_cap;
    u->heap_used = g->high;
    u->heap_size = g->heap_size;
    u->engine = (uint32_t)sizeof(KGame);
    u->constants = (uint32_t)g->nconsts;
    u->globals = (uint32_t)g->nglobals;
    u->frames = g->frame_no;
}
