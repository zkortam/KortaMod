#pragma once
/* Signed firmware updates from the companion site.
 *
 * The flash layout from the installer adds a staging slot (ota_0) next to the
 * factory app. An update downloads into ota_0, is checked against its SHA-256
 * and an ECDSA P-256 signature, and the Chromatic restarts into it. Once that
 * image has run normally it copies itself back into factory, verifies the copy
 * and boots factory again, so factory always holds the current firmware and
 * ModRetro's own updater keeps working. An image in ota_0 that keeps resetting
 * before it is marked healthy is abandoned for factory (the previous version).
 *
 * On the stock partition table (no ota_0) everything here refuses cleanly. */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KORTA_OTA_IDLE = 0,
    KORTA_OTA_DOWNLOAD,   /* fetching into ota_0; percent is valid */
    KORTA_OTA_VERIFY,     /* checking hash, signature and image */
    KORTA_OTA_RESTART,    /* about to restart into the new image */
    KORTA_OTA_FINISH,     /* running the new image: copying it into factory; percent valid */
    KORTA_OTA_FAILED,     /* error holds why; the running firmware is untouched */
} KortaOtaPhase;

typedef struct {
    KortaOtaPhase phase;
    uint8_t percent;
    char error[48];
} KortaOtaStatus;

/* First thing in app_main: counts boots of an unconfirmed ota_0 image and falls
 * back to factory after three that never reached KortaOta_MarkHealthy. */
void KortaOta_EarlyBoot(void);
/* The installer's layout (otadata + ota_0) is present. */
bool KortaOta_Available(void);
/* Running from ota_0: an update is waiting to be confirmed and copied. */
bool KortaOta_Pending(void);
/* Call once the firmware has run normally for a while (about 10 s). When
 * running from ota_0 this copies the image into factory in the background and
 * restarts into it; otherwise it does nothing. */
void KortaOta_MarkHealthy(void);
/* Starts an update in its own task. url must be https; sha256_hex is 64 hex
 * characters; signature_b64 is the base64 DER ECDSA signature of the image's
 * SHA-256. power_ok: charging or enough battery to finish. Returns 0 if
 * started, otherwise a reason (see KortaOta_Status().error). */
int KortaOta_Begin(const char *url, uint32_t size, const char *sha256_hex, const char *signature_b64, bool power_ok);
KortaOtaStatus KortaOta_Status(void);
