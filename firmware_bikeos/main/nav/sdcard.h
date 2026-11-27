#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_MOUNT "/sdcard"

// Optional map card: 1-bit SDMMC, FAT32/MBR. Never formats.
// Must run after lcd_init() and before display_init() (D3 shares GPIO21 with
// the panel's CS).
esp_err_t sdcard_init(void);
bool sdcard_ok(void);

// bike-os: what Settings > About shows of the card.
typedef struct {
    uint64_t bytes;     // capacity
    bool sdhc;          // SDHC/SDXC (else SDSC)
    int khz;            // bus clock it mounted at
    char name[8];       // product name from the CID
} sdcard_info_t;
bool sdcard_info(sdcard_info_t *out);
