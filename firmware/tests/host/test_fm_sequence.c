/* Host simulation of main/fm_control.c against models of the ADF4002 (shared SCK/MOSI, LE latch,
 * lock detect) and the FPGA register block (pico_ctrl_regs.vhd + the MPX core take-over rule).
 * Build twice: without and with -DCONFIG_FM_ALLOW_RF_OUTPUT=1. Virtual time; no hardware needed. */
#include <stdio.h>
#include <string.h>
#include "../../main/fm_control.c"

/* ------------------------------------------------------------------ simulator */
enum { EV_PIN, EV_ADF };
typedef struct { uint64_t us; int kind; int pin; uint32_t value; } event_t;

static struct {
    uint64_t us;
    int out[64];
    event_t log[4096]; int nlog;
    /* scenario */
    int fpga_present; int lock_delay_ms; int force_unlock;
    /* ADF4002 */
    uint32_t shift; uint32_t words[256]; int nwords;
    int cp_on; uint64_t cp_on_since; int adf_n, adf_g1;
    /* FPGA */
    int mute_req; uint64_t mute_since;
    uint16_t r_ctrl, r_kidx, r_n, r_kv, akidx; int aen;
    /* NVS */
    unsigned char blob[64]; size_t blob_size;
    int mutex_taken;
} sim;

static void sim_reset(void)
{
    memset(&sim, 0, sizeof(sim));
    sim.fpga_present = 1; sim.lock_delay_ms = 30; sim.mute_req = 1; sim.r_ctrl = 2;
    memset(&s, 0, sizeof(s));
    s.frequency_khz = 92000; s.kv_centi = 1500; s.comp_en = true;
}

static void logev(int kind, int pin, uint32_t value)
{
    if (sim.nlog < 4096) sim.log[sim.nlog++] = (event_t){sim.us, kind, pin, value};
}

static int fpga_muted(void)                  /* soft-mute ramp: 10.7 ms after the request */
{
    int req = sim.out[BOARD_MPX_MUTE] || (sim.r_ctrl & 2);
    if (req && !sim.mute_req) sim.mute_since = sim.us;
    sim.mute_req = req;
    return req && sim.us - sim.mute_since >= 10700;
}
static void fpga_takeover(void) { if (fpga_muted()) { sim.akidx = sim.r_kidx; sim.aen = sim.r_ctrl & 1; } }
static void advance(uint64_t us) { sim.us += us; fpga_takeover(); }

static int locked(void)
{
    if (sim.force_unlock || sim.lock_delay_ms < 0 || !sim.out[BOARD_ADF_CE]) return 0;
    return sim.cp_on_since && sim.us - sim.cp_on_since >= (uint64_t)sim.lock_delay_ms * 1000;
}

