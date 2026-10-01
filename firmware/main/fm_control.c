/*
 * FM 送信機制御: FPGA (MPX コア) と ADF4002、切替式ループフィルタ (FMTX Rev.B5 以降)。
 *
 * 周波数変更の手順 (FMTX hardware/kicad/TMUX1136_OPA2197_DEVICE_CHECK と同じ順序):
 *   1. RF OFF、MPX ミュート (FPGA とアナログの両方)。FPGA がミュートし終わるのを待つ
 *   2. FPGA へ N / Kv / KIDX / COMP_EN を書き、取り込まれたことをステータスで確認
 *      (MPX コアはミュート中にしか取り込まない)
 *   3. TRANSMIT → ACQUIRE: CP ハイインピーダンス、帰還 → 入力の順に切替、プリチャージ ON
 *   4. R、N (2.5mA)、CP ON。ロック検出が 50ms 続くまで待つ (最大 0.5 秒)
 *   5. C302 のプリチャージを 250ms 待つ (R306 1k × C302 22uF の 11 倍)
 *   6. ACQUIRE → TRANSMIT: CP ハイインピーダンス、入力 → 帰還の順に切替、プリチャージ OFF、
 *      N (625uA)、CP ON
 *   7. 0.5 秒待ち、最後の 100ms ロックしていることを確認
 * RF と MPX を出すのは fm_control_set_rf(true) だけ。
 */
#include "fm_control.h"
#include <string.h>
#include "adf4002.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define ADF_R                   100     /* REF 10MHz / 100 = PFD 100kHz */
/* CP → 反転型アクティブ PI → 反転 2 段 → VTUNE で正味 1 回反転するので、Kv が正の VCO では負極性。
 * 実機で要確認: 逆だと VTUNE が電源側に張り付き、ロック検出が上がらない */
#define ADF_PD_POSITIVE         false

#define FPGA_MUTE_TIMEOUT_MS    200     /* ソフトミュートのランプは 10.7ms */
#define LOCK_HOLD_MS            50
#define ACQ_TIMEOUT_MS          500
#define PRECHARGE_MS            250
#define TX_SETTLE_MS            500
#define TX_LOCK_HOLD_MS         100

/* FPGA レジスタ (pico_ctrl_regs.vhd) */
#define FPGA_REG_CTRL           0       /* bit0 COMP_EN, bit1 SOFT_MUTE */
#define FPGA_REG_KIDX           1
#define FPGA_REG_NCH            2
#define FPGA_REG_KVCAL          3
#define FPGA_CTRL_COMP_EN       0x1
#define FPGA_CTRL_SOFT_MUTE     0x2
/* ステータス語: [15] MUTED, [14] COMP 有効, [13:7] 有効 KIDX, [6] MMCM ロック, [4:0] = 00010 */
#define FPGA_ST_MUTED           0x8000
#define FPGA_ST_COMP_EN         0x4000
#define FPGA_ST_KIDX(st)        (((st) >> 7) & 0x7f)
#define FPGA_ST_ALIVE(st)       ((((st) & 0x1f) == 0x02) && ((st) & 0x40))

typedef struct {
    uint32_t frequency_khz;
    uint16_t kv_centi;
    uint8_t comp_en;
} fm_saved_t;

static const char *TAG = "fmctl";
static spi_device_handle_t s_fpga, s_adf;
static SemaphoreHandle_t s_lock;
static fm_status_t s = {.frequency_khz = 92000, .kv_centi = 1500, .comp_en = true};

