#pragma once
/*
 * Boot intro: plays main/assets/intro.gif once, full screen, then calls done.
 * A tap skips it. If the GIF is missing or will not decode, done is called on
 * the next timer tick, so the board always reaches the home screen.
 *
 * Swap the animation by replacing main/assets/intro.gif (412x412 or smaller,
 * centred on black) and rebuilding. tools/make_intro_gif.py makes the default.
 */
#include "lvgl.h"

void bos_intro_play(lv_obj_t *scr, void (*done)(void));
void bos_intro_destroy(void);

/* The GIF bytes: intro_data.c on the board (EMBED_FILES), the sim has its own. */
const uint8_t *bos_intro_gif(size_t *len);
