#pragma once

/*
 * USB-LAN 基板 rev 0.4 のピン割り当て (hardware/kicad/usb_lan.kicad_sch と一致させること)
 * rev 0.4: 切替式ループフィルタ (FMTX Rev.B5 以降) 用に J3 を追加。FPGA_RST は High アクティブに訂正
 *
 * 使用済み (触らない): GPIO19/20 = USB (TinyUSB), GPIO43/44 = UART0 コンソール,
 *                      GPIO0 = BOOT スイッチ, GPIO3/45/46 = ストラッピング (未接続)
 * 予備 (未配線): GPIO35-42 (39-42 は JTAG 用に空けてある), 47, 48
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
#define BOARD_FPGA_RST      GPIO_NUM_4      /* J2-9,  出力: FPGA リセット (High でリセット, 通常 Low) */
/* ADF4002 (PLL): SCK/MOSI を共用し、LE の立上りで 24bit を取り込む。
 * FPGA との通信中は LE を Low のまま保てば ADF4002 側には反映されない */
#define BOARD_ADF_LE        GPIO_NUM_14     /* J2-15, 出力 (アイドル Low) */
#define BOARD_ADF_MUXOUT    GPIO_NUM_6      /* J2-11, 入力: ロック検出 */
/* 送信制御・モニタ */
#define BOARD_FM_TX_EN      GPIO_NUM_5      /* J2-10, 出力: RF 出力 ON/OFF */
#define BOARD_FM_ADC        GPIO_NUM_1      /* J2-12, ADC1_CH0: RF レベル等 (0-3.1V 目安) */
/* ---- J3: 切替式ループフィルタ / MPX 制御 (3.3V ロジック, FMTX Rev.B5 の U306/U307 TMUX1136 へ) ----
 * 起動中 (ピンがハイインピーダンスの間) の状態は基板側の抵抗で決める */
#define BOARD_LF_INPUT_SEL  GPIO_NUM_15     /* J3-1, U306 SEL1: 1 = ACQUIRE (CP 直結), 0 = TRANSMIT (1M/1k)。10k プルアップ */
#define BOARD_LF_FB_SEL     GPIO_NUM_16     /* J3-2, U306 SEL2: 1 = ACQUIRE (220R+1uF), 0 = TRANSMIT (2.94k+22uF)。10k プルアップ */
#define BOARD_LF_PRECHARGE  GPIO_NUM_17     /* J3-3, U307 SEL1: 1 = C302 をプリチャージ */
#define BOARD_MPX_RUN       GPIO_NUM_18     /* J3-4, U307 SEL2: 1 = MPX を VT 加算器へ, 0 = AGND。10k プルダウン */
#define BOARD_MPX_MUTE      GPIO_NUM_7      /* J3-5, FPGA MPX_MUTE: 1 = ミュート。10k プルアップ */
#define BOARD_ADF_CE        GPIO_NUM_2      /* J3-6, ADF4002 CE: 0 = パワーダウン。10k プルダウン */
/* J3-7 = +3V3, J3-8 = GND */

/* 予備 I2C (EEPROM・センサ等) */
#define BOARD_I2C_SDA       GPIO_NUM_8      /* J2-5,  4.7k プルアップ (R7) */
#define BOARD_I2C_SCL       GPIO_NUM_9      /* J2-6,  4.7k プルアップ (R8) */
/* J2-1 = +3V3, J2-2 = +5V (PTC 後, ESP32 と共用で合計 0.5A), J2-3/4/16 = GND */

/* ---- 状態 LED ---- */
#define BOARD_LED_STATUS    GPIO_NUM_21     /* D1, High で点灯 */
