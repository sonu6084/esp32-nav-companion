#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_MAC_BT 2
static inline esp_err_t esp_read_mac(uint8_t *m, int t) { (void)t; const uint8_t v[6] = {0x24,0x0A,0xC4,0x12,0x51,0x46}; for (int i=0;i<6;i++) m[i]=v[i]; return 0; }
