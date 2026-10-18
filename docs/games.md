# KortaMod games

People describe a game on the companion site, a model (ChatGPT) writes it, the site
previews it, and **Install** puts it on the Chromatic as an app on Home.

## The language (KortaScript: a strict JavaScript subset)

Supported:
- `let`, `const`, `var`; numbers (32-bit float), strings, booleans, `null`, `undefined`,
  arrays `[1, 2]`, objects `{x: 1, y}` (shorthand ok), template literals `` `Score ${n}` ``.
- `function` declarations (hoisted) and expressions, arrow functions `(a, b) => a + b`,
  `x => { ... }`, closures.
- `if/else`, `for (;;)`, `for (const x of array)`, `while`, `do/while`, `switch`,
  `break`, `continue`, `return`, ternary `?:`.
- Operators: `+ - * / % **`, `++ --`, `= += -= *= /= %=`, `== != === !==` (no type
  coercion except `null == undefined`), `< > <= >=`, `&& || !`, `& | ^ ~ << >> >>>`, `typeof`.
- `obj.prop`, `obj["key"]`, `arr[i]`, `arr.length`, `str.length`.
- Array methods: `push pop shift unshift splice slice indexOf includes join reverse
  fill forEach map filter find findIndex some every reduce sort`.
- String methods: `charAt charCodeAt indexOf includes slice substring toUpperCase
  toLowerCase padStart padEnd repeat split trim`; numbers: `toFixed`, `toString`.
- `Math.floor ceil round trunc abs sign min max sqrt pow sin cos tan atan atan2 hypot random PI`,
  `String(x)`, `Number(x)`, `parseInt(s)`, `isNaN(x)`, `Array(n)` / `new Array(n)`,
  `console.log(...)` (shown in the preview).

Not supported: classes, `this`, `new` (except `new Array`), spread/rest, destructuring,
getters/setters, `async`/`await`, `try/catch`, regular expressions, `Date`, `Map`/`Set`,
labels, `with`, `delete`, `in`, optional chaining.

Limits: 16,000 characters of source; about 20 KB of game memory; each frame must
finish within a fixed instruction budget (no endless loops); 64 nested calls.
Errors stop the game and show `line N: message`.

## The game API

Screen 160 x 144, origin top-left, 16 colours (PICO-8 palette):
`0 black, 1 dark blue, 2 dark purple, 3 dark green, 4 brown, 5 dark grey, 6 light grey,
7 white, 8 red, 9 orange, 10 yellow, 11 green, 12 blue, 13 lavender, 14 pink, 15 peach`.

| Call | Meaning |
| --- | --- |
| `init()` | optional, once at start |
| `update()` | optional, 30 times a second: game logic |
| `draw()` | optional, 30 times a second after update: drawing |
| `cls(c = 0)` | clear the screen |
| `pset(x, y, c)`, `pget(x, y)` | one pixel |
| `rect(x, y, w, h, c)` / `rectb(...)` | filled / outlined rectangle |
| `circ(x, y, r, c)` / `circb(...)` | filled / outlined circle |
| `line(x0, y0, x1, y1, c)` | line |
| `text(s, x, y, c, scale = 1)` | 5x7 font, 6 px per character at scale 1; returns the width |
| `btn(name)` | true while held; name is `"left" "right" "up" "down" "a" "b"` |
| `btnp(name)` | true only on the frame it was pressed |
| `rnd(n)` | random number in [0, n) |
| `time()` | seconds since the game started |
| `save(n)`, `load()` | keep one number (e.g. a high score) across plays; `load()` is 0 at first |
| `WIDTH`, `HEIGHT` | 160, 144 |

Start pauses the game (Resume / Quit); games never see it. Coordinates are floored.

## The model prompt (site)

The site sends this specification plus the person's idea and asks for one
self-contained program in a single code block, no explanations. Compile or runtime
errors from the preview are sent back automatically for a fix.

## WebAssembly build (site preview)

The site's `kgame.wasm` is the same C engine (`source/mcu/extra/components/osd/kortamod_game.c`) built with
`source/mcu/build_wasm.sh`. Exports:

