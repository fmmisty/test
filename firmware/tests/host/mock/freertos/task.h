#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *handle);
