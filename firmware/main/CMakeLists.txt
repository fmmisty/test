# usb_ncm.c / usb_vendor.c は sdkconfig に応じてどちらか一方だけが有効になる
idf_component_register(SRCS "main.c" "wifi_bridge.c" "usb_ncm.c" "usb_vendor.c" "usb_ctrl.c" "ota_usb.c" "console.c"
                       INCLUDE_DIRS "." "../../protocol"
                       PRIV_REQUIRES esp_wifi esp_event nvs_flash console esp_driver_uart app_update esp_timer)
