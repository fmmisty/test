#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef esp_err_t (*usb_ncm_rx_cb_t)(void *buffer, uint16_t len, void *ctx);
typedef void (*usb_ncm_free_cb_t)(void *buff_free_arg, void *ctx);

/**
 * USB-OTG (GPIO19/20) を CDC-NCM デバイスとして起動する。
 *
 * @param mac     ホスト側 NIC に見せる MAC アドレス。L2 ブリッジでは Wi-Fi STA と同じにする。
 * @param rx_cb   ホスト → デバイスのフレーム受信 (TinyUSB タスクから呼ばれる。buffer は呼出し中のみ有効)
 * @param free_cb usb_ncm_send() に渡した buff_free_arg を解放するコールバック
 */
esp_err_t usb_ncm_init(const uint8_t mac[6], usb_ncm_rx_cb_t rx_cb, usb_ncm_free_cb_t free_cb);

/**
 * デバイス → ホストへフレームを送る。ESP_OK のとき buff_free_arg は free_cb で解放される。
 * 失敗時は呼出し側が解放すること。
 */
esp_err_t usb_ncm_send(void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout_ms);
