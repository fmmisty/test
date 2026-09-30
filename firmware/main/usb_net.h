#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * USB 側のネットワーク I/F。sdkconfig で実装が切り替わる:
 *   CONFIG_TINYUSB_NET_MODE_NCM=y      -> usb_ncm.c    (CDC-NCM, OS 標準ドライバ)
 *   CONFIG_TINYUSB_VENDOR_COUNT=1      -> usb_vendor.c (独自ベンダークラス, host/ の自作ドライバ)
 */

typedef esp_err_t (*usb_net_rx_cb_t)(void *buffer, uint16_t len, void *ctx);
typedef void (*usb_net_free_cb_t)(void *buff_free_arg, void *ctx);

/**
 * USB-OTG (GPIO19/20) を起動する。
 *
 * @param mac     ホスト側 NIC に使わせる MAC アドレス。L2 ブリッジでは Wi-Fi STA と同じにする。
 * @param rx_cb   ホスト → デバイスのフレーム受信 (TinyUSB タスクから呼ばれる。buffer は呼出し中のみ有効)
 * @param free_cb usb_net_send() に渡した buff_free_arg を解放するコールバック
 */
esp_err_t usb_net_init(const uint8_t mac[6], usb_net_rx_cb_t rx_cb, usb_net_free_cb_t free_cb);

/**
 * デバイス → ホストへフレームを送る。ESP_OK のとき buff_free_arg は free_cb で解放される。
 * 失敗時は呼出し側が解放すること。
 */
esp_err_t usb_net_send(void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout_ms);

/** 上流 (Wi-Fi) のリンク状態をホストへ伝える (ベンダークラスのみ有効) */
void usb_net_set_link(bool up);
