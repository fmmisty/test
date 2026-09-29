#include "usb_ncm.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "usb_ncm";

esp_err_t usb_ncm_init(const uint8_t mac[6], usb_ncm_rx_cb_t rx_cb, usb_ncm_free_cb_t free_cb)
{
    // 内蔵 PHY (GPIO19 = D-, GPIO20 = D+)。VID/PID・文字列は esp_tinyusb の既定 (menuconfig で変更)
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = rx_cb,
        .free_tx_buffer = free_cb,
    };
    memcpy(net_cfg.mac_addr, mac, 6);
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg), TAG, "tinyusb_net_init");

    ESP_LOGI(TAG, "CDC-NCM started, host MAC " MACSTR, MAC2STR(mac));
    return ESP_OK;
}

esp_err_t usb_ncm_send(void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout_ms)
{
    return tinyusb_net_send_sync(buffer, len, buff_free_arg, pdMS_TO_TICKS(timeout_ms));
}
