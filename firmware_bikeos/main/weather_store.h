#pragma once
/*
 * Weather data store — one snapshot shared by every producer and consumer.
 *
 * Producers: weather_net.c (Open-Meteo over Wi-Fi) today; the phone app over
 * BLE later — it only has to fill a wx_data_t and call wx_set().
 * Consumers: apps/weather.c, menu status line.
 *
 * Pure C (no ESP deps except an optional lock) so it is unit-tested on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include "rtc.h"
#include "wx_icons.h"

#define WX_HOURS 6
#define WX_DAYS  4          /* [0] = today */

typedef struct {
    uint8_t hour;           /* local hour 0-23 */
    int16_t code;           /* WMO weather code */
    float   temp;
    int8_t  pop;            /* precipitation probability %, -1 unknown */
    bool    is_day;
} wx_hour_t;

typedef struct {
    uint16_t year;
    uint8_t  month, day, dow;
    int16_t  code;
    float    tmax, tmin;
    int8_t   pop;
} wx_day_t;

typedef struct {
    bool     valid;
    uint32_t seq;           /* bumped by wx_set(); UIs redraw on change */
    datetime_t obs;         /* local time of the observation (from the source) */
    float    temp, feels, wind_kmh;
    int8_t   humidity;
    int16_t  code;
    bool     is_day;
    char     sunrise[6], sunset[6];   /* "HH:MM" */
    int      n_hours;
    wx_hour_t hours[WX_HOURS];        /* the next hours, starting after the current one */
    int      n_days;
    wx_day_t days[WX_DAYS];
} wx_data_t;

typedef enum {
    WX_ST_IDLE,
    WX_ST_NO_CONFIG,        /* no Wi-Fi SSID set */
    WX_ST_CONNECTING,
    WX_ST_FETCHING,
    WX_ST_OK,
    WX_ST_ERR_WIFI,
    WX_ST_ERR_HTTP,
    WX_ST_ERR_PARSE,
} wx_state_t;

/* Parse an Open-Meteo /v1/forecast response (see weather_net.c for the query). */
bool        wx_parse_open_meteo(const char *json, wx_data_t *out);

void        wx_get(wx_data_t *out);
void        wx_set(const wx_data_t *in);      /* copies, bumps seq, sets valid */
uint32_t    wx_seq(void);
wx_state_t  wx_state(void);
void        wx_set_state(wx_state_t s);
const char *wx_state_text(wx_state_t s);

const char *wx_code_text(int code);
wx_icon_t   wx_code_icon(int code, bool is_day);

/* Implemented by the network layer (stubbed on host). */
void        wx_request_refresh(void);
