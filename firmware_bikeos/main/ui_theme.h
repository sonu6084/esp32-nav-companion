#pragma once
#include "lvgl.h"

#define UI_W 412
#define UI_H 412

#define UI_COL_BG       lv_color_hex(0x000000)
#define UI_COL_SURFACE  lv_color_hex(0x16181D)
#define UI_COL_TEXT     lv_color_hex(0xFFFFFF)
#define UI_COL_MUTED    lv_color_hex(0x8A8F98)
#define UI_COL_ACCENT   lv_color_hex(0xFFB300)   /* amber: readable in sunlight, matches turn-blink yellow */
#define UI_COL_GREEN    lv_color_hex(0x2ECC71)
#define UI_COL_RED      lv_color_hex(0xFF4D4D)

/* Plain black, non-scrollable full-screen object */
lv_obj_t *ui_screen_create(void);
/* Round icon button: bg colour + symbol/text */
lv_obj_t *ui_round_btn(lv_obj_t *parent, int32_t d, lv_color_t bg, const char *txt, const lv_font_t *font);
void      ui_round_btn_set(lv_obj_t *btn, lv_color_t bg, const char *txt);

/* Roller with matching MAIN/SELECTED font + line spacing (LVGL 8 drift fix). NORMAL mode. */
lv_obj_t *ui_roller(lv_obj_t *parent, const char *opts, int rows, const lv_font_t *f, int w);
/* "00\n01\n..": fills buf with zero-padded numbers from..to */
const char *ui_num_opts(char *buf, int from, int to, bool pad2);
/* Small text pill button */
lv_obj_t *ui_pill_btn(lv_obj_t *parent, const char *txt, int32_t w, int32_t h, lv_color_t bg);
