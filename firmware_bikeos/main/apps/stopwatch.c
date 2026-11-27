/*
 * Stopwatch
 *
 *  Touch:  right button = Start / Stop     left button = Lap (running) / Reset (stopped)
 *  Keys:   BOOT tap     = Start / Stop     BOOT hold   = Lap / Reset
 *          PWR tap or swipe right = back to menu (stopwatch keeps running)
 *
 * Timing comes from esp_timer (µs, monotonic), not from LVGL ticks, so it stays
 * exact while the UI is closed or busy. State is static and outlives the screen.
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app_manager.h"
#include "ui_theme.h"

#define MAX_LAPS 99

/* ------------------------------ state ------------------------------ */
static struct {
    bool    running;
    int64_t start_us;           /* esp_timer value when last started */
    int64_t acc_us;             /* time accumulated before last start */
    int64_t laps[MAX_LAPS];     /* total elapsed at each lap mark */
    int     lap_count;
} sw;

static int64_t sw_elapsed(void)
{
    return sw.acc_us + (sw.running ? esp_timer_get_time() - sw.start_us : 0);
}

static void sw_start(void) { if (!sw.running) { sw.start_us = esp_timer_get_time(); sw.running = true; } }
static void sw_stop(void)  { if (sw.running)  { sw.acc_us = sw_elapsed(); sw.running = false; } }
static void sw_reset(void) { memset(&sw, 0, sizeof(sw)); }
static void sw_lap(void)
{
    if (!sw.running) return;
    if (sw.lap_count < MAX_LAPS) sw.laps[sw.lap_count++] = sw_elapsed();
}
static int64_t lap_len(int i) { return sw.laps[i] - (i ? sw.laps[i - 1] : 0); }

/* "MM:SS" or "H:MM:SS" into big, ".cc" into small */
static void fmt_split(int64_t us, char *big, size_t bn, char *small, size_t sn)
{
    int64_t cs = us / 10000;
    int c = cs % 100, s = (cs / 100) % 60, m = (cs / 6000) % 60, h = cs / 360000;
    if (h) snprintf(big, bn, "%d:%02d:%02d", h, m, s);
    else   snprintf(big, bn, "%02d:%02d", m, s);
    if (small) snprintf(small, sn, ".%02d", c);
}
static void fmt_full(int64_t us, char *out, size_t n)
{
    char b[24], s[8];
    fmt_split(us, b, sizeof b, s, sizeof s);
    snprintf(out, n, "%s%s", b, s);
}

/* ------------------------------ UI ------------------------------ */
static struct {
    lv_obj_t *arc, *big, *small, *lapline, *list, *btn_l, *btn_r;
    lv_timer_t *tmr;
    int shown_laps;
    int64_t last_cs;
    int last_state;             /* encodes running/zero so buttons repaint only on change */
} ui;

