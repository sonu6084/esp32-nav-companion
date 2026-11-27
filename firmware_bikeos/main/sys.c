#include "sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "board.h"
#include "touch.h"

#define POLL_MS         20
#define SHORT_MAX_MS    600
#define BOOT_LONG_MS    800
#define PWR_OFF_MS      3000      /* same as firmware_ws146 */

/* battery divider on GPIO8 = ADC1_CH7; Waveshare: V = mV * 3 / 1000 / 0.990476 */
#define BAT_CHAN        ADC_CHANNEL_7
#define BAT_SCALE       (3.0f / 0.990476f)

static const char *TAG = "sys";

static volatile uint32_t s_events;
static datetime_t        s_time;
static volatile float    s_bat_v;
static bool              s_booted_by_key;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

static inline void raise(uint32_t bits) { __atomic_fetch_or(&s_events, bits, __ATOMIC_SEQ_CST); }
uint32_t sys_take_events(void) { return __atomic_exchange_n(&s_events, 0, __ATOMIC_SEQ_CST); }

void  sys_get_time(datetime_t *out) { *out = s_time; }
float sys_battery_volts(void) { return s_bat_v; }
bool  sys_on_battery_latch(void) { return s_booted_by_key; }

int sys_battery_pct(void)
{
    float v = s_bat_v;
    if (v < 2.8f) return -1;                 /* no cell / USB only */
    static const float vt[] = {3.30f, 3.60f, 3.70f, 3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
    static const int   pt[] = {0,     10,    25,    40,    50,    65,    80,    92,    100};
    if (v <= vt[0]) return 0;
    for (int i = 1; i < 9; i++)
        if (v <= vt[i]) return pt[i - 1] + (int)((v - vt[i - 1]) / (vt[i] - vt[i - 1]) * (pt[i] - pt[i - 1]));
    return 100;
}

int sys_weekday(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int sys_days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (m == 2 && leap) ? 29 : dm[(m - 1) % 12];
}

void sys_set_time(const datetime_t *in)
{
    datetime_t t = *in;
    t.dotw = (uint8_t)sys_weekday(t.year, t.month, t.day);
    pcf_write(&t);
    s_time = t;
}

void sys_set_backlight(uint8_t pct) { board_backlight_set(pct); }

/* firmware_ws146's sleep_now(): touch INT (GPIO4) wakes, GPIO7 latch held through sleep */
void sys_deep_sleep(void)
{
    board_backlight_set(0);
    board_power_rails_off();
    touch_poll();                                  /* drain: INT idle high */
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGW(TAG, "deep sleep (tap to wake)");
    gpio_hold_en(BOARD_GPIO_PWR_HOLD);
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_ext0_wakeup(BOARD_GPIO_TOUCH_INT, 0);
    rtc_gpio_pullup_en(BOARD_GPIO_TOUCH_INT);
    rtc_gpio_pulldown_dis(BOARD_GPIO_TOUCH_INT);
    esp_deep_sleep_start();
}

void sys_power_off(void)
{
    board_backlight_set(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    board_power_off();                             /* battery: power is gone here */
    vTaskDelay(pdMS_TO_TICKS(500));
    sys_deep_sleep();                              /* still running: USB powered */
}

static float read_bat(void)
{
    int raw = 0, mv = 0;
    if (!s_adc || adc_oneshot_read(s_adc, BAT_CHAN, &raw) != ESP_OK) return 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv * BAT_SCALE / 1000.0f;
    return raw * 3.3f / 4095.0f * BAT_SCALE;
}

static void bat_init(void)
{
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { s_adc = NULL; return; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, BAT_CHAN, &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .chan = BAT_CHAN,
                                           .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
}

static void sys_task(void *arg)
{
    bool pwr_armed = !board_pwr_key_pressed();     /* the power-on press doesn't count */
    uint32_t pwr_ms = 0, boot_ms = 0, tick = 0;
    bool boot_long_sent = false, pwr_off_sent = false;

    for (;;) {
        if (board_pwr_key_pressed()) {
            pwr_ms += POLL_MS;
            if (pwr_armed && !pwr_off_sent && pwr_ms >= PWR_OFF_MS) { pwr_off_sent = true; raise(SYS_EVT_SHUTDOWN); }
        } else {
            if (pwr_armed && pwr_ms > 0 && pwr_ms <= SHORT_MAX_MS) raise(SYS_EVT_PWR_SHORT);
            pwr_armed = true;
            pwr_ms = 0;
            pwr_off_sent = false;
        }

        if (board_boot_button_pressed()) {
            boot_ms += POLL_MS;
            if (!boot_long_sent && boot_ms >= BOOT_LONG_MS) { boot_long_sent = true; raise(SYS_EVT_BOOT_LONG); }
        } else {
            if (boot_ms > 0 && !boot_long_sent && boot_ms <= SHORT_MAX_MS) raise(SYS_EVT_BOOT_SHORT);
            boot_ms = 0;
            boot_long_sent = false;
        }

        if (tick % (500 / POLL_MS) == 0) {
            datetime_t t;
            if (pcf_read(&t) == ESP_OK) s_time = t;
        }
        if (tick % (2000 / POLL_MS) == 0) s_bat_v = read_bat();
        tick++;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void bos_sys_init(void)
{
    s_booted_by_key = board_pwr_key_pressed();
    pcf_init();
    bat_init();
    s_bat_v = read_bat();
    pcf_read(&s_time);
    ESP_LOGW(TAG, "bat=%.2fV", s_bat_v);
    xTaskCreatePinnedToCore(sys_task, "sys", 4096, NULL, 3, NULL, 0);
}
