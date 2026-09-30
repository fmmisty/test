#include "sdkconfig.h"

#if CONFIG_TINYUSB_NET_MODE_NCM && (CONFIG_TINYUSB_VENDOR_COUNT > 0)
#error "Enable either CDC-NCM or the vendor interface, not both"
#endif
#if !CONFIG_TINYUSB_NET_MODE_NCM && !(CONFIG_TINYUSB_VENDOR_COUNT > 0)
#error "No USB network interface: set CONFIG_TINYUSB_NET_MODE_NCM or CONFIG_TINYUSB_VENDOR_COUNT=1"
#endif

#if CONFIG_TINYUSB_NET_MODE_NCM

#include "usb_net.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "usb_ncm";

static uint8_t s_mac[6];
static volatile bool s_link_up;

esp_err_t usb_net_init(const uint8_t mac[6], usb_net_rx_cb_t rx_cb, usb_net_free_cb_t free_cb)
{
    // 内蔵 PHY (GPIO19 = D-, GPIO20 = D+)。VID/PID・文字列は esp_tinyusb の既定 (menuconfig で変更)
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    memcpy(s_mac, mac, 6);
    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = rx_cb,
        .free_tx_buffer = free_cb,
    };
    memcpy(net_cfg.mac_addr, mac, 6);
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg), TAG, "tinyusb_net_init");

    ESP_LOGI(TAG, "CDC-NCM started, host MAC " MACSTR, MAC2STR(mac));
    return ESP_OK;
}

esp_err_t usb_net_send(void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout_ms)
{
    return tinyusb_net_send_sync(buffer, len, buff_free_arg, pdMS_TO_TICKS(timeout_ms));
}

void usb_net_set_link(bool up)
{
    // TODO: NCM の NETWORK_CONNECTION 通知でホストにリンク状態を伝える
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

void usb_net_on_driver_start(void)
{
    // NCM はフレーム境界を USB 側が持つので仕切り直すものはない
}

#endif // CONFIG_TINYUSB_NET_MODE_NCM
