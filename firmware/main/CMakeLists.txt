idf_component_register(SRCS "main.c" "wifi_bridge.c" "usb_ncm.c" "console.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event nvs_flash console esp_driver_uart)
