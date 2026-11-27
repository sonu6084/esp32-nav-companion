/*
 * Weather
 *
 *  Page 1 (Now):      icon, temperature, condition, feels-like / high-low, humidity / wind
 *  Page 2 (Forecast): next 4 hours + next 3 days
 *  Touch: swipe up/down = switch page
 *  Keys:  BOOT tap = refresh now     BOOT hold = switch page
 *
 * Reads the shared weather store; it never fetches by itself (see weather_net.c).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app_manager.h"
#include "ui_theme.h"
#include "weather_store.h"
#include "fonts.h"
#include "sys.h"

static const char *const DOW[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
#define COL_RAIN lv_color_hex(0x4FC3F7)

static struct {
    bool alive;
    lv_obj_t *tv, *t_now, *t_fc;
    /* now */
    lv_obj_t *icon, *temp, *cond, *line1, *line2, *upd, *msg;
    /* forecast */
    lv_obj_t *h_time[4], *h_icon[4], *h_temp[4], *h_pop[4];
    lv_obj_t *d_name[3], *d_icon[3], *d_temp[3], *d_pop[3];
    lv_obj_t *fc_msg;
    lv_timer_t *tmr;
    uint32_t seq;
    int state;
} ui;

static int rnd(float v) { return (int)lroundf(v); }

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, lv_align_t a, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, a, x, y);
    return l;
}

