/*
 * Timer (countdown)
 *
 *  Setup:   rollers H:M:S, preset chips (1/3/5/10 min), ▶ start
 *  Running: countdown ring, big MM:SS, "ends HH:MM";  ✗ cancel   ❚❚/▶ pause/resume
 *  Keys:    BOOT tap = start / pause / resume     BOOT hold = cancel
 *  Done:    alert over any app with beeps;  ✓ dismiss   "+1 min" restarts for a minute
 *
 * Counts down against esp_timer (µs), so it keeps running when the app is closed;
 * bg_tick() fires the alert. The last duration is remembered (NVS).
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app_manager.h"
#include "ui_theme.h"
#include "fonts.h"
#include "sys.h"
#include "settings.h"
#include "cfg.h"
#include "alert.h"

#define COLOR 0xBA68C8

typedef enum { T_IDLE, T_RUN, T_PAUSE } tstate_t;

static struct {
    tstate_t st;
    int64_t  deadline_us;   /* when running */
    int64_t  left_us;       /* when paused */
    int64_t  total_us;
} T;

static int64_t t_left(void)
{
    if (T.st == T_RUN) { int64_t l = T.deadline_us - esp_timer_get_time(); return l > 0 ? l : 0; }
    if (T.st == T_PAUSE) return T.left_us;
    return 0;
}
static void t_start(int64_t dur_us)
{
    T.total_us = dur_us;
    T.deadline_us = esp_timer_get_time() + dur_us;
    T.st = T_RUN;
}
static void t_pause(void)  { if (T.st == T_RUN) { T.left_us = t_left(); T.st = T_PAUSE; } }
static void t_resume(void) { if (T.st == T_PAUSE) { T.deadline_us = esp_timer_get_time() + T.left_us; T.st = T_RUN; } }
static void t_cancel(void) { T.st = T_IDLE; }

/* "MM:SS" (or "H:MM:SS"), rounding up so 00:00 shows only at the end */
static bool fmt_left(int64_t us, char *buf, size_t n)
{
    int64_t s = (us + 999999) / 1000000;
    int h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h) { snprintf(buf, n, "%d:%02d:%02d", h, m, sec); return true; }
    snprintf(buf, n, "%02d:%02d", m, sec);
    return false;
}

/* ------------------------------ UI ------------------------------ */
static struct {
    bool alive;
    lv_obj_t *scr;
    lv_obj_t *setup, *r_h, *r_m, *r_s;
    lv_obj_t *run, *arc, *big, *big_h, *sub, *b_cancel, *b_pause;
    lv_timer_t *tmr;
    int shown;      /* tstate_t currently built, -1 none */
    int64_t last_s;
} ui;

static void build(void);

static void setup_from_seconds(uint32_t s)
{
    if (!ui.setup) return;
    lv_roller_set_selected(ui.r_h, (s / 3600) % 24, LV_ANIM_ON);
    lv_roller_set_selected(ui.r_m, (s / 60) % 60, LV_ANIM_ON);
    lv_roller_set_selected(ui.r_s, s % 60, LV_ANIM_ON);
}

static void do_start(void)
{
    uint32_t s = lv_roller_get_selected(ui.r_h) * 3600 + lv_roller_get_selected(ui.r_m) * 60 +
                 lv_roller_get_selected(ui.r_s);
    if (s == 0) return;
    cfg_set_timer_last_s(s);
    t_start((int64_t)s * 1000000);
    build();
}

static void start_cb(lv_event_t *e)  { do_start(); }
static void preset_cb(lv_event_t *e) { setup_from_seconds((uint32_t)(intptr_t)lv_event_get_user_data(e)); }
static void cancel_cb(lv_event_t *e) { t_cancel(); build(); }
static void pause_cb(lv_event_t *e)  { if (T.st == T_RUN) t_pause(); else t_resume(); ui.last_s = -1; }

