#include "bos_faces.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "ble_service.h"
#include "map_view.h"
#include "ride.h"

#define W        412
#define C        206                 /* centre */
#define YEL      0xFFC107            /* Beeline-style accent */
#define GREY     0x9AA0A6
#define TRACK    0x3C4043

/* ------------------------------ waypoints (NAV_STOPS) ------------------------------ */
static struct {
    bool    present;
    uint8_t next, count;
    uint32_t dist_m;
    uint16_t secs;
    char    label[NAV_STOPS_LABEL_MAX + 1];
} S;

void bos_stops_set(const uint8_t *b, size_t len)
{
    if (len < NAV_STOPS_HDR) return;
    S.next = b[1];
    S.count = b[2];
    S.dist_m = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    S.secs = (uint16_t)(b[8] | b[9] << 8);
    size_t n = b[10] > NAV_STOPS_LABEL_MAX ? NAV_STOPS_LABEL_MAX : b[10];
    if (n > len - NAV_STOPS_HDR) n = len - NAV_STOPS_HDR;
    /* the font is Latin only: keep printable ASCII, drop the rest (like media/nav text) */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = b[NAV_STOPS_HDR + i];
        if (ch >= 0x20 && ch < 0x7F) S.label[o++] = (char)ch;
    }
    S.label[o] = 0;
    S.present = S.count > 0;
}
void bos_stops_clear(void) { memset(&S, 0, sizeof S); }
bool bos_stops_present(void) { return S.present; }

/* ------------------------------ drawing helpers ------------------------------ */
static void text(lv_layer_t *l, const char *t, const lv_font_t *f, uint32_t col, int cx, int cy, int w,
                 lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.font = f;
    d.color = lv_color_hex(col);
    d.align = al;
    int h = f->line_height;
    lv_area_t a = {cx - w / 2, cy - h / 2, cx + w / 2 - 1, cy + h / 2};
    if (al == LV_TEXT_ALIGN_LEFT) { a.x1 = cx; a.x2 = cx + w - 1; }
    if (al == LV_TEXT_ALIGN_RIGHT) { a.x1 = cx - w; a.x2 = cx - 1; }
    lv_draw_label(l, &d, &a);
}
#define CTEXT(l, t, f, col, cx, cy, w) text(l, t, f, col, cx, cy, w, LV_TEXT_ALIGN_CENTER)

static void arc(lv_layer_t *l, int cx, int cy, int r, int w, int a0, int a1, uint32_t col, bool round)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx; d.center.y = cy;
    d.radius = r; d.width = w;
    d.start_angle = a0; d.end_angle = a1;
    d.color = lv_color_hex(col);
    d.rounded = round;
    lv_draw_arc(l, &d);
}

static void disc(lv_layer_t *l, int cx, int cy, int r, uint32_t col)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(col);
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(l, &d, &a);
}

static void line(lv_layer_t *l, float x1, float y1, float x2, float y2, int w, uint32_t col)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(col);
    d.round_start = d.round_end = 1;
    lv_draw_line(l, &d);
}

static void box(lv_layer_t *l, int x1, int y1, int x2, int y2, int r, uint32_t border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = r;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_hex(border);
    d.border_width = 2;
    d.border_opa = LV_OPA_COVER;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(l, &d, &a);
}

static void icon_clock(lv_layer_t *l, int x, int y, int r, uint32_t col)
{
    arc(l, x, y, r, 2, 0, 360, col, false);
    line(l, x, y, x, y - r * 6 / 10, 2, col);
    line(l, x, y, x + r * 5 / 10, y, 2, col);
}
static void icon_flag(lv_layer_t *l, int x, int y, int r, uint32_t col)
{
    line(l, x - r * 6 / 10, y + r, x - r * 6 / 10, y - r, 2, col);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {x - r * 6 / 10, y - r, x + r * 7 / 10, y};
    lv_draw_rect(l, &d, &a);
}
static void icon_route(lv_layer_t *l, int x, int y, uint32_t col)   /* two stops and a winding path */
{
    arc(l, x - 8, y - 8, 4, 2, 0, 360, col, false);
    disc(l, x + 9, y + 9, 3, col);
    line(l, x - 8, y - 3, x - 2, y + 2, 2, col);
    line(l, x - 2, y + 2, x + 4, y + 1, 2, col);
    line(l, x + 4, y + 1, x + 8, y + 6, 2, col);
}
static void icon_speed(lv_layer_t *l, int x, int y, uint32_t col)   /* little gauge */
{
    arc(l, x, y + 3, 11, 2, 180, 360, col, true);
    line(l, x, y + 3, x + 6, y - 4, 2, col);
}
static void check_badge(lv_layer_t *l, int x, int y)
{
    disc(l, x, y, 18, YEL);
    line(l, x - 8, y, x - 2, y + 6, 4, 0x000000);
    line(l, x - 2, y + 6, x + 9, y - 6, 4, 0x000000);
}

