# Firmware updates over the air

KortaMod updates itself from the companion site. Signed images, a staging slot, an automatic
fall-back, and the factory slot always holding the current firmware, so ModRetro's own updater
and the USB restore keep working.

## Flash layout

`oss-chromatic-console-mcu-v4.2/partitions.csv` (selected by `CONFIG_PARTITION_TABLE_CUSTOM`):

| Name | Type | Offset | Size | Use |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB | settings (unchanged from stock) |
| phy_init | data/phy | 0xf000 | 4 KB | unchanged |
| factory | app | 0x10000 | 1 MB | the firmware that normally runs (unchanged) |
| otadata | data/ota | 0x110000 | 8 KB | which app the bootloader starts; erased = factory |
| ota_0 | app | 0x120000 | 1 MB | staging slot for a downloaded update |
| games | data/0x40 | 0x220000 | 512 KB | installed games |

Stock firmware leaves everything from 0x110000 empty (checked on two full backups). The
bootloader is ESP-IDF v5.3's: stock ModRetro's (Dec 22 2025) and the one now on the unit
(Oct 16 2026) both contain the otadata selection code. Rollback in the bootloader is off
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` unset), so KortaMod does its own fall-back.

The USB installer moves a stock unit to this layout once (after its full backup): it writes the
new table at 0x8000, clears otadata and the games area, and writes the app at 0x10000; NVS is not
touched. On a unit that already has the layout it clears otadata (so the newly written factory
app is what boots) and leaves games alone. Restoring a backup writes the original table back.

## How an update runs

1. The site offers an update when `GET /api/firmware?current=<version>` says `newer`.
2. The device gets a command with the manifest: `{url, size, sha256, signature}`.
3. `KortaOta_Begin` downloads into ota_0 over HTTPS, refusing anything whose embedded version is
   not newer than the running one, then checks SHA-256 and the ECDSA P-256 signature (public key
   compiled into `kortamod_ota.c`), validates the image (`esp_ota_end`), selects ota_0 and restarts.
   Any failure leaves the running firmware selected.
4. The new image boots from ota_0. `KortaOta_EarlyBoot` (first line of `app_main`) counts boots in
   RTC memory; after three resets that never reached `KortaOta_MarkHealthy`, it selects factory (the
   previous firmware, untouched) and restarts.
5. After ~10 s of normal running, `KortaOta_MarkHealthy` copies ota_0 into factory a sector at a
   time, compares both SHA-256s, selects factory (erasing otadata) and restarts. A power cut
   during the copy is safe: otadata still selects ota_0, which boots and copies again.

Steady state: otadata empty, factory = current KortaMod. ModRetro's updater writing factory works
as before.

## Publishing a release

```sh
# bump CONFIG_APP_PROJECT_VER (sdkconfig.defaults + sdkconfig), build, then:
idf-tools/python_env/idf5.3_py3.14_env/bin/python scripts/sign_firmware.py \
    oss-chromatic-console-mcu-v4.2/build/mcu_fw.bin 0.13.4-korta8 --notes "Games"
cd cloud && vercel deploy --prod
```

`sign_firmware.py` checks the image's embedded version, copies it to the site's `fw/<version>.bin`
and writes `fw/latest.json`. The private key is `~/.kortamod/ota-signing-key.pem`
(mode 600, never committed). **Back it up somewhere safe**: without it no further OTA updates can
be signed for units running this firmware (they would need the USB installer). If it leaks,
publish a firmware with a new key via the USB installer.

`GET /api/firmware[?current=0.13.4-kortaN]` serves the validated manifest (404 if none is
published); `newer` is only true for KortaMod versions (stock 0.13.4 needs the USB installer).

## Integration (to wire into core/port/web and the site)

### Device: command parsing (`kortamod_web.h` / `kortamod_web.c`)

```c
/* kortamod_web.h */
typedef enum { ..., KORTA_WEB_CMD_WIFI, KORTA_WEB_CMD_UPDATE } KortaWebCmdKind;
/* in KortaWebCmd */
    char url[160], sha256[65], signature[100];
    uint32_t size;

