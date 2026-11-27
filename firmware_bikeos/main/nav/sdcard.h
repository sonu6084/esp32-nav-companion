#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define SD_MOUNT "/sdcard"

// Optional map card: 1-bit SDMMC, FAT32/MBR. Never formats.
// Must run after lcd_init() and before display_init() (D3 shares GPIO21 with
// the panel's CS).
esp_err_t sdcard_init(void);
bool sdcard_ok(void);
