#pragma once
/*
 * sys — board services that run outside the LVGL thread.
 *
 * A low-priority task polls the PWR and BOOT keys, battery voltage and the RTC.
 * It never touches LVGL: it only raises event bits and caches values, which the
 * UI thread picks up via sys_take_events() / getters. (Same rule as the nav
 * firmware: background tasks stash, the GUI task consumes.)
 */
#include <stdint.h>
#include <stdbool.h>
#include "rtc.h"

/* Event bits returned by sys_take_events() */
#define SYS_EVT_PWR_SHORT   (1u << 0)   /* PWR key tapped            -> go home       */
#define SYS_EVT_BOOT_SHORT  (1u << 1)   /* BOOT key tapped           -> app primary   */
#define SYS_EVT_BOOT_LONG   (1u << 2)   /* BOOT key held ~0.8 s      -> app secondary */
#define SYS_EVT_SHUTDOWN    (1u << 3)   /* PWR key held 3 s          -> powering off  */

void     bos_sys_init(void);              /* after board_i2c_init(); starts the key/battery/RTC task */
uint32_t sys_take_events(void);       /* atomically fetch-and-clear pending events */

void     sys_get_time(datetime_t *out);   /* cached RTC time, refreshed every 500 ms */
float    sys_battery_volts(void);
int      sys_battery_pct(void);           /* 0..100, or -1 if no battery detected */
bool     sys_on_battery_latch(void);      /* true if the battery latch is holding power */
void     sys_set_backlight(uint8_t pct);     /* 0 = off */
void     sys_power_off(void);             /* battery: drop the latch; USB: deep sleep, tap to wake.
                                             GUI task only (drains touch before sleeping). */
void     sys_deep_sleep(void);            /* deep sleep now, wake on a tap. GUI task only. */

/* Write date/time to the RTC (weekday is computed; seconds taken from t). UI-thread safe. */
void     sys_set_time(const datetime_t *t);
int      sys_weekday(int y, int m, int d);    /* 0 = Sunday */
int      sys_days_in_month(int y, int m);
