#pragma once
#include <stdint.h>
extern int64_t g_fake_us;
/* g_time_creep_us > 0: every read moves the clock on a little, as the board's does within
   one GUI loop (fixes stamped after the loop's own clock). 0 keeps the old frozen clock. */
extern int64_t g_time_creep_us;
static inline int64_t esp_timer_get_time(void) { g_fake_us += g_time_creep_us; return g_fake_us; }
