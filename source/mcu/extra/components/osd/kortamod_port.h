#pragma once
#include "osd_shared.h"
#include <stdbool.h>
#include <stdint.h>

void Korta_Poll(void);
void Korta_OnVolume(uint8_t percent);
bool Korta_IsPlaying(void);
/* Paint the shell into the panel DMA buffer. Called from the LVGL flush, which
 * is the only place that buffer is stable. Returns false when stock UI owns it. */
bool Korta_Paint(uint16_t *dst);
OSD_Result_t Korta_Draw(void *arg);
OSD_Result_t Korta_OnButton(Button_t button, ButtonState_t state, void *arg);
OSD_Result_t Korta_OnTransition(void *arg);
