#pragma once
// Board support: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round, SPD2010).
//
// Pin map taken from Waveshare's own ESP-IDF demo for this board
// (ESP32-S3-Touch-LCD-1.46-Test: Display_SPD2010.h, TCA9554PWR.h, PWR_Key.h,
// SD_MMC.h, BAT_Driver.h). Same panel and touch controller as the SenseCAP
// Watcher port (firmware_watcher/), different wiring:
//
//   1. ONE shared I2C bus (GPIO11 SDA / GPIO10 SCL): TCA9554 expander (0x20),
//      SPD2010 touch (0x53), QMI8658 IMU, PCF85063 RTC.
//   2. The expander is an 8-bit TCA9554 that only carries the two reset lines
//      (touch, panel). It does NOT switch power rails like the Watcher's PCA9535.
//   3. Battery power is LATCHED: on battery, the PWR key (GPIO6) turns the
//      regulator on and the firmware must drive GPIO7 high to keep it on.
//      Releasing GPIO7 (low) powers the board off when it runs from battery.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// --- I2C0: the only bus (expander, touch, IMU, RTC) ---
#define BOARD_I2C_NUM I2C_NUM_0
#define BOARD_I2C_SDA 11
#define BOARD_I2C_SCL 10
#define BOARD_I2C_HZ 400000

#define BOARD_LCD_W 412
#define BOARD_LCD_H 412

// Screens were laid out against the 2.8C's 480x480 panel; UI_SC() maps those
// design-space numbers onto this 412x412 panel (scale 0.858). Same as the
// Watcher port, which has the same resolution.
#define UI_DESIGN 480
#define UI_SC(n) (((n) * BOARD_LCD_W + UI_DESIGN / 2) / UI_DESIGN)
#define UI_SCF(n) ((n) * BOARD_LCD_W / (float)UI_DESIGN)

// --- Plain GPIOs ---
#define BOARD_GPIO_BACKLIGHT 5   // LEDC PWM, active high
#define BOARD_GPIO_TOUCH_INT 4   // SPD2010 touch INT, active low, RTC-capable (deep-sleep wake)
#define BOARD_GPIO_PWR_KEY 6     // side PWR key, active low
#define BOARD_GPIO_PWR_HOLD 7    // battery power latch: high = stay on
#define BOARD_GPIO_BOOT 0        // BOOT key, active low (long-press unpair)
#define BOARD_GPIO_BAT_ADC 8     // battery divider -> ADC1_CH7 (not used yet)

// --- TCA9554 expander, 8-bit, address 0x20 ---
// Vendor numbering is EXIO1..EXIO8 = bit 0..7. Only two are wired to anything
// this firmware cares about.
#define EXIO_TOUCH_RST 0  // vendor EXIO1
#define EXIO_LCD_RST 1    // vendor EXIO2

esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);

// Kept for touch.c's call shape. On this board touch is on the shared bus, so
// this is a no-op and board_touch_i2c_bus() returns the same handle.
esp_err_t board_touch_i2c_init(void);
i2c_master_bus_handle_t board_touch_i2c_bus(void);

// Takes the expander, pulses the panel and touch resets. Must run after
// board_i2c_init() and before lcd_init()/touch_init().
esp_err_t board_exio_init(void);
esp_err_t board_exio_set(int bit, bool level);
bool board_exio_get(int bit);

// Battery latch. Call FIRST in app_main: on battery the board only stays on
// while the PWR key is held until GPIO7 goes high.
void board_power_hold(void);
// Releases the latch. On battery this powers the board off; on USB it does
// nothing visible.
void board_power_off(void);
bool board_pwr_key_pressed(void);

// Backlight PWM on GPIO5 (LEDC channel 0). 0-100 %.
void board_backlight_init(void);
void board_backlight_set(uint8_t pct);

// BOOT key (GPIO0), polled by the GUI task for the 5 s unpair.
void board_boot_button_init(void);
bool board_boot_button_pressed(void);

// Ahead of deep sleep. Nothing on this board is rail-switched, so this only
// parks the backlight; touch stays powered so a tap can wake (GPIO4).
void board_power_rails_off(void);
