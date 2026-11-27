/*
 * Rides — journey tracking review (Beeline's "route saved for later review").
 *
 *  List:    current ride (REC + live figures) with Start / End, then saved rides,
 *           newest first: date, distance, time.
 *  Detail:  the route trace drawn to fit, distance, time (total + moving),
 *           average and top speed; 🗑 deletes.
 *  Keys:    BOOT tap = start / end the ride (list) or back (detail)
 *           BOOT hold = back to the list
 * Recording itself is ride.c, fed by nav_core with every GPS fix.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app_manager.h"
#include "ui_theme.h"
#include "ride.h"
#include "bos_faces.h"

#define COLOR 0xFFC107
#define TRACE_MAX 600

static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static struct {
    bool alive;
    lv_obj_t *scr, *list, *now_lbl, *now_btn, *detail;
    lv_timer_t *tmr;
    int shown_count;
    uint32_t msg_until;
    lv_point_precise_t *pts;
} ui;

static void fmt_date(uint32_t ep, char *o, size_t n)
{
    if (!ep) { snprintf(o, n, "Ride"); return; }
    long days = ep / 86400;                     /* civil date from days (Hinnant) */
    long z = days + 719468, era = z / 146097;
    long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    snprintf(o, n, "%d %s  %02u:%02u", d, MON[(m + 11) % 12], (unsigned)(ep / 3600 % 24), (unsigned)(ep / 60 % 60));
}
static void fmt_km(uint32_t m, char *o, size_t n) { snprintf(o, n, "%lu.%lu km", (unsigned long)(m / 1000), (unsigned long)(m % 1000 / 100)); }
static void fmt_time(uint32_t s, char *o, size_t n)
{
    if (s >= 3600) snprintf(o, n, "%luh %02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
    else snprintf(o, n, "%lu min", (unsigned long)((s + 30) / 60));
}

static void build_list(void);

/* ------------------------------ detail ------------------------------ */
static int s_detail_idx = -1;

static void close_detail(void)
{
    if (!ui.detail) return;
    lv_obj_delete(ui.detail);
    ui.detail = NULL;
    s_detail_idx = -1;
}
static void back_cb(lv_event_t *e) { close_detail(); }
static void del_cb(lv_event_t *e)
{
    int i = s_detail_idx;
    close_detail();
    ride_delete(i);
    build_list();
}

static void stat(lv_obj_t *p, const char *k, const char *v, int x, int y)
{
    lv_obj_t *a = lv_label_create(p);
    lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(a, UI_COL_MUTED, 0);
    lv_label_set_text(a, k);
    lv_obj_align(a, LV_ALIGN_TOP_MID, x, y);
    lv_obj_t *b = lv_label_create(p);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_24, 0);
    lv_label_set_text(b, v);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y + 18);
}