static void adf_latch(uint32_t w)
{
    sim.words[sim.nwords++ & 255] = w; logev(EV_ADF, -1, w);
    switch (w & 3) {
    case 1: {
        int n = (w >> 8) & 0x1FFF;
        if (n != sim.adf_n) sim.cp_on_since = sim.cp_on ? sim.us : 0;      /* new channel: re-acquire */
        sim.adf_n = n; sim.adf_g1 = (w >> 21) & 1;
        break;
    }
    case 2: case 3: {
        int on = !((w >> 8) & 1);
        if (on && !sim.cp_on && !sim.cp_on_since) sim.cp_on_since = sim.us;
        sim.cp_on = on;
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ ESP-IDF mocks */
esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    fpga_takeover();
    level = level ? 1 : 0;
    if (pin == BOARD_ADF_LE && level && !sim.out[pin]) adf_latch(sim.shift & 0xFFFFFF);
    if (sim.out[pin] != (int)level) logev(EV_PIN, pin, level);
    sim.out[pin] = level;
    fpga_muted();
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { fpga_takeover(); return pin == BOARD_ADF_MUXOUT ? locked() : sim.out[pin]; }
esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d) { (void)h; (void)c; (void)d; return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *h)
{
    (void)host;
    *h = (spi_device_handle_t)(cfg->spics_io_num >= 0 ? 1L : 2L);          /* 1 = FPGA (has CS), 2 = ADF4002 */
    return ESP_OK;
}
esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    const uint8_t *tx = t->tx_buffer;
    fpga_takeover();
    for (size_t i = 0; i < t->length / 8; i++) sim.shift = (sim.shift << 8) | tx[i];   /* ADF4002 sees every clock */
    if (h == (spi_device_handle_t)1L) {
        uint16_t w = (uint16_t)((tx[0] << 8) | tx[1]);
        uint16_t st = (uint16_t)((fpga_muted() << 15) | (sim.aen << 14) | (sim.akidx << 7) | (1 << 6) | (sim.out[BOARD_MPX_MUTE] << 5) | 2);
        if (!sim.fpga_present) st = 0xFFFF;                                 /* MISO floating high */
        if (t->rx_buffer) { ((uint8_t *)t->rx_buffer)[0] = st >> 8; ((uint8_t *)t->rx_buffer)[1] = st & 0xff; }
        if (sim.fpga_present && !(w & 0x8000)) {
            uint16_t d = w & 0x0FFF;
            switch ((w >> 12) & 7) {
            case 0: sim.r_ctrl = d & 3; break;
            case 1: sim.r_kidx = d & 0x7F; break;
            case 2: sim.r_n = d & 0x7FF; break;
            case 3: sim.r_kv = d; break;
            }
        }
    }
    advance(t->length);                                                     /* 1 MHz clock */
    return ESP_OK;
}
void esp_rom_delay_us(uint32_t us) { advance(us); }
int64_t esp_timer_get_time(void) { return (int64_t)sim.us; }
void vTaskDelay(TickType_t ticks) { advance((uint64_t)(ticks ? ticks : 1) * 10000); }
int xTaskCreate(void (*fn)(void *), const char *n, int st, void *a, int p, void *h) { (void)fn; (void)n; (void)st; (void)a; (void)p; (void)h; return pdTRUE; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1L; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t w) { (void)m; (void)w; if (sim.mutex_taken) return pdFALSE; sim.mutex_taken = 1; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; sim.mutex_taken = 0; return pdTRUE; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *size)
{
    (void)h; (void)k;
    if (!sim.blob_size || *size < sim.blob_size) return ESP_FAIL;
    memcpy(out, sim.blob, sim.blob_size); *size = sim.blob_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *in, size_t size) { (void)h; (void)k; memcpy(sim.blob, in, size); sim.blob_size = size; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ------------------------------------------------------------------ helpers */
static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static uint64_t t_pin(int pin, int level, int from)
{
    for (int i = from; i < sim.nlog; i++) if (sim.log[i].kind == EV_PIN && sim.log[i].pin == pin && (int)sim.log[i].value == level) return sim.log[i].us;
    return UINT64_MAX;
}
static int i_word(uint32_t w, int from)
{
    for (int i = from; i < sim.nlog; i++) if (sim.log[i].kind == EV_ADF && sim.log[i].value == w) return i;
    return -1;
}
#define W_INIT 0x0C0113u
#define W_3ST  0x0C0112u
#define W_ON   0x0C0012u
#define W_R    0x100190u
static uint32_t w_n(int n, int g1) { return (g1 ? 0x200000u : 0u) | ((uint32_t)n << 8) | 1u; }
static void boot(void) { fm_control_init(); xSemaphoreTake(s_lock, portMAX_DELAY); tune_locked(); xSemaphoreGive(s_lock); }

int main(void)
{
    fm_status_t st;

    /* -------------------------------------------------------- A: boot and first tune (92.0 MHz) */
    sim_reset(); boot();
    check(s.state == FM_STATE_TUNED && s.pll_lock && s.fpga_ok, "A: TUNED, locked, FPGA configured");
    check(sim.words[0] == W_INIT && sim.words[1] == W_3ST && sim.words[2] == W_R && sim.words[3] == w_n(920, 1),
          "A: init sequence = init, function, R, N");
    check(sim.out[BOARD_FPGA_RST] == 0 && sim.out[BOARD_ADF_CE] == 1, "A: FPGA reset released (low), ADF4002 enabled");
    int i_on = i_word(W_ON, 0), i_3st = i_word(W_3ST, i_on);
    uint64_t t_in = t_pin(BOARD_LF_INPUT_SEL, 0, 0), t_fb = t_pin(BOARD_LF_FB_SEL, 0, 0), t_pre = t_pin(BOARD_LF_PRECHARGE, 0, 0);
    check(i_on >= 0 && i_3st >= 0 && sim.log[i_3st].us < t_in, "A: CP three-state before the input switch");
    check(t_in < t_fb && t_fb - t_in >= 1, "A: input path switched >=1 us before feedback");
    check(t_fb < t_pre && t_pre - t_fb >= 10, "A: precharge opened >=10 us after feedback");
    int i_ntx = i_word(w_n(920, 0), i_3st), i_on2 = i_word(W_ON, i_3st);
    check(i_ntx >= 0 && i_on2 > i_ntx && sim.log[i_ntx].us > t_pre, "A: 625 uA (G1=0) written, then CP re-enabled");
    check(t_in - sim.log[i_on].us >= (uint64_t)(sim.lock_delay_ms + LOCK_HOLD_MS + PRECHARGE_MS) * 1000 - 20000,
          "A: lock hold + 250 ms precharge before the transfer");
    check(sim.us - sim.log[i_on2].us >= (uint64_t)TX_SETTLE_MS * 1000 - 20000, "A: >=0.5 s settle in TRANSMIT");
    check(sim.r_n == 920 && sim.r_kv == 1500 && sim.r_kidx == 69 && sim.r_ctrl == 3, "A: FPGA N/Kv/KIDX=69/COMP_EN written, soft mute kept");
    check(sim.akidx == 69 && sim.aen, "A: FPGA took the values over while muted");
    check(t_pin(BOARD_FM_TX_EN, 1, 0) == UINT64_MAX && t_pin(BOARD_MPX_RUN, 1, 0) == UINT64_MAX && sim.out[BOARD_MPX_MUTE] == 1,
          "A: RF and MPX stay off after tuning");
    printf("     tune time %.0f ms\n", sim.us / 1000.0);

    /* -------------------------------------------------------- RF on / off */
#if CONFIG_FM_ALLOW_RF_OUTPUT
    check(fm_control_set_rf(true) == ESP_OK && s.rf_on, "RF: on accepted when tuned");
    check(sim.out[BOARD_FM_TX_EN] == 1 && sim.out[BOARD_MPX_RUN] == 1 && sim.out[BOARD_MPX_MUTE] == 0 && sim.r_ctrl == 1,
          "RF: TX_EN, MPX_RUN high, MPX un-muted, COMP_EN kept");
    /* unlock while transmitting */
    sim.force_unlock = 1; safety_poll();
    check(s.state == FM_STATE_FAULT && !s.rf_on && sim.out[BOARD_FM_TX_EN] == 0 && sim.out[BOARD_MPX_RUN] == 0 &&
          sim.out[BOARD_MPX_MUTE] == 1 && (sim.r_ctrl & 2), "RF: unlock -> RF and MPX off within one safety poll");
    sim.force_unlock = 0;
    check(fm_control_set_rf(true) == ESP_ERR_INVALID_STATE, "RF: on refused in FAULT until retuned");
    check(fm_control_set(92000, 1500, true) == ESP_OK && fm_control_set_rf(true) == ESP_OK, "RF: retune then on works");
#else
    check(fm_control_set_rf(true) == ESP_ERR_NOT_SUPPORTED && sim.out[BOARD_FM_TX_EN] == 0 && !s.rf_on,
          "RF: on refused while CONFIG_FM_ALLOW_RF_OUTPUT is off");
#endif
    check(fm_control_set_rf(false) == ESP_OK && sim.out[BOARD_FM_TX_EN] == 0 && sim.out[BOARD_MPX_MUTE] == 1, "RF: off always works");

    /* -------------------------------------------------------- A2: channel change to 76.0 MHz, Kv 20 */
    int mark = sim.nlog;
    check(fm_control_set(76000, 2000, true) == ESP_OK, "A2: set 76.0 MHz");
    check(t_pin(BOARD_LF_FB_SEL, 1, mark) < t_pin(BOARD_LF_INPUT_SEL, 1, mark), "A2: TRANSMIT->ACQUIRE switches feedback first");
    check(i_word(w_n(760, 1), mark) >= 0 && sim.adf_n == 760 && !sim.adf_g1, "A2: N=760 acquired at 2.5 mA, ends at 625 uA");
    check(sim.r_kidx == 127 && sim.akidx == 127 && sim.r_n == 760 && sim.r_kv == 2000, "A2: KIDX=127 taken over for Kv20/N760");
    check(fm_control_set(75900, 1500, true) == ESP_ERR_INVALID_ARG && fm_control_set(92050, 1500, true) == ESP_ERR_INVALID_ARG &&
          fm_control_set(92000, 900, true) == ESP_ERR_INVALID_ARG, "A2: out-of-range requests rejected");
    fm_control_get(&st);
    check(st.frequency_khz == 76000 && st.state == FM_STATE_TUNED, "A2: rejected requests leave the channel unchanged");

    /* -------------------------------------------------------- save / load */
    check(fm_control_save() == ESP_OK, "NVS: save");
    unsigned char keep[64]; size_t keep_size = sim.blob_size; memcpy(keep, sim.blob, sizeof(keep));
    sim_reset(); memcpy(sim.blob, keep, sizeof(keep)); sim.blob_size = keep_size; boot();
    check(s.frequency_khz == 76000 && s.kv_centi == 2000 && sim.adf_n == 760 && !s.rf_on, "NVS: saved channel restored at boot, RF off");

    /* -------------------------------------------------------- B: PLL never locks */
    sim_reset(); sim.lock_delay_ms = -1; boot();
    check(s.state == FM_STATE_FAULT && !s.pll_lock && strstr(s.error, "did not lock"), "B: no lock -> FAULT with message");
    check(sim.out[BOARD_FM_TX_EN] == 0 && sim.out[BOARD_MPX_RUN] == 0 && sim.out[BOARD_MPX_MUTE] == 1, "B: RF and MPX off");
    check(sim.out[BOARD_LF_INPUT_SEL] == 1 && sim.out[BOARD_LF_FB_SEL] == 1, "B: loop filter left in ACQUIRE");
    check(fm_control_set_rf(true) != ESP_OK && sim.out[BOARD_FM_TX_EN] == 0, "B: RF on refused");

    /* -------------------------------------------------------- C: FPGA absent */
    sim_reset(); sim.fpga_present = 0; boot();
    check(s.state == FM_STATE_TUNED && !s.fpga_ok && strstr(s.error, "FPGA"), "C: PLL tuned, FPGA reported missing");
    check(fm_control_set_rf(true) != ESP_OK && sim.out[BOARD_FM_TX_EN] == 0, "C: RF on refused without the FPGA");

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