static void fmt_clock(char *o, size_t n, uint32_t ep, bool h24)
{
    uint32_t m = ep / 60 % 1440;
    unsigned h = m / 60, mm = m % 60;
    if (h24) snprintf(o, n, "%02u:%02u", h, mm);
    else snprintf(o, n, "%u:%02u %s", h % 12 ? h % 12 : 12, mm, h < 12 ? "am" : "pm");
}
static void fmt_dur(char *o, size_t n, int s)
{
    int m = (s + 30) / 60;
    if (m >= 60) snprintf(o, n, "%dhr %dm", m / 60, m % 60);
    else snprintf(o, n, "%d min", m);
}
static void fmt_dist(char *o, size_t n, uint32_t m, bool mph)
{
    if (mph) { uint32_t t = m * 10 / 1609; snprintf(o, n, "%lu.%lu mi", (unsigned long)(t / 10), (unsigned long)(t % 10)); }
    else if (m < 1000) snprintf(o, n, "%lu m", (unsigned long)m);
    else snprintf(o, n, "%lu.%lu km", (unsigned long)(m / 1000), (unsigned long)(m % 1000 / 100));
}

/* ------------------------------ journey progress ------------------------------ */
void bos_draw_eta(lv_layer_t *l, const bos_face_ctx_t *c)
{
    const int r = 197;                                   /* pin sits below the page dots */
    arc(l, C, C, r, 10, 0, 360, TRACK, false);
    int p = c->progress_permille;
    if (p >= 0) {
        int end = 90 + 360 * p / 1000;
        if (p > 0) arc(l, C, C, r, 10, 90, end, 0xFFFFFF, true);
        float a = end * (float)M_PI / 180.0f;
        disc(l, C + (int)(cosf(a) * r), C + (int)(sinf(a) * r), 9, 0xFFFFFF);   /* you */
    }
    /* destination pin at the bottom, where the ring closes */
    disc(l, C, C + r - 1, 9, YEL);
    disc(l, C, C + r - 1, 3, 0x000000);

    char b[32];
    fmt_dur(b, sizeof b, c->remaining_s > 0 ? c->remaining_s : 0);
    CTEXT(l, b, &lv_font_montserrat_48, YEL, C, 132, 340);
    if (c->clock_epoch) {
        char now[16], eta[16];
        fmt_clock(now, sizeof now, c->clock_epoch, c->clock_24h);
        fmt_clock(eta, sizeof eta, c->clock_epoch + (uint32_t)(c->remaining_s > 0 ? c->remaining_s : 0), c->clock_24h);
        CTEXT(l, now, &lv_font_montserrat_32, 0xFFFFFF, C - 66, 202, 150);
        CTEXT(l, eta, &lv_font_montserrat_32, 0xFFFFFF, C + 66, 202, 150);
        icon_clock(l, C - 66, 236, 8, GREY);
        icon_flag(l, C + 66, 236, 8, GREY);
    }
    fmt_dist(b, sizeof b, (uint32_t)(c->remaining_m > 0 ? c->remaining_m : 0), c->mph);
    CTEXT(l, b, &lv_font_montserrat_28, 0xFFFFFF, C, 292, 220);
}

