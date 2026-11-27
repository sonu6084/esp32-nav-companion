#pragma once
/*
 * App framework.
 *
 * Every module (stopwatch, timer, clock, nav, alarm, rides, ...) is an app_t.
 * Add one by writing apps/<name>.c that exports `const app_t app_<name>` and
 * listing it in apps/registry.c — nothing else changes.
 *
 * Lifecycle (all calls happen in the LVGL thread):
 *   create(scr)  build UI on a fresh screen; start lv_timers here
 *   destroy()    the screen is about to be deleted: delete your lv_timers and
 *                forget widget pointers. Keep *state* (a stopwatch keeps
 *                running in the background).
 *   on_key(k)    physical BOOT key; return true if handled
 *   status()     optional one-line text shown under the app in the menu
 *                (e.g. a running stopwatch time); NULL if nothing to say
 *   bg_tick()    optional, called every 100 ms whether or not the app is open
 *                (timers firing, alarms ringing, nav auto-open)
 *   keep_awake() optional; return true to block the screen-off timeout
 *
 * Navigation: boot intro -> home (clock). Tap home -> menu. In an app, PWR
 * tap or swipe right -> menu; in the menu, PWR tap or swipe right -> home
 * (also after 30 s untouched, or when the screen sleeps). A route starting
 * opens Navigation from anywhere.
 */
#include <stdbool.h>
#include "lvgl.h"

typedef enum {
    APP_KEY_PRIMARY,     /* BOOT tap  */
    APP_KEY_SECONDARY,   /* BOOT hold */
} app_key_t;

typedef struct app_s {
    const char *name;
    const char *symbol;          /* LV_SYMBOL_* or short text */
    uint32_t    color;           /* 0xRRGGBB icon colour */
    void (*create)(lv_obj_t *scr, const struct app_s *self);
    void (*destroy)(void);
    bool (*on_key)(app_key_t key);
    const char *(*status)(void);
    void (*bg_tick)(void);
    bool (*keep_awake)(void);
    bool instant;                /* load the screen before create(), no slide animation */
    bool no_swipe_back;          /* the app uses horizontal drags itself (map) */
    bool ready;                  /* false = shows "coming soon" */
} app_t;

void am_init(void);                 /* builds the menu and starts the event poller */
void am_open(const app_t *app);     /* from the menu or from another app (switches) */
const app_t *am_find(const char *name);
void am_back(void);                 /* to the app open before the last app->app switch, else menu */
void am_screen_wake(void);          /* backlight on + reset the idle timer */
bool am_screen_off(void);
void am_home(void);                 /* to the menu */
void am_show_home(void);            /* to the home (clock) screen */
void am_show_menu(void);            /* home -> menu */
bool am_on_home(void);
const app_t *am_current(void);      /* NULL while in the menu */