static void open_detail(int i)
{
    ride_summary_t r;
    if (!ride_get(i, &r)) return;
    s_detail_idx = i;
    ui.detail = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(ui.detail);
    lv_obj_set_size(ui.detail, UI_W, UI_H);
    lv_obj_set_style_bg_color(ui.detail, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ui.detail, LV_OPA_COVER, 0);
    lv_obj_set_clickable(ui.detail, true);
    lv_obj_set_scrollable(ui.detail, false);

    char b[40];
    fmt_date(r.start_epoch, b, sizeof b);
    lv_obj_t *t = lv_label_create(ui.detail);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_label_set_text(t, b);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);

    /* the trace, fitted into a 240 x 150 box, north up */
    static int32_t la[TRACE_MAX], lo[TRACE_MAX];
    int n = ride_trace(i, la, lo, TRACE_MAX);
    if (n >= 2) {
        int32_t mnla = la[0], mxla = la[0], mnlo = lo[0], mxlo = lo[0];
        for (int k = 1; k < n; k++) {
            if (la[k] < mnla) mnla = la[k];
            if (la[k] > mxla) mxla = la[k];
            if (lo[k] < mnlo) mnlo = lo[k];
            if (lo[k] > mxlo) mxlo = lo[k];
        }
        double kx = cos((mnla + mxla) / 2e7 * M_PI / 180.0);
        double w = (mxlo - mnlo) * kx, h = (mxla - mnla);
        double sc = fmin(236.0 / (w > 1 ? w : 1), 146.0 / (h > 1 ? h : 1));
        if (!ui.pts) ui.pts = lv_malloc(TRACE_MAX * sizeof(lv_point_precise_t));
        for (int k = 0; k < n; k++) {
            ui.pts[k].x = (lv_value_precise_t)(120 + ((lo[k] - mnlo) * kx - w / 2) * sc);
            ui.pts[k].y = (lv_value_precise_t)(75 - ((la[k] - mnla) - h / 2) * sc);
        }
        lv_obj_t *box = lv_obj_create(ui.detail);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 240, 150);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_t *ln = lv_line_create(box);
        lv_line_set_points(ln, ui.pts, (uint32_t)n);
        lv_obj_set_style_line_width(ln, 4, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(COLOR), 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_t *s0 = lv_obj_create(box);                 /* start / end dots */
        lv_obj_remove_style_all(s0);
        lv_obj_set_size(s0, 12, 12);
        lv_obj_set_style_radius(s0, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s0, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s0, UI_COL_GREEN, 0);
        lv_obj_set_pos(s0, (int32_t)ui.pts[0].x - 6, (int32_t)ui.pts[0].y - 6);
        lv_obj_t *s1 = lv_obj_create(box);
        lv_obj_remove_style_all(s1);
        lv_obj_set_size(s1, 12, 12);
        lv_obj_set_style_radius(s1, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s1, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s1, UI_COL_RED, 0);
        lv_obj_set_pos(s1, (int32_t)ui.pts[n - 1].x - 6, (int32_t)ui.pts[n - 1].y - 6);
    }

    char v[24];
    fmt_km(r.distance_m, v, sizeof v);       stat(ui.detail, "Distance", v, -72, 214);
    fmt_time(r.elapsed_s, v, sizeof v);      stat(ui.detail, "Time", v, 72, 214);
    float avg = r.moving_s ? r.distance_m / (float)r.moving_s * 3.6f : 0;
    snprintf(v, sizeof v, "%.1f km/h", avg); stat(ui.detail, "Average", v, -72, 266);
    snprintf(v, sizeof v, "%.0f km/h", r.max_cms * 0.036f); stat(ui.detail, "Top", v, 72, 266);

    lv_obj_t *bk = ui_round_btn(ui.detail, 56, lv_color_hex(0xDDDDDD), LV_SYMBOL_LEFT, &lv_font_montserrat_24);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, -44, -26);
    lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = ui_round_btn(ui.detail, 56, UI_COL_RED, LV_SYMBOL_TRASH, &lv_font_montserrat_24);
    lv_obj_align(dl, LV_ALIGN_BOTTOM_MID, 44, -26);
    lv_obj_add_event_cb(dl, del_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------ list ------------------------------ */
static void row_cb(lv_event_t *e) { open_detail((int)(intptr_t)lv_event_get_user_data(e)); }
static void now_cb(lv_event_t *e)
{
    if (ride_state() == RIDE_RECORDING) {
        ride_result_t res = ride_stop(true);
        build_list();
        if (res != RIDE_RES_SAVED) {                  /* say why nothing new is listed */
            lv_label_set_text(ui.now_lbl, bos_ride_result_text(res));
            ui.msg_until = lv_tick_get() + 4000;
        }
        return;
    }
    ride_start();
    build_list();
}

static void refresh_now(void)
{
    if (ui.msg_until && (int32_t)(lv_tick_get() - ui.msg_until) < 0) return;   /* a message is showing */
    ui.msg_until = 0;
    char b[64], d[16], t[16];
    const ride_summary_t *r = ride_current();
    if (ride_state() == RIDE_RECORDING) {
        fmt_km(r->distance_m, d, sizeof d);
        fmt_time(r->elapsed_s, t, sizeof t);
        snprintf(b, sizeof b, "#FF3B30 " LV_SYMBOL_BULLET "# REC  %s  " LV_SYMBOL_BULLET "  %s", d, t);
        lv_label_set_text(ui.now_lbl, b);
        ui_round_btn_set(ui.now_btn, UI_COL_RED, LV_SYMBOL_STOP);
    } else {
        lv_label_set_text(ui.now_lbl, ride_storage_ok() ? "Not recording" : "#FF4D4D Storage missing#: flash the full image");
        ui_round_btn_set(ui.now_btn, UI_COL_GREEN, LV_SYMBOL_PLAY);
    }
}

static void build_list(void)
{
    lv_obj_clean(ui.list);
    int n = ride_count();
    for (int i = 0; i < n; i++) {
        ride_summary_t r;
        ride_get(i, &r);
        lv_obj_t *row = lv_obj_create(ui.list);
        lv_obj_set_size(row, 290, 64);
        lv_obj_set_style_radius(row, 32, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        char b[40], d[16], t[16];
        fmt_date(r.start_epoch, b, sizeof b);
        lv_obj_t *a = lv_label_create(row);
        lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(COLOR), 0);
        lv_label_set_text(a, b);
        lv_obj_align(a, LV_ALIGN_TOP_LEFT, 24, 10);
        fmt_km(r.distance_m, d, sizeof d);
        fmt_time(r.elapsed_s, t, sizeof t);
        snprintf(b, sizeof b, "%s  " LV_SYMBOL_BULLET "  %s", d, t);
        lv_obj_t *c = lv_label_create(row);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_20, 0);
        lv_label_set_text(c, b);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 24, 30);
    }
    if (n == 0) {
        lv_obj_t *e = lv_label_create(ui.list);
        lv_obj_set_style_text_color(e, UI_COL_MUTED, 0);
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(e, "No saved rides yet\nRides record by themselves\nwhen you start riding");
    }
    ui.shown_count = n;
    refresh_now();
}

