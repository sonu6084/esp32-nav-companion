/*
 * Settings > About: the board, its memory and storage in use, its sensors and the
 * firmware. Text only; settings_app.c lays it out and refreshes it every second.
 *
 * The QMI8658 (accelerometer + gyro, I2C0 0x6B) is not used by any app. About switches
 * it on while it is open (about_open) to show it working, and off again (about_close),
 * so it costs no power otherwise. GUI task only: the I2C driver serialises the bus it
 * shares with the touch controller, the expander and the RTC.
 */
#include "about.h"

#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "sdkconfig.h"

#include "audio.h"
#include "board.h"
#include "ride.h"
#include "rtc.h"
#include "sdcard.h"
#include "settings.h"
#include "sys.h"

#define BIKEOS_VER "0.10.1"

#define QMI_ADDR_A    0x6B
#define QMI_ADDR_B    0x6A
#define QMI_WHO_AM_I  0x00     /* reads 0x05 */
#define QMI_CTRL1     0x02     /* bit 6: address auto-increment */
#define QMI_CTRL2     0x03     /* accelerometer: +-2 g, 125 Hz */
#define QMI_CTRL3     0x04     /* gyroscope: +-256 dps, 112 Hz */
#define QMI_CTRL7     0x08     /* bit 0 accel on, bit 1 gyro on */
#define QMI_TEMP_L    0x33     /* then TEMP_H, AX..GZ (little-endian) */

static i2c_master_dev_handle_t s_qmi;
static bool s_qmi_found, s_qmi_on;
static temperature_sensor_handle_t s_tsens;

/* ------------------------------ helpers ------------------------------ */
static void kb(char *out, size_t n, uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024) snprintf(out, n, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024) snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024));
    else snprintf(out, n, "%u KB", (unsigned)(bytes / 1024));
}

static bool qmi_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_qmi, b, 2, 50) == ESP_OK;
}

static bool qmi_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_qmi, &reg, 1, out, n, 50) == ESP_OK;
}

static void qmi_attach(void)
{
    if (s_qmi) return;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return;
    const uint8_t addrs[2] = {QMI_ADDR_A, QMI_ADDR_B};
    for (int i = 0; i < 2 && !s_qmi; i++) {
        if (i2c_master_probe(bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(bus, &dc, &s_qmi) != ESP_OK) { s_qmi = NULL; continue; }
        uint8_t who = 0;
        s_qmi_found = qmi_read(QMI_WHO_AM_I, &who, 1) && who == 0x05;
        if (!s_qmi_found) { i2c_master_bus_rm_device(s_qmi); s_qmi = NULL; }
    }
}

/* ------------------------------ public ------------------------------ */
void about_open(void)
{
    qmi_attach();
    if (s_qmi_found && !s_qmi_on) {
        s_qmi_on = qmi_write(QMI_CTRL1, 0x40) &&        /* auto-increment, little-endian */
                   qmi_write(QMI_CTRL2, 0x06) &&        /* +-2 g, 125 Hz */
                   qmi_write(QMI_CTRL3, 0x46) &&        /* +-256 dps (gFS 100), 112 Hz */
                   qmi_write(QMI_CTRL7, 0x03);          /* accel + gyro on */
    }
    if (!s_tsens) {
        temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tc, &s_tsens) != ESP_OK) s_tsens = NULL;
    }
    if (s_tsens) temperature_sensor_enable(s_tsens);
}

void about_close(void)
{
    if (s_qmi_on) { qmi_write(QMI_CTRL7, 0x00); s_qmi_on = false; }   /* power down */
    if (s_tsens) temperature_sensor_disable(s_tsens);
}

