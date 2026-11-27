#pragma once
/* Starts the background fetcher: one fetch ~5 s after boot, then every
   CONFIG_BIKEOS_WEATHER_REFRESH_MIN, or on wx_request_refresh().
   Wi-Fi is switched on only for the fetch, then off again (battery). */
void weather_net_init(void);
