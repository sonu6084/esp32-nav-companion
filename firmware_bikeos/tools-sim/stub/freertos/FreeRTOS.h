#pragma once
/* Host stub: single-threaded FreeRTOS subset for the simulator. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
typedef struct sim_queue *QueueHandle_t;
typedef struct sim_queue *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) (void)(m)
#define portEXIT_CRITICAL(m) (void)(m)
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t st, void *arg, UBaseType_t pr, TaskHandle_t *h, BaseType_t core);
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
#define taskENTER_CRITICAL(m) (void)(m)
#define taskEXIT_CRITICAL(m) (void)(m)
