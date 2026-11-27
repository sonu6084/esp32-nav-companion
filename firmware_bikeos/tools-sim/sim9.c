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
/* same as main/nav/display.c indev_read_cb (bezel at 98 % of the radius) */
#define BEZ (206 * 98 / 100)
static void indev_cb(lv_indev_t *i, lv_indev_data_t *d)
{
    static bool down;
    if (!g_touch.n) { down = false; d->state = LV_INDEV_STATE_RELEASED; return; }
    int dx = g_touch.x[0] - 206, dy = g_touch.y[0] - 206, d2 = dx * dx + dy * dy;
    if (d2 > BEZ * BEZ) {
        if (!down) { d->state = LV_INDEV_STATE_RELEASED; return; }
        float k = BEZ / sqrtf((float)d2);
        d->point.x = 206 + (int32_t)(dx * k); d->point.y = 206 + (int32_t)(dy * k);
    } else { d->point.x = g_touch.x[0]; d->point.y = g_touch.y[0]; }
    down = true;
    d->state = LV_INDEV_STATE_PRESSED;
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


#include "touch_filter.h"
#include <stdlib.h>
static touch_filter_t filt;
static int use_filter;
/* One finger on the real chip: polled every 10 ms, and every third poll lands
   between two controller reports and comes back empty. */
static void real_swipe(int x0, int y0, int x1, int y1, int ms, int arc)
{
    int steps = ms / 10;
    for (int i = 0; i <= steps; i++) {
        touch_sample_t raw = {0};
        if (i % 3 != 2) {
            float t = (float)i / steps;
            raw.n = 1;
            raw.x[0] = (int16_t)(x0 + (x1 - x0) * t);
            raw.y[0] = (int16_t)(y0 + (y1 - y0) * t - arc * 4 * t * (1 - t));
        }
        if (use_filter) touch_filter_feed(&filt, &raw, g_fake_us); else g_touch = raw;
        if (use_filter) g_touch = filt.out;
        run(10);
    }
    for (int i = 0; i < 20; i++) {                 /* finger off, 200 ms */
        touch_sample_t raw = {0};
        if (use_filter) { touch_filter_feed(&filt, &raw, g_fake_us); g_touch = filt.out; } else g_touch = raw;
        run(10);
    }
}
static void real_tap(int x, int y) { real_swipe(x, y, x + 2, y + 1, 120, 0); }


static double plat = 28.6139, plon = 77.2090;
/* one second of riding north at kmh, the phone sending speed or not */
static void ride_s(double kmh, int phone_speed)
{
    plat += kmh / 3.6 / 111320.0;
    fix(plat, plon, phone_speed ? (uint16_t)(kmh / 3.6 * 100) : 0, 0);
    run(1000);
}
/* parked: GPS wanders +-5 m round the spot, speed 0 */
static void parked_s(void)
{
    double jl = ((rand() % 1001) - 500) / 100.0 / 111320.0, jo = ((rand() % 1001) - 500) / 100.0 / 97800.0;
    fix(plat + jl, plon + jo, 0, 0);
    run(1000);
}
/* a fingertip tap on the real chip: the centroid wanders ~25 px while down */
static void jitter_tap(int x, int y)
{
    int path[][2] = {{0, 0}, {6, 4}, {12, 9}, {17, 12}, {14, 15}, {11, 13}};
    for (int i = 0; i < 6; i++) {
        touch_sample_t raw = {0};
        if (i != 2) { raw.n = 1; raw.x[0] = x + path[i][0]; raw.y[0] = y + path[i][1]; }
        touch_filter_feed(&filt, &raw, g_fake_us); g_touch = filt.out; run(10);
    }
    for (int i = 0; i < 20; i++) { touch_sample_t raw = {0}; touch_filter_feed(&filt, &raw, g_fake_us); g_touch = filt.out; run(10); }
}

int main(void)
{
    mkdir("out", 0755); mkdir("out/rides9", 0755);
    remove("out/rides9/index.bin");
    lv_init();
    lv_tick_set_cb(tick);
    g_disp = lv_display_create(W, W);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(g_disp, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_cb);
    lv_timer_set_period(lv_indev_get_read_timer(in), 15);
    use_filter = 1;

    settings_init(); cfg_init(); ride_init("out/rides9"); nav_core_init(); am_init(); run(500);
    CHECK(ride_storage_ok(), "ride storage writable");
    am_open(am_find("Navigation")); run(300);
    g_cbs->on_link(true);
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)settings_token_str(), 6); run(200);
    g_cbs->on_phone_info(81, true, true, 1795620000u);
    fix(plat, plon, 0, 0); run(500);

    printf("TAP ON START RIDE (jittery fingertip)\n");
    real_swipe(302, 206, 122, 206, 220, 0);          /* speedo -> track */
    real_swipe(302, 206, 122, 206, 220, 0);          /* track -> ride */
    CHECK(map_view_face() == 5, "on the ride face (%d)", map_view_face());
    jitter_tap(200, 200);
    CHECK(ride_state() == RIDE_RECORDING, "a tap that wanders 25 px starts the ride");
    shot("r01_rec");
    for (int i = 0; i < 10; i++) parked_s();
    jitter_tap(200, 200); run(300); jitter_tap(200, 200); run(300);
    CHECK(ride_state() == RIDE_IDLE && ride_count() == 0, "tap twice ends it; parked = under 50 m, not saved");
    shot("r02_too_short");

    printf("PHONE SENDS NO SPEED\n");
    for (int i = 0; i < 25; i++) ride_s(30, 0);
    CHECK(ride_state() == RIDE_RECORDING, "auto-start from position change alone (30 km/h, phone speed 0)");
    int sp = ride_speed_cms() * 36 / 1000;
    CHECK(sp >= 25 && sp <= 35, "speed worked out from positions: %d km/h (speedo shows this)", sp);
    for (int i = 0; i < 120; i++) ride_s(30, 0);
    uint32_t d = ride_current()->distance_m;
    CHECK(d > 950 && d < 1100, "distance counted: %u m (2 min at 30 km/h = 1000 m)", (unsigned)d);
    shot("r03_rec_moving");

    printf("PARKED WITH GPS DRIFT, ROUTE RUNNING\n");
    nav_state(NAV_F_ACTIVE, NAV_LEFT, 300, 5000, 900, "MG Road"); run(300);
    for (int i = 0; i < 12 * 60; i++) { parked_s(); if (i % 20 == 0) nav_state(NAV_F_ACTIVE, NAV_LEFT, 300, 5000, 900, "MG Road"); }
    CHECK(ride_state() == RIDE_RECORDING, "12 min parked with a route running: still recording");
    uint32_t drift = ride_current()->distance_m - d;
    CHECK(drift < 60, "GPS drift while parked added only %u m", (unsigned)drift);
    nav_state(0, NAV_NONE, 0, 0, 0, ""); run(300);
    for (int i = 0; i < 11 * 60; i++) parked_s();
    CHECK(ride_state() == RIDE_IDLE && ride_count() == 1, "route ended + 10 min parked: stopped and saved (%d saved)", ride_count());
    ride_summary_t r; ride_get(0, &r);
    CHECK(r.distance_m > 950, "saved ride has %u m, %u points", (unsigned)r.distance_m, (unsigned)r.points);

    printf("PHONE SENDS SPEED (as before)\n");
    for (int i = 0; i < 25; i++) ride_s(40, 1);
    CHECK(ride_state() == RIDE_RECORDING, "auto-start with phone speed");
    uint32_t d0 = ride_current()->distance_m;
    for (int i = 0; i < 60; i++) ride_s(40, 1);
    uint32_t dd = ride_current()->distance_m - d0;
    CHECK(dd > 600 && dd < 720, "a minute at 40 km/h: %u m (about 667)", (unsigned)dd);
    plat += 0.05; fix(plat, plon, 1100, 0); run(1000);           /* 5.5 km jump: bad fix */
    CHECK(ride_current()->distance_m - d0 - dd < 30, "a 5 km GPS jump is not counted");
    CHECK(ride_stop(true) == RIDE_RES_SAVED && ride_count() == 2, "stop: saved (%d)", ride_count());

    printf("COMPASS PAGE\n");
    real_swipe(302, 206, 122, 206, 220, 0);               /* speedo -> compass/odometer */
    CHECK(map_view_face() == 4, "on the compass face (%d)", map_view_face());
    for (int i = 0; i < 15; i++) { plon += 30 / 3.6 / 97800.0; fix(plat, plon, 0, 9000); run(1000); }
    shot("r04_compass_moving");
    for (int i = 0; i < 16; i++) parked_s();
    shot("r05_compass_parked");

    printf("\n%s (%d failures)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
