#pragma once
#include <cstddef>
#include <cstdint>
using QueueHandle_t = void*;
using BaseType_t = int;
using TickType_t = std::uint32_t;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait);
BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned stack_bytes, void* argument,
                       unsigned priority, void* handle);
unsigned uxTaskGetStackHighWaterMark(void*);
void vTaskDelay(TickType_t);
void taskYIELD();
