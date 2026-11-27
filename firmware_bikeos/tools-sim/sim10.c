/* bike-os v0.5 host test: firmware_ws146 navigation inside the bike-os app framework. */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "sys.h"
#include "app_manager.h"
#include "registry.h"
#include "settings.h"
#include "cfg.h"
#include "nav_core.h"
#include "ble_service.h"
#include "touch.h"
#include "alert.h"
#include "ride.h"
#include "map_view.h"
#include "nav_view.h"
#include "bos_faces.h"
#include <math.h>
#include <sys/stat.h>

extern int64_t g_fake_us;
extern uint32_t g_events;
extern lv_display_t *g_disp;
extern touch_sample_t g_touch;
extern const nav_ble_cbs_t *g_cbs;
extern int g_call_cmds, g_last_call_cmd, g_rtc_sets, g_backlight;

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)
#define CUR() (am_current() ? am_current()->name : "menu")

#define W 412
static uint16_t fb[W * W];
static uint8_t buf1[W * W * 2], buf2[W * W * 2];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    uint16_t *p = (uint16_t *)px;                      /* FULL mode: whole frame */
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = p[y * W + x];
    lv_display_flush_ready(d);
}
static void indev_cb(lv_indev_t *i, lv_indev_data_t *d)
{
    if (g_touch.n) { d->point.x = g_touch.x[0]; d->point.y = g_touch.y[0]; d->state = LV_INDEV_STATE_PRESSED; }
    else d->state = LV_INDEV_STATE_RELEASED;
}
static uint32_t tick(void) { return (uint32_t)(g_fake_us / 1000); }

