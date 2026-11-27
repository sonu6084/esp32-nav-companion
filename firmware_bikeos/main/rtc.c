#include "rtc.h"

#include "board.h"
#include "esp_log.h"

#define PCF85063_ADDR   0x51
#define REG_CTRL1       0x00
#define REG_SECONDS     0x04
#define CTRL1_CAP_12P5  0x01      /* 12.5 pF crystal load, as in Waveshare's driver */
#define YEAR_BASE       2000

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd(int v)    { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int     dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

esp_err_t pcf_init(void)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t e = i2c_master_bus_add_device(board_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) return e;
    uint8_t w[2] = {REG_CTRL1, CTRL1_CAP_12P5};       /* normal mode, RTC running, 24 h */
    e = i2c_master_transmit(s_dev, w, 2, 100);
    if (e != ESP_OK) ESP_LOGW("rtc", "PCF85063 not answering (%s)", esp_err_to_name(e));
    return e;
}

esp_err_t pcf_read(datetime_t *t)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t reg = REG_SECONDS, b[7];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 100);
    if (e != ESP_OK) return e;
    t->second = dec(b[0] & 0x7F);
    t->minute = dec(b[1] & 0x7F);
    t->hour   = dec(b[2] & 0x3F);
    t->day    = dec(b[3] & 0x3F);
    t->dotw   = b[4] & 0x07;
    t->month  = dec(b[5] & 0x1F);
    t->year   = YEAR_BASE + dec(b[6]);
    return ESP_OK;
}

esp_err_t pcf_write(const datetime_t *t)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t w[8] = {REG_SECONDS, bcd(t->second), bcd(t->minute), bcd(t->hour), bcd(t->day),
                    (uint8_t)(t->dotw & 7), bcd(t->month), bcd(t->year - YEAR_BASE)};
    return i2c_master_transmit(s_dev, w, sizeof w, 100);
}
