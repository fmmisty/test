/* protocol/usblan_proto.h のフレーム分解テスト (ファームとホストで同じコードを使う) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usblan_proto.h"

static int n_frames;
static uint16_t lens[16];
static uint8_t first[16];

static void cb(void *ctx, const uint8_t *f, uint16_t len)
{
    (void)ctx;
    lens[n_frames] = len;
    first[n_frames] = f[0];
    n_frames++;
}

static size_t put(uint8_t *out, uint16_t len, uint8_t fill)
{
    usblan_encode_header(out, len);
    memset(out + USBLAN_HDR_LEN, fill, len);
    return USBLAN_HDR_LEN + len;
}

static usblan_parser_t p;

int main(void)
{
    static uint8_t s[8192];
    size_t n = 0;

    /* 1) 連結された 3 フレームを 1 バイトずつ流しても全部取り出せる */
    n += put(s + n, 60, 0x11);
    n += put(s + n, USBLAN_MAX_FRAME, 0x22);
    n += put(s + n, USBLAN_MIN_FRAME, 0x33);
    usblan_parser_reset(&p);
    n_frames = 0;
    for (size_t i = 0; i < n; i++) {
        usblan_parser_feed(&p, &s[i], 1, cb, NULL);
    }
    assert(n_frames == 3);
    assert(lens[0] == 60 && first[0] == 0x11);
    assert(lens[1] == USBLAN_MAX_FRAME && first[1] == 0x22);
    assert(lens[2] == USBLAN_MIN_FRAME && first[2] == 0x33);

    /* 2) 64 バイト単位 (USB FS パケット) でも同じ */
    usblan_parser_reset(&p);
    n_frames = 0;
    for (size_t i = 0; i < n; i += 64) {
        usblan_parser_feed(&p, &s[i], (n - i) < 64 ? (n - i) : 64, cb, NULL);
    }
    assert(n_frames == 3);

    /* 3) 先頭のゴミ・範囲外の長さ・途中で切れたフレームの後でも再同期する */
    n = 0;
    s[n++] = 0x00; s[n++] = 0xA5; s[n++] = 0xA5;           /* ゴミと重複マジック */
    s[n++] = 0x5A; s[n++] = 0xFF; s[n++] = 0xFF;           /* len=65535 は範囲外 */
    n += put(s + n, 100, 0x44);
    usblan_parser_reset(&p);
    n_frames = 0;
    usblan_parser_feed(&p, s, n, cb, NULL);
    assert(n_frames == 1 && lens[0] == 100 && first[0] == 0x44);
    assert(p.bad_len == 1);

    /* 4) 1 回の feed に複数フレームがまとまって来るケース */
    n = 0;
    for (int i = 0; i < 10; i++) {
        n += put(s + n, (uint16_t)(20 + i), (uint8_t)i);
    }
    usblan_parser_reset(&p);
    n_frames = 0;
    usblan_parser_feed(&p, s, n, cb, NULL);
    assert(n_frames == 10 && lens[9] == 29 && first[9] == 9);

    printf("test_parser: all tests passed\n");
    return 0;
}
