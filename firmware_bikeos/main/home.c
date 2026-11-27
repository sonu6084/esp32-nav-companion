#include "home.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_theme.h"
#include "fonts.h"
#include "sys.h"
#include "nav_core.h"
#include "map_view.h"

#define COL_BLUE 0x1E6EFF      /* the intro's blue */
#define COL_RED  0xFF2D2D
#define COL_DIM  0x2A2F38
#define CX (UI_W / 2)
#define CY (UI_H / 2)
#define RIM_R 198

static const char *const DOW[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static lv_obj_t *s_scr, *s_rim, *s_brand, *s_time, *s_ampm, *s_date, *s_link, *s_bat, *s_hint, *s_pill;
static void (*s_on_tap)(void), (*s_on_nav)(void);
static int s_sec = -1, s_min = -1, s_link_state = -1, s_bat_pct = -2, s_nav = -1;

/* 60 ticks round the rim; the seconds gone this minute are lit, the last few in red */
static void rim_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    int lit = s_sec < 0 ? 0 : s_sec;
    for (int i = 0; i < 60; i++) {
        float a = (float)i * 6.0f * (float)M_PI / 180.0f - (float)M_PI / 2;
        bool major = i % 5 == 0;
        int r1 = RIM_R - (major ? 16 : 8);
        d.width = major ? 4 : 2;
        d.color = lv_color_hex(i <= lit ? (i >= 55 ? COL_RED : COL_BLUE) : COL_DIM);
        d.p1.x = (int32_t)(CX + r1 * cosf(a));
        d.p1.y = (int32_t)(CY + r1 * sinf(a));
        d.p2.x = (int32_t)(CX + RIM_R * cosf(a));
        d.p2.y = (int32_t)(CY + RIM_R * sinf(a));
        lv_draw_line(layer, &d);
    }
}

static void tap_cb(lv_event_t *e) { (void)e; if (s_on_tap) s_on_tap(); }
static void pill_cb(lv_event_t *e) { (void)e; if (s_on_nav) s_on_nav(); }

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *home_create(void (*on_tap)(void), void (*on_nav)(void))
{
    s_on_tap = on_tap;
    s_on_nav = on_nav;
    s_scr = ui_screen_create();
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_clickable(s_scr, true);
    lv_obj_add_event_cb(s_scr, tap_cb, LV_EVENT_SHORT_CLICKED, NULL);

    s_rim = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_rim);
    lv_obj_set_size(s_rim, UI_W, UI_H);
    lv_obj_set_clickable(s_rim, false);
    lv_obj_add_event_cb(s_rim, rim_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_brand = label(s_scr, &lv_font_montserrat_14, UI_COL_MUTED);
    lv_obj_set_style_text_letter_space(s_brand, 6, 0);
    lv_label_set_text(s_brand, "SUZUKI GIXXER");
    lv_obj_align(s_brand, LV_ALIGN_TOP_MID, 3, 62);

    s_date = label(s_scr, &lv_font_montserrat_24, lv_color_hex(COL_BLUE));
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, -84);

    s_time = label(s_scr, &font_clock_120, UI_COL_TEXT);
    lv_label_set_text(s_time, "00:00");
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 4);

    s_ampm = label(s_scr, &lv_font_montserrat_16, UI_COL_MUTED);
    lv_obj_align(s_ampm, LV_ALIGN_CENTER, 0, 66);

    s_link = label(s_scr, &lv_font_montserrat_20, UI_COL_MUTED);
    lv_obj_align(s_link, LV_ALIGN_CENTER, -50, 98);
    s_bat = label(s_scr, &lv_font_montserrat_20, UI_COL_MUTED);
    lv_obj_align(s_bat, LV_ALIGN_CENTER, 30, 98);

    s_hint = label(s_scr, &lv_font_montserrat_14, lv_color_hex(0x5F6368));
    lv_label_set_text(s_hint, "Tap for menu");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -46);

    /* a route is running but Navigation is closed: one tap takes you back */
    s_pill = lv_button_create(s_scr);
    lv_obj_set_size(s_pill, 190, 44);
    lv_obj_set_style_radius(s_pill, 22, 0);
    lv_obj_set_style_bg_color(s_pill, UI_COL_GREEN, 0);
    lv_obj_set_style_shadow_width(s_pill, 0, 0);
    lv_obj_align(s_pill, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_add_event_cb(s_pill, pill_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = label(s_pill, &lv_font_montserrat_16, lv_color_black());
    lv_label_set_text(pl, LV_SYMBOL_GPS "  Navigating");
    lv_obj_center(pl);
    lv_obj_set_hidden(s_pill, true);

    s_sec = s_min = s_link_state = s_nav = -1;
    s_bat_pct = -2;
    home_refresh();
    return s_scr;
}

void home_refresh(void)
{
    if (!s_scr) return;
    datetime_t t;
    sys_get_time(&t);

    if (t.second != s_sec) {
        s_sec = t.second % 60;
        lv_obj_invalidate(s_rim);
    }
    if (t.minute != s_min) {
        s_min = t.minute;
        bool h24 = map_view_clock_24h();
        int h = t.hour % 24;
        if (h24) {
            lv_label_set_text_fmt(s_time, "%02d:%02d", h, t.minute % 60);
            lv_label_set_text(s_ampm, "");
        } else {
            lv_label_set_text_fmt(s_time, "%d:%02d", h % 12 ? h % 12 : 12, t.minute % 60);
            lv_label_set_text(s_ampm, h < 12 ? "AM" : "PM");
        }
        int w = sys_weekday(t.year, t.month ? t.month : 1, t.day ? t.day : 1);
        lv_label_set_text_fmt(s_date, "%s  %d %s", DOW[w % 7], t.day, MON[(t.month ? t.month - 1 : 0) % 12]);
    }

    int link = nav_core_connected();
    if (link != s_link_state) {
        s_link_state = link;
        lv_label_set_text(s_link, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(s_link, link ? lv_color_hex(COL_BLUE) : lv_color_hex(0x5F6368), 0);
    }

    int pct = sys_battery_pct();
    if (pct != s_bat_pct) {
        s_bat_pct = pct;
        if (pct < 0) lv_label_set_text(s_bat, LV_SYMBOL_USB);
        else lv_label_set_text_fmt(s_bat, "%s %d%%",
                                   pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
                                   pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 :
                                   LV_SYMBOL_BATTERY_EMPTY, pct);
        lv_obj_set_style_text_color(s_bat, pct >= 0 && pct <= 15 ? UI_COL_RED : UI_COL_MUTED, 0);
    }

    int nav = nav_core_navigating();
    if (nav != s_nav) {
        s_nav = nav;
        lv_obj_set_hidden(s_pill, !nav);
        lv_obj_set_hidden(s_hint, nav);
    }
}
