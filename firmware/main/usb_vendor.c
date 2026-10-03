#include "sdkconfig.h"

#if CONFIG_TINYUSB_VENDOR_COUNT > 0

#include "usb_net.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "usblan_proto.h"

/*
 * 独自ベンダークラスによる USB-LAN。プロトコルは protocol/usblan_proto.h を参照。
 * 対になるホスト側ドライバは host/linux/usblan_tap.c。
 *
 * esp_tinyusb 既定のディスクリプタ (CONFIG_TINYUSB_VENDOR_COUNT=1, CDC/MSC/NET 無効) を使うので
 * ベンダーインタフェースは interface 0、エンドポイントは EP1 OUT / EP1 IN (64 byte) になる。
 */

#define VENDOR_ITF          0
// 送信中のフレームが途中で止まったときに諦めるまでの時間。超えるとストリームがずれるが
// ホスト側パーサがマジックで再同期する
#define TX_STALL_LIMIT_MS   500

static const char *TAG = "usb_vendor";

static uint8_t s_mac[6];
static usb_net_rx_cb_t s_rx_cb;
static usb_net_free_cb_t s_free_cb;
static volatile bool s_link_up;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_tx_done;
static usblan_parser_t s_parser;   // TinyUSB タスクからのみ触る

// ---- ホスト → デバイス ---------------------------------------------------

static void on_frame(void *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    if (s_rx_cb) {
        s_rx_cb((void *)frame, len, NULL);
    }
}

// TinyUSB タスクから呼ばれる。受信 FIFO (64 byte) を即座に空けて次の OUT パケットを受けられるようにする
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    uint8_t chunk[64];
    uint32_t n;
    while ((n = tud_vendor_n_read(itf, chunk, sizeof(chunk))) > 0) {
        usblan_parser_feed(&s_parser, chunk, n, on_frame, NULL);
    }
}

// ---- デバイス → ホスト ---------------------------------------------------

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    (void)itf;
    (void)sent_bytes;
    xSemaphoreGive(s_tx_done);
}

static bool wait_tx_space(uint32_t need, TickType_t deadline)
{
    while (tud_vendor_n_write_available(VENDOR_ITF) < need) {
        if (!tud_vendor_n_mounted(VENDOR_ITF) || (int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return false;
        }
        tud_vendor_n_write_flush(VENDOR_ITF);
        xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(2));
    }
    return true;
}

static bool write_all(const uint8_t *p, uint32_t n, TickType_t deadline)
{
    while (n > 0) {
        if (!wait_tx_space(1, deadline)) {
            return false;
        }
        uint32_t w = tud_vendor_n_write(VENDOR_ITF, p, n);
        p += w;
        n -= w;
    }
    return true;
}

esp_err_t usb_net_send(void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout_ms)
{
    if (!tud_vendor_n_mounted(VENDOR_ITF)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len < USBLAN_MIN_FRAME || len > USBLAN_MAX_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    uint8_t hdr[USBLAN_HDR_LEN];
    usblan_encode_header(hdr, len);

    // ヘッダを書く余地ができるまでは timeout_ms で待ち、書けなければ何も送らずに失敗する
    if (!wait_tx_space(USBLAN_HDR_LEN, xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms))) {
        ret = ESP_ERR_TIMEOUT;
    } else {
        // 書き始めたフレームは最後まで送る (途中で止めるとストリームがずれるため)
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_STALL_LIMIT_MS);
        if (!write_all(hdr, sizeof(hdr), deadline) || !write_all(buffer, len, deadline)) {
            ESP_LOGW(TAG, "tx stalled mid-frame, host will resync");
            ret = ESP_FAIL;
        }
        tud_vendor_n_write_flush(VENDOR_ITF);
    }
    xSemaphoreGive(s_tx_lock);

    // フレームは FIFO にコピー済みなので、成功時はここで解放してよい (NCM 版と同じ約束)
    if (ret == ESP_OK && s_free_cb) {
        s_free_cb(buff_free_arg, NULL);
    }
    return ret;
}

// ---- 初期化 --------------------------------------------------------------

esp_err_t usb_net_init(const uint8_t mac[6], usb_net_rx_cb_t rx_cb, usb_net_free_cb_t free_cb)
{
    memcpy(s_mac, mac, 6);
    s_rx_cb = rx_cb;
    s_free_cb = free_cb;
    usblan_parser_reset(&s_parser);

    s_tx_lock = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_tx_lock && s_tx_done, ESP_ERR_NO_MEM, TAG, "semaphore");

    // 既定ディスクリプタ: VID/PID は CONFIG_TINYUSB_DESC_CUSTOM_VID/PID (sdkconfig.vendor 参照)
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    ESP_LOGI(TAG, "vendor USB-LAN started (proto v%d), host MAC " MACSTR,
             USBLAN_PROTO_VERSION, MAC2STR(mac));
    return ESP_OK;
}

void usb_net_set_link(bool up)
{
    s_link_up = up;
}

void usb_net_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}

bool usb_net_get_link(void)
{
    return s_link_up;
}

// TinyUSB タスク (制御要求の処理中) から呼ばれるので、rx_cb と競合しない
void usb_net_on_driver_start(void)
{
    usblan_parser_reset(&s_parser);
    tud_vendor_n_read_flush(VENDOR_ITF);
}

#endif // CONFIG_TINYUSB_VENDOR_COUNT > 0