/* the firmware's GUI loop, condensed */
static void run(int ms)
{
    for (int i = 0; i < ms; i += 5) { g_fake_us += 5000; nav_core_poll(); lv_timer_handler(); }
}
static void tap(int x, int y) { g_touch.n = 1; g_touch.x[0] = x; g_touch.y[0] = y; run(80); g_touch.n = 0; run(120); }
static void key(uint32_t ev, int ms) { g_events = ev; run(ms); }
static void shot(const char *name)
{
    lv_refr_now(NULL);
    char p[64];
    snprintf(p, sizeof p, "out/%s.ppm", name);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6 %d %d 255\n", W, W);
    for (int i = 0; i < W * W; i++) {
        int x = i % W, y = i / W, dx = x - 206, dy = y - 206, in = dx * dx + dy * dy <= 206 * 206;
        uint16_t c = fb[i];
        unsigned char px[3] = {in ? (c >> 11) << 3 : 40, in ? ((c >> 5) & 63) << 2 : 40, in ? (c & 31) << 3 : 40};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void nav_state(uint8_t flags, uint8_t man, uint16_t d, uint32_t rem_m, uint16_t rem_s, const char *street)
{
    nav_state_t st = {.flags = flags, .maneuver = man, .dist_m = d, .remaining_m = rem_m, .remaining_s = rem_s};
    g_cbs->on_nav_state(&st, street);
}
static void fix(double lat, double lon, uint16_t cms, uint16_t cog)
{
    nav_position_t p = {.lat_e7 = (int32_t)(lat * 1e7), .lon_e7 = (int32_t)(lon * 1e7), .speed_cms = cms, .cog_cdeg = cog, .sats = 9, .fix = 1};
    g_cbs->on_position(&p);
}


static void drag(int x0, int y0, int x1, int y1)
{
    for (int i = 0; i <= 12; i++) { g_touch.n = 1; g_touch.x[0] = x0 + (x1 - x0) * i / 12; g_touch.y[0] = y0 + (y1 - y0) * i / 12; run(20); }
    g_touch.n = 0; run(300);
}
#define SWIPE_NEXT() drag(300, 206, 110, 206)   /* right-to-left = next face */

/* nearby streets around (lat0, lon0): a grid + a diagonal arterial, as NAV_ROADS chunks */
static void send_roads(double lat0, double lon0)
{
    uint8_t b[252];
    int o = 0;
    b[o++] = NAV_ROADS; b[o++] = NAV_ROADS_F_START | NAV_ROADS_F_END; b[o++] = 0; b[o++] = 0;
    int32_t la = (int32_t)(lat0 * 1e7), lo = (int32_t)(lon0 * 1e7);
    memcpy(b + o, &la, 4); o += 4; memcpy(b + o, &lo, 4); o += 4;
    /* 1e-5 deg units: ~1.1 m. Streets every ~120 m, +-450 m */
    for (int k = -4; k <= 4 && o + 2 + 2 * 4 <= 240; k++) {
        int16_t v = (int16_t)(k * 110);
        b[o++] = 2; b[o++] = (k == 0) ? 1 : 0;                       /* N-S street */
        int16_t p[4] = {-420, v, 420, v};
        memcpy(b + o, p, 8); o += 8;
        b[o++] = 2; b[o++] = 0;                                         /* E-W street */
        int16_t q[4] = {v, -420, v, 420};
        memcpy(b + o, q, 8); o += 8;
    }
    b[o++] = 3; b[o++] = 1;                                             /* arterial */
    int16_t r[6] = {-400, -400, 0, 30, 380, 300};
    memcpy(b + o, r, 12); o += 12;
    g_cbs->on_roads_chunk(b, (size_t)o);
}

static void stops(uint8_t next, uint8_t count, uint32_t dist, uint16_t secs, const char *label)
{
    uint8_t b[NAV_STOPS_HDR + NAV_STOPS_LABEL_MAX] = {NAV_STOPS, next, count, 0};
    memcpy(b + 4, &dist, 4); memcpy(b + 8, &secs, 2);
    size_t n = strlen(label); if (n > NAV_STOPS_LABEL_MAX) n = NAV_STOPS_LABEL_MAX;
    b[10] = (uint8_t)n; memcpy(b + 11, label, n);
    g_cbs->on_stops(b, NAV_STOPS_HDR + n);
}


/* send a route as the phone does: 31-point chunks */
static void send_route(const int32_t *la, const int32_t *lo, int n)
{
    for (int i = 0, seq = 0; i < n; i += 31, seq++) {
        int k = n - i < 31 ? n - i : 31;
        uint8_t f = (i == 0 ? NAV_ROUTE_F_START : 0) | (i + k >= n ? NAV_ROUTE_F_END : 0);
        g_cbs->on_route_chunk(f, (uint8_t)seq, (uint8_t)k, la + i, lo + i);
    }
    run(100);
}
static int32_t la[2048], lo[2048];
static void at(int i, uint16_t cog) { fix(la[i] / 1e7, lo[i] / 1e7, 1100, cog); nav_state(NAV_F_ACTIVE, NAV_LEFT, 200, 9000, 900, "NH 48"); run(300); }

int main(void)
{
    mkdir("out", 0755);
    lv_init();
    lv_tick_set_cb(tick);
    g_disp = lv_display_create(W, W);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(g_disp, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_cb);
    settings_init(); cfg_init(); ride_init("out/rides"); nav_core_init(); am_init(); run(300);
    am_open(am_find("Navigation")); run(300);
    g_cbs->on_link(true);
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)settings_token_str(), 6); run(200);

    printf("LONG ROUTE, 2048 POINTS\n");
    /* 40 km north with a bend every 20 m (a winding road) */
    for (int i = 0; i < 2048; i++) {
        la[i] = (int32_t)((28.40 + i * 0.000175) * 1e7);
        lo[i] = (int32_t)((77.10 + ((i % 4) == 1 ? 0.00008 : (i % 4) == 3 ? -0.00008 : 0)) * 1e7);
    }
    send_route(la, lo, 2048);
    CHECK(map_view_route_points() == 2048, "all 2048 points held (was 512): %d", map_view_route_points());
    for (int i = 0; i <= 1200; i += 40) at(i, 0);
    float f0 = map_view_route_frac();
    CHECK(f0 > 0.55f && f0 < 0.62f, "marker found along the line at %.3f (1200/2047 = 0.586)", f0);
    shot("q01_long_route");
    send_route(la, lo, 2048);                                /* the phone resends as you go */
    run(50);
    float f1 = map_view_route_frac();
    CHECK(fabsf(f1 - f0) < 0.01f, "resend: picked up where we were (%.3f -> %.3f)", f0, f1);
    at(1240, 0);
    CHECK(map_view_route_frac() > f0, "and keeps following forward (%.3f)", map_view_route_frac());

    printf("OUT AND BACK ON THE SAME ROAD\n");
    /* 1024 points out, then back 12 m to the side: the legs sit side by side */
    for (int i = 0; i < 1024; i++) {
        la[i] = (int32_t)((28.50 + i * 0.00009) * 1e7); lo[i] = (int32_t)(77.20 * 1e7);
        la[2047 - i] = la[i]; lo[2047 - i] = (int32_t)((77.20 + 0.00012) * 1e7);
    }
    send_route(la, lo, 2048);
    for (int i = 0; i <= 300; i += 20) at(i, 0);
    float g0 = map_view_route_frac();
    CHECK(g0 > 0.12f && g0 < 0.17f, "on the way out at %.3f", g0);
    send_route(la, lo, 2048);
    run(50);
    at(310, 0);
    float g1 = map_view_route_frac();
    CHECK(g1 > 0.12f && g1 < 0.18f, "resend did not jump to the way back (%.3f, way back would be ~0.85)", g1);

    printf("\n%s (%d failures)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
