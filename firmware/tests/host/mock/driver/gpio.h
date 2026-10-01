#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
enum { GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8,
       GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16,
       GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21 };
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en; int pull_down_en; int intr_type; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *c);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
