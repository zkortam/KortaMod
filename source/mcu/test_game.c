/* Host tests for the games engine: language, API, errors, limits and a random
 * mutation fuzz. Build with ASan/UBSan (see README). */
#include "kortamod_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static unsigned char memory[64 * 1024];

static KGame *load(const char *src) {
    KGame *g = kgame_create(memory, sizeof memory, 1234);
    if (!g) { printf("create failed\n"); failures++; return NULL; }
    kgame_load(g, src, strlen(src));
    return g;
}
static int pixel(KGame *g, int x, int y) {
    const unsigned char *p = kgame_pixels(g);
    unsigned char b = p[(y * KGAME_W + x) / 2];
    return (x & 1) ? (b & 15) : (b >> 4);
}
/* Runs `src` whose draw() writes its result with text; returns the log. */
static const char *logged(const char *src) {
    KGame *g = load(src);
    if (!g) return "";
    if (kgame_error(g)[0]) { printf("  error: %s\n", kgame_error(g)); return kgame_error(g); }
    kgame_frame(g, 0);
    if (kgame_error(g)[0]) { printf("  error: %s\n", kgame_error(g)); return kgame_error(g); }
    static char out[1024];
    snprintf(out, sizeof out, "%s", kgame_log(g));
    size_t n = strlen(out);
    if (n && out[n - 1] == '\n') out[n - 1] = '\0';
    return out;
}
#define EXPECT_LOG(src, want) do { const char *got_ = logged(src); if (strcmp(got_, want)) { printf("FAIL %s:%d\n  src: %s\n  got: %s\n  want: %s\n", __FILE__, __LINE__, src, got_, want); failures++; } } while (0)
static void expect_error(const char *src, const char *part, int line) {
    KGame *g = load(src);
    if (g && !kgame_error(g)[0]) {
        for (int i = 0; i < 3 && !kgame_error(g)[0]; i++) kgame_frame(g, 0);
    }
    const char *e = g ? kgame_error(g) : "";
    if (!strstr(e, part)) { printf("FAIL line %d: error for\n  %s\n  got '%s', want '%s'\n", line, src, e, part); failures++; }
}
#define EXPECT_ERROR(src, part) expect_error(src, part, __LINE__)

