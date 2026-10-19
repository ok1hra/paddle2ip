// Host mock of the few Arduino / FreeRTOS / LEDC symbols keyer.cpp uses.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
typedef void* TaskHandle_t; typedef uint32_t TickType_t;
struct IPAddress { uint32_t a; };
struct portMUX_TYPE { int x; };
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) (void)(m)
#define portEXIT_CRITICAL(m) (void)(m)
#define configMAX_PRIORITIES 25
static inline void xTaskNotifyGive(TaskHandle_t) {}
extern void ledcWrite(uint8_t ch, uint32_t duty);
static inline uint32_t ledcSetup(uint8_t, uint32_t, uint8_t) { return 0; }
static inline void ledcAttachPin(uint8_t, uint8_t) {}
static inline uint32_t ledcChangeFrequency(uint8_t, uint32_t, uint8_t) { return 0; }
static inline void vTaskDelayUntil(TickType_t*, int) {}
static inline TickType_t xTaskGetTickCount() { return 0; }
static inline void xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) {}
