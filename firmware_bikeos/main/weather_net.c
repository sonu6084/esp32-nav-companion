/*
 * Weather source #1: Open-Meteo (free, no API key) over Wi-Fi.
 *
 * Wi-Fi is brought up only for a fetch and stopped afterwards. Everything runs in
 * one task on core 0; results go to the weather store (no LVGL calls here).
 */
#include "weather_net.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "weather_store.h"
#include "sys.h"

static const char *TAG = "wx";

#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1
#define WIFI_TIMEOUT_MS  15000
#define HTTP_BUF_MAX     (24 * 1024)

static TaskHandle_t       s_task;
static EventGroupHandle_t s_evt;
static bool               s_wifi_ready;
static int                s_retries;

void wx_request_refresh(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* ------------------------------ Wi-Fi ------------------------------ */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_retries < 4) esp_wifi_connect();
        else xEventGroupSetBits(s_evt, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, BIT_GOT_IP);
    }
}

static bool wifi_setup_once(void)
{
    if (s_wifi_ready) return true;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) return false;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_BIKEOS_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_BIKEOS_WIFI_PASS, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_BIKEOS_WIFI_PASS) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_wifi_ready = true;
    return true;
}

static bool wifi_up(void)
{
    if (!wifi_setup_once()) return false;
    s_retries = 0;
    xEventGroupClearBits(s_evt, BIT_GOT_IP | BIT_FAILED);
    if (esp_wifi_start() != ESP_OK) return false;
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    return (b & BIT_GOT_IP) != 0;
}

static void wifi_down(void)
{
    if (!s_wifi_ready) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* ------------------------------ HTTP ------------------------------ */
typedef struct { char *buf; int len; bool overflow; } body_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + e->data_len >= HTTP_BUF_MAX) { b->overflow = true; return ESP_OK; }
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

static wx_state_t fetch(void)
{
    char url[512];
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
             "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
             "&timezone=auto&forecast_days=%d&forecast_hours=%d",
             CONFIG_BIKEOS_LAT, CONFIG_BIKEOS_LON, WX_DAYS, WX_HOURS + 2);

    body_t body = { .buf = heap_caps_malloc(HTTP_BUF_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) };
    if (!body.buf) return WX_ST_ERR_HTTP;
    body.buf[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .event_handler = http_evt,
        .user_data = &body,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    wx_state_t st;
    if (err != ESP_OK || status != 200 || body.overflow) {
        ESP_LOGW(TAG, "http err=%s status=%d len=%d", esp_err_to_name(err), status, body.len);
        st = WX_ST_ERR_HTTP;
    } else {
        wx_data_t d;
        if (wx_parse_open_meteo(body.buf, &d)) {
            wx_set(&d);
            st = WX_ST_OK;
#if CONFIG_BIKEOS_TIME_FROM_WEATHER
            datetime_t now;
            sys_get_time(&now);
            long a = ((long)now.hour * 60 + now.minute), b = ((long)d.obs.hour * 60 + d.obs.minute);
            bool same_day = now.year == d.obs.year && now.month == d.obs.month && now.day == d.obs.day;
            if (!same_day || labs(a - b) > 2) {
                datetime_t t = d.obs;
                t.second = 0;
                sys_set_time(&t);
                ESP_LOGW(TAG, "RTC corrected to %02d:%02d from weather server", t.hour, t.minute);
            }
#endif
        } else {
            st = WX_ST_ERR_PARSE;
        }
    }
    free(body.buf);
    return st;
}

/* ------------------------------ task ------------------------------ */
static void wx_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));             /* let the UI come up first */
    const TickType_t period = pdMS_TO_TICKS(CONFIG_BIKEOS_WEATHER_REFRESH_MIN * 60 * 1000);
    for (;;) {
        if (strlen(CONFIG_BIKEOS_WIFI_SSID) == 0) {
            wx_set_state(WX_ST_NO_CONFIG);
        } else {
            wx_set_state(WX_ST_CONNECTING);
            if (wifi_up()) {
                wx_set_state(WX_ST_FETCHING);
                wx_set_state(fetch());
            } else {
                wx_set_state(WX_ST_ERR_WIFI);
            }
            wifi_down();
        }
        ulTaskNotifyTake(pdTRUE, period);        /* wait for the period or a manual refresh */
    }
}

void weather_net_init(void)
{
    s_evt = xEventGroupCreate();
    xTaskCreatePinnedToCore(wx_task, "wx", 8192, NULL, 2, &s_task, 0);
}
