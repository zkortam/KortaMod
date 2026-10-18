#include "kortamod_sfx.h"
#include "kortamod_core.h"

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

/*
 * Stock FPGA: I2S_BCLK (GPIO33) is menuDisabled, not a clock.
 * WS/DIN/DOUT are unused FPGA inputs. MCU is I2S master on those three.
 * FPGA mcu_sfx_i2s mixes this into the TLV320 path.
 */
#define SFX_BCLK GPIO_NUM_26
#define SFX_WS   GPIO_NUM_25
#define SFX_DOUT GPIO_NUM_27

enum { SR = 32768, CHUNK = 128, MAX_HITS = 8 };

typedef struct {
    uint16_t hz;
    uint16_t start;
    uint16_t dur;
    uint8_t amp;
    uint8_t bell;
} Hit;

static const int8_t SINE[256] = {
    0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,
    90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,
    127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92,
    90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
    0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,-49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,
    -90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,-117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,
    -127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,-117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92,
    -90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3
};

static i2s_chan_handle_t tx;
static TaskHandle_t task;
/* Stock FPGA 18.8 has no PCM mixer. Enable only with a validated FPGA build. */
#ifndef KORTA_EXPERIMENTAL_AUDIO
#define KORTA_EXPERIMENTAL_AUDIO 0
#endif
static uint32_t request;
static bool ready;

static uint32_t step_hz(uint32_t hz) {
    return (uint32_t)(((uint64_t)hz << 32) / (uint32_t)SR);
}

static void recipe(uint8_t kind, Hit *hits, int *nh, int *total) {
    switch (kind) {
        case KORTA_SFX_MOVE:
            hits[0] = (Hit){1760, 0, 1700, 90, 1};
            hits[1] = (Hit){2637, 180, 900, 40, 1};
            *nh = 2; *total = 1900;
            break;
        case KORTA_SFX_OK:
            hits[0] = (Hit){1047, 0, 5600, 90, 1};
            hits[1] = (Hit){1319, 580, 5200, 78, 1};
            hits[2] = (Hit){1568, 1100, 4600, 64, 1};
            hits[3] = (Hit){2093, 1650, 3000, 36, 1};
            *nh = 4; *total = 6200;
            break;
        case KORTA_SFX_BACK:
            hits[0] = (Hit){784, 0, 4200, 86, 1};
            hits[1] = (Hit){523, 1500, 5000, 80, 1};
            *nh = 2; *total = 6800;
            break;
        case KORTA_SFX_OPEN:
            hits[0] = (Hit){523, 0, 5200, 80, 1};
            hits[1] = (Hit){659, 1100, 5200, 84, 1};
            hits[2] = (Hit){784, 2200, 5200, 88, 1};
            hits[3] = (Hit){1047, 3300, 6400, 92, 1};
            *nh = 4; *total = 9800;
            break;
        case KORTA_SFX_TOGGLE:
            hits[0] = (Hit){880, 0, 3000, 82, 1};
            hits[1] = (Hit){1320, 0, 2400, 50, 1};
            *nh = 2; *total = 3200;
            break;
        case KORTA_SFX_DENY:
            hits[0] = (Hit){196, 0, 5000, 70, 0};
            hits[1] = (Hit){147, 800, 4200, 50, 0};
            *nh = 2; *total = 5600;
            break;
        case KORTA_SFX_ALERT:
            hits[0] = (Hit){784, 0, 4800, 86, 1};
            hits[1] = (Hit){1047, 1600, 4800, 90, 1};
            hits[2] = (Hit){1319, 3200, 5200, 90, 1};
            hits[3] = (Hit){1568, 5000, 7000, 94, 1};
            hits[4] = (Hit){2093, 6200, 4000, 48, 1};
            *nh = 5; *total = 12000;
            break;
        case KORTA_SFX_TICK:
            hits[0] = (Hit){3136, 0, 720, 72, 1};
            *nh = 1; *total = 800;
            break;
        case KORTA_SFX_TYPE:
            hits[0] = (Hit){2093, 0, 520, 48, 1};
            hits[1] = (Hit){3136, 80, 280, 20, 1};
            *nh = 2; *total = 600;
            break;
        case KORTA_SFX_DICE:
            /* Clicks that slow down over the 640 ms roll, then a low thud. */
            hits[0] = (Hit){1400, 0, 700, 70, 0};
            hits[1] = (Hit){1100, 2200, 700, 66, 0};
            hits[2] = (Hit){1650, 4800, 700, 62, 0};
            hits[3] = (Hit){1250, 7800, 700, 58, 0};
            hits[4] = (Hit){1500, 11400, 700, 54, 0};
            hits[5] = (Hit){1000, 15600, 800, 50, 0};
            hits[6] = (Hit){330, 20200, 2600, 80, 0};
            *nh = 7; *total = 23000;
            break;
        case KORTA_SFX_COIN:
            /* A quiet shimmer while it spins, then the landing ring. */
            hits[0] = (Hit){2637, 0, 1800, 30, 1};
            hits[1] = (Hit){3136, 4000, 1800, 28, 1};
            hits[2] = (Hit){2637, 8000, 1800, 26, 1};
            hits[3] = (Hit){3136, 12000, 1800, 24, 1};
            hits[4] = (Hit){2637, 20400, 9000, 82, 1};
            hits[5] = (Hit){3951, 20400, 6000, 40, 1};
            *nh = 6; *total = 29500;
            break;
        default:
            *nh = 0; *total = 0;
            break;
    }
}