static void build_now(lv_obj_t *p)
{
    ui.upd   = label(p, &lv_font_montserrat_16, UI_COL_MUTED, LV_ALIGN_TOP_MID, 0, 34);
    ui.icon  = lv_image_create(p);
    lv_obj_align(ui.icon, LV_ALIGN_CENTER, -78, -52);
    ui.temp  = label(p, &font_temp_100, UI_COL_TEXT, LV_ALIGN_CENTER, 62, -48);
    ui.cond  = label(p, &lv_font_montserrat_28, UI_COL_ACCENT, LV_ALIGN_CENTER, 0, 36);
    ui.line1 = label(p, &lv_font_montserrat_20, UI_COL_TEXT, LV_ALIGN_CENTER, 0, 78);
    ui.line2 = label(p, &lv_font_montserrat_16, UI_COL_MUTED, LV_ALIGN_CENTER, 0, 110);
    lv_obj_t *hint = label(p, &lv_font_montserrat_16, UI_COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_label_set_text(hint, LV_SYMBOL_DOWN " forecast");
    ui.msg   = label(p, &lv_font_montserrat_20, UI_COL_MUTED, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ui.msg, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_fc(lv_obj_t *p)
{
    lv_obj_t *t1 = label(p, &lv_font_montserrat_16, UI_COL_ACCENT, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(t1, "NEXT HOURS");
    for (int i = 0; i < 4; i++) {
        int x = -129 + i * 86;
        ui.h_time[i] = label(p, &lv_font_montserrat_16, UI_COL_MUTED, LV_ALIGN_CENTER, x, -118);
        ui.h_icon[i] = lv_image_create(p);
        lv_obj_align(ui.h_icon[i], LV_ALIGN_CENTER, x, -76);
        ui.h_temp[i] = label(p, &lv_font_montserrat_24, UI_COL_TEXT, LV_ALIGN_CENTER, x, -34);
        ui.h_pop[i]  = label(p, &lv_font_montserrat_14, COL_RAIN, LV_ALIGN_CENTER, x, -10);
    }
    lv_obj_t *t2 = label(p, &lv_font_montserrat_16, UI_COL_ACCENT, LV_ALIGN_CENTER, 0, 22);
    lv_label_set_text(t2, "NEXT DAYS");
    for (int i = 0; i < 3; i++) {
        int y = 58 + i * 42;
        ui.d_name[i] = label(p, &lv_font_montserrat_20, UI_COL_TEXT, LV_ALIGN_CENTER, -112, y);
        ui.d_icon[i] = lv_image_create(p);
        lv_image_set_scale(ui.d_icon[i], 205);           /* 56 px -> ~45 px */
        lv_obj_align(ui.d_icon[i], LV_ALIGN_CENTER, -54, y);
        ui.d_temp[i] = label(p, &lv_font_montserrat_20, UI_COL_TEXT, LV_ALIGN_CENTER, 28, y);
        ui.d_pop[i]  = label(p, &lv_font_montserrat_16, COL_RAIN, LV_ALIGN_CENTER, 108, y);
    }
    ui.fc_msg = label(p, &lv_font_montserrat_20, UI_COL_MUTED, LV_ALIGN_CENTER, 0, 0);
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_set_hidden(o, false);
    else lv_obj_set_hidden(o, true);
}

static void render(void)
{
    wx_data_t d;
    wx_get(&d);
    wx_state_t st = wx_state();
    bool has = d.valid;

    /* --- now page --- */
    lv_obj_t *now_parts[] = {ui.icon, ui.temp, ui.cond, ui.line1, ui.line2};
    for (int i = 0; i < 5; i++) show(now_parts[i], has);
    show(ui.msg, !has);

    const char *busy = (st == WX_ST_CONNECTING || st == WX_ST_FETCHING) ? "  " LV_SYMBOL_REFRESH : "";
    if (!has) {
        lv_label_set_text(ui.upd, "");
        lv_label_set_text(ui.msg, st == WX_ST_IDLE ? "Waiting for weather..." : wx_state_text(st));
    } else {
        lv_image_set_src(ui.icon, wx_icons_120[wx_code_icon(d.code, d.is_day)]);
        lv_label_set_text_fmt(ui.temp, "%d\xc2\xb0", rnd(d.temp));
        lv_label_set_text(ui.cond, wx_code_text(d.code));
        if (d.n_days > 0)
            lv_label_set_text_fmt(ui.line1, "Feels %d\xc2\xb0   H %d\xc2\xb0  L %d\xc2\xb0",
                                  rnd(d.feels), rnd(d.days[0].tmax), rnd(d.days[0].tmin));
        else
            lv_label_set_text_fmt(ui.line1, "Feels %d\xc2\xb0", rnd(d.feels));

        char hum[16] = "", wind[24] = "";
        if (d.humidity >= 0) snprintf(hum, sizeof hum, "Hum %d%%", d.humidity);
        if (d.wind_kmh >= 0) snprintf(wind, sizeof wind, "Wind %d km/h", rnd(d.wind_kmh));
        lv_label_set_text_fmt(ui.line2, "%s%s%s", hum, (hum[0] && wind[0]) ? "   " : "", wind);

        bool err = st >= WX_ST_ERR_WIFI;
        lv_label_set_text_fmt(ui.upd, "%s %02d:%02d%s", err ? "Old data" : "Updated",
                              d.obs.hour, d.obs.minute, busy);
        lv_obj_set_style_text_color(ui.upd, err ? UI_COL_RED : UI_COL_MUTED, 0);
    }

    /* --- forecast page --- */
    show(ui.fc_msg, !has);
    lv_label_set_text(ui.fc_msg, has ? "" : "No forecast yet");
    for (int i = 0; i < 4; i++) {
        bool on = has && i < d.n_hours;
        show(ui.h_time[i], on); show(ui.h_icon[i], on); show(ui.h_temp[i], on); show(ui.h_pop[i], on);
        if (!on) continue;
        const wx_hour_t *h = &d.hours[i];
        lv_label_set_text_fmt(ui.h_time[i], "%02d:00", h->hour);
        lv_image_set_src(ui.h_icon[i], wx_icons_56[wx_code_icon(h->code, h->is_day)]);
        lv_label_set_text_fmt(ui.h_temp[i], "%d\xc2\xb0", rnd(h->temp));
        if (h->pop > 0) lv_label_set_text_fmt(ui.h_pop[i], "%d%%", h->pop);
        else lv_label_set_text(ui.h_pop[i], "");
    }
    for (int i = 0; i < 3; i++) {
        bool on = has && (i + 1) < d.n_days;
        show(ui.d_name[i], on); show(ui.d_icon[i], on); show(ui.d_temp[i], on); show(ui.d_pop[i], on);
        if (!on) continue;
        const wx_day_t *w = &d.days[i + 1];
        lv_label_set_text(ui.d_name[i], i == 0 ? "Tmrw" : DOW[w->dow % 7]);
        lv_image_set_src(ui.d_icon[i], wx_icons_56[wx_code_icon(w->code, true)]);
        lv_label_set_text_fmt(ui.d_temp[i], "%d\xc2\xb0 / %d\xc2\xb0", rnd(w->tmax), rnd(w->tmin));
        if (w->pop > 0) lv_label_set_text_fmt(ui.d_pop[i], "%d%%", w->pop);
        else lv_label_set_text(ui.d_pop[i], "");
    }
}

static void tick_cb(lv_timer_t *t)
{
    uint32_t s = wx_seq();
    int st = wx_state();
    if (s == ui.seq && st == ui.state) return;
    ui.seq = s;
    ui.state = st;
    render();
}

static void flip_page(void)
{
    /* lv_tileview_get_tile_active() is NULL until the first scroll, so derive the page
       from the scroll position instead */
    bool on_fc = lv_obj_get_scroll_y(ui.tv) > UI_H / 2;
    lv_tileview_set_tile(ui.tv, on_fc ? ui.t_now : ui.t_fc, LV_ANIM_ON);
}

static lv_obj_t *mk_tile(int row)
{
    lv_obj_t *t = lv_tileview_add_tile(ui.tv, 0, row, row == 0 ? LV_DIR_BOTTOM : LV_DIR_TOP);
    lv_obj_set_scrollable(t, false);
    return t;
}

static void wx_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.state = -1;

    ui.tv = lv_tileview_create(scr);
    lv_obj_set_size(ui.tv, UI_W, UI_H);
    lv_obj_set_style_bg_opa(ui.tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(ui.tv, LV_SCROLLBAR_MODE_OFF);
    ui.t_now = mk_tile(0);
    ui.t_fc  = mk_tile(1);
    build_now(ui.t_now);
    build_fc(ui.t_fc);

    ui.seq = wx_seq();
    render();
    ui.tmr = lv_timer_create(tick_cb, 300, NULL);

    /* opening the app with stale or no data triggers a refresh */
    wx_data_t d;
    wx_get(&d);
    datetime_t now;
    sys_get_time(&now);
    int age = (now.hour * 60 + now.minute) - (d.obs.hour * 60 + d.obs.minute);
    if (!d.valid || d.obs.day != now.day || age > 15 || age < 0) wx_request_refresh();
}

static void wx_destroy(void)
{
    if (ui.tmr) lv_timer_delete(ui.tmr);
    memset(&ui, 0, sizeof ui);
}

static bool wx_on_key(app_key_t k)
{
    if (k == APP_KEY_PRIMARY) wx_request_refresh();
    else flip_page();
    return true;
}

static const char *wx_status(void)
{
    static char buf[32];
    wx_data_t d;
    wx_get(&d);
    if (!d.valid) return NULL;
    snprintf(buf, sizeof buf, "%d\xc2\xb0  %s", rnd(d.temp), wx_code_text(d.code));
    return buf;
}

const app_t app_weather = {
    .name    = "Weather",
    .symbol  = LV_SYMBOL_IMAGE,
    .color   = 0x64B5F6,
    .create  = wx_create,
    .destroy = wx_destroy,
    .on_key  = wx_on_key,
    .status  = wx_status,
    .ready   = true,
};