/* ------------------------------ waypoints ------------------------------ */
void bos_draw_stops(lv_layer_t *l, const bos_face_ctx_t *c)
{
    /* stop pin with chevrons */
    disc(l, C, 104, 22, 0x5F6368);
    line(l, C - 9, 96, C - 2, 104, 3, 0xFFFFFF);
    line(l, C - 2, 104, C - 9, 112, 3, 0xFFFFFF);
    line(l, C + 1, 96, C + 8, 104, 3, 0xFFFFFF);
    line(l, C + 8, 104, C + 1, 112, 3, 0xFFFFFF);

    char b[48];
    if (S.next == 0) {
        CTEXT(l, "All stops", &lv_font_montserrat_40, 0xFFFFFF, C, 196, 320);
        CTEXT(l, "reached", &lv_font_montserrat_24, GREY, C, 240, 220);
        check_badge(l, C, 330);
        return;
    }
    snprintf(b, sizeof b, "%u", S.next);
    text(l, b, &lv_font_montserrat_48, 0xFFFFFF, C + 4, 190, 120, LV_TEXT_ALIGN_RIGHT);
    snprintf(b, sizeof b, "/%u", S.count);
    text(l, b, &lv_font_montserrat_32, 0xFFFFFF, C + 4, 196, 120, LV_TEXT_ALIGN_LEFT);
    CTEXT(l, "waypoints", &lv_font_montserrat_24, 0xFFFFFF, C, 238, 240);
    if (S.label[0]) CTEXT(l, S.label, &lv_font_montserrat_20, YEL, C, 284, 300);
    char d[16], t[16];
    fmt_dist(d, sizeof d, S.dist_m, c->mph);
    fmt_dur(t, sizeof t, S.secs);
    snprintf(b, sizeof b, "%s  " LV_SYMBOL_BULLET "  %s", d, t);
    CTEXT(l, b, &lv_font_montserrat_20, GREY, C, 318, 280);
}
int32_t bos_key_stops(void)
{
    return (int32_t)((S.next * 64u + S.count) * 65536u + (S.dist_m / 100u) % 65536u) & 0x7FFFFFFF;
}

/* ------------------------------ compass + odometer ------------------------------ */
/* The heading is the GPS course from the phone: the board has no magnetometer
   (QMI8658 = accelerometer + gyro), so it only means something while moving.
   Held for HEADING_HOLD_MS after slowing down so a red light does not blank it. */
#define HEADING_HOLD_MS 10000
static uint32_t s_head_ok_ms;
static uint16_t s_head_cdeg;

static bool heading_ok(const bos_face_ctx_t *c)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    if (c->live && ride_moving_now()) {
        s_head_ok_ms = t ? t : 1;
        s_head_cdeg = c->cog_cdeg;
    }
    return s_head_ok_ms && t - s_head_ok_ms < HEADING_HOLD_MS;
}

