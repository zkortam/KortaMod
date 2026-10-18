# Phone link (companion site)

Anyone running KortaMod can manage their Chromatic from **https://kortamod.app** on any phone or computer: notes (typed with a real keyboard), alarm, clock, battery and versions. No app install and no account. The phone and the Chromatic do not need to share a network; the Chromatic only needs Wi-Fi with internet.

## Using it

1. On the Chromatic: **Home → Phone**, switch **Phone link** on. This also turns Wi-Fi on; pick a network in Settings → Wi-Fi if none is saved.
2. When it shows **kortamod.app** and a 6-digit code, open the site and enter the code.
3. The browser stays paired until it is unpaired, or **Forget phones** on the Chromatic revokes every browser.

## How it works

```
phone/laptop browser ──HTTPS──> kortamod.app (Vercel functions) <──HTTPS── Chromatic
                                      │
                                Upstash Redis (state, codes, tokens, command queue)
```

- **Device** (`native/kortamod_web.c`): while the link is on and Wi-Fi is up, a task keeps one keep-alive HTTPS connection (certificate verified against the common CA bundle) and POSTs `/api/device/sync` with its state and current pairing code. The site sets the pace in each reply (`poll_ms`): every 2 s while a browser has the page open, every 20 s when browsers are paired, every 4 minutes when none are. The device checks in every 2 s while its Phone screen is open and every 4 s for 10 minutes after showing a code, so a new pairing connects at once. Failures back off to 60 s. With the link off or offline it holds no connection and no TLS memory. Commands are handed to the UI task, which applies them and republishes before the result is reported.
- **Site** (`cloud/`): static page plus eleven functions. A browser write queues a command and waits for the result: up to 9 s if the device is checking in fast, else up to 29 s (the page shows a pulsing dot meanwhile, "Waking your Chromatic"). Results: 200, 409 (notes changed on the device), 423 (busy: a note open in the keyboard, a running timer, an alarm ringing), 400, 503 (device offline), 504 (no answer). Each command carries a deadline in server time; past it the device refuses it, so nothing is applied after the browser was told it failed.
- **Storage cost:** one Redis hash per device (`km:d:{id}`) holds its secret, state, code and flags. A routine check-in is 2 database commands (HGETALL, HSET); the queue is read only when a browser has flagged a command. An idle paired Chromatic uses about 260,000 commands a month, a lonely one about 45,000, and an open page about 5,000 an hour (device every 2 s, page every 4 s; hidden tabs stop). The Upstash free tier is 500,000; beyond it, pay-as-you-go is $0.20 per 100,000. `cloud/test/relay.test.js` asserts these budgets.
- **Text** is cleaned to what the 5x7 font shows (smart quotes, dashes, Latin-1 accents mapped; other characters become `?`), the same function in the page and on the device.

## Games

The Games tab writes games with ChatGPT, previews them in the browser with the
device's own engine (WebAssembly), and installs them on the Chromatic. The device
downloads the source itself from `/api/device/game` with its own credentials;
only the device a game was sent to can fetch it. See [games.md](games.md).

## Security model

- **Device identity:** on first use the Chromatic makes a random 64-bit id and 256-bit secret (not derived from its MAC) and keeps them in NVS. The site stores only the secret's SHA-256; the first sync registers it.
- **Pairing:** the code exists only on the Chromatic's screen and in the database, expires after 10 minutes, works once (the device then shows a new one), and collides safely (a device whose code is taken picks another). Pairing attempts are limited to 20 per IP per 10 minutes.
- **Browsers** get a random 256-bit token; the site stores only its hash, for 180 days. At most 8 browsers per device. Tokens travel in a header, not a cookie (no CSRF). **Forget phones** is persisted on the device until the site confirms it.
- **Transport:** HTTPS both ways, HSTS on the site, a strict Content-Security-Policy, no framing.
- **Data at rest:** the device's last state (notes, alarm, clock, battery, versions) is stored in Upstash Redis for this project, encrypted at rest by the provider. It is not end-to-end encrypted: whoever controls the Vercel project can read it.

## Deploying

```sh
cd cloud
npm install
npm test                       # relay logic against an in-memory store
vercel deploy --prod           # needs the Upstash for Redis integration linked to the project
```

The firmware's site address is `CLOUD_HOST` in `native/kortamod_web.c`.

## Tests against a real Chromatic

Both open the USB console (which resets the ESP32), turn the link on, read the code from `korta web`, and restore the device's notes and alarm afterwards.

```sh
PY=idf-tools/python_env/idf5.3_py3.14_env/bin/python
$PY cloud/test/device_e2e.py /dev/cu.usbmodem0123456783                    # API through the deployed site
$PY cloud/test/run_ui_device.py /dev/cu.usbmodem0123456783 <dir with puppeteer-core> [shots]  # phone-size browser
```

Console: `korta web` prints the link state, code, and last sync HTTP status (`0` = connection/TLS failure); `korta web on|off|forget`.
