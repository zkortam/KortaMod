#pragma once
#include "osd_shared.h"
#include <stdbool.h>
#include <stdint.h>

void Korta_Poll(void);
void Korta_OnVolume(uint8_t percent);
/* Paint the shell into the panel DMA buffer. Called from the LVGL flush, which
 * is the only place that buffer is stable. Returns false when stock UI owns it. */
bool Korta_Paint(uint16_t *dst);
OSD_Result_t Korta_Draw(void *arg);
OSD_Result_t Korta_OnButton(Button_t button, ButtonState_t state, void *arg);
OSD_Result_t Korta_OnTransition(void *arg);

/* Power task snapshots; the UI task remains the sole owner of shell state. */
/* Milliseconds until a timer or alarm needs the MCU awake; 0 when none is set. */
uint32_t Korta_WakeInMs(void);
/* True while a Menu toggle is being sent to the FPGA; sleeping would strand it. */
bool Korta_KeepAwake(void);