static void rebuild_laps(void)
{
    lv_obj_clean(ui.list);
    int best = -1, worst = -1;
    if (sw.lap_count >= 3) {
        best = worst = 0;
        for (int i = 1; i < sw.lap_count; i++) {
            if (lap_len(i) < lap_len(best))  best = i;
            if (lap_len(i) > lap_len(worst)) worst = i;
        }
    }
    /* newest first */
    for (int i = sw.lap_count - 1; i >= 0; i--) {
        char a[40], b[40];
        fmt_full(lap_len(i), a, sizeof a);
        fmt_full(sw.laps[i], b, sizeof b);
        lv_obj_t *row = lv_label_create(ui.list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_label_set_text_fmt(row, "#%-2d   %s   %s", i + 1, a, b);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(row, i == best ? UI_COL_GREEN : i == worst ? UI_COL_RED : UI_COL_TEXT, 0);
    }
    ui.shown_laps = sw.lap_count;
}

static void refresh_buttons(void)
{
    int64_t e = sw_elapsed();
    int state = sw.running ? 2 : (e > 0 ? 1 : 0);
    if (state == ui.last_state) return;
    ui.last_state = state;

    if (sw.running) ui_round_btn_set(ui.btn_r, UI_COL_RED, LV_SYMBOL_PAUSE);
    else            ui_round_btn_set(ui.btn_r, UI_COL_GREEN, LV_SYMBOL_PLAY);

    if (state == 2)      ui_round_btn_set(ui.btn_l, lv_color_hex(0xDDDDDD), LV_SYMBOL_PLUS);   /* lap */
    else                 ui_round_btn_set(ui.btn_l, lv_color_hex(0xDDDDDD), LV_SYMBOL_REFRESH); /* reset */
    if (state == 0) lv_obj_add_state(ui.btn_l, LV_STATE_DISABLED);
    else            lv_obj_remove_state(ui.btn_l, LV_STATE_DISABLED);

    lv_obj_set_style_arc_color(ui.arc, sw.running ? UI_COL_ACCENT : UI_COL_MUTED, LV_PART_INDICATOR);
}

static void refresh(void)
{
    int64_t e  = sw_elapsed();
    int64_t cs = e / 10000;

    if (cs != ui.last_cs) {
        ui.last_cs = cs;
        char big[16], small[8];
        fmt_split(e, big, sizeof big, small, sizeof small);
        lv_label_set_text(ui.big, big);
        lv_label_set_text(ui.small, small);
        lv_arc_set_value(ui.arc, (int16_t)((e / 1000) % 60000 / 100));   /* 0..599, one lap per minute */

        if (sw.lap_count > 0 || sw.running) {
            char cur[40];
            int64_t since = e - (sw.lap_count ? sw.laps[sw.lap_count - 1] : 0);
            fmt_full(since, cur, sizeof cur);
            lv_label_set_text_fmt(ui.lapline, "Lap %d   %s", sw.lap_count + 1, cur);
        } else {
            lv_label_set_text(ui.lapline, "");
        }
    }
    if (ui.shown_laps != sw.lap_count) rebuild_laps();
    refresh_buttons();
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static void primary(void)   { if (sw.running) sw_stop(); else sw_start(); refresh(); }
static void secondary(void) { if (sw.running) sw_lap(); else sw_reset(); ui.last_cs = -1; refresh(); }

static void btn_r_cb(lv_event_t *e) { primary(); }
static void btn_l_cb(lv_event_t *e) { secondary(); }

static void sw_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.last_cs = -1;
    ui.last_state = -1;

    /* seconds ring around the bezel */
    ui.arc = lv_arc_create(scr);
    lv_obj_set_size(ui.arc, UI_W - 8, UI_H - 8);
    lv_obj_center(ui.arc);
    lv_arc_set_rotation(ui.arc, 270);
    lv_arc_set_bg_angles(ui.arc, 0, 360);
    lv_arc_set_range(ui.arc, 0, 599);
    lv_arc_set_value(ui.arc, 0);
    lv_obj_remove_style(ui.arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui.arc, false);
    lv_obj_set_style_arc_width(ui.arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.arc, lv_color_hex(0x22252B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.arc, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.arc, false, LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(self->color), 0);
    lv_label_set_text(title, "STOPWATCH");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    /* big digits + small centiseconds side by side */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -72);
    lv_obj_set_clickable(row, false);

    ui.big = lv_label_create(row);
    lv_obj_set_style_text_font(ui.big, &lv_font_montserrat_48, 0);
    lv_label_set_text(ui.big, "00:00");
    ui.small = lv_label_create(row);
    lv_obj_set_style_text_font(ui.small, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ui.small, UI_COL_ACCENT, 0);
    lv_obj_set_style_pad_bottom(ui.small, 3, 0);
    lv_label_set_text(ui.small, ".00");

    ui.lapline = lv_label_create(scr);
    lv_obj_set_style_text_font(ui.lapline, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui.lapline, UI_COL_MUTED, 0);
    lv_label_set_text(ui.lapline, "");
    lv_obj_align(ui.lapline, LV_ALIGN_CENTER, 0, -22);

    /* lap history: scrollable, newest on top */
    ui.list = lv_obj_create(scr);
    lv_obj_set_size(ui.list, 290, 92);
    lv_obj_align(ui.list, LV_ALIGN_CENTER, 0, 46);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_pad_all(ui.list, 0, 0);
    lv_obj_set_style_pad_row(ui.list, 4, 0);
    lv_obj_set_style_text_font(ui.list, &lv_font_montserrat_16, 0);
    lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    ui.btn_l = ui_round_btn(scr, 76, lv_color_hex(0xDDDDDD), LV_SYMBOL_REFRESH, &lv_font_montserrat_28);
    lv_obj_align(ui.btn_l, LV_ALIGN_CENTER, -66, 140);
    lv_obj_add_event_cb(ui.btn_l, btn_l_cb, LV_EVENT_CLICKED, NULL);

    ui.btn_r = ui_round_btn(scr, 76, UI_COL_GREEN, LV_SYMBOL_PLAY, &lv_font_montserrat_28);
    lv_obj_align(ui.btn_r, LV_ALIGN_CENTER, 66, 140);
    lv_obj_add_event_cb(ui.btn_r, btn_r_cb, LV_EVENT_CLICKED, NULL);

    refresh();
    ui.tmr = lv_timer_create(tick_cb, 33, NULL);    /* ~30 fps; only changed labels redraw */
}

static void sw_destroy(void)
{
    if (ui.tmr) lv_timer_delete(ui.tmr);
    memset(&ui, 0, sizeof ui);
}

static bool sw_on_key(app_key_t k)
{
    if (k == APP_KEY_PRIMARY) primary();
    else secondary();
    return true;
}

/* shown under "Stopwatch" in the menu */
static const char *sw_status(void)
{
    static char buf[24];
    int64_t e = sw_elapsed();
    if (!sw.running && e == 0) return NULL;
    char b[16];
    fmt_split(e, b, sizeof b, NULL, 0);
    snprintf(buf, sizeof buf, "%s %s", sw.running ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, b);
    return buf;
}

const app_t app_stopwatch = {
    .name    = "Stopwatch",
    .symbol  = LV_SYMBOL_PLAY,
    .color   = 0xFFB300,
    .create  = sw_create,
    .destroy = sw_destroy,
    .on_key  = sw_on_key,
    .status  = sw_status,
    .ready   = true,
};
