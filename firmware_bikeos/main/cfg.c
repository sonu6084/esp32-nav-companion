#include "cfg.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#define NS "bikeos"
#endif

static struct {
    uint8_t  volume, face, keep_on_nav;
    uint16_t timeout_s;
    uint32_t timer_last_s;
    alarm_t  alarms[ALARM_MAX];
} S = { .volume = 70, .face = 0, .keep_on_nav = 1, .timeout_s = 0, .timer_last_s = 5 * 60 };

#ifdef ESP_PLATFORM
static void put(const char *k, const void *p, size_t n, int kind)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    switch (kind) {
    case 1: nvs_set_u8(h, k, *(const uint8_t *)p); break;
    case 2: nvs_set_u16(h, k, *(const uint16_t *)p); break;
    case 4: nvs_set_u32(h, k, *(const uint32_t *)p); break;
    default: nvs_set_blob(h, k, p, n); break;
    }
    nvs_commit(h);
    nvs_close(h);
}
#define PUT8(k, v)  do { uint8_t _v = (v);  put(k, &_v, 1, 1); } while (0)
#define PUT16(k, v) do { uint16_t _v = (v); put(k, &_v, 2, 2); } while (0)
#define PUT32(k, v) do { uint32_t _v = (v); put(k, &_v, 4, 4); } while (0)
#define PUTB(k, p, n) put(k, p, n, 0)
#else
#define PUT8(k, v)
#define PUT16(k, v)
#define PUT32(k, v)
#define PUTB(k, p, n)
#endif

void cfg_init(void)
{
    for (int i = 0; i < ALARM_MAX; i++) S.alarms[i].hour = 0xFF;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t v8; uint16_t v16; uint32_t v32;
        if (nvs_get_u8(h, "vol", &v8) == ESP_OK && v8 <= 100) S.volume = v8;
        if (nvs_get_u8(h, "face", &v8) == ESP_OK)    S.face = v8;
        if (nvs_get_u8(h, "navon", &v8) == ESP_OK)   S.keep_on_nav = v8;
        if (nvs_get_u16(h, "tmo", &v16) == ESP_OK)   S.timeout_s = v16;
        if (nvs_get_u32(h, "tlast", &v32) == ESP_OK) S.timer_last_s = v32;
        alarm_t tmp[ALARM_MAX];
        size_t n = sizeof tmp;
        if (nvs_get_blob(h, "alarms", tmp, &n) == ESP_OK && n == sizeof tmp) memcpy(S.alarms, tmp, n);
        nvs_close(h);
    }
#endif
}

uint8_t  cfg_volume(void) { return S.volume; }
void     cfg_set_volume(uint8_t v) { if (v > 100) v = 100; if (v != S.volume) { S.volume = v; PUT8("vol", v); } }
uint16_t cfg_screen_timeout_s(void) { return S.timeout_s; }
void     cfg_set_screen_timeout_s(uint16_t s) { if (s != S.timeout_s) { S.timeout_s = s; PUT16("tmo", s); } }
bool     cfg_keep_on_nav(void) { return S.keep_on_nav; }
void     cfg_set_keep_on_nav(bool on) { if (on != S.keep_on_nav) { S.keep_on_nav = on; PUT8("navon", on); } }
uint8_t  cfg_clock_face(void) { return S.face; }
void     cfg_set_clock_face(uint8_t f) { if (f != S.face) { S.face = f; PUT8("face", f); } }
uint32_t cfg_timer_last_s(void) { return S.timer_last_s; }
void     cfg_set_timer_last_s(uint32_t s) { if (s != S.timer_last_s) { S.timer_last_s = s; PUT32("tlast", s); } }

const alarm_t *cfg_alarms(void) { return S.alarms; }
int cfg_alarm_count(void)
{
    int n = 0;
    for (int i = 0; i < ALARM_MAX; i++) n += S.alarms[i].hour != 0xFF;
    return n;
}
void cfg_set_alarm(int i, const alarm_t *a)
{
    if (i < 0 || i >= ALARM_MAX) return;
    if (a) S.alarms[i] = *a;
    else { memset(&S.alarms[i], 0, sizeof S.alarms[i]); S.alarms[i].hour = 0xFF; }
    PUTB("alarms", S.alarms, sizeof S.alarms);
}