static void build_setup(lv_obj_t *p)
{
    lv_obj_t *t = lv_label_create(p);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_label_set_text(t, "TIMER");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    static char oh[24 * 3], om[60 * 3], os[60 * 3];
    ui.r_h = ui_roller(p, ui_num_opts(oh, 0, 23, true), 3, &lv_font_montserrat_32, 84);
    ui.r_m = ui_roller(p, ui_num_opts(om, 0, 59, true), 3, &lv_font_montserrat_32, 84);
    ui.r_s = ui_roller(p, ui_num_opts(os, 0, 59, true), 3, &lv_font_montserrat_32, 84);
    lv_obj_align(ui.r_h, LV_ALIGN_CENTER, -100, -48);
    lv_obj_align(ui.r_m, LV_ALIGN_CENTER, 0, -48);
    lv_obj_align(ui.r_s, LV_ALIGN_CENTER, 100, -48);
    const char *unit[] = {"h", "m", "s"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *u = lv_label_create(p);
        lv_obj_set_style_text_color(u, UI_COL_MUTED, 0);
        lv_label_set_text(u, unit[i]);
        lv_obj_align(u, LV_ALIGN_CENTER, -100 + i * 100, -132);
    }

    static const uint32_t pre[] = {60, 180, 300, 600};
    static const char *pl[] = {"1m", "3m", "5m", "10m"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_pill_btn(p, pl[i], 68, 40, UI_COL_SURFACE);
        lv_obj_align(b, LV_ALIGN_CENTER, -111 + i * 74, 62);
        lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pre[i]);
    }

    lv_obj_t *go = ui_round_btn(p, 80, UI_COL_GREEN, LV_SYMBOL_PLAY, &lv_font_montserrat_28);
    lv_obj_align(go, LV_ALIGN_CENTER, 0, 136);
    lv_obj_add_event_cb(go, start_cb, LV_EVENT_CLICKED, NULL);

    uint32_t last = cfg_timer_last_s();
    lv_roller_set_selected(ui.r_h, (last / 3600) % 24, LV_ANIM_OFF);
    lv_roller_set_selected(ui.r_m, (last / 60) % 60, LV_ANIM_OFF);
    lv_roller_set_selected(ui.r_s, last % 60, LV_ANIM_OFF);
}

static void build_run(lv_obj_t *p)
{
    ui.arc = lv_arc_create(p);
    lv_obj_set_size(ui.arc, UI_W - 8, UI_H - 8);
    lv_obj_center(ui.arc);
    lv_arc_set_rotation(ui.arc, 270);
    lv_arc_set_bg_angles(ui.arc, 0, 360);
    lv_arc_set_range(ui.arc, 0, 1000);
    lv_obj_remove_style(ui.arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui.arc, false);
    lv_obj_set_style_arc_width(ui.arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.arc, lv_color_hex(0x22252B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.arc, lv_color_hex(COLOR), LV_PART_INDICATOR);

    lv_obj_t *t = lv_label_create(p);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_label_set_text(t, "TIMER");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 46);

    ui.big = lv_label_create(p);
    lv_obj_set_style_text_font(ui.big, &font_clock_120, 0);
    lv_obj_align(ui.big, LV_ALIGN_CENTER, 0, -28);
    ui.big_h = lv_label_create(p);                      /* over an hour: smaller font */
    lv_obj_set_style_text_font(ui.big_h, &lv_font_montserrat_48, 0);
    lv_obj_align(ui.big_h, LV_ALIGN_CENTER, 0, -28);

    ui.sub = lv_label_create(p);
    lv_obj_set_style_text_font(ui.sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui.sub, UI_COL_MUTED, 0);
    lv_obj_align(ui.sub, LV_ALIGN_CENTER, 0, 52);

    ui.b_cancel = ui_round_btn(p, 76, lv_color_hex(0xDDDDDD), LV_SYMBOL_CLOSE, &lv_font_montserrat_28);
    lv_obj_align(ui.b_cancel, LV_ALIGN_CENTER, -66, 132);
    lv_obj_add_event_cb(ui.b_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    ui.b_pause = ui_round_btn(p, 76, UI_COL_ACCENT, LV_SYMBOL_PAUSE, &lv_font_montserrat_28);
    lv_obj_align(ui.b_pause, LV_ALIGN_CENTER, 66, 132);
    lv_obj_add_event_cb(ui.b_pause, pause_cb, LV_EVENT_CLICKED, NULL);
}

static void build(void)
{
    if (!ui.alive) return;
    tstate_t want = T.st == T_IDLE ? T_IDLE : T_RUN;     /* RUN and PAUSE share a page */
    if (ui.shown == (int)want) return;
    if (ui.setup) { lv_obj_delete(ui.setup); ui.setup = NULL; }
    if (ui.run)   { lv_obj_delete(ui.run); ui.run = NULL; }
    lv_obj_t *p = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, UI_W, UI_H);
    lv_obj_set_scrollable(p, false);
    if (want == T_IDLE) { ui.setup = p; build_setup(p); }
    else                { ui.run = p;   build_run(p); }
    ui.shown = want;
    ui.last_s = -1;
}