static void language(void) {
    EXPECT_LOG("console.log(1 + 2 * 3, 7 / 2, 7 % 3, -7 % 3, 2 ** 10)", "7 3.5 1 -1 1024");
    EXPECT_LOG("console.log(0.1 + 0.2, 1 / 3, 100000.5, 1e6, -0.25)", "0.3 0.33333 100000.5 1000000 -0.25");
    EXPECT_LOG("console.log('a' + 1, 1 + 'a', 'x' + true + null + undefined)", "a1 1a xtruenullundefined");
    EXPECT_LOG("let s = `Score ${3 * 4}!`; console.log(s, `${'a'}${'b'}`, `plain`)", "Score 12! ab plain");
    EXPECT_LOG("console.log(1 === 1, 1 == '1', null == undefined, 'a' < 'b', 2 >= 3, !0, !!'x')", "true false true true false true true");
    EXPECT_LOG("console.log(5 & 3, 5 | 3, 5 ^ 3, ~5, 1 << 4, -16 >> 2, -1 >>> 28)", "1 7 6 -6 16 -4 15");
    EXPECT_LOG("let a = 0 || 'd', b = 1 && 2, c = null ?? 'n', d = 0 ?? 'z'; console.log(a, b, c, d)", "d 2 n 0");
    EXPECT_LOG("let x = 5; x += 2; x -= 1; x *= 3; x /= 2; x %= 5; console.log(x)", "4");
    EXPECT_LOG("let i = 1; let j = i++; let k = ++i; console.log(i, j, k, i--, --i)", "3 1 3 3 1");
    EXPECT_LOG("let o = {a: 1}; o.a++; ++o.a; o['b'] = 5; o.b += 1; console.log(o.a, o.b, o.c)", "3 6 undefined");
    EXPECT_LOG("let a = [1,2]; a[0]++; a[1] += 10; a[4] = 9; console.log(a.length, a.join('-'))", "5 2-12---9");
    EXPECT_LOG("console.log(typeof 1, typeof 'a', typeof {}, typeof [], typeof null, typeof undefined, typeof print, typeof cls)", "number string object object object undefined undefined function");
    EXPECT_LOG("let r = ''; for (let i = 0; i < 5; i++) { if (i == 1) continue; if (i == 4) break; r += i; } console.log(r)", "023");
    EXPECT_LOG("let r = []; let n = 0; while (n < 3) r.push(n++); do { r.push('d'); } while (false); console.log(r.join())", "0,1,2,d");
    EXPECT_LOG("let r = ''; for (const c of ['a','b']) r += c; for (const ch of 'xy') r += ch; console.log(r)", "abxy");
    EXPECT_LOG("function f(x) { switch (x) { case 1: return 'one'; case 2: case 3: return 'few'; default: return 'many'; } } console.log(f(1), f(3), f(9))", "one few many");
    EXPECT_LOG("let r = ''; for (let i = 0; i < 4; i++) { switch (i % 2) { case 0: r += 'e'; break; default: r += 'o'; continue; } r += '.'; } console.log(r)", "e.oe.o");
    EXPECT_LOG("let r = ''; switch (2) { default: r += 'd'; case 1: r += '1'; break; case 3: r += '3'; } console.log(r)", "d1");
    EXPECT_LOG("console.log(true ? 'y' : 'n', 0 ? 'y' : 1 ? 'a' : 'b')", "y a");
    EXPECT_LOG("function add(a, b) { return a + b; } const mul = (a, b) => a * b; const sq = x => x * x; console.log(add(2, 3), mul(3, 4), sq(5))", "5 12 25");
    EXPECT_LOG("function counter() { let c = 0; return () => ++c; } const k = counter(); k(); k(); console.log(k())", "3");
    EXPECT_LOG("function outer() { let a = 1; function mid() { let b = 2; return () => a + b; } return mid()(); } console.log(outer())", "3");
    EXPECT_LOG("function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); } console.log(fact(10))", "3628800");
    EXPECT_LOG("console.log(early()); function early() { return 'hoisted'; }", "hoisted");
    EXPECT_LOG("function f() { return g(); function g() { return 'inner'; } } console.log(f())", "inner");
    EXPECT_LOG("const fib = n => n < 2 ? n : fib(n - 1) + fib(n - 2); console.log(fib(15))", "610");
    EXPECT_LOG("let p = {x: 1, y: 2}; let {z} = {z: 3}", "line 1: destructuring is not supported");
    EXPECT_LOG("let x = 3, y = 4; let p = {x, y, 'q r': 1, 5: 'five'}; console.log(p.x + p.y, p['q r'], p[5])", "7 1 five");
    EXPECT_LOG("let o = {f: function(a) { return a * 2; }, g: x => x + 1}; console.log(o.f(4), o.g(1))", "8 2");
    EXPECT_LOG("let a = [3, 1, 2]; a.sort(); let b = [10, 9, 100].sort((x, y) => x - y); console.log(a.join(), b.join())", "1,2,3 9,10,100");
    EXPECT_LOG("let a = [1,2,3,4]; console.log(a.map(x => x * 2).join(), a.filter(x => x % 2).join(), a.reduce((s, x) => s + x, 0), a.reduce((s, x) => s * x))", "2,4,6,8 1,3 10 24");
    EXPECT_LOG("let a = [5,6,7]; console.log(a.indexOf(6), a.includes(9), a.find(x => x > 5), a.findIndex(x => x > 6), a.some(x => x > 6), a.every(x => x > 4))", "1 false 6 2 true true");
    EXPECT_LOG("let a = [1,2,3,4,5]; let r = a.splice(1, 2, 'x'); console.log(r.join(), a.join(), a.slice(-2).join(), a.slice(1, 2).join())", "2,3 1,x,4,5 4,5 x");
    EXPECT_LOG("let a = [1,2]; a.unshift(0); let f = a.shift(); a.push(3, 4); let l = a.pop(); console.log(f, l, a.join(), a.reverse().join())", "0 4 1,2,3 3,2,1");
    EXPECT_LOG("let g = new Array(3).fill(0); let h = Array(2); console.log(g.join(), h.length, h[0])", "0,0,0 2 undefined");
    EXPECT_LOG("let grid = []; for (let y = 0; y < 3; y++) { grid.push([]); for (let x = 0; x < 3; x++) grid[y].push(x + y); } console.log(grid[2][1], grid.length)", "3 3");
    EXPECT_LOG("let s = 'Hello'; console.log(s.length, s[1], s.charAt(4), s.toUpperCase(), s.slice(1, 3), s.substring(3, 1), s.indexOf('l'), s.includes('lo'))", "5 e o HELLO el el 2 true");
    EXPECT_LOG("console.log('7'.padStart(3, '0'), 'ab'.padEnd(4, '.'), 'ab'.repeat(3), ' x '.trim(), 'a,b,c'.split(',').length, 'abc'.split('').join('-'))", "007 ab.. ababab x 3 a-b-c");
    EXPECT_LOG("console.log((3.14159).toFixed(2), (2).toFixed(1), (-0.5).toFixed(0), (12.5).toString(), String(42), Number('3.5'), parseInt('12px'))", "3.14 2.0 -1 12.5 42 3.5 12");
    EXPECT_LOG("console.log(Math.floor(-1.5), Math.ceil(1.2), Math.round(2.5), Math.abs(-3), Math.max(1, 5, 3), Math.min(4, 2), Math.sign(-9), Math.trunc(-2.7))", "-2 2 3 3 5 2 -1 -2");
    EXPECT_LOG("console.log(Math.sqrt(16), Math.pow(2, 8), Math.hypot(3, 4), Math.round(Math.sin(Math.PI / 2) * 1000), Math.round(Math.cos(Math.PI) * 1000), Math.round(Math.atan2(1, 1) * 1000))", "4 256 5 1000 -1000 785");
    EXPECT_LOG("let r = Math.random(); let q = rnd(10); console.log(r >= 0 && r < 1, q >= 0 && q < 10, isNaN('x' * 2))", "true true true");
    EXPECT_LOG("// comment\n/* block\ncomment */ let a = 1 // trailing\nlet b = 2\nconsole.log(a + b)", "3");
    EXPECT_LOG("let a = [1, 2, 3,]; let o = {a: 1,}; console.log(a.length, o.a)", "3 1");
    EXPECT_LOG("Math.extra = [1]; console.log(Math.extra.length)", "1");
    EXPECT_LOG("let a = [1]; a.push(a); console.log(String(a).length > 0)", "true");
    EXPECT_LOG("console.log('tab\\there', \"q\\\"\", 'u\\u0041', 'x\\x42')", "tab\there q\" uA xB");
}

