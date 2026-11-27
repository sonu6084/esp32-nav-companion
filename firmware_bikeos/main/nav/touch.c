#include "touch.h"
#include "touch_filter.h"
#include "board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "touch";

// Empty polls and I2C errors while a finger is down are bridged by
// touch_filter.h; the lift is reported TOUCH_RELEASE_US after the last point.

static esp_lcd_touch_handle_t s_tp;
static touch_filter_t s_filt;  // bike-os fix: bridges empty polls mid-touch

esp_err_t touch_init(void)
{
    // Waveshare 1.46: touch shares I2C0 (GPIO11/10) with the expander, IMU
    // and RTC. INT is GPIO4; it is polled here (single reader, AGENTS rule 9)
    // and only used as the deep-sleep wake source.
    esp_err_t err = board_touch_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c1: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_i2c(board_touch_i2c_bus(), &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_W,
        .y_max = BOARD_LCD_H,
        // Reset is on the expander (EXIO_TOUCH_RST), pulsed in board_exio_init().
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels.reset = 0,
        .levels.interrupt = 0,
        .flags.swap_xy = 0,
        .flags.mirror_x = 0,
        .flags.mirror_y = 0,
    };
    err = esp_lcd_touch_new_i2c_spd2010(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPD2010 touch not responding: %s", esp_err_to_name(err));
        return err;
    }

    // The vendor reads once to settle the controller right after init
    // (sensecap-watcher.c:756-758).
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(s_tp);
    return ESP_OK;
}

void touch_poll(void)
{
    if (!s_tp) return;
    int64_t now = esp_timer_get_time();
    touch_sample_t s = {0};

    // A failed read (I2C error) counts as an empty poll: the filter keeps the
    // finger down for a short gap and lifts it after TOUCH_RELEASE_US.
    if (esp_lcd_touch_read_data(s_tp) == ESP_OK) {
        // Never assume the driver wrote point_cnt: esp_lcd_touch_get_data
        // returns early (ESP_OK) when nothing is touched, and only guarantees
        // the count on a hit.
        uint8_t n = 0;
        esp_lcd_touch_point_data_t pts[2];
        if (esp_lcd_touch_get_data(s_tp, pts, &n, 2) == ESP_OK && n > 0) {
            if (n > 2) n = 2;
            for (int i = 0; i < n; i++) {
                int x = pts[i].x, y = pts[i].y;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                if (x >= BOARD_LCD_W) x = BOARD_LCD_W - 1;
                if (y >= BOARD_LCD_H) y = BOARD_LCD_H - 1;
                s.x[i] = (int16_t)x;
                s.y[i] = (int16_t)y;
            }
            s.n = (uint8_t)n;
        }
    }
    touch_filter_feed(&s_filt, &s, now);
}

const touch_sample_t *touch_get(void)
{
    return &s_filt.out;
}
