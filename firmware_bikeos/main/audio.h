#pragma once
/*
 * Tone generator on the PCM5101 DAC (I2S0: BCLK 48, WS 38, DOUT 47).
 * A dedicated task renders the pattern; calls are non-blocking from any task.
 */
#include <stdbool.h>

typedef enum {
    SND_NONE,
    SND_CLICK,        /* one short tick (UI feedback) */
    SND_TIMER,        /* repeating double beep until stopped */
    SND_ALARM,        /* repeating rising triple beep until stopped */
    SND_TURN,         /* two quick high beeps: navigation turn coming up */
} snd_t;

void audio_init(void);
void audio_play(snd_t s);          /* replaces whatever is playing */
void audio_stop(void);
bool audio_busy(void);