static void errors(void) {
    EXPECT_ERROR("let a = ;", "line 1:");
    EXPECT_ERROR("let x = 1;\nfoo();", "line 2: foo is not defined");
    EXPECT_ERROR("let o;\nconsole.log(o.x)", "line 2: cannot read 'x' of undefined");
    EXPECT_ERROR("let o = null; o.y = 1;", "cannot set a property of null");
    EXPECT_ERROR("function update() { while (true) {} }", "too slow");
    EXPECT_ERROR("function f() { return f(); } f();", "recursion");
    EXPECT_ERROR("class A {}", "classes are not supported");
    EXPECT_ERROR("let s = 'abc", "unterminated string");
    EXPECT_ERROR("let a = 5; a();", "not a function");
    EXPECT_ERROR("[].nope()", "arrays have no method 'nope'");
    EXPECT_ERROR("let a = []; while (true) a.push(new Array(500));", "out of memory");
    EXPECT_ERROR("let s = 'x'; while (true) s = s + s;", "too long");
    EXPECT_ERROR("break;", "break outside a loop");
    EXPECT_ERROR("return 1;", "return outside a function");
    EXPECT_ERROR("for (const k in {}) {}", "for...in is not supported");
    EXPECT_ERROR("let x = ((((((((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))))))));", "nested too deeply");
    EXPECT_ERROR("let o = new Map();", "only new Array");
    /* A runtime error stops the game for good. */
    KGame *g = load("let n = 0; function update() { n++; if (n == 2) missing(); }");
    CHECK(kgame_frame(g, 0) == 0);
    CHECK(kgame_frame(g, 0) != 0);
    CHECK(strstr(kgame_error(g), "missing is not defined"));
    CHECK(kgame_frame(g, 0) != 0);
    /* A small device stack: deep nesting stops with an error instead of overflowing. */
    g = kgame_create(memory, sizeof memory, 1);
    kgame_set_stack_limit(g, 4096);
    const char *deep = "function f(n) { return n ? [1].map(x => f(n - 1))[0] : 0; } function update() { f(5); }";
    kgame_load(g, deep, strlen(deep));
    CHECK(kgame_frame(g, 0) != 0 && strstr(kgame_error(g), "nested too deeply"));
    g = kgame_create(memory, sizeof memory, 1);
    kgame_set_stack_limit(g, 1);
    kgame_load(g, "let x = (((1)));", 16);
    CHECK(strstr(kgame_error(g), "nested too deeply") != NULL);
}

