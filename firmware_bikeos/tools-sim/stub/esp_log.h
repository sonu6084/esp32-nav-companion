#pragma once
#include <stdio.h>
#define ESP_LOGW(t, ...) do { printf("  [%s] ", t); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGE ESP_LOGW
#define ESP_LOGI(t, ...) do { } while (0)
#define ESP_LOGD(t, ...) do { } while (0)
