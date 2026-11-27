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

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    g_disp = lv_display_create(W, W);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(g_disp, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_cb);

    settings_init();
    cfg_init();
    nav_core_init();
    am_init();
    run(500);
    const app_t *NAV = am_find("Navigation"), *MUS = am_find("Music"), *CLK = am_find("Clock"), *TMR = am_find("Timer");
    CHECK(NAV && NAV->ready && MUS && MUS->ready, "Navigation and Music registered (firmware_ws146 views)");
    CHECK(g_cbs && g_cbs->on_nav_state && g_cbs->on_route_chunk && g_cbs->on_call_ring && g_cbs->on_media_state,
          "nav_core registered the full firmware_ws146 BLE callback table");
    CHECK(!strcmp(NAV->status(), "Not paired"), "menu status: '%s'", NAV->status());
    shot("v01_menu");

    printf("PAIRING\n");
    am_open(NAV); run(400);
    CHECK(!strcmp(CUR(), "Navigation"), "Navigation open");
    shot("v02_pair_qr");
    g_cbs->on_link(true); run(100);
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)"000000", 6); run(100);
    CHECK(!settings_paired(), "wrong token rejected");
    g_cbs->on_cmd(NAV_CMD_PAIR, (const uint8_t *)settings_token_str(), 6); run(300);
    CHECK(settings_paired(), "correct token pairs");
    g_cbs->on_phone_info(81, true, true, 1795620000u);  /* phone clock */
    for (int i = 0; i < 5; i++) { fix(28.6139 + i * 0.0002, 77.2090, 1100, 0); run(1000); }
    CHECK(g_rtc_sets > 0, "RTC synced from the phone clock");
    shot("v03_map_minimal");

    printf("ROUTE STARTS WHILE IN ANOTHER APP\n");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(CLK); run(500);
    CHECK(!strcmp(CUR(), "Clock"), "in Clock");
    /* route geometry + first instruction */
    int32_t lat[4], lon[4];
    for (int i = 0; i < 4; i++) { lat[i] = (int32_t)((28.6150 + i * 0.002) * 1e7); lon[i] = (int32_t)((77.2090 - i * 0.0015) * 1e7); }
    g_cbs->on_route_chunk(NAV_ROUTE_F_START | NAV_ROUTE_F_END, 0, 4, lat, lon);
    g_cbs->on_nav_dest(true, lat[3], lon[3]);
    g_cbs->on_nav_limit(false, 50);
    nav_state(NAV_F_ACTIVE, NAV_LEFT, 420, 1240, 1080, "Janpath");
    run(800);
    CHECK(!strcmp(CUR(), "Navigation"), "route start pulled Navigation to the front (%s)", CUR());
    for (int d = 400; d >= 120; d -= 40) { fix(28.6150, 77.2090, 1300, 0); nav_state(NAV_F_ACTIVE, NAV_LEFT, d, 1200, 1050, "Janpath"); run(400); }
    shot("v04_nav_left_120");
    run(130); shot("v05_nav_left_120_b");
    CHECK(NAV->keep_awake(), "screen kept awake while navigating");
    CHECK(strstr(NAV->status(), "Turn in") != NULL, "status '%s'", NAV->status());

    printf("LEAVE NAV DURING ROUTE\n");
    key(SYS_EVT_PWR_SHORT, 600);
    CHECK(!strcmp(CUR(), "menu"), "PWR -> menu");
    nav_state(NAV_F_ACTIVE, NAV_LEFT, 80, 1150, 1000, "Janpath"); run(400);
    shot("v06_menu_during_route");       /* the orange edge must not show over the menu */
    CHECK(strstr(NAV->status(), "80 m") != NULL, "BLE kept updating while closed: '%s'", NAV->status());

    printf("INCOMING CALL OVER ANOTHER APP\n");
    am_open(TMR); run(500);
    g_cbs->on_call_ring("Rahul", "+91 98765 43210"); run(600);
    CHECK(!strcmp(CUR(), "Navigation"), "ring opened Navigation (%s)", CUR());
    shot("v07_call");
    tap(299, 295); run(400);           /* Answer */
    CHECK(g_call_cmds == 1 && g_last_call_cmd == NAV_CCMD_ANSWER, "Answer sent to the phone");
    run(600);
    CHECK(!strcmp(CUR(), "Timer"), "after the call, back to Timer (%s)", CUR());

    printf("MUSIC\n");
    nav_media_state_t ms = {.flags = NAV_MEDIA_F_PLAYING | NAV_MEDIA_F_SESSION, .volume = 60, .position_ms = 83000, .duration_ms = 214000};
    g_cbs->on_media_state(&ms);
    g_cbs->on_media_text("Kesariya", "Arijit Singh");
    run(400);
    key(SYS_EVT_PWR_SHORT, 500);
    run(600);
    CHECK(MUS->status() && !strcmp(MUS->status(), "Kesariya"), "menu shows the track: '%s'", MUS->status() ? MUS->status() : "(null)");
    shot("v08_menu_music_status");
    am_open(MUS); run(600);
    shot("v09_music");
    key(SYS_EVT_PWR_SHORT, 500);

    printf("QUICK SWITCHING (screen-load race)\n");
    am_open(CLK); run(60);
    nav_state(NAV_F_ACTIVE, NAV_RIGHT, 300, 900, 800, "Rajpath"); run(5);   /* nav pulls focus mid-animation */
    g_cbs->on_call_ring("Mom", ""); run(5);
    g_cbs->on_call_end(); run(50);
    am_open(TMR); run(20); am_open(CLK); run(20); am_home(); run(600);
    CHECK(true, "no crash / ASan error during rapid switching");

    printf("OTHER APPS ON LVGL 9\n");
    am_open(CLK); run(500); shot("v10_clock_digital");
    key(SYS_EVT_BOOT_SHORT, 400); shot("v11_clock_analog");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(am_find("Stopwatch")); run(300); key(SYS_EVT_BOOT_SHORT, 200); run(12340); key(SYS_EVT_BOOT_LONG, 200); run(3000);
    shot("v12_stopwatch");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(TMR); run(500); shot("v13_timer");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(am_find("Alarm")); run(400); key(SYS_EVT_BOOT_SHORT, 400); shot("v14_alarm_editor"); key(SYS_EVT_BOOT_SHORT, 400);
    shot("v15_alarm_list");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(am_find("Weather")); run(500); shot("v16_weather");
    key(SYS_EVT_PWR_SHORT, 500);
    am_open(am_find("Settings")); run(500); shot("v17_settings");
    nav_core_forget_phone(); run(200);
    CHECK(!settings_paired(), "Settings > Forget phone unpairs (new QR token)");
    key(SYS_EVT_PWR_SHORT, 500);
    run(600); shot("v18_menu_end");

    printf("\n%s (%d failures)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
