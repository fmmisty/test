#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Wi-Fi STA <-> USB CDC-NCM の L2 ブリッジを開始する。
 *
 *   社内 AP  <--Wi-Fi-->  [ESP32-S3 STA ==(L2)== USB NCM]  <--USB-->  PC
 *
 * NCM でホストに見せる MAC を Wi-Fi STA の MAC と同一にしているので、フレームは
 * 書き換えずにそのまま転送できる (ESP 側に IP スタックは持たない)。
 * PC は社内ネットワークの DHCP から直接アドレスを取得する。
 */
esp_err_t wifi_bridge_start(void);

/** STA 設定を差し替えて再接続する (設定は Wi-Fi ドライバが NVS に保存する)。 */
esp_err_t wifi_bridge_set_credentials(const char *ssid, const char *password);

bool wifi_bridge_is_connected(void);