static void set_error(const char *msg)
{
    strlcpy(s.error, msg, sizeof(s.error));
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- FPGA (16bit, モード 0)。FPGA 通信中は ADF_LE を Low に保つ ---- */
static esp_err_t fpga_xfer(uint16_t word, uint16_t *status)
{
    uint8_t tx[2] = {word >> 8, word & 0xff}, rx[2] = {0, 0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    gpio_set_level(BOARD_ADF_LE, 0);
    ESP_RETURN_ON_ERROR(spi_device_transmit(s_fpga, &t), TAG, "FPGA SPI");
    if (status) {
        *status = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return ESP_OK;
}

static esp_err_t fpga_write(uint8_t addr, uint16_t data)
{
    return fpga_xfer(((addr & 7) << 12) | (data & 0xfff), NULL);
}

static uint16_t fpga_status(void)
{
    uint16_t st = 0;
    fpga_xfer(0x8000, &st);
    s.fpga_status = st;
    return st;
}

/* ---- ADF4002 (24bit, MSB 先頭, SCK/MOSI は FPGA と共用, LE の立上りでラッチ) ---- */
static esp_err_t adf_write(uint32_t word)
{
    uint8_t b[3] = {word >> 16, word >> 8, word};
    spi_transaction_t t = {.length = 24, .tx_buffer = b};
    gpio_set_level(BOARD_ADF_LE, 0);
    ESP_RETURN_ON_ERROR(spi_device_transmit(s_adf, &t), TAG, "ADF SPI");
    gpio_set_level(BOARD_ADF_LE, 1);
    esp_rom_delay_us(2);
    gpio_set_level(BOARD_ADF_LE, 0);
    return ESP_OK;
}

static uint32_t adf_function(bool cp_three_state)
{
    /* 電流設定 1 = 625uA (TRANSMIT, G1=0), 電流設定 2 = 2.5mA (ACQUIRE, G1=1) */
    const adf4002_function_t f = {
        .setting1 = ADF_CP_0MA625, .setting2 = ADF_CP_2MA5, .pd_positive = ADF_PD_POSITIVE,
        .cp_three_state = cp_three_state, .muxout = ADF_MUX_DIGITAL_LOCK_DETECT,
    };
    return adf4002_function_latch(&f);
}

/* ロック検出が hold_ms のあいだ途切れずに High なら true */
static bool wait_lock(uint32_t hold_ms, uint32_t timeout_ms)
{
    uint32_t t0 = now_ms(), high = t0;
    for (;;) {
        uint32_t now = now_ms();
        if (!gpio_get_level(BOARD_ADF_MUXOUT)) {
            high = now;
        } else if (now - high >= hold_ms) {
            return true;
        }
        if (now - t0 >= timeout_ms) {
            return false;
        }
        vTaskDelay(1);
    }
}

static void mute_all(void)
{
    gpio_set_level(BOARD_FM_TX_EN, 0);
    gpio_set_level(BOARD_MPX_MUTE, 1);
    gpio_set_level(BOARD_MPX_RUN, 0);
    fpga_write(FPGA_REG_CTRL, FPGA_CTRL_SOFT_MUTE | (s.comp_en ? FPGA_CTRL_COMP_EN : 0));
    s.rf_on = false;
}

/* MPX コアはミュート中にしか KIDX / COMP_EN を取り込まない。MUTED を待ってから書き、取り込みを確認する */
static bool apply_fpga(uint16_t n)
{
    uint32_t t0 = now_ms();
    uint16_t st = fpga_status();
    if (!FPGA_ST_ALIVE(st)) {
        set_error("FPGA not ready");
        return false;
    }
    while (!(st & FPGA_ST_MUTED)) {
        if (now_ms() - t0 >= FPGA_MUTE_TIMEOUT_MS) {
            set_error("FPGA did not mute");
            return false;
        }
        vTaskDelay(1);
        st = fpga_status();
    }
    fpga_write(FPGA_REG_NCH, n);
    fpga_write(FPGA_REG_KVCAL, s.kv_centi);
    fpga_write(FPGA_REG_KIDX, s.kidx);
    fpga_write(FPGA_REG_CTRL, FPGA_CTRL_SOFT_MUTE | (s.comp_en ? FPGA_CTRL_COMP_EN : 0));
    vTaskDelay(1);
    st = fpga_status();
    if (FPGA_ST_KIDX(st) != s.kidx || !!(st & FPGA_ST_COMP_EN) != s.comp_en) {
        set_error("FPGA take-over failed");
        return false;
    }
    return true;
}

/* TRANSMIT → ACQUIRE: CP を止め、帰還 → 入力の順に切り替え、プリチャージを入れる */
static void loop_to_acquire(void)
{
    adf_write(adf_function(true));
    gpio_set_level(BOARD_LF_FB_SEL, 1);
    esp_rom_delay_us(10);
    gpio_set_level(BOARD_LF_INPUT_SEL, 1);
    esp_rom_delay_us(10);
    gpio_set_level(BOARD_LF_PRECHARGE, 1);
}

/* ACQUIRE → TRANSMIT: CP を止め、入力 → 帰還の順に切り替え、プリチャージを切り、625uA にする */
static void loop_to_transmit(uint16_t n)
{
    adf_write(adf_function(true));
    gpio_set_level(BOARD_LF_INPUT_SEL, 0);
    esp_rom_delay_us(2);
    gpio_set_level(BOARD_LF_FB_SEL, 0);
    esp_rom_delay_us(20);
    gpio_set_level(BOARD_LF_PRECHARGE, 0);
    adf_write(adf4002_n_latch(n, false));
    adf_write(adf_function(false));
}

static esp_err_t tune_locked(void)
{
    uint16_t n = s.frequency_khz / 100;
    s.error[0] = 0;
    s.kidx = fm_comp_kidx(n, s.kv_centi);
    mute_all();
    s.fpga_ok = apply_fpga(n);          /* 失敗しても PLL は合わせる。RF/MPX は出せないまま */

    loop_to_acquire();
    adf_write(adf4002_r_latch(ADF_R, true));
    adf_write(adf4002_n_latch(n, true));
    adf_write(adf_function(false));
    if (!wait_lock(LOCK_HOLD_MS, ACQ_TIMEOUT_MS)) {
        set_error("PLL did not lock (ACQUIRE)");
        s.state = FM_STATE_FAULT;
        s.pll_lock = false;
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(PRECHARGE_MS));

    loop_to_transmit(n);
    vTaskDelay(pdMS_TO_TICKS(TX_SETTLE_MS - TX_LOCK_HOLD_MS));
    if (!wait_lock(TX_LOCK_HOLD_MS, TX_LOCK_HOLD_MS + 200)) {
        set_error("PLL lost lock (TRANSMIT)");
        s.state = FM_STATE_FAULT;
        s.pll_lock = false;
        return ESP_ERR_TIMEOUT;
    }
    s.state = FM_STATE_TUNED;
    s.pll_lock = true;
    ESP_LOGI(TAG, "tuned %lu kHz, KIDX %u%s", (unsigned long)s.frequency_khz, s.kidx, s.fpga_ok ? "" : " (FPGA not configured)");
    return ESP_OK;
}

/* 10ms 周期: RF を出している間にロックが外れたら、すぐ RF と MPX を止める */
static void safety_poll(void)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;                         /* 同調中 */
    }
    bool lock = gpio_get_level(BOARD_ADF_MUXOUT);
    s.pll_lock = lock;
    if (!lock && s.state == FM_STATE_TUNED) {
        mute_all();
        s.state = FM_STATE_FAULT;
        set_error("PLL unlocked");
        ESP_LOGW(TAG, "PLL unlocked: RF and MPX off");
    }
    xSemaphoreGive(s_lock);
}

static void safety_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tune_locked();                      /* 保存済みのチャンネルに合わせる (RF は出さない) */
    xSemaphoreGive(s_lock);
    for (;;) {
        safety_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t fm_control_init(void)
{
    /* 先に安全側へ: RF OFF、MPX ミュート、ループは ACQUIRE、FPGA リセット解除 */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_ADF_LE) | (1ULL << BOARD_FM_TX_EN) | (1ULL << BOARD_FPGA_RST) |
                        (1ULL << BOARD_LF_INPUT_SEL) | (1ULL << BOARD_LF_FB_SEL) | (1ULL << BOARD_LF_PRECHARGE) |
                        (1ULL << BOARD_MPX_RUN) | (1ULL << BOARD_MPX_MUTE) | (1ULL << BOARD_ADF_CE),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(BOARD_FM_TX_EN, 0);
    gpio_set_level(BOARD_MPX_MUTE, 1);
    gpio_set_level(BOARD_MPX_RUN, 0);
    gpio_set_level(BOARD_ADF_LE, 0);
    gpio_set_level(BOARD_FPGA_RST, 0);
    gpio_set_level(BOARD_LF_FB_SEL, 1);
    gpio_set_level(BOARD_LF_INPUT_SEL, 1);
    gpio_set_level(BOARD_LF_PRECHARGE, 1);
    gpio_set_level(BOARD_ADF_CE, 1);
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {.pin_bit_mask = 1ULL << BOARD_ADF_MUXOUT, .mode = GPIO_MODE_INPUT, .pull_down_en = 1};
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SPI_MOSI, .miso_io_num = BOARD_SPI_MISO, .sclk_io_num = BOARD_SPI_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t fpga = {.clock_speed_hz = 1000000, .mode = 0, .spics_io_num = BOARD_FPGA_CS, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_SPI_HOST, &fpga, &s_fpga));
    spi_device_interface_config_t adf = {.clock_speed_hz = 1000000, .mode = 0, .spics_io_num = -1, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_SPI_HOST, &adf, &s_adf));

    nvs_handle_t h;
    if (nvs_open("fmctl", NVS_READONLY, &h) == ESP_OK) {
        fm_saved_t saved;
        size_t size = sizeof(saved);
        if (nvs_get_blob(h, "cfg1", &saved, &size) == ESP_OK && size == sizeof(saved) &&
            saved.frequency_khz >= 76000 && saved.frequency_khz <= 108000 && saved.frequency_khz % 100 == 0 &&
            saved.kv_centi >= 1000 && saved.kv_centi <= 2000) {
            s.frequency_khz = saved.frequency_khz;
            s.kv_centi = saved.kv_centi;
            s.comp_en = saved.comp_en;
        }
        nvs_close(h);
    }

    /* 初期化ラッチ方式 (データシート Rev. C 16 ページ)。CP は止めたまま */
    const adf4002_function_t f = {
        .setting1 = ADF_CP_0MA625, .setting2 = ADF_CP_2MA5, .pd_positive = ADF_PD_POSITIVE,
        .cp_three_state = true, .muxout = ADF_MUX_DIGITAL_LOCK_DETECT,
    };
    adf_write(adf4002_init_latch(&f));
    adf_write(adf_function(true));
    adf_write(adf4002_r_latch(ADF_R, true));
    adf_write(adf4002_n_latch(s.frequency_khz / 100, true));

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    xTaskCreate(safety_task, "fm_safe", 4096, NULL, 8, NULL);
    return ESP_OK;
}