/* kortamod_web.c, parse_command(), next to "wifi" */
if (strcmp(kind->valuestring, "update") == 0) {
    const cJSON *url = cJSON_GetObjectItem(c, "url"), *size = cJSON_GetObjectItem(c, "size");
    const cJSON *sha = cJSON_GetObjectItem(c, "sha256"), *sig = cJSON_GetObjectItem(c, "signature");
    if (!cJSON_IsString(url) || !cJSON_IsNumber(size) || !cJSON_IsString(sha) || !cJSON_IsString(sig)
        || strlen(url->valuestring) >= sizeof out->url || strlen(sha->valuestring) != 64
        || strlen(sig->valuestring) >= sizeof out->signature || size->valuedouble <= 0 || size->valuedouble > 0x100000)
        return false;
    out->kind = KORTA_WEB_CMD_UPDATE;
    snprintf(out->url, sizeof out->url, "%s", url->valuestring);
    snprintf(out->sha256, sizeof out->sha256, "%s", sha->valuestring);
    snprintf(out->signature, sizeof out->signature, "%s", sig->valuestring);
    out->size = (uint32_t)size->valuedouble;
    return true;
}
```

State for the site (in `build_sync`, inside the `device` object; publish the values from the port):

```c
cJSON_AddBoolToObject(device, "ota", view.ota_available);          /* KortaOta_Available() */
if (view.ota_phase) {                                                /* KortaOta_Status() */
    cJSON *u = cJSON_AddObjectToObject(state, "update");
    static const char *const phases[] = {"idle", "download", "verify", "restart", "finish", "failed"};
    cJSON_AddStringToObject(u, "phase", phases[view.ota_phase]);
    cJSON_AddNumberToObject(u, "percent", view.ota_percent);
    if (view.ota_error[0]) cJSON_AddStringToObject(u, "error", view.ota_error);
}
```

### Device: port (`kortamod_port.c`)

Start the download a moment after answering, like the remote Wi-Fi join, so the result reaches
the site first; then pause the phone link so its TLS session doesn't compete for memory with the
download's (52 KB free heap with one session open).

```c
#include "kortamod_ota.h"
static uint32_t update_at;            /* start KortaOta_Begin at this time (ms), 0 = none */
static KortaWebCmd update_cmd;

