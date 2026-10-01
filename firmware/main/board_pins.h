#pragma once

/*
 * USB-LAN 基板 rev 0.3 のピン割り当て (hardware/kicad/usb_lan.kicad_sch と一致させること)
 *
 * 使用済み (触らない): GPIO19/20 = USB (TinyUSB), GPIO43/44 = UART0 コンソール,
 *                      GPIO0 = BOOT スイッチ, GPIO3/45/46 = ストラッピング (未接続)
 * 予備 (未配線): GPIO2, 7, 15-18, 35-42 (39-42 は JTAG 用に空けてある), 47, 48
 */

#include "driver/gpio.h"
#include "driver/spi_common.h"

/* ---- J2: 送信機 (FM) 接続用 拡張ヘッダ (3.3V ロジック) ---- */
/* SPI2 (IO_MUX 直結ピン。GPIO マトリクスを通らないので最大 80MHz まで使える) */
#define BOARD_SPI_HOST      SPI2_HOST
#define BOARD_SPI_SCK       GPIO_NUM_12     /* J2-13, FPGA と ADF4002 (CLK) で共用 */
#define BOARD_SPI_MOSI      GPIO_NUM_11     /* J2-14, FPGA と ADF4002 (DATA) で共用 */
#define BOARD_SPI_MISO      GPIO_NUM_13     /* J2-7,  FPGA → ESP32 */
/* FPGA (Rev.B2, 4 線 SPI) */
#define BOARD_FPGA_CS       GPIO_NUM_10     /* J2-8,  SPI2 CS0 (Low アクティブ) */
#define BOARD_FPGA_RST      GPIO_NUM_4      /* J2-9,  出力: FPGA リセット */
/* ADF4002 (PLL): SCK/MOSI を共用し、LE の立上りで 24bit を取り込む。
 * FPGA との通信中は LE を Low のまま保てば ADF4002 側には反映されない */
#define BOARD_ADF_LE        GPIO_NUM_14     /* J2-15, 出力 (アイドル Low) */
#define BOARD_ADF_MUXOUT    GPIO_NUM_6      /* J2-11, 入力: ロック検出 */
/* 送信制御・モニタ */
#define BOARD_FM_TX_EN      GPIO_NUM_5      /* J2-10, 出力: RF 出力 ON/OFF */
#define BOARD_FM_ADC        GPIO_NUM_1      /* J2-12, ADC1_CH0: RF レベル等 (0-3.1V 目安) */
/* 予備 I2C (EEPROM・センサ等) */
#define BOARD_I2C_SDA       GPIO_NUM_8      /* J2-5,  4.7k プルアップ (R7) */
#define BOARD_I2C_SCL       GPIO_NUM_9      /* J2-6,  4.7k プルアップ (R8) */
/* J2-1 = +3V3, J2-2 = +5V (PTC 後, ESP32 と共用で合計 0.5A), J2-3/4/16 = GND */

/* ---- 状態 LED ---- */
#define BOARD_LED_STATUS    GPIO_NUM_21     /* D1, High で点灯 */
