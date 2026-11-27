#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define TCA9554_ADDR 0x20
#define TCA9554_REG_INPUT 0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

#define BL_RES LEDC_TIMER_13_BIT
#define BL_MAX ((1 << 13) - 1)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exio;
// Shadow of the output register: never read-modify-write over I2C.
static uint8_t s_exio_out;
static bool s_exio_ready;

// ---------------------------------------------------------------- power latch

void board_power_hold(void)
{
    // Drive high before anything else takes time: on battery the regulator is
    // only held by the user's finger on the PWR key until this pin is up.
    gpio_config_t o = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&o);
    gpio_set_level(BOARD_GPIO_PWR_HOLD, 1);
    // Keep it latched through light sleep / pad reconfig.
    gpio_hold_en(BOARD_GPIO_PWR_HOLD);

    gpio_config_t i = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_PWR_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&i);
}

void board_power_off(void)
{
    gpio_hold_dis(BOARD_GPIO_PWR_HOLD);
    gpio_set_level(BOARD_GPIO_PWR_HOLD, 0);
}

bool board_pwr_key_pressed(void)
{
    return gpio_get_level(BOARD_GPIO_PWR_KEY) == 0;
}

// ---------------------------------------------------------------- I2C

esp_err_t board_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_NUM,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

esp_err_t board_touch_i2c_init(void)
{
    return s_bus ? ESP_OK : ESP_ERR_INVALID_STATE;
}

i2c_master_bus_handle_t board_touch_i2c_bus(void)
{
    return s_bus;
}

// ---------------------------------------------------------------- expander

static esp_err_t exio_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_exio, b, sizeof(b), 100);
}

esp_err_t board_exio_init(void)
{
    if (s_exio_ready) return ESP_OK;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, &s_exio);
    if (err != ESP_OK) return err;

    // Power-on default of the output register is 0xFF (resets released). Write
    // that explicitly first so taking the pins as outputs cannot glitch them
    // low, then make all 8 outputs (the vendor's EXIO_Init does 0x00 too).
    s_exio_out = 0xFF;
    err = exio_write(TCA9554_REG_OUTPUT, s_exio_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not responding: %s", esp_err_to_name(err));
        return err;
    }
    err = exio_write(TCA9554_REG_CONFIG, 0x00);
    if (err != ESP_OK) return err;
    s_exio_ready = true;

    // Pulse both resets, the way the vendor drivers do (50 ms low, 50 ms high
    // for touch; the panel driver's own reset is a no-op with reset_gpio=-1).
    board_exio_set(EXIO_LCD_RST, false);
    board_exio_set(EXIO_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    board_exio_set(EXIO_LCD_RST, true);
    board_exio_set(EXIO_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));  // SPD2010 needs >100 ms after reset
    ESP_LOGW(TAG, "expander up, panel + touch reset");
    return ESP_OK;
}

esp_err_t board_exio_set(int bit, bool level)
{
    if (!s_exio_ready || bit < 0 || bit > 7) return ESP_ERR_INVALID_ARG;
    if (level) {
        s_exio_out |= (uint8_t)(1u << bit);
    } else {
        s_exio_out &= (uint8_t) ~(1u << bit);
    }
    return exio_write(TCA9554_REG_OUTPUT, s_exio_out);
}

bool board_exio_get(int bit)
{
    if (!s_exio_ready || bit < 0 || bit > 7) return false;
    uint8_t reg = TCA9554_REG_INPUT, in = 0;
    if (i2c_master_transmit_receive(s_exio, &reg, 1, &in, 1, 100) != ESP_OK) return false;
    return in & (1u << bit);
}

void board_power_rails_off(void)
{
    // No switchable rails. Touch must stay powered: it is the wake source.
}

// ---------------------------------------------------------------- backlight

void board_backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = BOARD_GPIO_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&c);
}

void board_backlight_set(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * BL_MAX / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---------------------------------------------------------------- buttons

void board_boot_button_init(void)
{
    gpio_config_t i = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&i);
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(BOARD_GPIO_BOOT) == 0;
}
