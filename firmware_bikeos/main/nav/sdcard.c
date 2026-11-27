#include "sdcard.h"
#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

// Waveshare 1.46: 1-bit SDMMC (vendor SD_MMC.h). The card's D3 is wired to
// GPIO21, which is also the panel's QSPI CS. CS idles high, which is exactly
// what a card wants on D3 to stay in SD (not SPI) mode, so this works as long
// as the mount runs while no panel transfer is in flight (i.e. before
// display_init(), which is where app_main calls it).
//
// The SD card is OPTIONAL. The nav display ("minimal" mode) draws streets the
// phone streams over BLE (NAV_ROADS), so no card is needed. A card only adds
// the raster street map (/sdcard/street_map/{z}/{x}/{y}.bin).
#define SD_CLK 14
#define SD_CMD 17
#define SD_D0 16

static bool s_ok;

static esp_err_t try_mount(int khz)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,  // never touch the user's map card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = -1;
    slot.d2 = -1;
    slot.d3 = -1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount, &card);
}

esp_err_t sdcard_init(void)
{
    esp_err_t err = try_mount(SDMMC_FREQ_HIGHSPEED);
    if (err != ESP_OK) {
        // ESP_ERR_TIMEOUT / ESP_FAIL here just means "no card": not an error
        // for this board, where the card is optional.
        ESP_LOGW(TAG, "mount @40MHz failed (%s), retry @20MHz", esp_err_to_name(err));
        err = try_mount(SDMMC_FREQ_DEFAULT);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD map card (%s): BLE-only (minimal) display", esp_err_to_name(err));
        return err;
    }
    s_ok = true;
    return ESP_OK;
}

bool sdcard_ok(void)
{
    return s_ok;
}
