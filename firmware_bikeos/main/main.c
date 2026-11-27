/*
 * bike-os v0.5 — multi-app firmware for the Waveshare ESP32-S3-Touch-LCD-1.46
 *
 * Board layer, display (LVGL 9, full-frame, QSPI) and the whole navigation
 * system are firmware_ws146 (main/nav/, unchanged). nav_core.c is
 * firmware_ws146's main.c split so navigation keeps running behind the other
 * apps. Everything else is bike-os: menu, Clock, Stopwatch, Timer, Alarm,
 * Weather, Settings.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "lcd.h"
#include "touch.h"
#include "sdcard.h"
#include "display.h"
#include "settings.h"
#include "bos_intro.h"

#include "cfg.h"
#include "sys.h"
#include "audio.h"
#include "app_manager.h"
#include "nav_core.h"
#include "weather_net.h"
#include "ride.h"
#include "esp_vfs_fat.h"

static const char *TAG = "main";

#define TOUCH_POLL_MS 10

/* Until the phone syncs the clock, seed an unset RTC from the build time. */
static void rtc_seed_from_build_time(void)
{
    datetime_t now;
    sys_get_time(&now);
    if (now.year >= 2025 && now.year < 2100 && now.month >= 1 && now.month <= 12) return;
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {0};
    int d = 1, y = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", m, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(mon, m);
    datetime_t t = { .year = y, .month = p ? (uint8_t)((p - mon) / 3 + 1) : 1, .day = d,
                     .hour = hh, .minute = mm, .second = ss };
    sys_set_time(&t);
    ESP_LOGW(TAG, "RTC seeded from build time");
}

/* Saved rides live on their own FAT partition ("rides", see partitions.csv). */
static void rides_mount(void)
{
    static wl_handle_t wl;
    const esp_vfs_fat_mount_config_t mc = {
        .max_files = 4,
        .format_if_mount_failed = true,      /* our own partition: blank on first boot */
        .allocation_unit_size = 4096,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/rides", "rides", &mc, &wl);
    if (e != ESP_OK) ESP_LOGW(TAG, "rides partition not mounted (%s): rides won't be saved", esp_err_to_name(e));
    ride_init("/rides");
}

static volatile bool s_boot_done;
static void boot_done(void) { s_boot_done = true; }

static void gui_task(void *arg)
{
    bool backlight_on = false, menu_up = false;
    uint32_t last_touch = 0;
    for (;;) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
        if (t - last_touch >= TOUCH_POLL_MS) {     /* single touch reader (AGENTS.md rule 9) */
            last_touch = t;
            touch_poll();
        }
        if (!menu_up && s_boot_done) {             /* splash played: on to the menu */
            menu_up = true;
            am_init();                             /* home fades in over the intro */
            bos_intro_destroy();
        }
        nav_core_poll();                           /* BLE -> map/nav/media/call, every loop */

        uint32_t wait = lv_timer_handler();
        if (!backlight_on) {                       /* first frame is on the glass */
            backlight_on = true;
            board_backlight_set(settings_brightness());
        }
        if (wait > 5) wait = 5;
        if (wait < 1) wait = 1;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void app_main(void)
{
    /* First: on battery the board only stays on while GPIO7 is high. */
    board_power_hold();
    ESP_ERROR_CHECK(settings_init());              /* NVS + nav settings (namespace "nav") */
    cfg_init();                                    /* bike-os settings (namespace "bikeos") */

    /* firmware_ws146 order: I2C -> expander (panel/touch resets) -> backlight -> panel -> touch -> SD */
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_exio_init());
    board_backlight_init();                        /* off until the first frame */
    ESP_ERROR_CHECK(lcd_init());
    if (touch_init() != ESP_OK) ESP_LOGE(TAG, "touch unavailable");
    sdcard_init();                                 /* optional: no card = minimal BLE display */
    board_boot_button_init();

    sys_init();                                    /* keys, battery, RTC (shared I2C bus) */
    rtc_seed_from_build_time();
    audio_init();
    rides_mount();

    display_init();
    nav_core_init();                               /* tiles, map state, BLE advertising */

    /* boot intro (main/assets/intro.gif), then the home screen */
    bos_intro_play(lv_screen_active(), boot_done);
    weather_net_init();

    xTaskCreatePinnedToCore(gui_task, "gui", 10 * 1024, NULL, 5, NULL, 1);
}
