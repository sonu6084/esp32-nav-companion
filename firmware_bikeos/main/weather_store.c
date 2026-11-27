#include "weather_store.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define WX_LOCK()   portENTER_CRITICAL(&s_mux)
#define WX_UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#define WX_LOCK()
#define WX_UNLOCK()
#endif

static wx_data_t           s_data;
static volatile wx_state_t s_state = WX_ST_IDLE;

void wx_get(wx_data_t *out) { WX_LOCK(); *out = s_data; WX_UNLOCK(); }
uint32_t wx_seq(void) { return s_data.seq; }
wx_state_t wx_state(void) { return s_state; }
void wx_set_state(wx_state_t s) { s_state = s; }

void wx_set(const wx_data_t *in)
{
    WX_LOCK();
    uint32_t seq = s_data.seq;
    s_data = *in;
    s_data.valid = true;
    s_data.seq = seq + 1;
    WX_UNLOCK();
}

const char *wx_state_text(wx_state_t s)
{
    switch (s) {
    case WX_ST_NO_CONFIG:  return "Wi-Fi not set\n(idf.py menuconfig > bike-os)";
    case WX_ST_CONNECTING: return "Connecting to Wi-Fi...";
    case WX_ST_FETCHING:   return "Fetching weather...";
    case WX_ST_ERR_WIFI:   return "Wi-Fi not reachable";
    case WX_ST_ERR_HTTP:   return "Weather server error";
    case WX_ST_ERR_PARSE:  return "Bad weather data";
    default:               return "";
    }
}

/* WMO weather interpretation codes (Open-Meteo docs) */
const char *wx_code_text(int c)
{
    switch (c) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm + hail";
    default: return "--";
    }
}

wx_icon_t wx_code_icon(int c, bool day)
{
    if (c == 0) return day ? WX_ICON_CLEAR_DAY : WX_ICON_CLEAR_NIGHT;
    if (c == 1 || c == 2) return day ? WX_ICON_PARTLY_DAY : WX_ICON_PARTLY_NIGHT;
    if (c == 3) return WX_ICON_CLOUDY;
    if (c == 45 || c == 48) return WX_ICON_FOG;
    if (c >= 51 && c <= 57) return WX_ICON_DRIZZLE;
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_ICON_RAIN;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_ICON_SNOW;
    if (c >= 95 && c <= 99) return WX_ICON_THUNDER;
    return WX_ICON_UNKNOWN;
}

/* ------------------------------ parsing ------------------------------ */
static double num(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
static double arr_num(const cJSON *a, int i, double dflt)
{
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
static const char *arr_str(const cJSON *a, int i)
{
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
/* "YYYY-MM-DDTHH:MM" or "YYYY-MM-DD" */
static bool parse_iso(const char *s, datetime_t *t)
{
    if (!s) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi);
    if (n < 3 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    memset(t, 0, sizeof *t);
    t->year = y; t->month = mo; t->day = d; t->hour = h; t->minute = mi;
    return true;
}
static int dow(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static int8_t clamp_pct(double v) { return v < 0 ? -1 : v > 100 ? 100 : (int8_t)lround(v); }

bool wx_parse_open_meteo(const char *json, wx_data_t *o)
{
    memset(o, 0, sizeof *o);
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cJSON_IsObject(cur)) goto out;
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(cur, "time");
    if (!cJSON_IsString(ct) || !parse_iso(ct->valuestring, &o->obs)) goto out;
    o->obs.dotw = dow(o->obs.year, o->obs.month, o->obs.day);

    o->temp     = (float)num(cur, "temperature_2m", NAN);
    if (isnan(o->temp)) goto out;
    o->feels    = (float)num(cur, "apparent_temperature", o->temp);
    o->humidity = clamp_pct(num(cur, "relative_humidity_2m", -1));
    o->wind_kmh = (float)num(cur, "wind_speed_10m", -1);
    o->code     = (int16_t)num(cur, "weather_code", -1);
    o->is_day   = num(cur, "is_day", 1) != 0;

    /* hourly: skip entries at or before the current hour */
    const cJSON *hr = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (cJSON_IsObject(hr)) {
        const cJSON *ht = cJSON_GetObjectItemCaseSensitive(hr, "time");
        const cJSON *htp = cJSON_GetObjectItemCaseSensitive(hr, "temperature_2m");
        const cJSON *hc = cJSON_GetObjectItemCaseSensitive(hr, "weather_code");
        const cJSON *hp = cJSON_GetObjectItemCaseSensitive(hr, "precipitation_probability");
        const cJSON *hd = cJSON_GetObjectItemCaseSensitive(hr, "is_day");
        int n = cJSON_GetArraySize(ht);
        long now_key = ((long)o->obs.year * 10000 + o->obs.month * 100 + o->obs.day) * 100 + o->obs.hour;
        for (int i = 0; i < n && o->n_hours < WX_HOURS; i++) {
            datetime_t t;
            if (!parse_iso(arr_str(ht, i), &t)) continue;
            long key = ((long)t.year * 10000 + t.month * 100 + t.day) * 100 + t.hour;
            if (key <= now_key) continue;
            double tv = arr_num(htp, i, NAN);
            if (isnan(tv)) continue;
            wx_hour_t *h = &o->hours[o->n_hours++];
            h->hour = t.hour;
            h->temp = (float)tv;
            h->code = (int16_t)arr_num(hc, i, -1);
            h->pop = clamp_pct(arr_num(hp, i, -1));
            h->is_day = hd ? arr_num(hd, i, 1) != 0 : (t.hour >= 6 && t.hour < 18);
        }
    }

    const cJSON *dy = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (cJSON_IsObject(dy)) {
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(dy, "time");
        const cJSON *dc = cJSON_GetObjectItemCaseSensitive(dy, "weather_code");
        const cJSON *dmx = cJSON_GetObjectItemCaseSensitive(dy, "temperature_2m_max");
        const cJSON *dmn = cJSON_GetObjectItemCaseSensitive(dy, "temperature_2m_min");
        const cJSON *dp = cJSON_GetObjectItemCaseSensitive(dy, "precipitation_probability_max");
        const cJSON *sr = cJSON_GetObjectItemCaseSensitive(dy, "sunrise");
        const cJSON *ss = cJSON_GetObjectItemCaseSensitive(dy, "sunset");
        int n = cJSON_GetArraySize(dt);
        for (int i = 0; i < n && o->n_days < WX_DAYS; i++) {
            datetime_t t;
            if (!parse_iso(arr_str(dt, i), &t)) continue;
            double mx = arr_num(dmx, i, NAN), mn = arr_num(dmn, i, NAN);
            if (isnan(mx) || isnan(mn)) continue;
            wx_day_t *d = &o->days[o->n_days++];
            d->year = t.year; d->month = t.month; d->day = t.day;
            d->dow = dow(t.year, t.month, t.day);
            d->code = (int16_t)arr_num(dc, i, -1);
            d->tmax = (float)mx; d->tmin = (float)mn;
            d->pop = clamp_pct(arr_num(dp, i, -1));
        }
        datetime_t t;
        if (parse_iso(arr_str(sr, 0), &t)) snprintf(o->sunrise, sizeof o->sunrise, "%02u:%02u", (unsigned)(t.hour % 24), (unsigned)(t.minute % 60));
        if (parse_iso(arr_str(ss, 0), &t)) snprintf(o->sunset, sizeof o->sunset, "%02u:%02u", (unsigned)(t.hour % 24), (unsigned)(t.minute % 60));
    }
    ok = true;
out:
    cJSON_Delete(root);
    return ok;
}