void bos_draw_track(lv_layer_t *l, const bos_face_ctx_t *c)
{
    bool ok = heading_ok(c);
    float hd = ok ? s_head_cdeg / 100.0f : 0.0f;        /* heading-up ring; north-up when unknown */
    for (int deg = 0; deg < 360; deg += 5) {
        float a = (deg - hd - 90.0f) * (float)M_PI / 180.0f;
        float ca = cosf(a), sa = sinf(a);
        bool major = deg % 30 == 0;
        int r0 = major ? 180 : 188, r1 = 198;
        if (deg % 90 == 0) continue;                    /* letters instead */
        line(l, C + ca * r0, C + sa * r0, C + ca * r1, C + sa * r1, major ? 3 : 2,
             !ok ? 0x3C4043 : major ? 0xFFFFFF : 0x80868B);
    }
    static const char *const card[4] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; i++) {
        float a = (i * 90 - hd - 90.0f) * (float)M_PI / 180.0f;
        CTEXT(l, card[i], &lv_font_montserrat_20, !ok ? 0x5F6368 : i == 0 ? 0xFF5252 : 0xFFFFFF,
              C + (int)(cosf(a) * 186), C + (int)(sinf(a) * 186), 40);
    }
    /* fixed marker: where you are heading */
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(0xFF3B30);
    t.p[0].x = C - 11; t.p[0].y = 8;
    t.p[1].x = C + 11; t.p[1].y = 8;
    t.p[2].x = C;      t.p[2].y = 28;
    if (ok) lv_draw_triangle(l, &t);

    static const char *const pts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int deg = ((int)lroundf(hd)) % 360;
    char b[24];
    if (ok) {
        snprintf(b, sizeof b, "%d\xC2\xB0", deg);
        CTEXT(l, b, &lv_font_montserrat_32, YEL, C, 74, 160);
        CTEXT(l, pts[(deg * 2 + 45) / 90 % 8], &lv_font_montserrat_20, YEL, C, 104, 80);
    } else {
        CTEXT(l, "--\xC2\xB0", &lv_font_montserrat_32, GREY, C, 74, 160);
        CTEXT(l, "Ride to see heading", &lv_font_montserrat_16, GREY, C, 106, 220);
    }
    bool rec = ride_state() == RIDE_RECORDING;
    CTEXT(l, rec ? "RIDE" : "TRIP", &lv_font_montserrat_16, rec ? 0xFF3B30 : GREY, C, 150, 80);

    /* odometer: the ride's distance while recording, else the trip since power-on */
    double km = ride_state() == RIDE_RECORDING ? ride_current()->distance_m / 1000.0 : c->trip_m / 1000.0;
    if (c->mph) km = km / 1.609344;
    long tenths = lround(km * 10.0) % 10000;
    if (tenths < 0) tenths = 0;
    char dg[12];
    snprintf(dg, sizeof dg, "%04ld", tenths);
    const int bw = 52, bh = 66, gap = 6, x0 = C - (3 * bw + 2 * gap + 8 + 36) / 2, y0 = C - bh / 2 + 4;
    for (int i = 0; i < 3; i++) {
        int x = x0 + i * (bw + gap);
        box(l, x, y0, x + bw, y0 + bh, 6, 0xFFFFFF);
        char d1[2] = {dg[i], 0};
        CTEXT(l, d1, &lv_font_montserrat_48, 0xFFFFFF, x + bw / 2, y0 + bh / 2, bw);
    }
    int xs = x0 + 3 * (bw + gap) + 2;
    box(l, xs, y0, xs + 30, y0 + 36, 4, 0xFFFFFF);
    char d2[2] = {dg[3], 0};
    CTEXT(l, d2, &lv_font_montserrat_24, 0xFFFFFF, xs + 15, y0 + 18, 30);
    CTEXT(l, c->mph ? "mi" : "km", &lv_font_montserrat_20, 0xFFFFFF, xs + 15, y0 + 54, 40);

    if (c->clock_epoch) {
        fmt_clock(b, sizeof b, c->clock_epoch, c->clock_24h);
        CTEXT(l, b, &lv_font_montserrat_32, 0xFFFFFF, C, 292, 220);
    }
}
int32_t bos_key_track(const bos_face_ctx_t *c)
{
    uint32_t d = ride_state() == RIDE_RECORDING ? ride_current()->distance_m / 100 : (uint32_t)(c->trip_m / 100);
    bool ok = heading_ok(c);
    uint32_t h = ok ? s_head_cdeg / 100u + 1u : 0u;       /* 0 = no heading */
    return (int32_t)(((((h * 8192u + d % 8192u) * 1440u + c->clock_epoch / 60 % 1440) * 2u) +
                      (ride_state() == RIDE_RECORDING)) & 0x7FFFFFFF);
}

/* ------------------------------ ride ------------------------------ */
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t s_end_tap_ms;

