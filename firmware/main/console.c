#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_bridge.h"

static int print_result(esp_err_t err)
{
    printf("%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [<password>]\n");
        return 1;
    }
    return print_result(wifi_bridge_set_credentials(argv[1], argc == 3 ? argv[2] : NULL));
}

static int cmd_ap(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        printf("usage: ap <ssid> <password(8-63)> [channel(1-13)]\n");
        return 1;
    }
    uint8_t ch = (argc == 4) ? (uint8_t)atoi(argv[3]) : CONFIG_USB_LAN_AP_CHANNEL;
    return print_result(wifi_bridge_set_ap(argv[1], argv[2], ch));
}

static int cmd_mode(int argc, char **argv)
{
    wifi_bridge_mode_t m;
    if (argc == 2 && strcmp(argv[1], "sta") == 0) {
        m = WIFI_BRIDGE_MODE_STA;
    } else {
        printf("usage: mode sta   (AP bridge mode was removed)\n");
        return 1;
    }
    esp_err_t err = wifi_bridge_save_mode(m);
    if (err != ESP_OK) {
        return print_result(err);
    }
    // MAC (= ホストの NIC) が変わるので再起動して USB から作り直す
    printf("saved, rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
    }
}

static int cmd_scan(int argc, char **argv)
{
    static wifi_ap_record_t recs[20];
    uint16_t n = sizeof(recs) / sizeof(recs[0]);
    esp_err_t err = wifi_bridge_scan(recs, &n);
    if (err != ESP_OK) {
        return print_result(err);
    }
    printf("%-32s %4s %3s %s\n", "SSID", "RSSI", "CH", "AUTH");
    for (int i = 0; i < n; i++) {
        printf("%-32s %4d %3d %s\n", (char *)recs[i].ssid, recs[i].rssi, recs[i].primary,
               auth_name(recs[i].authmode));
    }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    uint8_t mac[6];
    wifi_bridge_get_mac(mac);
    printf("Mode     : APSTA (STA=L2 bridge, AP=management)\n");
    printf("Host MAC : " MACSTR "\n", MAC2STR(mac));

    if (false) {
        wifi_config_t wc;
        wifi_sta_list_t list;
        printf("SoftAP   : %s\n", wifi_bridge_is_connected() ? "running" : "stopped");
        if (esp_wifi_get_config(WIFI_IF_AP, &wc) == ESP_OK) {
            printf("SSID     : %s\nChannel  : %d\n", (char *)wc.ap.ssid, wc.ap.channel);
        }
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            printf("Stations : %d\n", list.num);
            for (int i = 0; i < list.num; i++) {
                printf("  " MACSTR "  %d dBm\n", MAC2STR(list.sta[i].mac), list.sta[i].rssi);
            }
        }
    } else {
        wifi_ap_record_t rec;
        printf("Wi-Fi    : %s\n", wifi_bridge_is_connected() ? "connected" : "disconnected");
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            printf("SSID     : %s\nRSSI     : %d dBm\nChannel  : %d\n", (char *)rec.ssid, rec.rssi, rec.primary);
        }
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
        { .command = "status", .help = "Show mode / MAC / Wi-Fi status", .func = cmd_status },
        { .command = "mode", .help = "Switch Wi-Fi mode (saved to NVS, reboots)",
          .hint = "sta|ap", .func = cmd_mode },
        { .command = "wifi", .help = "STA: set network to join (saved to NVS) and reconnect",
          .hint = "<ssid> [<password>]", .func = cmd_wifi },
        { .command = "scan", .help = "STA: list nearby access points", .func = cmd_scan },
        { .command = "ap", .help = "AP: set SoftAP SSID/password/channel (saved to NVS)",
          .hint = "<ssid> <password> [channel]", .func = cmd_ap },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
