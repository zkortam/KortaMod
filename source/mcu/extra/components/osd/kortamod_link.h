#pragma once
#include "kortamod_core.h"

enum {
    KORTA_LINK_ON = 1,
    KORTA_LINK_OFF,
    KORTA_LINK_SCAN,
    KORTA_LINK_JOIN,
    KORTA_LINK_FORGET
};

void KortaLink_Start(void);
void KortaLink_Command(int op, const char *ssid, const char *password);
void KortaLink_Snapshot(KortaLink *out);
bool KortaLink_RadioOn(void);