static void api(void) {
    KGame *g = load(
        "let x = 10, frames = 0;\n"
        "function update() { frames++; if (btn('right')) x++; if (btnp('a')) x += 100; }\n"
        "function draw() { cls(1); rect(x, 20, 4, 3, 8); pset(0, 0, 7); circ(80, 72, 5, 11); line(0, 143, 159, 143, 10); text('HI', 2, 100, 7); }\n");
    CHECK(g && !kgame_error(g)[0]);
    CHECK(kgame_frame(g, 0) == 0);
    CHECK(pixel(g, 0, 0) == 7 && pixel(g, 1, 1) == 1);
    CHECK(pixel(g, 10, 20) == 8 && pixel(g, 13, 22) == 8 && pixel(g, 14, 20) == 1);
    CHECK(pixel(g, 80, 72) == 11 && pixel(g, 85, 72) == 11 && pixel(g, 80, 78) == 1);
    CHECK(pixel(g, 0, 143) == 10 && pixel(g, 159, 143) == 10);
    CHECK(kgame_frame(g, KGAME_RIGHT) == 0);    /* x 11 */
    CHECK(pixel(g, 11, 20) == 8 && pixel(g, 10, 20) == 1);
    CHECK(kgame_frame(g, KGAME_RIGHT | KGAME_A) == 0);   /* held right +1, pressed a +100 */
    CHECK(kgame_frame(g, KGAME_RIGHT | KGAME_A) == 0);   /* a still held: not pressed again */
    CHECK(pixel(g, 113, 20) == 8);
    /* Drawing far off screen is clipped, never out of bounds. */
    g = load("function draw() { rect(-1000, -1000, 5000, 5000, 3); circ(-50, 500, 400, 4); line(-400, -400, 600, 600, 5); text('xyz', 158, 140, 6, 8); pset(1e9, -1e9, 2); }");
    CHECK(kgame_frame(g, 0) == 0);
    CHECK(pixel(g, 5, 5) == 5 || pixel(g, 5, 5) == 3 || pixel(g, 5, 5) == 4);
    /* save/load */
    g = load("function init() { save(load() + 5); }");
    float v = 0;
    CHECK(kgame_saved(g, &v) && v == 5);
    CHECK(!kgame_saved(g, &v));
    g = kgame_create(memory, sizeof memory, 1);
    kgame_set_saved(g, 41);
    const char *src = "let s = load(); function update() { if (s === 41) save(42); }";
    kgame_load(g, src, strlen(src));
    kgame_frame(g, 0);
    CHECK(kgame_saved(g, &v) && v == 42);
    /* time() advances one 30th per frame */
    g = load("let t = 0; function update() { t = time(); } function draw() { if (t > 0.9) cls(9); }");
    for (int i = 0; i < 31; i++) kgame_frame(g, 0);
    CHECK(pixel(g, 50, 50) == 9);
    /* palettes */
    CHECK(kgame_palette_rgb[8] == 0xff004d && kgame_palette565[7] != 0);
    CHECK(kgame_create(memory, 1000, 1) == NULL);
}

