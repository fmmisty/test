#include "wifi_bridge.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "usb_net.h"

static const char *TAG = "bridge";

#define NVS_NS              "usblan"
#define RECONNECT_MIN_MS    1000
#define RECONNECT_MAX_MS    30000

static wifi_bridge_mode_t s_mode;
static wifi_interface_t s_ifx;          // WIFI_IF_STA or WIFI_IF_AP
static uint8_t s_mac[6];                // ホストに見せる MAC (= 使う Wi-Fi I/F の MAC)
static volatile bool s_connected;       // STA: AP に接続中 / AP: SoftAP 起動中
static TimerHandle_t s_reconnect_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static bool s_wifi_started;

// ---- USB (host) -> Wi-Fi -------------------------------------------------
static esp_err_t usb_rx(void *buffer, uint16_t len, void *ctx)
{
    if (s_connected) {
        // esp_wifi_internal_tx() はバッファをコピーするので、この後 TinyUSB に返してよい
        if (esp_wifi_internal_tx(s_ifx, buffer, len) != ESP_OK) {
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
        // ホスト未接続 / ドライバ未起動時はここに来る
        esp_wifi_internal_free_rx_buffer(eb);
        ESP_LOGD(TAG, "usb tx drop (%u)", len);
    }
    return ESP_OK;
}

static void set_link(bool up)
{
    s_connected = up;
    esp_wifi_internal_reg_rxcb(s_ifx, up ? wifi_rx : NULL);
    usb_net_set_link(up);
}

// ---- STA: 再接続 (指数バックオフ) ----------------------------------------
static void reconnect_cb(TimerHandle_t t)
{
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    ESP_LOGI(TAG, "reconnect in %lu ms", (unsigned long)s_backoff_ms);
    xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_backoff_ms), 0);
    xTimerStart(s_reconnect_timer, 0);
    s_backoff_ms = (s_backoff_ms * 2 > RECONNECT_MAX_MS) ? RECONNECT_MAX_MS : s_backoff_ms * 2;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    // -- STA --
    case WIFI_EVENT_STA_START: {
        wifi_config_t wc;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
            esp_wifi_connect();
        }
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi connected");
        s_backoff_ms = RECONNECT_MIN_MS;
        set_link(true);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d)", ev->reason);
        set_link(false);
        schedule_reconnect();
        break;
    }
    // -- AP --
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "SoftAP started");
        set_link(true);
        break;
    case WIFI_EVENT_AP_STOP:
        set_link(false);
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *ev = data;
        ESP_LOGI(TAG, "station " MACSTR " joined (aid %d)", MAC2STR(ev->mac), ev->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *ev = data;
        ESP_LOGI(TAG, "station " MACSTR " left", MAC2STR(ev->mac));
        break;
    }
    default:
        break;
    }
}

// ---- NVS に保存するアプリ設定 (モード / SoftAP) ---------------------------
static wifi_bridge_mode_t load_mode(void)
{
    uint8_t m = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mode", &m);
        nvs_close(h);
    }
    if (m == WIFI_BRIDGE_MODE_STA || m == WIFI_BRIDGE_MODE_AP) {
        return (wifi_bridge_mode_t)m;
    }
#if CONFIG_USB_LAN_WIFI_MODE_AP
    return WIFI_BRIDGE_MODE_AP;
#else
    return WIFI_BRIDGE_MODE_STA;
#endif
}

esp_err_t wifi_bridge_save_mode(wifi_bridge_mode_t mode)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(h, "mode", (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void build_ap_config(wifi_config_t *wc)
{
    char ssid[33] = CONFIG_USB_LAN_AP_SSID;
    char pass[65] = CONFIG_USB_LAN_AP_PASSWORD;
    uint8_t ch = CONFIG_USB_LAN_AP_CHANNEL;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(ssid);
        nvs_get_str(h, "ap_ssid", ssid, &len);
        len = sizeof(pass);
        nvs_get_str(h, "ap_pass", pass, &len);
        nvs_get_u8(h, "ap_ch", &ch);
        nvs_close(h);
    }

    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->ap.ssid, ssid, sizeof(wc->ap.ssid));
    wc->ap.ssid_len = strlen(ssid);
    strlcpy((char *)wc->ap.password, pass, sizeof(wc->ap.password));
    wc->ap.channel = ch;
    wc->ap.max_connection = CONFIG_USB_LAN_AP_MAX_CONN;
    wc->ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc->ap.pmf_cfg.required = false;
}

