#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "console.h"
#include "ota_usb.h"
#include "wifi_bridge.h"
#include "fm_control.h"
#include "fm_web.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(fm_control_init());
    ESP_ERROR_CHECK(wifi_bridge_start());
    ESP_ERROR_CHECK(fm_web_start());
    ESP_ERROR_CHECK(console_start());

    // ここまで来れば起動成功。USB 経由で更新した直後なら、これで新しいファームが確定する
    ota_usb_mark_app_valid();
}
