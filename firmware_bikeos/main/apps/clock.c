/*
 * Clock
 *
 *  Faces:   digital (large HH:MM, seconds ring) and analog (hands)
 *  Touch:   tap = switch face      long-press = set time & date
 *  Keys:    BOOT tap = switch face BOOT hold  = set time & date
 *  In "set time":  ✓ / BOOT tap = save     ✗ / BOOT hold = cancel
 *
 * Time comes from the PCF85063 RTC via sys (cached every 500 ms).
 */
#include <stdio.h>
#include <string.h>
#include "app_manager.h"
#include "ui_theme.h"
#include "sys.h"
#include "fonts.h"
#include "settings.h"
#include "cfg.h"
#include "ui_settime.h"

static const char *const DOW[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

#define s_face (cfg_clock_face() & 1)   /* 0 digital, 1 analog — saved in NVS */

static struct {
    bool alive;
    lv_obj_t *scr, *face;
    /* digital */
    lv_obj_t *d_time, *d_date, *d_ring, *d_bat;
    /* analog */
    lv_obj_t *meter, *a_date;
    lv_obj_t *n_hour, *n_min, *n_sec;
    lv_obj_t *face_end;          /* marker: face widgets live above this field */
    lv_timer_t *tmr;
    int last_sec;
} ui;

static void build_face(void);

static const char *dow_of(const datetime_t *t)
{
    int w = sys_weekday(t->year, t->month ? t->month : 1, t->day ? t->day : 1);
    return DOW[w];
}

/* ------------------------------ digital ------------------------------ */
static void build_digital(lv_obj_t *p)
{
    ui.d_ring = lv_arc_create(p);
    lv_obj_set_size(ui.d_ring, UI_W - 8, UI_H - 8);
    lv_obj_center(ui.d_ring);
    lv_arc_set_rotation(ui.d_ring, 270);
    lv_arc_set_bg_angles(ui.d_ring, 0, 360);
    lv_arc_set_range(ui.d_ring, 0, 60);
    lv_obj_remove_style(ui.d_ring, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui.d_ring, false);
    lv_obj_set_style_arc_width(ui.d_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.d_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.d_ring, lv_color_hex(0x22252B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.d_ring, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.d_ring, false, LV_PART_INDICATOR);

    ui.d_date = lv_label_create(p);
    lv_obj_set_style_text_font(ui.d_date, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ui.d_date, UI_COL_ACCENT, 0);
    lv_obj_align(ui.d_date, LV_ALIGN_CENTER, 0, -100);

    ui.d_time = lv_label_create(p);
    lv_obj_set_style_text_font(ui.d_time, &font_clock_120, 0);
    lv_label_set_text(ui.d_time, "00:00");
    lv_obj_align(ui.d_time, LV_ALIGN_CENTER, 0, 4);

    ui.d_bat = lv_label_create(p);
    lv_obj_set_style_text_font(ui.d_bat, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui.d_bat, UI_COL_MUTED, 0);
    lv_obj_align(ui.d_bat, LV_ALIGN_CENTER, 0, 105);
}

static void update_digital(const datetime_t *t)
{
    lv_label_set_text_fmt(ui.d_time, "%02d:%02d", t->hour % 24, t->minute % 60);
    lv_label_set_text_fmt(ui.d_date, "%s  %d %s", dow_of(t), t->day, MON[(t->month + 11) % 12]);
    lv_arc_set_value(ui.d_ring, t->second % 60);
    int pct = sys_battery_pct();
    if (pct < 0) lv_label_set_text(ui.d_bat, LV_SYMBOL_USB);
    else lv_label_set_text_fmt(ui.d_bat, LV_SYMBOL_BATTERY_FULL " %d%%", pct);
}

/* ------------------------------ analog ------------------------------ */
static lv_obj_t *needle(lv_obj_t *scale, int w, lv_color_t c)
{
    lv_obj_t *n = lv_line_create(scale);
    lv_obj_set_style_line_width(n, w, 0);
    lv_obj_set_style_line_color(n, c, 0);
    lv_obj_set_style_line_rounded(n, true, 0);
    return n;
}

static void build_analog(lv_obj_t *p)
{
    /* LVGL 9: lv_scale replaces lv_meter. 61 ticks over 360 deg, a label every 5. */
    static const char *hours[] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "", NULL};
    ui.meter = lv_scale_create(p);
    lv_obj_set_size(ui.meter, UI_W - 6, UI_H - 6);
    lv_obj_center(ui.meter);
    lv_scale_set_mode(ui.meter, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(ui.meter, 0, 60);
    lv_scale_set_angle_range(ui.meter, 360);
    lv_scale_set_rotation(ui.meter, 270);
    lv_scale_set_total_tick_count(ui.meter, 61);
    lv_scale_set_major_tick_every(ui.meter, 5);
    lv_scale_set_label_show(ui.meter, true);
    lv_scale_set_text_src(ui.meter, hours);
    lv_obj_set_style_bg_opa(ui.meter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.meter, 0, 0);
    lv_obj_set_style_arc_width(ui.meter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.meter, 6, 0);
    lv_obj_set_style_length(ui.meter, 12, LV_PART_ITEMS);
    lv_obj_set_style_line_width(ui.meter, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(ui.meter, UI_COL_MUTED, LV_PART_ITEMS);
    lv_obj_set_style_length(ui.meter, 24, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ui.meter, 5, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(ui.meter, UI_COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(ui.meter, &lv_font_montserrat_28, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(ui.meter, UI_COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(ui.meter, 22, LV_PART_INDICATOR);   /* label gap from the ticks */
    lv_obj_set_clickable(ui.meter, false);

    ui.a_date = lv_label_create(p);
    lv_obj_set_style_text_font(ui.a_date, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui.a_date, UI_COL_ACCENT, 0);
    lv_obj_set_style_bg_color(ui.a_date, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(ui.a_date, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.a_date, 8, 0);
    lv_obj_set_style_pad_hor(ui.a_date, 10, 0);
    lv_obj_set_style_pad_ver(ui.a_date, 4, 0);
    lv_obj_align(ui.a_date, LV_ALIGN_CENTER, 0, 90);

    /* hands last so they draw over the date window */
    ui.n_hour = needle(ui.meter, 10, UI_COL_TEXT);
    ui.n_min  = needle(ui.meter, 7, UI_COL_TEXT);
    ui.n_sec  = needle(ui.meter, 3, UI_COL_ACCENT);
    lv_obj_t *cap = lv_obj_create(p);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 18, 18);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, UI_COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_center(cap);
}

static void update_analog(const datetime_t *t)
{
    int h = t->hour % 12, m = t->minute % 60, s = t->second % 60;
    int r = (UI_W - 6) / 2;
    lv_scale_set_line_needle_value(ui.meter, ui.n_hour, r - 100, h * 5 + m / 12);
    lv_scale_set_line_needle_value(ui.meter, ui.n_min, r - 48, m);
    lv_scale_set_line_needle_value(ui.meter, ui.n_sec, r - 28, s);
    lv_label_set_text_fmt(ui.a_date, "%s %d", dow_of(t), t->day);
}

/* ------------------------------ face plumbing ------------------------------ */
static void refresh(bool force)
{
    datetime_t t;
    sys_get_time(&t);
    if (!force && t.second == ui.last_sec) return;
    ui.last_sec = t.second;
    if (s_face == 0) update_digital(&t);
    else update_analog(&t);
}

static void tick_cb(lv_timer_t *tm) { if (!ui_settime_active()) refresh(false); }

static void build_face(void)
{
    if (ui.face) lv_obj_delete(ui.face);
    memset(&ui.d_time, 0, (char *)&ui.face_end - (char *)&ui.d_time);   /* forget face widgets */
    ui.face = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(ui.face);
    lv_obj_set_size(ui.face, UI_W, UI_H);
    lv_obj_set_scrollable(ui.face, false); lv_obj_set_clickable(ui.face, false);
    lv_obj_move_background(ui.face);
    if (s_face == 0) build_digital(ui.face);
    else build_analog(ui.face);
    refresh(true);
}

static void toggle_face(void)
{
    cfg_set_clock_face(s_face ^ 1);
    build_face();
}

/* ------------------------------ set time ------------------------------ */
static void set_closed(bool saved) { if (ui.alive) refresh(true); }
static void open_set(void) { ui_settime_open(ui.scr, set_closed); }

/* ------------------------------ events ------------------------------ */
static void scr_event_cb(lv_event_t *e)
{
    if (!ui.alive || ui_settime_active()) return;   /* screen may be mid-exit, or set-time is up */
    if ((lv_obj_t *)lv_event_get_target(e) != ui.scr) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SHORT_CLICKED) toggle_face();
    else if (c == LV_EVENT_LONG_PRESSED) open_set();
}

static void clock_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.scr = scr;
    ui.last_sec = -1;
    lv_obj_set_clickable(scr, true);
    lv_obj_add_event_cb(scr, scr_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, scr_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_face();
    ui.tmr = lv_timer_create(tick_cb, 200, NULL);
}

static void clock_destroy(void)
{
    ui_settime_close(false);      /* overlay lives on our screen, which is about to go */
    if (ui.tmr) lv_timer_delete(ui.tmr);
    memset(&ui, 0, sizeof ui);   /* alive = false */
}

static bool clock_on_key(app_key_t k)
{
    if (ui_settime_active()) ui_settime_close(k == APP_KEY_PRIMARY);
    else if (k == APP_KEY_PRIMARY) toggle_face();
    else open_set();
    return true;
}

static const char *clock_status(void)
{
    static char buf[24];
    datetime_t t;
    sys_get_time(&t);
    snprintf(buf, sizeof buf, "%02d:%02d  %s %d", t.hour % 24, t.minute % 60, dow_of(&t), t.day);
    return buf;
}

const app_t app_clock = {
    .name    = "Clock",
    .symbol  = LV_SYMBOL_HOME,
    .color   = 0x4FC3F7,
    .create  = clock_create,
    .destroy = clock_destroy,
    .on_key  = clock_on_key,
    .status  = clock_status,
    .ready   = true,
};
