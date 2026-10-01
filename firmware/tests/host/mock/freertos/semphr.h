#pragma once
#include "freertos/FreeRTOS.h"
typedef struct sem_t_ *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t s);
