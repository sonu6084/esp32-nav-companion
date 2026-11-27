#include "ui_settime.h"

#include <stdio.h>
#include "ui_theme.h"
#include "sys.h"

#define YEAR_MIN 2024
#define YEAR_MAX 2040

static const char *const MON = "JAN\nFEB\nMAR\nAPR\nMAY\nJUN\nJUL\nAUG\nSEP\nOCT\nNOV\nDEC";

static lv_obj_t *s_ov, *r_h, *r_m, *r_d, *r_mo, *r_y;
static void (*s_cb)(bool);

bool ui_settime_active(void) { return s_ov != NULL; }

void ui_settime_close(bool save)
{
    if (!s_ov) return;
    if (save) {
        datetime_t t = {
            .hour   = lv_roller_get_selected(r_h),
            .minute = lv_roller_get_selected(r_m),
            .second = 0,
            .day    = lv_roller_get_selected(r_d) + 1,
            .month  = lv_roller_get_selected(r_mo) + 1,
            .year   = YEAR_MIN + lv_roller_get_selected(r_y),
        };
        int dim = sys_days_in_month(t.year, t.month);
        if (t.day > dim) t.day = dim;           /* 31 Feb -> 28/29 Feb */
        sys_set_time(&t);
    }
    lv_obj_delete(s_ov);
    s_ov = NULL;
    void (*cb)(bool) = s_cb;
    s_cb = NULL;
    if (cb) cb(save);
}

static void ok_cb(lv_event_t *e)     { ui_settime_close(true); }
static void cancel_cb(lv_event_t *e) { ui_settime_close(false); }

void ui_settime_open(lv_obj_t *parent, void (*on_close)(bool saved))
{
    if (s_ov) return;
    s_cb = on_close;
    datetime_t now;
    sys_get_time(&now);

    s_ov = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_ov, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_ov, false);
    lv_obj_set_clickable(s_ov, true);

    lv_obj_t *t = lv_label_create(s_ov);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, UI_COL_ACCENT, 0);
    lv_label_set_text(t, "SET TIME & DATE");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 36);

    static char o_h[24 * 3], o_m[60 * 3], o_d[31 * 3], o_y[(YEAR_MAX - YEAR_MIN + 1) * 5];
    r_h = ui_roller(s_ov, ui_num_opts(o_h, 0, 23, true), 3, &lv_font_montserrat_32, 90);
    lv_obj_align(r_h, LV_ALIGN_CENTER, -58, -62);
    lv_obj_t *colon = lv_label_create(s_ov);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_32, 0);
    lv_label_set_text(colon, ":");
    lv_obj_align(colon, LV_ALIGN_CENTER, 0, -64);
    r_m = ui_roller(s_ov, ui_num_opts(o_m, 0, 59, true), 3, &lv_font_montserrat_32, 90);
    lv_obj_align(r_m, LV_ALIGN_CENTER, 58, -62);

    r_d  = ui_roller(s_ov, ui_num_opts(o_d, 1, 31, false), 1, &lv_font_montserrat_24, 70);
    lv_obj_align(r_d, LV_ALIGN_CENTER, -100, 62);
    r_mo = ui_roller(s_ov, MON, 1, &lv_font_montserrat_24, 90);
    lv_obj_align(r_mo, LV_ALIGN_CENTER, -12, 62);
    r_y  = ui_roller(s_ov, ui_num_opts(o_y, YEAR_MIN, YEAR_MAX, false), 1, &lv_font_montserrat_24, 100);
    lv_obj_align(r_y, LV_ALIGN_CENTER, 92, 62);

    lv_roller_set_selected(r_h, now.hour % 24, LV_ANIM_OFF);
    lv_roller_set_selected(r_m, now.minute % 60, LV_ANIM_OFF);
    lv_roller_set_selected(r_d, (now.day >= 1 && now.day <= 31) ? now.day - 1 : 0, LV_ANIM_OFF);
    lv_roller_set_selected(r_mo, (now.month >= 1 && now.month <= 12) ? now.month - 1 : 0, LV_ANIM_OFF);
    int yi = now.year - YEAR_MIN;
    lv_roller_set_selected(r_y, (yi >= 0 && yi <= YEAR_MAX - YEAR_MIN) ? yi : 2, LV_ANIM_OFF);

    lv_obj_t *bc = ui_round_btn(s_ov, 70, lv_color_hex(0xDDDDDD), LV_SYMBOL_CLOSE, &lv_font_montserrat_28);
    lv_obj_align(bc, LV_ALIGN_CENTER, -62, 140);
    lv_obj_add_event_cb(bc, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bo = ui_round_btn(s_ov, 70, UI_COL_GREEN, LV_SYMBOL_OK, &lv_font_montserrat_28);
    lv_obj_align(bo, LV_ALIGN_CENTER, 62, 140);
    lv_obj_add_event_cb(bo, ok_cb, LV_EVENT_CLICKED, NULL);
}
