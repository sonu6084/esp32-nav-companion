#pragma once
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERROR_CHECK(x) do { esp_err_t _e = (x); if (_e) { fprintf(stderr, "ESP_ERROR_CHECK failed %d at %s:%d\n", _e, __FILE__, __LINE__); } } while (0)
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