static void tick_cb(lv_timer_t *t)
{
    if (ui.detail) return;
    if (ride_count() != ui.shown_count) build_list();
    else refresh_now();
}

static void rd_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.scr = scr;

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_label_set_text(t, "RIDES");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);

    ui.now_lbl = lv_label_create(scr);
    lv_label_set_recolor(ui.now_lbl, true);
    lv_obj_set_style_text_font(ui.now_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(ui.now_lbl, LV_ALIGN_TOP_MID, -30, 74);
    ui.now_btn = ui_round_btn(scr, 48, UI_COL_GREEN, LV_SYMBOL_PLAY, &lv_font_montserrat_20);
    lv_obj_align(ui.now_btn, LV_ALIGN_TOP_MID, 116, 60);
    lv_obj_add_event_cb(ui.now_btn, now_cb, LV_EVENT_CLICKED, NULL);

    ui.list = lv_obj_create(scr);
    lv_obj_set_size(ui.list, 310, 270);
    lv_obj_align(ui.list, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_pad_all(ui.list, 2, 0);
    lv_obj_set_style_pad_row(ui.list, 10, 0);
    lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    build_list();
    ui.tmr = lv_timer_create(tick_cb, 1000, NULL);
}

static void rd_destroy(void)
{
    if (ui.tmr) lv_timer_delete(ui.tmr);
    if (ui.pts) lv_free(ui.pts);
    s_detail_idx = -1;
    memset(&ui, 0, sizeof ui);
}

static bool rd_on_key(app_key_t k)
{
    if (ui.detail) { close_detail(); return true; }
    if (k == APP_KEY_PRIMARY) { now_cb(NULL); return true; }
    return false;
}

static const char *rd_status(void)
{
    static char buf[32];
    const ride_summary_t *r = ride_current();
    if (ride_state() == RIDE_RECORDING) {
        snprintf(buf, sizeof buf, LV_SYMBOL_BULLET " REC %lu.%lu km", (unsigned long)(r->distance_m / 1000),
                 (unsigned long)(r->distance_m % 1000 / 100));
        return buf;
    }
    if (!ride_count()) return NULL;
    snprintf(buf, sizeof buf, "Last %lu.%lu km", (unsigned long)(r->distance_m / 1000), (unsigned long)(r->distance_m % 1000 / 100));
    return buf;
}

const app_t app_rides = {
    .name    = "Rides",
    .symbol  = LV_SYMBOL_DIRECTORY,
    .color   = COLOR,
    .create  = rd_create,
    .destroy = rd_destroy,
    .on_key  = rd_on_key,
    .status  = rd_status,
    .ready   = true,
};
