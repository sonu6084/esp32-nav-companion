/* bike-os 0.11 design previews: drives the real screens with sample data and saves each
   as out/d_*.ppm. Windows/MinGW friendly (no POSIX mkdir).
     PATH=/c/msys64/mingw64/bin:$PATH sh tools-sim/build_win.sh sim_design */
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include "lvgl.h"
#include "sys.h"
#include "app_manager.h"
#include "registry.h"
#include "settings.h"
#include "cfg.h"
#include "nav_core.h"
#include "ble_service.h"
#include "touch.h"
#include "ride.h"
#include "map_view.h"
#include "nav_view.h"

extern int64_t g_fake_us;
extern int64_t g_time_creep_us;
extern uint32_t g_events;
extern lv_display_t *g_disp;
extern touch_sample_t g_touch;
extern const nav_ble_cbs_t *g_cbs;

#define W 412
static uint16_t fb[W * W];
static uint8_t buf1[W * W * 2], buf2[W * W * 2];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    uint16_t *p = (uint16_t *)px;
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
static void run(int ms) { for (int i = 0; i < ms; i += 5) { g_fake_us += 5000; nav_core_poll(); lv_timer_handler(); } }
static void tap(int x, int y) { g_touch.n = 1; g_touch.x[0] = x; g_touch.y[0] = y; run(80); g_touch.n = 0; run(150); }
static void drag(int x0, int y0, int x1, int y1)
{
    for (int i = 0; i <= 12; i++) { g_touch.n = 1; g_touch.x[0] = x0 + (x1 - x0) * i / 12; g_touch.y[0] = y0 + (y1 - y0) * i / 12; run(20); }
    g_touch.n = 0; run(300);
}
#define SWIPE_NEXT() drag(300, 206, 110, 206)

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
    printf("shot %s\n", name);
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
/* nearby streets: a grid and an arterial, as one NAV_ROADS chunk (from sim6) */
static void send_roads(double lat0, double lon0)
{
    uint8_t b[252];
    int o = 0;
    b[o++] = NAV_ROADS; b[o++] = NAV_ROADS_F_START | NAV_ROADS_F_END; b[o++] = 0; b[o++] = 0;
    int32_t la = (int32_t)(lat0 * 1e7), lo = (int32_t)(lon0 * 1e7);
    memcpy(b + o, &la, 4); o += 4; memcpy(b + o, &lo, 4); o += 4;
    for (int k = -4; k <= 4 && o + 2 + 2 * 4 <= 240; k++) {
        int16_t v = (int16_t)(k * 110);
        b[o++] = 2; b[o++] = (k == 0) ? 1 : 0;
        int16_t p[4] = {-420, v, 420, v};
        memcpy(b + o, p, 8); o += 8;
        b[o++] = 2; b[o++] = 0;
        int16_t q[4] = {v, -420, v, 420};
        memcpy(b + o, q, 8); o += 8;
    }
    b[o++] = 3; b[o++] = 1;
    int16_t r[6] = {-400, -400, 0, 30, 380, 300};
    memcpy(b + o, r, 12); o += 12;
    g_cbs->on_roads_chunk(b, (size_t)o);
}

int main(void)
{
    _mkdir("out"); _mkdir("out/rides");
    remove("out/rides/index.bin");
    lv_init();
    lv_tick_set_cb(tick);
    g_disp = lv_display_create(W, W);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(g_disp, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_cb);

    g_time_creep_us = 200;   /* the board's clock moves within a GUI loop: catches the 0.10 ride restarts */
    settings_init(); cfg_init(); ride_init("out/rides"); nav_core_init(); am_init(); run(3000);
    g_cbs->on_link(true); run(300);
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)settings_token_str(), 6); run(300);   /* as the phone does */
    uint32_t base = 1795000000u - 1795000000u % 86400u + 18 * 3600 + 25 * 60;   /* 18:25 local */
    g_cbs->on_phone_info(76, false, true, base);
    run(300);
    const app_t *NAV = am_find("Navigation");
    am_open(NAV); run(800);

    /* --- turn card (Google Maps' turns: no route line) --- */
    const uint8_t TO = NAV_F_ACTIVE | NAV_F_TURN_ONLY;
    nav_state(TO, NAV_RIGHT, 450, 30000, 2400, "Outer Ring Road"); run(400);
    nav_state(TO, NAV_RIGHT, 450, 12000, 1020, "Outer Ring Road"); run(600);
    shot("d_card_turn");
    g_cbs->on_nav_limit(false, 50); run(300);
    nav_state(TO, NAV_LEFT, 450, 12000, 1020, "Outer Ring Road"); run(600);
    shot("d_card_limit");
    nav_state(TO, NAV_SLIGHT_LEFT, 1200, 9500, 840, "Keep left to take the flyover"); run(600);
    shot("d_card_km");
    nav_state(TO, NAV_KEEP_LEFT, 350, 9200, 820, "Keep left to avoid the flyover"); run(600);
    shot("d_card_below");
    nav_state(TO, NAV_CONTINUE, 600, 8800, 780, "Continue onto Ring Road Tunnel"); run(600);
    shot("d_card_tunnel");
    nav_state(TO, NAV_ARRIVE, 0, 0, 0, "Cafe Terrace"); run(600);
    shot("d_card_arrived");
    nav_state(0, NAV_NONE, 0, 0, 0, ""); g_cbs->on_nav_limit(false, 0); run(7000);

    /* --- faces: moving east at 43 km/h --- */
    double lat = 38.5449, lon = -121.7405;
    for (int i = 0; i < 30; i++) { lon += 0.00012; fix(lat, lon, 1195, 9000); run(1000); }
    g_cbs->on_nav_limit(false, 60); run(300);
    shot("d_face_cruise");
    SWIPE_NEXT(); run(400); shot("d_face_2");
    SWIPE_NEXT(); run(400); shot("d_face_3");
    SWIPE_NEXT(); run(400); shot("d_face_4");
    tap(206, 206); run(300);                                   /* start a ride if this is the ride face */
    for (int i = 0; i < 60; i++) { lon += 0.00012; fix(lat, lon, 1195 + (i % 7) * 40, 9000); run(1000); }
    shot("d_face_4_ride");
    { const ride_summary_t *r = ride_current(); printf("ride: state=%d elapsed=%us dist=%um moving=%us top=%ucm/s\n", (int)ride_state(), (unsigned)r->elapsed_s, (unsigned)r->distance_m, (unsigned)r->moving_s, (unsigned)r->max_cms); }

    /* --- streets around a route drive --- */
    nav_state(NAV_F_ACTIVE, NAV_KEEP_RIGHT, 180, 4200, 480, "Lake Road Flyover"); run(800);
    am_home(); run(400); am_open(NAV); run(800);       /* a drive opens on the route face */
    send_roads(lat, lon); run(800);
    for (int i = 0; i < 3; i++) { fix(lat, lon, 1195, 9000); nav_state(NAV_F_ACTIVE, NAV_KEEP_RIGHT, 180, 4200, 480, "Lake Road Flyover"); run(1000); }
    shot("d_streets");

    /* --- now playing --- */
    nav_media_state_t ms = {.flags = NAV_MEDIA_F_PLAYING | NAV_MEDIA_F_SESSION, .volume = 60, .position_ms = 74000, .duration_ms = 246000};
    g_cbs->on_media_state(&ms);
    g_cbs->on_media_text("Midnight Highway", "Northbound");
    run(300);
    drag(110, 206, 300, 206); run(600);
    shot("d_media");
    return 0;
}