void bos_draw_ride(lv_layer_t *l, const bos_face_ctx_t *c)
{
    bool rec = ride_state() == RIDE_RECORDING, saved = ride_just_saved(now_ms());
    if (!rec && !saved) {                                  /* Start ride */
        arc(l, C, 128, 22, 3, 0, 360, 0xFFFFFF, false);
        disc(l, C, 128, 10, 0xFFFFFF);
        CTEXT(l, "Start", &lv_font_montserrat_40, 0xFFFFFF, C, 196, 260);
        CTEXT(l, "ride", &lv_font_montserrat_40, 0xFFFFFF, C, 242, 260);
        CTEXT(l, "Tap to record", &lv_font_montserrat_16, GREY, C, 306, 240);
        return;
    }
    const ride_summary_t *r = ride_current();
    char b[40];
    if (rec) {
        disc(l, C - 52, 92, 7, 0xFF3B30);
        int s = (int)r->elapsed_s;
        snprintf(b, sizeof b, "REC %d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        text(l, b, &lv_font_montserrat_20, 0xFFFFFF, C - 38, 92, 200, LV_TEXT_ALIGN_LEFT);
    }
    const int xi = C - 92, xt = C - 66;
    icon_route(l, xi, 150, YEL);
    fmt_dist(b, sizeof b, r->distance_m, c->mph);
    text(l, b, &lv_font_montserrat_32, 0xFFFFFF, xt, 150, 220, LV_TEXT_ALIGN_LEFT);
    icon_clock(l, xi, 204, 10, YEL);
    int m = (int)(r->elapsed_s + 30) / 60;
    if (m >= 60) snprintf(b, sizeof b, "%d hr %d min", m / 60, m % 60);
    else snprintf(b, sizeof b, "%d min", m);
    text(l, b, &lv_font_montserrat_28, 0xFFFFFF, xt, 204, 220, LV_TEXT_ALIGN_LEFT);
    icon_speed(l, xi, 256, YEL);
    float avg = r->moving_s ? r->distance_m / (float)r->moving_s * (c->mph ? 2.23694f : 3.6f) : 0;
    snprintf(b, sizeof b, "%.1f %s", avg, c->mph ? "mph" : "km/h");
    text(l, b, &lv_font_montserrat_28, 0xFFFFFF, xt, 256, 220, LV_TEXT_ALIGN_LEFT);
    if (rec) CTEXT(l, "Tap twice to end", &lv_font_montserrat_16, GREY, C, 318, 240);
    else check_badge(l, C, 330);                           /* saved */
}

int32_t bos_key_ride(void)
{
    const ride_summary_t *r = ride_current();
    return (int32_t)((((uint32_t)ride_state() * 2u + (ride_just_saved(now_ms()) ? 1u : 0u)) * 65536u +
                      (r->distance_m / 10u) % 65536u) * 4096u + r->elapsed_s % 4096u) & 0x7FFFFFFF;
}

const char *bos_ride_result_text(int res)
{
    switch (res) {
    case RIDE_RES_SAVED:      return "Ride saved";
    case RIDE_RES_TOO_SHORT:  return "Under 50 m: not saved";
    case RIDE_RES_NO_STORAGE: return "Can't save: flash full image";
    default:                  return "Ride ended";
    }
}

void bos_tap_ride(void)
{
    uint32_t t = now_ms();
    if (ride_state() != RIDE_RECORDING) {
        ride_start();
        map_view_toast("Ride started");
    } else if (s_end_tap_ms && t - s_end_tap_ms < 2000) {
        s_end_tap_ms = 0;
        map_view_toast(bos_ride_result_text(ride_stop(true)));
    } else {
        s_end_tap_ms = t;
        map_view_toast("Tap again to end the ride");
    }
}

/* ------------------------------ rejoin route ------------------------------ */
int bos_nearest_on_route(const int32_t *lat, const int32_t *lon, int n, int32_t plat, int32_t plon,
                         int32_t *out_lat, int32_t *out_lon, double *dist_m)
{
    /* local equirectangular metres around the position */
    const double ky = 6371000.0 * M_PI / 180.0 / 1e7;
    const double kx = ky * cos(plat / 1e7 * M_PI / 180.0);
    double best = 1e18;
    int bi = 0;
    double bx = 0, by = 0;
    for (int i = 0; i + 1 < n; i++) {
        double ax = (lon[i] - plon) * kx, ay = (lat[i] - plat) * ky;
        double cx = (lon[i + 1] - plon) * kx, cy = (lat[i + 1] - plat) * ky;
        double dx = cx - ax, dy = cy - ay, L = dx * dx + dy * dy;
        double t = L > 0 ? -(ax * dx + ay * dy) / L : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double qx = ax + dx * t, qy = ay + dy * t, d = qx * qx + qy * qy;
        if (d < best) { best = d; bi = i; bx = qx; by = qy; }
    }
    *out_lat = plat + (int32_t)lround(by / ky);
    *out_lon = plon + (int32_t)lround(bx / kx);
    *dist_m = sqrt(best);
    return bi;
}
