/* Host platform for the bike-os v0.5 simulator: FreeRTOS queue shim, in-memory
 * NVS, and stand-ins for the hardware modules of firmware_ws146 (board, display,
 * touch, SD, tiles, media art, BLE). The nav VIEW code itself is the real file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "sdcard.h"
#include "tiles.h"
#include "media_art.h"
#include "ble_service.h"
#include "sys.h"
#include "audio.h"

int64_t g_fake_us;

/* ---------------- FreeRTOS queue shim (single thread) ---------------- */
struct sim_queue { UBaseType_t len, size, count, head; uint8_t *buf; };
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    q->len = len; q->size = size ? size : 1; q->buf = calloc(len, q->size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t t)
{
    (void)t;
    if (q->count == q->len) return pdFALSE;
    memcpy(q->buf + ((q->head + q->count) % q->len) * q->size, it, q->size);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *it) { memcpy(q->buf, it, q->size); q->head = 0; q->count = 1; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t)
{
    (void)t;
    if (!q->count) return pdFALSE;
    memcpy(out, q->buf + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->count--;
    return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xQueueCreate(1, 1); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { uint8_t b; return xQueueReceive(s, &b, t); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { uint8_t b = 1; return xQueueSend(s, &b, 0); }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t st, void *arg, UBaseType_t pr, TaskHandle_t *h, BaseType_t core)
{ (void)fn; (void)n; (void)st; (void)arg; (void)pr; (void)core; if (h) *h = NULL; return pdFAIL; }  /* -> single-threaded paths */

/* ---------------- NVS in memory ---------------- */
typedef struct { char ns[16], key[16]; size_t len; uint8_t val[256]; } kv_t;
static kv_t s_kv[64];
static int s_nkv;
static const char *s_ns_of[8];
static int s_nh;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { s_nkv = 0; return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)m; s_ns_of[s_nh % 8] = ns; *h = s_nh++ % 8; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static kv_t *find(nvs_handle_t h, const char *k, bool make)
{
    for (int i = 0; i < s_nkv; i++) if (!strcmp(s_kv[i].ns, s_ns_of[h]) && !strcmp(s_kv[i].key, k)) return &s_kv[i];
    if (!make || s_nkv == 64) return NULL;
    kv_t *e = &s_kv[s_nkv++];
    snprintf(e->ns, sizeof e->ns, "%s", s_ns_of[h]); snprintf(e->key, sizeof e->key, "%s", k);
    return e;
}
static esp_err_t get(nvs_handle_t h, const char *k, void *v, size_t n) { kv_t *e = find(h, k, false); if (!e) return ESP_ERR_NVS_NOT_FOUND; memcpy(v, e->val, n); return ESP_OK; }
static esp_err_t set(nvs_handle_t h, const char *k, const void *v, size_t n) { kv_t *e = find(h, k, true); if (!e || n > 256) return ESP_FAIL; memcpy(e->val, v, n); e->len = n; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { return get(h, k, v, 1); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return set(h, k, &v, 1); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { return get(h, k, v, 2); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return set(h, k, &v, 2); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { return get(h, k, v, 4); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return set(h, k, &v, 4); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { return get(h, k, v, 4); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { return set(h, k, &v, 4); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len)
{ kv_t *e = find(h, k, false); if (!e) return ESP_ERR_NVS_NOT_FOUND; if (*len < e->len) return ESP_FAIL; memcpy(v, e->val, e->len); *len = e->len; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return set(h, k, v, strlen(v) + 1); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{ kv_t *e = find(h, k, false); if (!e) return ESP_ERR_NVS_NOT_FOUND; if (*len < e->len) return ESP_FAIL; memcpy(v, e->val, e->len); *len = e->len; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return set(h, k, v, len); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; (void)k; return ESP_OK; }

/* ---------------- board / display / touch / sd / tiles / art ---------------- */
int g_backlight = -1;
void board_backlight_set(uint8_t p) { g_backlight = p; }
void board_backlight_init(void) {}
void board_power_off(void) {}
void board_power_rails_off(void) {}
bool board_pwr_key_pressed(void) { return false; }
bool board_boot_button_pressed(void) { return false; }

lv_display_t *g_disp;
lv_display_t *display_get(void) { return g_disp; }
uint32_t display_fps(void) { return 30; }
uint32_t display_last_draw_ms(void) { return 0; }
uint32_t display_last_wait_ms(void) { return 0; }

touch_sample_t g_touch;
const touch_sample_t *touch_get(void) { return &g_touch; }
void touch_poll(void) {}

esp_err_t sdcard_init(void) { return ESP_OK; }
bool sdcard_ok(void) { return false; }              /* no SD card: minimal display, as on your board */

void tiles_init(void) {}
static bool s_dark = true;
void tiles_set_dark(bool d) { s_dark = d; }
bool tiles_dark(void) { return s_dark; }
int tiles_zoom_min(void) { return 0; }
int tiles_zoom_max(void) { return 0; }
void tiles_begin_frame(void) {}
const lv_image_dsc_t *tiles_get(int z, int x, int y, bool u, tile_state_t *st) { (void)z; (void)x; (void)y; (void)u; (void)st; return NULL; }
void tiles_prefetch(int z, int x, int y) { (void)z; (void)x; (void)y; }
void tiles_set_active_zoom(int z) { (void)z; }
bool tiles_poll(void) { return false; }
void tiles_get_stats(tiles_stats_t *o) { memset(o, 0, sizeof *o); }

void media_art_init(void) {}
void media_art_feed(const uint8_t *d, size_t n) { (void)d; (void)n; }
bool media_art_poll(const lv_image_dsc_t **img, uint16_t *id) { (void)img; (void)id; return false; }
uint16_t media_art_rx_id(void) { return 0; }
uint32_t media_art_rx_idle_ms(void) { return 0; }

/* ---------------- BLE: capture the callback table nav_core registers ---------------- */
const nav_ble_cbs_t *g_cbs;
int g_media_cmds, g_call_cmds, g_last_call_cmd;
void nav_ble_start(const nav_ble_cbs_t *cbs, const char *name) { (void)name; g_cbs = cbs; }
bool nav_ble_connected(void) { return true; }
void nav_ble_set_fps(uint8_t f) { (void)f; }
void nav_ble_set_theme(uint8_t d) { (void)d; }
void nav_ble_set_minimal(uint8_t on) { (void)on; }
void nav_ble_media_cmd(uint8_t c, uint8_t a) { (void)c; (void)a; g_media_cmds++; }
bool nav_ble_media_ready(void) { return true; }
void nav_ble_call_cmd(uint8_t c) { g_call_cmds++; g_last_call_cmd = c; }
bool nav_ble_call_ready(void) { return true; }
void nav_ble_disconnect(void) {}
void nav_ble_forget_current(void) {}

/* ---------------- bike-os sys / audio / weather ---------------- */
uint32_t g_events;
static int64_t base_s = ((14 * 60) + 32) * 60 + 5;
static datetime_t base_d = {2026, 11, 26, 4, 0, 0, 0};
int g_rtc_sets;
uint32_t sys_take_events(void) { uint32_t e = g_events; g_events = 0; return e; }
int sys_weekday(int y, int m, int d) { static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4}; if (m < 3) y -= 1; return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7; }
int sys_days_in_month(int y, int m) { static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31}; int l = (y%4==0&&y%100!=0)||y%400==0; return (m==2&&l)?29:dm[m-1]; }
void sys_get_time(datetime_t *o) { int64_t s = base_s + g_fake_us / 1000000; *o = base_d; o->hour = (s / 3600) % 24; o->minute = (s / 60) % 60; o->second = s % 60; }
void sys_set_time(const datetime_t *t) { base_d = *t; base_s = t->hour * 3600 + t->minute * 60 + t->second - g_fake_us / 1000000; g_rtc_sets++; }
float sys_battery_volts(void) { return 3.95f; }
int sys_battery_pct(void) { return 72; }
bool sys_on_battery_latch(void) { return true; }
void sys_set_backlight(uint8_t p) { g_backlight = p; }
void sys_power_off(void) {}
void sys_deep_sleep(void) { printf("  [sys] deep sleep\n"); }
int g_snd;
void audio_init(void) {}
void audio_play(snd_t s) { g_snd = s; }
void audio_stop(void) { g_snd = 0; }
bool audio_busy(void) { return false; }
void wx_request_refresh(void) {}