esp_err_t fm_control_set(uint32_t khz, uint16_t kv, bool comp)
{
    ESP_RETURN_ON_FALSE(khz >= 76000 && khz <= 108000 && khz % 100 == 0, ESP_ERR_INVALID_ARG, TAG, "frequency");
    ESP_RETURN_ON_FALSE(kv >= 1000 && kv <= 2000, ESP_ERR_INVALID_ARG, TAG, "Kv");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.frequency_khz = khz;
    s.kv_centi = kv;
    s.comp_en = comp;
    esp_err_t e = tune_locked();
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t fm_control_set_rf(bool on)
{
    esp_err_t e = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!on) {
        mute_all();
    } else {
#if CONFIG_FM_ALLOW_RF_OUTPUT
        if (s.state != FM_STATE_TUNED || !gpio_get_level(BOARD_ADF_MUXOUT)) {
            set_error("PLL not locked");
            e = ESP_ERR_INVALID_STATE;
        } else if (!s.fpga_ok) {
            set_error("FPGA not configured");
            e = ESP_ERR_INVALID_STATE;
        } else {
            gpio_set_level(BOARD_FM_TX_EN, 1);
            gpio_set_level(BOARD_MPX_RUN, 1);
            fpga_write(FPGA_REG_CTRL, s.comp_en ? FPGA_CTRL_COMP_EN : 0);
            gpio_set_level(BOARD_MPX_MUTE, 0);
            s.rf_on = true;
        }
#else
        set_error("RF output locked out");
        e = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    xSemaphoreGive(s_lock);
    return e;
}

void fm_control_get(fm_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.pll_lock = gpio_get_level(BOARD_ADF_MUXOUT);
    fpga_status();
    *out = s;
    xSemaphoreGive(s_lock);
}

esp_err_t fm_control_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("fmctl", NVS_READWRITE, &h), TAG, "nvs");
    fm_saved_t saved = {.frequency_khz = s.frequency_khz, .kv_centi = s.kv_centi, .comp_en = s.comp_en};
    esp_err_t e = nvs_set_blob(h, "cfg1", &saved, sizeof(saved));
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}
