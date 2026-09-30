#include "wifi_bridge.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "sdkconfig.h"
#include "usb_net.h"

static const char *TAG = "bridge";

static volatile bool s_connected;

// ---- USB (host) -> Wi-Fi -------------------------------------------------
static esp_err_t usb_rx(void *buffer, uint16_t len, void *ctx)
{
    if (s_connected) {
        // esp_wifi_internal_tx() はバッファをコピーするので、この後 TinyUSB に返してよい
        if (esp_wifi_internal_tx(WIFI_IF_STA, buffer, len) != ESP_OK) {
            ESP_LOGD(TAG, "wifi tx drop (%u)", len);
        }
    }
    return ESP_OK;
}

// ---- Wi-Fi -> USB (host) -------------------------------------------------
static void wifi_rx_free(void *eb, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(eb);
}

static esp_err_t wifi_rx(void *buffer, uint16_t len, void *eb)
{
    if (usb_net_send(buffer, len, eb, CONFIG_USB_LAN_TX_TIMEOUT_MS) != ESP_OK) {
        // ホスト未接続 / NCM 未オープン時はここに来る
        esp_wifi_internal_free_rx_buffer(eb);
        ESP_LOGD(TAG, "usb tx drop (%u)", len);
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi connected");
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx);
        s_connected = true;
        usb_net_set_link(true);
        // TODO: ホストに再 DHCP させたい場合は NCM のリンク状態通知 (NETWORK_CONNECTION) を検討
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", ev->reason);
        s_connected = false;
        usb_net_set_link(false);
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        // TODO: 指数バックオフ
        esp_wifi_connect();
        break;
    }
    default:
        break;
    }
}

esp_err_t wifi_bridge_start(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    // ESP 側に IP を持たない純 L2 ブリッジなので esp_netif の STA インタフェースは作らない
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 省電力はレイテンシとスループットを悪化させるので無効
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t wc = {0};
    esp_wifi_get_config(WIFI_IF_STA, &wc);  // NVS に保存済みの設定
    if (wc.sta.ssid[0] == '\0' && CONFIG_USB_LAN_WIFI_SSID[0] != '\0') {
        strlcpy((char *)wc.sta.ssid, CONFIG_USB_LAN_WIFI_SSID, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, CONFIG_USB_LAN_WIFI_PASSWORD, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }
    if (wc.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "No Wi-Fi credentials. Use console: wifi <ssid> <password>");
    }

    // USB はホストから見て常に存在させ、Wi-Fi 未接続中のフレームは破棄する
    ESP_ERROR_CHECK(usb_net_init(mac, usb_rx, wifi_rx_free));
    ESP_ERROR_CHECK(esp_wifi_start());  // SSID があれば STA_START で接続開始
    return ESP_OK;
}

esp_err_t wifi_bridge_set_credentials(const char *ssid, const char *password)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (password) {
        strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    }
    wc.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // TODO: 社内が WPA2/WPA3-Enterprise (802.1X) の場合は esp_eap_client で設定する

    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config");
    return esp_wifi_connect();
}

bool wifi_bridge_is_connected(void)
{
    return s_connected;
}