void about_board(char *out, size_t n)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(out, n,
             "Waveshare ESP32-S3-Touch-LCD-1.46\n"
             "ESP32-S3 rev %d.%d, %d cores @ %d MHz\n"
             "Flash %u MB, PSRAM %u MB\n"
             "Display 412 x 412 round, SPD2010\n"
             "Bluetooth LE %02X:%02X:%02X:%02X:%02X:%02X",
             ci.revision / 100, ci.revision % 100, ci.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)(flash / (1024 * 1024)), (unsigned)(esp_psram_get_size() / (1024 * 1024)),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void about_usage(char *out, size_t n)
{
    char a[16], b[16], c[16], d[16];
    size_t o = 0;
    kb(a, sizeof a, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    kb(b, sizeof b, heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    kb(c, sizeof c, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    kb(d, sizeof d, heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    o += snprintf(out + o, n - o, "RAM free %s of %s\nPSRAM free %s of %s\n", a, b, c, d);

    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p) {
        esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
        esp_image_metadata_t md;
        kb(b, sizeof b, p->size);
        if (esp_image_get_metadata(&pos, &md) == ESP_OK) {
            kb(a, sizeof a, md.image_len);
            o += snprintf(out + o, n - o, "App %s of %s\n", a, b);
        } else {
            o += snprintf(out + o, n - o, "App partition %s\n", b);
        }
    }

    uint64_t tot = 0, fr = 0;
    if (ride_storage_ok() && esp_vfs_fat_info("/rides", &tot, &fr) == ESP_OK) {
        kb(a, sizeof a, fr);
        kb(b, sizeof b, tot);
        o += snprintf(out + o, n - o, "Rides %d saved, %s free of %s\n", ride_count(), a, b);
    } else {
        o += snprintf(out + o, n - o, "Rides storage missing\n");
    }

    sdcard_info_t sd;
    if (sdcard_info(&sd)) {
        kb(b, sizeof b, sd.bytes);
        o += snprintf(out + o, n - o, "SD %s %s, %s", sd.name, sd.sdhc ? "SDHC/XC" : "SDSC", b);
        if (esp_vfs_fat_info(SD_MOUNT, &tot, &fr) == ESP_OK) {
            kb(a, sizeof a, fr);
            o += snprintf(out + o, n - o, ", %s free", a);
        }
        o += snprintf(out + o, n - o, ", %d MHz", sd.khz / 1000);
    } else {
        o += snprintf(out + o, n - o, "SD card: none");
    }
}

void about_sensors(char *out, size_t n)
{
    size_t o = 0;
    int pct = sys_battery_pct();
    if (pct < 0) o += snprintf(out + o, n - o, "Battery: USB power (%.2f V)\n", sys_battery_volts());
    else o += snprintf(out + o, n - o, "Battery %d%%, %.2f V\n", pct, sys_battery_volts());

    datetime_t t;
    if (pcf_read(&t) == ESP_OK)
        o += snprintf(out + o, n - o, "RTC PCF85063 %02d:%02d:%02d\n", t.hour, t.minute, t.second);
    else
        o += snprintf(out + o, n - o, "RTC PCF85063 not answering\n");

    uint8_t r[14];
    if (s_qmi_on && qmi_read(QMI_TEMP_L, r, sizeof r)) {
        int16_t v[7];
        for (int i = 0; i < 7; i++) v[i] = (int16_t)(r[2 * i] | (r[2 * i + 1] << 8));
        o += snprintf(out + o, n - o,
                      "IMU QMI8658 %.1f C\n"
                      " accel %+.2f %+.2f %+.2f g\n"
                      " gyro %+.0f %+.0f %+.0f dps\n",
                      v[0] / 256.0f,
                      v[1] / 16384.0f, v[2] / 16384.0f, v[3] / 16384.0f,
                      v[4] / 128.0f, v[5] / 128.0f, v[6] / 128.0f);
    } else {
        o += snprintf(out + o, n - o, "IMU QMI8658 %s\n", s_qmi_found ? "found, no data" : "not found");
    }

    float c;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK)
        o += snprintf(out + o, n - o, "Chip %.1f C\n", c);

    o += snprintf(out + o, n - o, "Touch SPD2010, speaker %s\n", audio_ok() ? "ok" : "off");
    snprintf(out + o, n - o, "GPS, speed, heading: from the phone");
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "wake from sleep";
    case ESP_RST_BROWNOUT: return "low power";
    case ESP_RST_USB:      return "USB";
    default:               return "other";
    }
}

void about_firmware(char *out, size_t n)
{
    const esp_app_desc_t *d = esp_app_get_description();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(out, n,
             "bike-os %s\nESP-IDF %s\nBuilt %s\nUp %luh %02lum, last start: %s\nName %s",
             BIKEOS_VER, esp_get_idf_version(), d->date,
             (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), reset_reason(),
             settings_device_name());
}
