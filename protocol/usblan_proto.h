/*
 * USB-LAN ベンダークラス プロトコル定義 (ESP32 ファームとホストドライバで共有)
 *
 * インタフェース: bInterfaceClass = 0xFF (Vendor), バルク OUT/IN 各 1 本 (FS: 64 byte)
 *
 * バルク転送はバイトストリームとして扱い、Ethernet フレームを次の形式で連結する:
 *
 *   +------+------+---------+---------+------------------------+
 *   | 0xA5 | 0x5A | len LSB | len MSB | Ethernet frame (len B) |
 *   +------+------+---------+---------+------------------------+
 *
 *   len = 14..1518 (宛先 MAC から、FCS は含まない)。
 *   マジック 2 バイトは取りこぼし時の再同期用。
 *
 * ベンダー制御要求 (Device-to-Host, bmRequestType = 0xC0/0xC1):
 *   0x01 GET_VERSION : 2 byte LE プロトコル版数。受信側パーサもリセットされる (ドライバ起動時に呼ぶ)
 *   0x02 GET_MAC     : 6 byte ホスト側 NIC に設定すべき MAC (= 使用中の Wi-Fi I/F の MAC)
 *   0x03 GET_LINK    : 1 byte  1 = Wi-Fi 接続中 (AP モードでは SoftAP 起動中), 0 = 切断
 *   0x13 OTA_STATUS  : 8 byte  u32 LE 書込み済みバイト数, i32 LE 最後のエラー (esp_err_t, 0 = OK)
 *
 * ファーム更新 (Host-to-Device, bmRequestType = 0x40/0x41)。ベンダー型の制御要求は
 * インタフェース構成に関係なく届くので、CDC-NCM ビルドでも使える (recipient = device でよい):
 *   0x10 OTA_BEGIN   : data = u32 LE イメージサイズ。次の OTA パーティションを消去して準備
 *   0x11 OTA_DATA    : data = イメージの続き (最大 USBLAN_OTA_CHUNK byte, 先頭から順に送る)
 *   0x12 OTA_END     : data なし。検証して起動パーティションを切替え、約 0.5 秒後に再起動
 *   失敗した要求は STALL する。詳細は OTA_STATUS で取得する。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define USBLAN_PROTO_VERSION    2

/* 開発用の既定 VID/PID (pid.codes のテスト用 PID。社外配布する場合は正式な PID を取得すること) */
#define USBLAN_DEFAULT_VID      0x1209
#define USBLAN_DEFAULT_PID      0x0001

#define USBLAN_MAGIC0           0xA5
#define USBLAN_MAGIC1           0x5A
#define USBLAN_HDR_LEN          4
#define USBLAN_MIN_FRAME        14
#define USBLAN_MAX_FRAME        1518

#define USBLAN_REQ_GET_VERSION  0x01
#define USBLAN_REQ_GET_MAC      0x02
#define USBLAN_REQ_GET_LINK     0x03
#define USBLAN_REQ_OTA_BEGIN    0x10
#define USBLAN_REQ_OTA_DATA     0x11
#define USBLAN_REQ_OTA_END      0x12
#define USBLAN_REQ_OTA_STATUS   0x13

#define USBLAN_OTA_CHUNK        1024

static inline void usblan_encode_header(uint8_t hdr[USBLAN_HDR_LEN], uint16_t len)
{
    hdr[0] = USBLAN_MAGIC0;
    hdr[1] = USBLAN_MAGIC1;
    hdr[2] = (uint8_t)(len & 0xff);
    hdr[3] = (uint8_t)(len >> 8);
}

/* ---- ストリーム → フレーム 分解 ---------------------------------------- */

typedef void (*usblan_frame_cb_t)(void *ctx, const uint8_t *frame, uint16_t len);

typedef struct {
    uint8_t  hdr[USBLAN_HDR_LEN];
    uint8_t  hdr_len;           /* 受信済みヘッダ長 (0..4) */
    uint16_t frame_len;         /* ヘッダで宣言されたフレーム長 */
    uint16_t pos;               /* 受信済みペイロード長 */
    uint32_t frames;            /* 取り出したフレーム数 */
    uint32_t resync_bytes;      /* 再同期のため読み捨てたバイト数 */
    uint32_t bad_len;           /* 範囲外の長さで破棄したヘッダ数 */
    uint8_t  frame[USBLAN_MAX_FRAME];
} usblan_parser_t;

static inline void usblan_parser_reset(usblan_parser_t *p)
{
    p->hdr_len = 0;
    p->frame_len = 0;
    p->pos = 0;
}

static inline void usblan_parser_feed(usblan_parser_t *p, const uint8_t *data, size_t n,
                                      usblan_frame_cb_t cb, void *ctx)
{
    while (n > 0) {
        if (p->hdr_len < USBLAN_HDR_LEN) {
            uint8_t b = *data++;
            n--;
            if (p->hdr_len == 0 && b != USBLAN_MAGIC0) {
                p->resync_bytes++;
                continue;
            }
            if (p->hdr_len == 1 && b != USBLAN_MAGIC1) {
                /* 0xA5 0xA5 0x5A のような並びでも追従できるように */
                p->resync_bytes++;
                p->hdr_len = (b == USBLAN_MAGIC0) ? 1 : 0;
                continue;
            }
            p->hdr[p->hdr_len++] = b;
            if (p->hdr_len == USBLAN_HDR_LEN) {
                uint16_t len = (uint16_t)(p->hdr[2] | (p->hdr[3] << 8));
                if (len < USBLAN_MIN_FRAME || len > USBLAN_MAX_FRAME) {
                    p->bad_len++;
                    p->hdr_len = 0;
                    continue;
                }
                p->frame_len = len;
                p->pos = 0;
            }
            continue;
        }

        size_t chunk = (size_t)(p->frame_len - p->pos);
        if (chunk > n) {
            chunk = n;
        }
        memcpy(&p->frame[p->pos], data, chunk);
        p->pos = (uint16_t)(p->pos + chunk);
        data += chunk;
        n -= chunk;

        if (p->pos == p->frame_len) {
            p->frames++;
            p->hdr_len = 0;
            if (cb) {
                cb(ctx, p->frame, p->frame_len);
            }
        }
    }
}