| Export | Use |
| --- | --- |
| `memory` | the module's memory |
| `kg_init(seed) -> 0/1` | create the engine (call before each load) |
| `kg_source() -> ptr` | 16,384-byte buffer: write UTF-8 source here |
| `kg_load(len) -> 0/1` | compile and run top level + init(); 0 = ok |
| `kg_frame(buttons) -> 0/1` | one frame; buttons: left 1, right 2, up 4, down 8, a 16, b 32 |
| `kg_error() -> ptr` | NUL-terminated error text |
| `kg_pixels() -> ptr` | 160*144/2 bytes, 4-bit colour indices, high nibble = left pixel |
| `kg_palette() -> ptr` | 16 x uint32 0x00RRGGBB |
| `kg_log() -> ptr` | NUL-terminated console.log output since the last call |
| `kg_saved() -> f32`, `kg_set_saved(f32)` | the saved number |

## Installing (cloud protocol)

- `POST /api/games` (browser token) `{name (1-12 chars), source}` stores the source
  for an hour under `km:game:{gid}` (gid: 16 hex) and queues a device command
  `{kind: "game", op: "install", gid, name, size, sha256}`; reply as other commands.
- `DELETE /api/games` `{slot}` (0-31) queues `{kind: "game", op: "delete", slot}`.
- The device downloads `GET /api/device/game?gid=...` with its own credentials
  (`X-Device-Id`, `Authorization: Bearer <secret>`), gets `text/plain` source, checks
  size and SHA-256, writes it to a free slot of its `games` flash partition, and answers
  the command. Results: 0 ok, 2 busy (a game is running), 3 invalid/too big/no space,
  5 storage missing (needs the installer's new flash layout).
- The device reports `state.games = [{slot, name, size}]` (up to 32).

On the Chromatic installed games appear on Home after the built-in apps.

## Site implementation (findings)

The site's Games tab offers three ways to have a model write a game.
The prompt is `SPEC` in that file, derived from this document.

- **ChatGPT (copy and paste):** copies the full prompt to the clipboard and opens
  chatgpt.com in a new tab. The person pastes ChatGPT's reply back and the site runs
  the code block it contains. Works with any ChatGPT plan, including free. A `?q=`
  prefill URL isn't documented by OpenAI, so it isn't used. Errors become a
  "Copy fix request" for the same chat.
- **API key:** the person's OpenAI API key is kept only in their browser
  (localStorage) and sent only to `https://api.openai.com/v1/responses`, called
  directly from the page (`connect-src` allows it). The model is editable; the
  default is `gpt-6.1-sol` (OpenAI's Codex default as of October 2026).
- **ChatGPT plan ("Sign in with ChatGPT"):** OpenAI's open-source dynamic-client flow
  ([sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in),
  [models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference),
  [accounts and sessions](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions),
  [errors](https://developers.openai.com/siwc/token-sharing-open-source/errors-and-recovery),
  [limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations)).
  - The redirect must be `http://127.0.0.1:<port>/auth/callback`, which a website
    can't receive, so the person pastes the address of the page that fails to load.
  - OpenAI asks that tokens stay out of browser storage, so the site's relay
    keeps them in an AES-GCM-encrypted, HttpOnly, SameSite=Strict cookie scoped to
    `/api/openai` and stores nothing server-side. The key derives from
    `SIWC_COOKIE_KEY` or, failing that, the database token.
  - The function exchanges the code, verifies the ID token against OpenAI's JWKS
    (issuer, audience, expiry, nonce), requires the `chatgpt.tokens.use.direct`
    scope, refreshes (refresh tokens rotate), lists models (`visibility == "list"`),
    streams `/v1/responses` with `store: false, stream: true`, maps the
    `subscription_sharing_*` errors, and revokes on sign-out.
  - **Caveat:** OpenAI documents this flow for open-source and locally hosted apps
    and points paid or remotely hosted apps to an interest form
    ([overview](https://developers.openai.com/siwc/token-sharing-open-source)). For
    a proper hosted "Sign in with ChatGPT" button, request an OAuth client
    ([on your website](https://developers.openai.com/siwc/website)).
- **Testing and auto-fix:** every generated program first runs headless in the
  WebAssembly engine for 180 frames: 90 idle, then 90 with pseudo-random buttons. In
  the API key and plan modes, a compile or runtime error goes back to the model
  with the engine's message, up to twice. The preview runs at 30 fps with arrows and
  Z/X (A/B), plus on-screen buttons on touch screens.
- **Deploying:** generation with reasoning models can take longer than the 45 s
  `maxDuration` set for `api/**`; `api/openai.js` should get up to 300 s. Vercel's
  Hobby plan allows 12 functions per deployment.
