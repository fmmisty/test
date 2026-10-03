#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t frequency_khz; uint16_t kv_centi; uint8_t kidx; bool comp_en; bool pll_lock; bool rf_on; uint16_t fpga_status; } fm_status_t;
esp_err_t fm_control_init(void);
esp_err_t fm_control_set(uint32_t frequency_khz,uint16_t kv_centi,bool comp_en);
esp_err_t fm_control_set_rf(bool on);
void fm_control_get(fm_status_t *out);
esp_err_t fm_control_save(void);
