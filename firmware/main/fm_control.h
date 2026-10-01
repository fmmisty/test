#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    FM_STATE_BOOT = 0,      /* 起動直後。まだ一度も同調していない */
    FM_STATE_TUNED,         /* TRANSMIT ループ (8Hz) でロック中 */
    FM_STATE_FAULT,         /* ロックできない / ロックが外れた */
} fm_state_t;

typedef struct {
    uint32_t frequency_khz;
    uint16_t kv_centi;
    uint8_t kidx;
    bool comp_en;
    bool pll_lock;
    bool rf_on;
    uint16_t fpga_status;
    fm_state_t state;
    bool fpga_ok;           /* FPGA が N/Kv/KIDX/COMP を取り込んだことを確認済み */
    char error[40];         /* 最後の失敗理由 (成功時は空) */
} fm_status_t;

esp_err_t fm_control_init(void);
/* 周波数変更。MPX と RF を止め、ACQUIRE でロック → プリチャージ → TRANSMIT に切り替える (約 0.9 秒)。
 * 成功しても RF と MPX は止めたまま。出すのは fm_control_set_rf(true) */
esp_err_t fm_control_set(uint32_t frequency_khz, uint16_t kv_centi, bool comp_en);
esp_err_t fm_control_set_rf(bool on);
void fm_control_get(fm_status_t *out);
esp_err_t fm_control_save(void);