static void mix_chunk(int32_t *mix, int pos, int n, const Hit *hits, int nh) {
    memset(mix, 0, (size_t)n * sizeof *mix);
    for (int h = 0; h < nh; h++) {
        int start = hits[h].start;
        int dur = hits[h].dur;
        if (pos + n <= start || pos >= start + dur) continue;
        uint32_t ph = 0, ph2 = 0;
        uint32_t st = step_hz(hits[h].hz);
        uint32_t st2 = step_hz((uint32_t)hits[h].hz * 2u + hits[h].hz / 40u);
        if (pos > start) {
            uint32_t skip = (uint32_t)(pos - start);
            ph = st * skip;
            ph2 = st2 * skip;
        }
        for (int i = 0; i < n; i++) {
            int t = pos + i - start;
            if (t < 0 || t >= dur) {
                ph += st;
                ph2 += st2;
                continue;
            }
            int32_t env = (int32_t)(dur - t) * (dur - t) / dur;
            int32_t s;
            if (hits[h].bell) {
                ph += st;
                ph2 += st2;
                s = (int32_t)SINE[ph >> 24] * 3 + (int32_t)SINE[ph2 >> 24];
            } else {
                ph += st;
                s = ((int32_t)SINE[ph >> 24] > 0) ? 90 : -90;
            }
            mix[i] += s * (int32_t)hits[h].amp * env / (dur * 2);
        }
    }
}

static bool open_i2s(void) {
    if (tx) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
        tx = NULL;
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    if (i2s_new_channel(&chan, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SFX_BCLK,
            .ws = SFX_WS,
            .dout = SFX_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK) {
        i2s_del_channel(tx);
        tx = NULL;
        return false;
    }
    if (i2s_channel_enable(tx) != ESP_OK) {
        i2s_del_channel(tx);
        tx = NULL;
        return false;
    }
    return true;
}

static void write_pcm(const int16_t *pcm, size_t frames) {
    size_t wrote = 0;
    (void)i2s_channel_write(tx, pcm, frames * 4u, &wrote, portMAX_DELAY);
}

static void sfx_task(void *arg) {
    (void)arg;
    if (!open_i2s()) { vTaskDelete(NULL); return; }
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
    int16_t pcm[CHUNK * 2];
    int32_t mix[CHUNK];
    Hit hits[MAX_HITS];
    int pos = 0, total = 0, nh = 0;
    uint8_t playing = 0, vol = 100;
    for (;;) {
        uint32_t next = __atomic_exchange_n(&request, 0, __ATOMIC_ACQ_REL);
        uint8_t kind = (uint8_t)next;
        if (kind) {
            vol = (uint8_t)(next >> 8);
            recipe(kind, hits, &nh, &total);
            pos = 0;
            playing = vol && total > 0;
        }
        if (playing) {
            int n = total - pos;
            if (n > CHUNK) n = CHUNK;
            mix_chunk(mix, pos, n, hits, nh);
            for (int i = 0; i < n; i++) {
                int32_t s = mix[i] * (int32_t)vol / 100;
                if (s > 28000) s = 28000;
                if (s < -28000) s = -28000;
                pcm[i * 2] = (int16_t)s;
                pcm[i * 2 + 1] = (int16_t)s;
            }
            for (int i = n; i < CHUNK; i++) pcm[i * 2] = pcm[i * 2 + 1] = 0;
            write_pcm(pcm, CHUNK);
            pos += n;
            if (pos >= total) playing = 0;
        } else {
            memset(pcm, 0, sizeof pcm);
            write_pcm(pcm, CHUNK);
        }
    }
}

void KortaSfx_Init(void) {
    if (!KORTA_EXPERIMENTAL_AUDIO || task) return;
    xTaskCreate(sfx_task, "korta_sfx", 4096, NULL, 6, &task);
}

void KortaSfx_Play(uint8_t kind, uint8_t volume) {
    if (!kind || !__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    uint32_t vol = volume > 100 ? 100 : volume;
    __atomic_store_n(&request, (vol << 8) | kind, __ATOMIC_RELEASE);
}