/* A typical model-written game, run for 20 seconds of frames with input. */
static const char *SNAKE =
    "// Snake\n"
    "const CELL = 8, COLS = 20, ROWS = 17;\n"
    "let snake, dir, nextDir, food, score, best, over, tick;\n"
    "function reset() {\n"
    "  snake = [{x: 10, y: 8}, {x: 9, y: 8}, {x: 8, y: 8}];\n"
    "  dir = {x: 1, y: 0}; nextDir = dir; score = 0; over = false; tick = 0;\n"
    "  placeFood();\n"
    "}\n"
    "function placeFood() {\n"
    "  do { food = {x: Math.floor(rnd(COLS)), y: Math.floor(rnd(ROWS))}; }\n"
    "  while (snake.some(s => s.x === food.x && s.y === food.y));\n"
    "}\n"
    "function init() { best = load(); reset(); }\n"
    "function update() {\n"
    "  if (over) { if (btnp('a')) reset(); return; }\n"
    "  if (btnp('up') && dir.y === 0) nextDir = {x: 0, y: -1};\n"
    "  else if (btnp('down') && dir.y === 0) nextDir = {x: 0, y: 1};\n"
    "  else if (btnp('left') && dir.x === 0) nextDir = {x: -1, y: 0};\n"
    "  else if (btnp('right') && dir.x === 0) nextDir = {x: 1, y: 0};\n"
    "  if (++tick % 5 !== 0) return;\n"
    "  dir = nextDir;\n"
    "  const head = {x: (snake[0].x + dir.x + COLS) % COLS, y: (snake[0].y + dir.y + ROWS) % ROWS};\n"
    "  if (snake.some(s => s.x === head.x && s.y === head.y)) { over = true; if (score > best) { best = score; save(best); } return; }\n"
    "  snake.unshift(head);\n"
    "  if (head.x === food.x && head.y === food.y) { score += 10; placeFood(); } else snake.pop();\n"
    "}\n"
    "function draw() {\n"
    "  cls(0);\n"
    "  rect(food.x * CELL + 1, food.y * CELL + 9, CELL - 2, CELL - 2, 8);\n"
    "  snake.forEach((s, i) => rect(s.x * CELL, s.y * CELL + 8, CELL - 1, CELL - 1, i === 0 ? 11 : 3));\n"
    "  rect(0, 0, WIDTH, 8, 1);\n"
    "  text(`SCORE ${score}`, 2, 1, 7);\n"
    "  text(`BEST ${best}`, WIDTH - text(`BEST ${best}`, 0, -10, 0) - 2, 1, 6);\n"
    "  if (over) { rect(30, 60, 100, 24, 2); text('GAME OVER', 53, 64, 7); text('A: RETRY', 56, 74, 6); }\n"
    "}\n";

static void games(void) {
    KGame *g = load(SNAKE);
    CHECK(g && !kgame_error(g)[0]);
    unsigned seq[] = {0, KGAME_UP, 0, KGAME_LEFT, 0, KGAME_DOWN, 0, KGAME_RIGHT, KGAME_A, 0};
    int ok = 1;
    for (int f = 0; f < 600 && ok; f++) ok = kgame_frame(g, seq[(f / 23) % 10]) == 0;
    if (!ok) printf("  snake error: %s\n", kgame_error(g));
    CHECK(ok);
    CHECK(pixel(g, 1, 1) == 1);  /* the status bar */
}

/* Mutation fuzz: random byte edits of real programs must never crash or hang. */
static void fuzz(void) {
    static const char *seeds[] = {
        "let a = [1, {b: 'c'}, `t${1}`]; function f(x, y) { return x ? y : [x]; } for (const q of a) f(q, a); switch (a.length) { case 3: break; default: a.push(1); }",
        NULL,
    };
    seeds[1] = SNAKE;
    unsigned rng = 12345;
    char buf[8192];
    for (int round = 0; round < 3000; round++) {
        const char *seed = seeds[round & 1];
        size_t n = strlen(seed);
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, seed, n);
        int edits = 1 + (int)((rng >> 8) % 6);
        for (int e = 0; e < edits; e++) {
            rng = rng * 1103515245u + 12345u;
            size_t at = (rng >> 4) % n;
            static const char pool[] = "(){}[];,.=+-*/%<>!&|?:`'\"$ \n0123456789abcxyzfunctionletforwhile=>";
            buf[at] = pool[(rng >> 16) % (sizeof pool - 1)];
        }
        buf[n] = '\0';
        KGame *g = kgame_create(memory, sizeof memory, rng);
        kgame_load(g, buf, n);
        for (int f = 0; f < 5 && !kgame_error(g)[0]; f++) kgame_frame(g, (unsigned)(rng >> 3) & 63);
    }
}

int main(void) {
    language();
    errors();
    api();
    games();
    fuzz();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("KortaMod games: language, errors, API, a full game and fuzzing passed\n");
    return 0;
}
