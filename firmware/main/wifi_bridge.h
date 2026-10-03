#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/**
 * Wi-Fi <-> USB の L2 ブリッジ。ESP 側に IP スタックは持たず、フレームを書き換えずに転送する。
 * USB 側でホストに見せる MAC を、使う Wi-Fi I/F の MAC と同一にしているのがミソ。
 *
 * STA モード (既存の Wi-Fi に参加):
 *   ルーター <--Wi-Fi--> [ESP32 STA ==(L2)== USB] <--USB--> ホスト
 *   ホストはルーターの DHCP から IP を取得する。
 *
 * AP モード (ESP32 がアクセスポイントになり、スマホが直接つながる):
 *   スマホ <--Wi-Fi--> [ESP32 SoftAP ==(L2)== USB] <--USB--> ホスト
 *   ホストが AP の「中の人」として IP を持ち、DHCP サーバを動かす (host/linux/README.md 参照)。
 */
typedef enum { WIFI_BRIDGE_MODE_STA = 1 } wifi_bridge_mode_t;

esp_err_t wifi_bridge_start(void);

wifi_bridge_mode_t wifi_bridge_get_mode(void);

/** 次回起動時のモードを NVS に保存する (反映には再起動が必要) */
esp_err_t wifi_bridge_save_mode(wifi_bridge_mode_t mode);

/** STA: 参加先を差し替えて再接続する (設定は Wi-Fi ドライバが NVS に保存する) */
esp_err_t wifi_bridge_set_credentials(const char *ssid, const char *password);

/** AP: SSID / パスワード (8-63 文字) / チャネルを NVS に保存し、AP モードなら即反映する */
esp_err_t wifi_bridge_set_ap(const char *ssid, const char *password, uint8_t channel);

/** STA: 周囲の AP を走査する (count は入力で配列長、出力で件数) */
esp_err_t wifi_bridge_scan(wifi_ap_record_t *records, uint16_t *count);

/** ホストに見せている MAC (STA または SoftAP の MAC) */
void wifi_bridge_get_mac(uint8_t mac[6]);

/** STA: AP に接続中 / AP: SoftAP 起動中 */
bool wifi_bridge_is_connected(void);
