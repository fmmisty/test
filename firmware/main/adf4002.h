#pragma once

/*
 * ADF4002 のラッチ語と、FPGA の PLL 逆補償バンク番号 (KIDX)。
 * ビット配置は Analog Devices ADF4002 データシート Rev. C 図 15-19 による。
 * ハードウェアに依存しないので、PC 上で単体テストできる (tests/host/test_adf4002.c)。
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* チャージポンプ電流コード (RSET = 5.1kΩ のとき) */
enum {
    ADF_CP_0MA625 = 0, ADF_CP_1MA25 = 1, ADF_CP_1MA875 = 2, ADF_CP_2MA5 = 3,
    ADF_CP_3MA125 = 4, ADF_CP_3MA75 = 5, ADF_CP_4MA375 = 6, ADF_CP_5MA = 7,
};

/* MUXOUT コード (M3..M1) */
enum { ADF_MUX_THREE_STATE = 0, ADF_MUX_DIGITAL_LOCK_DETECT = 1 };

/* R カウンタラッチ (C2,C1 = 0,0): DB20 LDP (1 = PFD 5 周期), DB17..16 ABP (00 = 2.9ns), DB15..2 R。
 * DB22..21 は予約で 0 のままにする */
static inline uint32_t adf4002_r_latch(uint16_t r, bool ldp_five_cycles)
{
    return ((uint32_t)(ldp_five_cycles ? 1u : 0u) << 20) | ((uint32_t)(r & 0x3FFFu) << 2);
}

/* N カウンタラッチ (C2,C1 = 0,1): DB21 G1 (ファストロック無効時は 0 = 電流設定 1, 1 = 電流設定 2), DB20..8 N */
static inline uint32_t adf4002_n_latch(uint16_t n, bool cp_gain_setting2)
{
    return ((uint32_t)(cp_gain_setting2 ? 1u : 0u) << 21) | ((uint32_t)(n & 0x1FFFu) << 8) | 0x1u;
}

typedef struct {
    uint8_t setting1;       /* CPI3..1 (DB17..15): G1 = 0 のとき使う */
    uint8_t setting2;       /* CPI6..4 (DB20..18): G1 = 1 のとき使う */
    bool pd_positive;       /* F2 (DB7): 1 = 正極性 */
    bool cp_three_state;    /* F3 (DB8): 1 = CP 出力をハイインピーダンス */
    uint8_t muxout;         /* M3..M1 (DB6..4) */
} adf4002_function_t;

/* ファンクションラッチ / 初期化ラッチ共通部。ファストロック・パワーダウン・カウンタリセットは使わない */
static inline uint32_t adf4002_function_bits(const adf4002_function_t *f)
{
    return ((uint32_t)(f->setting2 & 7u) << 18) | ((uint32_t)(f->setting1 & 7u) << 15) |
           ((uint32_t)(f->cp_three_state ? 1u : 0u) << 8) | ((uint32_t)(f->pd_positive ? 1u : 0u) << 7) |
           ((uint32_t)(f->muxout & 7u) << 4);
}
static inline uint32_t adf4002_function_latch(const adf4002_function_t *f) { return adf4002_function_bits(f) | 0x2u; }
static inline uint32_t adf4002_init_latch(const adf4002_function_t *f) { return adf4002_function_bits(f) | 0x3u; }

/* KIDX = round(127 * ln(r / RMIN) / ln(RMAX / RMIN)),  r = Kv[MHz/V] / N,  RMIN = 10/1080, RMAX = 20/760 */
static inline uint8_t fm_comp_kidx(uint16_t n, uint16_t kv_centi)
{
    const double rmin = 10.0 / 1080.0, rmax = 20.0 / 760.0;
    if (n == 0) {
        return 0;
    }
    double k = 127.0 * log(((double)kv_centi / 100.0) / (double)n / rmin) / log(rmax / rmin);
    if (k < 0.0) {
        k = 0.0;
    }
    if (k > 127.0) {
        k = 127.0;
    }
    return (uint8_t)(k + 0.5);
}
