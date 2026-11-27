#pragma once
/*
 * bike-os faces for firmware_ws146's minimal display (Beeline Moto II style).
 * map_view.c calls these from a few hooks marked "bike-os"; everything they
 * draw lives here so map_view.c stays close to the original.
 *
 *   journey progress  (replaces FACE_ETA's drawing)
 *   waypoints         (only while the phone reports stops: NAV_STOPS)
 *   compass + odometer
 *   ride              (Start ride / live ride / saved summary)
 */
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    bool     live;            /* a fresh GPS fix */
    int      speed;           /* in the display units */
    bool     mph;
    uint16_t cog_cdeg;        /* course over ground, 0.01 deg */
    uint32_t clock_epoch;     /* local time, 0 if unknown */
    bool     clock_24h;
    bool     navigating;
    int      remaining_m, remaining_s, progress_permille;   /* -1 when not navigating */
    double   trip_m;          /* distance since power-on (map_view's trip counter) */
    uint16_t limit;           /* posted limit in the display units, 0 unknown */
} bos_face_ctx_t;

/* waypoint data from the phone (nav_core, GUI task) */
void bos_stops_set(const uint8_t *msg, size_t len);
void bos_stops_clear(void);
bool bos_stops_present(void);

void    bos_draw_cruise(lv_layer_t *layer, const bos_face_ctx_t *c);   /* 0.11: speed + compass ring */
int32_t bos_key_cruise(const bos_face_ctx_t *c);
void    bos_draw_eta(lv_layer_t *layer, const bos_face_ctx_t *c);
void    bos_draw_stops(lv_layer_t *layer, const bos_face_ctx_t *c);
void    bos_draw_track(lv_layer_t *layer, const bos_face_ctx_t *c);
void    bos_draw_ride(lv_layer_t *layer, const bos_face_ctx_t *c);
int32_t bos_key_stops(void);
int32_t bos_key_track(const bos_face_ctx_t *c);
int32_t bos_key_ride(void);
void    bos_tap_ride(void);       /* Start ride / tap twice to end */

/* rejoin-route: nearest point on a polyline (lat/lon e7) to a position.
   Returns the index of the segment start and writes the point; dist in metres. */
int bos_nearest_on_route(const int32_t *lat, const int32_t *lon, int n, int32_t plat, int32_t plon,
                         int32_t *out_lat, int32_t *out_lon, double *dist_m);
const char *bos_ride_result_text(int res);   /* ride_result_t -> a short toast */
