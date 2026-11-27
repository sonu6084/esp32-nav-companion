#pragma once
/* PCF85063 RTC (0x51) on the shared I2C bus from nav/board.c (i2c_master driver). */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t year;
    uint8_t month, day, dotw, hour, minute, second;
} datetime_t;

esp_err_t pcf_init(void);                 /* after board_i2c_init() */
esp_err_t pcf_read(datetime_t *t);
esp_err_t pcf_write(const datetime_t *t);