/* service_phone_link(), with the other kinds */
} else if (took && cmd.kind == KORTA_WEB_CMD_UPDATE) {
    if (!KortaOta_Available()) result = 5;                 /* needs the USB installer's layout */
    else if (KortaOta_Status().phase != KORTA_OTA_IDLE && KortaOta_Status().phase != KORTA_OTA_FAILED)
        result = KORTA_WEB_BUSY;
    else if (!(state.hw.charging || (state.hw.battery_known && state.hw.battery_pct >= 30)))
        result = KORTA_WEB_BUSY;                           /* say "charge first" on the site */
    else {
        update_cmd = cmd;
        update_at = now_ms() + 2000u; if (!update_at) update_at = 1;
        result = KORTA_WEB_OK;
    }

/* each poll */
if (update_at && (int32_t)(now_ms() - update_at) >= 0) {
    update_at = 0;
    bool power_ok = state.hw.charging || (state.hw.battery_known && state.hw.battery_pct >= 30);
    KortaOta_Begin(update_cmd.url, update_cmd.size, update_cmd.sha256, update_cmd.signature, power_ok);
}
KortaOtaStatus ota = KortaOta_Status();
bool ota_busy = ota.phase == KORTA_OTA_DOWNLOAD || ota.phase == KORTA_OTA_VERIFY;
KortaWeb_Service(state.web_keep && !ota_busy, link->phase == KORTA_WIFI_ONLINE, state.view == KORTA_PHONE);

/* once, after the firmware has run normally for 10 s (Korta_Poll) */
static bool healthy;
if (!healthy && esp_timer_get_time() > 10 * 1000000LL) { healthy = true; KortaOta_MarkHealthy(); }
```

Keep the Chromatic awake (`Korta_KeepAwake`) while `ota.phase` is DOWNLOAD, VERIFY or FINISH,
and keep Wi-Fi on (don't park the radio) during DOWNLOAD.

UI: while `ota.phase` is DOWNLOAD/VERIFY/RESTART/FINISH show a full-screen view, e.g.
"UPDATING 45%" (DOWNLOAD), "CHECKING" (VERIFY), "RESTARTING", "FINISHING 60%" (FINISH, after the
first restart; the unit restarts once more by itself), with "DON'T TURN OFF". On FAILED show
`ota.error` briefly (e.g. through `korta_web_message`). When `KortaOta_Pending()` at boot, the
FINISH screen starts as soon as `MarkHealthy` runs.

### Site

The site's update endpoint (browser token), queueing the published manifest:

```js
// Installs the newest signed firmware on the Chromatic.
import { browserDevice, deviceStatus, command, replyFor, json, fail, guarded } from "./_lib.js";
import { valid, build } from "./firmware.js";

export const POST = guarded(async request => {
  const who = await browserDevice(request, { renew: true });
  if (!who) return fail(401, "Not paired.");
  if (!deviceStatus(who.dev).online) return fail(503, "Your Chromatic is offline. Turn on Phone link and Wi-Fi.");
  const r = await fetch(new URL("/fw/latest.json", request.url), { cache: "no-store" });
  const m = r.ok ? await r.json() : null;
  if (!valid(m)) return fail(404, "No firmware update is published.");
  let current = "";
  try { current = (JSON.parse(who.dev.state || "{}").device || {}).mcu || ""; } catch {}
  if (build(m.version) <= build(current.replace(/^v/, ""))) return fail(409, "Already up to date.");
  const result = await command(who.id, who.dev, "update", { url: m.url, size: m.size, sha256: m.sha256, signature: m.signature });
  if (result === 5) return fail(409, "Run the USB installer once to enable updates from the site.");
  if (result === 2) return fail(423, "Charge your Chromatic (30% or plugged in) and try again.");
  return replyFor(result);
});
```

(`device.mcu` is reported as e.g. `v0.13.4-korta7`; strip the `v`.) In `_lib.js` `replyFor`, map
result 5 if used elsewhere.

Device tab: on load, `GET /api/firmware?current=<device.mcu without v>`; when `newer`, show a row
"Update to korta8" + notes and an "Update" button (disabled unless `device.ota`; otherwise say
"Run the USB installer once to enable updates"). After POST, poll state and show
`state.update.phase/percent`; the Chromatic goes offline twice briefly (two restarts), then reports
the new `device.mcu`.

Sync could also advertise the update without the site asking: in `device/sync.js`, if the reported
`device.mcu` is older than `latest.json`, add `update_available: "<version>"` to the reply; not
required, since the site drives updates.

## Risks and limits

- **Signing key loss/leak:** see above. The key is only on this Mac.
- **Brick window:** none during download or copy (otadata always selects a complete, verified
  image). The remaining risk is an image that passes signature checks but hangs *without*
  resetting before `MarkHealthy`: the boot counter only counts resets. A task watchdog panic
  counts; a silent hang needs a power cycle, which counts too (RTC memory survives the panic path,
  not a power cut: after a cut the counter restarts, so a pure hang-on-boot image needs three
  resets of the panic/watchdog kind). Keep the 10 s healthy delay short and the call early.
- **Deep sleep** resets would count as boots; KortaMod uses light sleep, so this does not apply
  unless that changes.
- **Downgrades** are refused (embedded version must be newer), so a compromised site can't push an
  older signed image; it can withhold updates.
- **ModRetro updater after an OTA:** factory holds KortaMod and otadata is empty, so their updater
  replaces factory normally. If their updater runs while an update is pending in ota_0 (between the
  two restarts, ~30 s), otadata would still select ota_0; the next KortaMod USB install clears it.
- Size: the module is ~3.7 KB of code; with esp_image_verify and the OTA APIs linked expect up to
  ~10 KB once wired. App is 0xdb590 bytes today (14% of 1 MB free).
