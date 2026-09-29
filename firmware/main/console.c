#include "console.h"

#include <stdio.h>
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "wifi_bridge.h"

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [<password>]\n");
        return 1;
    }
    esp_err_t err = wifi_bridge_set_credentials(argv[1], argc == 3 ? argv[2] : NULL);
    printf("%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("MAC      : " MACSTR "\n", MAC2STR(mac));
    printf("Wi-Fi    : %s\n", wifi_bridge_is_connected() ? "connected" : "disconnected");

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("SSID     : %s\nRSSI     : %d dBm\nChannel  : %d\n", (char *)ap.ssid, ap.rssi, ap.primary);
    }
    return 0;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "usb-lan>";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "Set Wi-Fi credentials (saved to NVS) and reconnect",
          .hint = "<ssid> [<password>]", .func = cmd_wifi },
        { .command = "status", .help = "Show MAC / Wi-Fi status", .func = cmd_status },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
