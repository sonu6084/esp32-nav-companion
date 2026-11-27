/*
 * App list — menu order. To add a module:
 *   1. write apps/<name>.c exporting `const app_t app_<name>` with .ready = true
 *   2. replace its placeholder below with `&app_<name>`
 */
#include "registry.h"
#include "ui_theme.h"

extern const app_t app_clock;
extern const app_t app_stopwatch;
extern const app_t app_weather;
extern const app_t app_timer;
extern const app_t app_alarm;
extern const app_t app_nav;
extern const app_t app_settings;
extern const app_t app_music;
extern const app_t app_rides;

#define SOON(n, sym, col) (&(const app_t){ .name = n, .symbol = sym, .color = col, .ready = false })

const app_t *const g_apps[] = {
    &app_clock,
    &app_stopwatch,
    &app_timer,
    &app_nav,
    &app_rides,
    &app_alarm,
    &app_weather,
    &app_music,
    &app_settings,
};
const int g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);
_Static_assert(sizeof(g_apps) / sizeof(g_apps[0]) <= APP_MAX, "raise APP_MAX");
