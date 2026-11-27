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
#include "bos_intro.h"
#include "home.h"
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


/* the board links main/assets/intro.gif in; the sim reads the same file */
static uint8_t *gif_buf; static size_t gif_len; static int gif_broken;
const uint8_t *bos_intro_gif(size_t *len)
{
    if (!gif_buf) {
        FILE *f = fopen("../main/assets/intro.gif", "rb");
        fseek(f, 0, SEEK_END); gif_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        gif_buf = malloc(gif_len); fread(gif_buf, 1, gif_len, f); fclose(f);
    }
    static const uint8_t junk[] = "not a gif at all";
    if (gif_broken) { *len = sizeof junk; return junk; }
    *len = gif_len;
    return gif_buf;
}

static int done_at = -1;
static void intro_done(void) { done_at = (int)(g_fake_us / 1000); }

static int lit_pixels(int x0, int y0, int x1, int y1)
{
    lv_refr_now(NULL);
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (fb[y * W + x] > 0x4208) n++;
    return n;
}

int main(void)
{
    mkdir("out", 0755); mkdir("out/rides", 0755);
    lv_init();
    lv_tick_set_cb(tick);
    g_disp = lv_display_create(W, W);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(g_disp, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_cb);
    settings_init(); cfg_init(); ride_init("out/rides"); nav_core_init();

    printf("BOOT INTRO\n");
    int t0 = (int)(g_fake_us / 1000);
    bos_intro_play(lv_screen_active(), intro_done);
    run(500);  shot("h01_intro_0_5s");
    CHECK(done_at < 0, "intro is playing at 0.5 s");
    run(800);  shot("h02_intro_1_3s");
    int name_px = lit_pixels(100, 170, 312, 240);
    CHECK(name_px > 2000, "the GIF decodes on screen: name drawn (%d lit px)", name_px);
    run(1500); shot("h03_intro_2_8s");
    for (int i = 0; i < 400 && done_at < 0; i++) run(10);
    int took = done_at - t0;
    CHECK(took >= 2800 && took <= 4200, "intro played once and finished after %d ms (3.0 s GIF + hold)", took);

    printf("HOME\n");
    am_init();                    /* what gui_task does when the intro is done */
    bos_intro_destroy();
    run(600);
    CHECK(am_on_home() && am_current() == NULL, "home screen shows after the intro");
    shot("h04_home");

    tap(206, 206); run(400);
    CHECK(!am_on_home() && am_current() == NULL, "tap on home opens the menu");
    shot("h05_menu");
    run(31000);
    CHECK(am_on_home(), "menu untouched for 30 s falls back to home");
    tap(206, 206); run(400);
    key(SYS_EVT_PWR_SHORT, 500);
    CHECK(am_on_home(), "PWR in the menu goes home");
    key(SYS_EVT_BOOT_SHORT, 500);
    CHECK(!am_on_home(), "BOOT on home opens the menu");
    key(SYS_EVT_PWR_SHORT, 500);

    printf("ROUTE STARTS -> NAVIGATION\n");
    g_cbs->on_link(true);
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)settings_token_str(), 6); run(200);
    g_cbs->on_phone_info(81, true, true, 1795620000u);
    run(500);
    shot("h06_home_linked");
    CHECK(am_on_home(), "still home while the phone just connects");
    nav_state(NAV_F_ACTIVE, NAV_LEFT, 300, 5200, 900, "MG Road"); run(600);
    CHECK(am_current() == am_find("Navigation"), "from home: route start opens Navigation (%s)", CUR());
    key(SYS_EVT_PWR_SHORT, 500);
    key(SYS_EVT_PWR_SHORT, 500);
    CHECK(am_on_home(), "PWR, PWR: back to home with the route still running");
    shot("h07_home_navigating");
    tap(206, 412 - 34 - 22); run(600);
    CHECK(am_current() == am_find("Navigation"), "the Navigating pill on home reopens Navigation");
    nav_state(0, NAV_NONE, 0, 0, 0, ""); run(600);
    key(SYS_EVT_PWR_SHORT, 500);                         /* nav -> menu */
    CHECK(!am_on_home() && am_current() == NULL, "in the menu");
    nav_state(NAV_F_ACTIVE, NAV_RIGHT, 200, 3000, 500, "Ring Road"); run(600);
    CHECK(am_current() == am_find("Navigation"), "from the menu: route start opens Navigation");
    nav_state(0, NAV_NONE, 0, 0, 0, ""); run(600);
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(am_find("Stopwatch")); run(500);
    nav_state(NAV_F_ACTIVE, NAV_LEFT, 150, 2000, 400, "Outer Ring Rd"); run(600);
    CHECK(am_current() == am_find("Navigation"), "from another app: route start opens Navigation");
    nav_state(0, NAV_NONE, 0, 0, 0, ""); run(600);

    printf("SCREEN SLEEP\n");
    key(SYS_EVT_PWR_SHORT, 500);                         /* nav -> menu */
    cfg_set_screen_timeout_s(10);
    run(11000);
    CHECK(am_screen_off() && am_on_home(), "screen sleeps from the menu and wakes on home");
    tap(206, 206); run(400);
    CHECK(!am_screen_off() && am_on_home(), "the waking tap only wakes (stays on home)");
    cfg_set_screen_timeout_s(0);

    printf("INTRO EDGE CASES\n");
    lv_obj_t *s2 = lv_obj_create(NULL); lv_screen_load(s2);
    done_at = -1; t0 = (int)(g_fake_us / 1000);
    bos_intro_play(s2, intro_done); run(500);
    tap(206, 206); run(100);
    CHECK(done_at >= 0 && done_at - t0 < 900, "a tap skips the intro (%d ms)", done_at - t0);
    bos_intro_destroy();
    gif_broken = 1; done_at = -1;
    bos_intro_play(s2, intro_done); run(50);
    CHECK(done_at >= 0, "a broken GIF does not hold the boot");
    bos_intro_destroy();

    printf("\n%s (%d failures)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