static void refresh(void)
{
    build();
    if (!ui.run) return;
    int64_t l = t_left();
    int64_t s = (l + 999999) / 1000000;
    lv_arc_set_value(ui.arc, T.total_us ? (int16_t)(l * 1000 / T.total_us) : 0);
    if (s == ui.last_s) return;
    ui.last_s = s;

    char buf[16];
    bool hours = fmt_left(l, buf, sizeof buf);
    lv_label_set_text(hours ? ui.big_h : ui.big, buf);
    lv_label_set_text(hours ? ui.big : ui.big_h, "");

    if (T.st == T_RUN) {
        datetime_t now;
        sys_get_time(&now);
        int64_t end = (int64_t)now.hour * 3600 + now.minute * 60 + now.second + s;
        lv_label_set_text_fmt(ui.sub, "Ends %02d:%02d", (int)(end / 3600) % 24, (int)(end / 60) % 60);
        ui_round_btn_set(ui.b_pause, UI_COL_ACCENT, LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(ui.big, UI_COL_TEXT, 0);
    } else {
        lv_label_set_text(ui.sub, "Paused");
        ui_round_btn_set(ui.b_pause, UI_COL_GREEN, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(ui.big, UI_COL_MUTED, 0);
    }
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static void tm_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.scr = scr;
    ui.shown = -1;
    refresh();
    ui.tmr = lv_timer_create(tick_cb, 100, NULL);
}

static void tm_destroy(void)
{
    if (ui.tmr) lv_timer_delete(ui.tmr);
    memset(&ui, 0, sizeof ui);
}

static bool tm_on_key(app_key_t k)
{
    if (k == APP_KEY_PRIMARY) {
        if (T.st == T_IDLE) do_start();
        else if (T.st == T_RUN) t_pause();
        else t_resume();
    } else if (T.st != T_IDLE) {
        t_cancel();
    }
    ui.last_s = -1;
    refresh();
    return true;
}

/* ------------------------------ background ------------------------------ */
static void done_cb(alert_action_t a)
{
    if (a == ALERT_SECONDARY) t_start(60LL * 1000000);    /* +1 min */
    else t_cancel();
    if (ui.alive) { ui.last_s = -1; refresh(); }
}

static void tm_bg_tick(void)
{
    if (T.st != T_RUN || t_left() > 0) return;
    T.st = T_PAUSE;           /* hold at 00:00 until the alert is answered */
    T.left_us = 0;
    char total[16];
    fmt_left(T.total_us, total, sizeof total);
    alert_show("TIME'S UP", total, COLOR, SND_TIMER, "+1 min", done_cb);
}

static const char *tm_status(void)
{
    static char buf[40];
    if (T.st == T_IDLE) return NULL;
    char b[16];
    fmt_left(t_left(), b, sizeof b);
    snprintf(buf, sizeof buf, "%s %s left", T.st == T_RUN ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, b);
    return buf;
}

const app_t app_timer = {
    .name    = "Timer",
    .symbol  = LV_SYMBOL_LOOP,
    .color   = COLOR,
    .create  = tm_create,
    .destroy = tm_destroy,
    .on_key  = tm_on_key,
    .status  = tm_status,
    .bg_tick = tm_bg_tick,
    .ready   = true,
};