esp_err_t wifi_bridge_set_ap(const char *ssid, const char *password, uint8_t channel)
{
    size_t ssid_len = strlen(ssid), pass_len = strlen(password);
    ESP_RETURN_ON_FALSE(ssid_len >= 1 && ssid_len <= 32, ESP_ERR_INVALID_ARG, TAG, "SSID must be 1-32 chars");
    ESP_RETURN_ON_FALSE(pass_len >= 8 && pass_len <= 63, ESP_ERR_INVALID_ARG, TAG, "password must be 8-63 chars");
    ESP_RETURN_ON_FALSE(channel >= 1 && channel <= 13, ESP_ERR_INVALID_ARG, TAG, "channel must be 1-13");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, "ap_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ap_pass", password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "ap_ch", channel);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    if (s_mode != WIFI_BRIDGE_MODE_AP) {
        return ESP_OK;   // 次に AP モードで起動したときに使われる
    }
    wifi_config_t wc;
    build_ap_config(&wc);
    if (!s_wifi_started) {
        // パスワード未設定で起動を見送っていた場合はここで SoftAP を立ち上げる
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "set_config");
        ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        s_wifi_started = true;
        return ESP_OK;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &wc);   // 接続中の端末は一度切断される
}

// ---- 起動 ----------------------------------------------------------------
esp_err_t wifi_bridge_start(void)
{
    s_mode = load_mode();
    s_ifx = (s_mode == WIFI_BRIDGE_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
    ESP_ERROR_CHECK(esp_read_mac(s_mac, s_mode == WIFI_BRIDGE_MODE_AP ? ESP_MAC_WIFI_SOFTAP : ESP_MAC_WIFI_STA));

    s_reconnect_timer = xTimerCreate("reconnect", pdMS_TO_TICKS(RECONNECT_MIN_MS), pdFALSE, NULL, reconnect_cb);
    ESP_RETURN_ON_FALSE(s_reconnect_timer, ESP_ERR_NO_MEM, TAG, "timer");

    // ESP 側に IP を持たない純 L2 ブリッジなので esp_netif のインタフェースは作らない
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_country_code(CONFIG_USB_LAN_WIFI_COUNTRY, false));

    if (s_mode == WIFI_BRIDGE_MODE_AP) {
        wifi_config_t wc;
        build_ap_config(&wc);
        if (strlen((char *)wc.ap.password) < 8) {
            // 無防備なオープン AP は作らない
            ESP_LOGE(TAG, "SoftAP password is not set (8+ chars). Use console: ap <ssid> <password> [channel]");
            ESP_ERROR_CHECK(usb_net_init(s_mac, usb_rx, wifi_rx_free));
            return ESP_OK;
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
        ESP_LOGI(TAG, "mode AP: SSID \"%s\" ch %d", (char *)wc.ap.ssid, wc.ap.channel);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t wc = {0};
        esp_wifi_get_config(WIFI_IF_STA, &wc);  // Wi-Fi ドライバが NVS に保存している設定
        if (wc.sta.ssid[0] == '\0' && CONFIG_USB_LAN_WIFI_SSID[0] != '\0') {
            strlcpy((char *)wc.sta.ssid, CONFIG_USB_LAN_WIFI_SSID, sizeof(wc.sta.ssid));
            strlcpy((char *)wc.sta.password, CONFIG_USB_LAN_WIFI_PASSWORD, sizeof(wc.sta.password));
            wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        }
        if (wc.sta.ssid[0] == '\0') {
            ESP_LOGW(TAG, "No Wi-Fi credentials. Use console: wifi <ssid> <password>");
        }
        ESP_LOGI(TAG, "mode STA");
    }
    // 省電力はレイテンシとスループットを悪化させるので無効
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // USB はホストから見て常に存在させ、Wi-Fi 未接続中のフレームは破棄する
    ESP_ERROR_CHECK(usb_net_init(s_mac, usb_rx, wifi_rx_free));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t wifi_bridge_set_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(s_mode == WIFI_BRIDGE_MODE_STA, ESP_ERR_INVALID_STATE, TAG, "not in STA mode");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (password) {
        strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    }
    wc.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // TODO: WPA2/WPA3-Enterprise (802.1X) の場合は esp_eap_client で設定する

    xTimerStop(s_reconnect_timer, 0);
    s_backoff_ms = RECONNECT_MIN_MS;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config");
    return esp_wifi_connect();
}

esp_err_t wifi_bridge_scan(wifi_ap_record_t *records, uint16_t *count)
{
    ESP_RETURN_ON_FALSE(s_mode == WIFI_BRIDGE_MODE_STA, ESP_ERR_INVALID_STATE, TAG, "scan is STA mode only");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan_start");
    return esp_wifi_scan_get_ap_records(count, records);
}

wifi_bridge_mode_t wifi_bridge_get_mode(void)
{
    return s_mode;
}

void wifi_bridge_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}

bool wifi_bridge_is_connected(void)
{
    return s_connected;
}
