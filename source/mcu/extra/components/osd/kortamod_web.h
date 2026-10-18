#pragma once
#include "kortamod_core.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>

/* Phone link: the device keeps an HTTPS connection to the companion site
 * (kortamod.app), reporting its state and pairing code and collecting
 * what paired browsers ask for. It never touches the UI state: the UI task
 * publishes a view and applies the commands it takes, through the core. */

/* What a paired browser may read. Published by the UI task. */
typedef struct {
    uint16_t rev;
    uint8_t count, pinned;     /* the first `pinned` notes are pinned */
    char notes[KORTA_NOTE_SLOTS][KORTA_NOTE_CAPACITY + 1];
    bool editing;              /* a note is open in the keyboard on the device */
    uint8_t alert;             /* 0 none, 1 an alarm rings, 2 the timer rings */
    char alert_label[KORTA_ALARM_LABEL + 1];
    KortaAlarm alarms[KORTA_ALARM_MAX];
    bool hour12;
    bool time_known;
    int16_t zone_minutes;
    bool battery_known, charging;
    uint8_t battery_pct;
    char mcu[20], fpga[12], chromatic[8];
    struct {
        uint8_t brightness, low_batt, player, palette;
        bool night, flow, hour12, silent, blend, cc_lcd, cc_usb, transit, dpad4, gbc;
    } settings;
    struct { bool countdown, running, done; uint16_t set_s; uint32_t shown_ms; } timer;
    struct { uint8_t phase; char ssid[33]; uint8_t count; KortaNet nets[KORTA_NET_MAX]; } wifi;  /* never a password */
    uint8_t game_count;
    struct { uint8_t slot; char name[KORTA_GAME_NAME + 1]; uint16_t size; } games[KORTA_GAME_MAX];
    bool games_ready;          /* the flash layout has a games partition */
    bool ota_available;        /* the flash layout can take updates from the site */
    uint8_t ota_phase, ota_percent;  /* KortaOtaPhase */
    char ota_error[48];
} KortaWebView;

typedef enum {
    KORTA_WEB_CMD_NOTES = 1, KORTA_WEB_CMD_ALARM, KORTA_WEB_CMD_CLOCK,
    KORTA_WEB_CMD_SETTING, KORTA_WEB_CMD_MESSAGE, KORTA_WEB_CMD_TIMER, KORTA_WEB_CMD_WIFI,
    KORTA_WEB_CMD_GAMES,     /* the installed games changed (the phone link wrote them): reload */
    KORTA_WEB_CMD_UPDATE     /* install signed firmware from url */
} KortaWebCmdKind;

/* One change requested by a browser, already cleaned to what the screen can show. */
typedef struct {
    KortaWebCmdKind kind;
    uint16_t rev;            /* notes revision the browser edited */
    uint8_t count, pinned;
    char notes[KORTA_NOTE_SLOTS][KORTA_NOTE_CAPACITY + 1];
    bool alarm_on;
    uint8_t alarm, hour, minute, days;    /* alarm index; label in text */
    uint32_t epoch;
    int16_t zone_minutes;
    int setting, value;                   /* KORTA_SET_* and its value */
    char text[KORTA_NOTE_CAPACITY + 1];   /* message or alarm label, cleaned */
    bool find;
    int timer_action, seconds;
    bool countdown;
    char ssid[33], password[64];
    char url[160], sha256[65], signature[100];
    uint32_t size;
} KortaWebCmd;

void KortaWeb_Init(nvs_handle_t nvs);
/* UI task, every poll: sync while enabled and the network is up; more often while
 * the Phone screen is shown. */
void KortaWeb_Service(bool enabled, bool online, bool screen);
/* UI task: fill the published view in place (no copy) between Begin and End. */
KortaWebView *KortaWeb_ViewBegin(void);
void KortaWeb_ViewEnd(void);
/* UI task: take a pending command, apply it, then report a KORTA_WEB_* result. */
/* The pending command, owned by the UI task until KortaWeb_Complete; NULL if none. */
KortaWebCmd *KortaWeb_TakeCommand(void);
void KortaWeb_Complete(int result);
/* Fills the web_* fields of the link shown on the Phone screen. */
void KortaWeb_Status(KortaLink *link);
void KortaWeb_ForgetAll(void);
/* Last sync outcome for diagnostics: HTTP status, or 0 for a connection/TLS failure. */
int KortaWeb_LastStatus(void);
/* The last game install problem, if any (console diagnostics). */
const char *KortaWeb_GameDiag(void);
