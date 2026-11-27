#include "audio.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "settings.h"
#include "cfg.h"

#define RATE        16000
#define PIN_BCLK    48
#define PIN_WS      38
#define PIN_DOUT    47
#define CHUNK       256          /* samples per write (16 ms) */

static const char *TAG = "audio";
static i2s_chan_handle_t s_tx;
static TaskHandle_t      s_task;
static volatile snd_t    s_req = SND_NONE;
static volatile bool     s_busy;

/* A pattern is a list of {freq Hz, duration ms}; freq 0 = silence. */
typedef struct { uint16_t f, ms; } step_t;
static const step_t P_CLICK[] = {{2400, 12}};
static const step_t P_TIMER[] = {{1760, 140}, {0, 90}, {1760, 140}, {0, 700}};
static const step_t P_ALARM[] = {{1320, 120}, {0, 60}, {1568, 120}, {0, 60}, {1976, 160}, {0, 640}};
static const step_t P_TURN[]  = {{2093, 90}, {0, 60}, {2637, 120}};

static void pattern(snd_t s, const step_t **p, int *n, bool *loop)
{
    *loop = false;
    switch (s) {
    case SND_CLICK: *p = P_CLICK; *n = 1; break;
    case SND_TIMER: *p = P_TIMER; *n = 4; *loop = true; break;
    case SND_ALARM: *p = P_ALARM; *n = 6; *loop = true; break;
    case SND_TURN:  *p = P_TURN;  *n = 3; break;
    default: *p = NULL; *n = 0; break;
    }
}

static void write_silence(int ms)
{
    static int16_t z[CHUNK];
    int samples = RATE * ms / 1000;
    while (samples > 0) {
        int n = samples > CHUNK ? CHUNK : samples;
        size_t w;
        i2s_channel_write(s_tx, z, n * 2, &w, portMAX_DELAY);
        samples -= n;
    }
}

/* returns false if interrupted by a new request */
static bool play_step(const step_t *st, snd_t cur)
{
    if (st->f == 0) {
        for (int t = 0; t < st->ms; t += 16) {
            if (s_req != cur) return false;
            write_silence(16);
        }
        return true;
    }
    static int16_t buf[CHUNK];
    float amp = 9000.0f * cfg_volume() / 100.0f;
    int total = RATE * st->ms / 1000, done = 0;
    float ph = 0, dph = 2.0f * (float)M_PI * st->f / RATE;
    int ramp = RATE / 400;                       /* 2.5 ms fade in/out: no clicks */
    while (done < total) {
        if (s_req != cur) return false;
        int n = total - done > CHUNK ? CHUNK : total - done;
        for (int i = 0; i < n; i++, done++) {
            float env = 1.0f;
            if (done < ramp) env = (float)done / ramp;
            else if (total - done < ramp) env = (float)(total - done) / ramp;
            buf[i] = (int16_t)(sinf(ph) * amp * env);
            ph += dph;
            if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
        }
        size_t w;
        i2s_channel_write(s_tx, buf, n * 2, &w, portMAX_DELAY);
    }
    return true;
}

static void audio_task(void *arg)
{
    for (;;) {
        snd_t cur = s_req;
        if (cur == SND_NONE) {
            s_busy = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        s_busy = true;
        const step_t *p; int n; bool loop;
        pattern(cur, &p, &n, &loop);
        i2s_channel_enable(s_tx);
        bool ok = true;
        do {
            for (int i = 0; i < n && ok; i++) ok = play_step(&p[i], cur);
        } while (ok && loop && s_req == cur && cfg_volume() > 0);
        write_silence(20);                       /* flush DMA with zeros */
        i2s_channel_disable(s_tx);
        if (ok && s_req == cur) s_req = SND_NONE;
    }
}

void audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { ESP_LOGW(TAG, "no i2s"); return; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK, .ws = PIN_WS,
            .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) { ESP_LOGW(TAG, "i2s init"); return; }
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 4, &s_task, 0);
}

void audio_play(snd_t s)
{
    if (!s_task) return;
    if (cfg_volume() == 0 && s != SND_NONE) return;
    s_req = s;
    xTaskNotifyGive(s_task);
}

void audio_stop(void) { s_req = SND_NONE; }
bool audio_busy(void) { return s_busy; }

/* bike-os: the speaker came up (Settings > About). */
bool audio_ok(void) { return s_task != NULL; }
