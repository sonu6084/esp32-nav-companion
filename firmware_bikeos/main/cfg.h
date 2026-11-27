#pragma once
/*
 * bike-os settings (NVS namespace "bikeos"). Navigation, pairing and brightness
 * stay in nav/settings.c (namespace "nav", unchanged from firmware_ws146), so an
 * existing pairing survives. Call cfg_init() after settings_init() (which brings
 * up NVS). Setters write through: LVGL/GUI task only.
 */
#include <stdbool.h>
#include <stdint.h>

#define ALARM_MAX 6

typedef struct {
    bool    enabled;
    uint8_t hour, minute;
    uint8_t days;           /* bit0 = Sunday ... bit6 = Saturday; 0 = once */
} alarm_t;

void        cfg_init(void);

uint8_t     cfg_volume(void);               /* 0..100 */
void        cfg_set_volume(uint8_t v);
uint16_t    cfg_screen_timeout_s(void);     /* 0 = never */
void        cfg_set_screen_timeout_s(uint16_t s);
bool        cfg_keep_on_nav(void);          /* screen stays on while navigating */
void        cfg_set_keep_on_nav(bool on);
uint8_t     cfg_clock_face(void);
void        cfg_set_clock_face(uint8_t f);
uint32_t    cfg_timer_last_s(void);
void        cfg_set_timer_last_s(uint32_t s);

/* alarms: fixed table of ALARM_MAX; a slot with hour == 0xFF is unused */
const alarm_t *cfg_alarms(void);
void        cfg_set_alarm(int i, const alarm_t *a);   /* NULL deletes */
int         cfg_alarm_count(void);
