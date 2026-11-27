#include <stdio.h>
#include "ui_theme.h"

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UI_COL_TEXT, 0);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

lv_obj_t *ui_round_btn(lv_obj_t *parent, int32_t d, lv_color_t bg, const char *txt, const lv_font_t *font)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, 10);   /* gloves */
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

void ui_round_btn_set(lv_obj_t *btn, lv_color_t bg, const char *txt)
{
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), txt);
}

lv_obj_t *ui_roller(lv_obj_t *p, const char *opts, int rows, const lv_font_t *f, int w)
{
    lv_obj_t *r = lv_roller_create(p);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, rows);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, f, 0);
    lv_obj_set_style_text_font(r, f, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(r, 14, 0);
    lv_obj_set_style_text_line_space(r, 14, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, UI_COL_SURFACE, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_text_color(r, UI_COL_MUTED, 0);
    lv_obj_set_style_bg_color(r, UI_COL_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_black(), LV_PART_SELECTED);
    return r;
}

const char *ui_num_opts(char *buf, int from, int to, bool pad2)
{
    char *p = buf;
    for (int i = from; i <= to; i++) p += sprintf(p, pad2 ? "%s%02d" : "%s%d", i > from ? "\n" : "", i);
    return buf;
}

lv_obj_t *ui_pill_btn(lv_obj_t *parent, const char *txt, int32_t w, int32_t h, lv_color_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, UI_COL_TEXT, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}
