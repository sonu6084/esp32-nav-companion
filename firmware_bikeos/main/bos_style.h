#pragma once
/*
 * bike-os look (0.11): one palette and one set of type sizes for the turn card, the
 * faces and the media page, so they read as one device.
 *
 *   near-black ground, white for the number that matters, grey for labels, green for
 *   "go" (the turn arrow, the arrival time, the ride/progress ring), blue for the route
 *   and the media ring, a red-ringed sign for the speed limit, amber for warnings.
 *
 * Big numbers use font_clock_120 (digits and ':') and font_temp_100 (digits, '-', '.',
 * degree); units and labels use Montserrat. Layout is in real panel pixels (412 x 412)
 * and keeps text inside the circle: the usable chord is 2*sqrt(206^2 - (y-206)^2).
 */
#include "lvgl.h"
#include "fonts.h"

#define BOS_BG        0x000000
#define BOS_TEXT      0xFFFFFF
#define BOS_SOFT      0xC7CBD1   /* secondary values */
#define BOS_MUTED     0x7D838C   /* labels: DISTANCE, km, onto */
#define BOS_TRACK     0x23272D   /* the unlit part of a ring */
#define BOS_RULE      0x2E333A   /* divider lines */
#define BOS_GO        0x2FD267   /* turn arrow, arrival time, progress */
#define BOS_ROUTE     0x2F80FF   /* route, media progress */
#define BOS_LIMIT     0xE53935   /* speed-limit ring, over the limit */
#define BOS_WARN      0xFFB300
#define BOS_NORTH     0xFF4D4D

/* streets on the minimal map: navy ground, a dim wide halo under a bright core */
#define BOS_MAP_BG     0x060B14
#define BOS_STREET_HALO_MINOR 0x0D2140
#define BOS_STREET_CORE_MINOR 0x2563B8
#define BOS_STREET_HALO_MAJOR 0x123566
#define BOS_STREET_CORE_MAJOR 0x4C9BFF

#define BOS_FONT_HUGE   (&font_clock_120)        /* 450, 43, 18:25 */
#define BOS_FONT_BIG    (&font_temp_100)         /* 1.2, 12.4 */
#define BOS_FONT_UNIT   (&lv_font_montserrat_28)
#define BOS_FONT_VALUE  (&lv_font_montserrat_32)
#define BOS_FONT_LABEL  (&lv_font_montserrat_14)
#define BOS_FONT_LINE   (&lv_font_montserrat_20)
#define BOS_FONT_STREET (&lv_font_montserrat_24)
