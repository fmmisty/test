/*
 * usblan_ota - USB-LAN の線のままファームを書き換える (ボタン操作不要)
 *
 * ベンダー制御要求 (protocol/usblan_proto.h の OTA_*) でイメージを送る。制御要求は
 * recipient = device なので、CDC-NCM ビルドでカーネルの cdc_ncm が使用中でも、
 * ベンダークラスビルドで usblan_tap が動作中でも送れる。
 *
 * 使い方: sudo ./usblan_ota [-v VID -p PID] build/usb_lan.bin
 */
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "usblan_proto.h"

#define CTRL_TIMEOUT_MS  5000   /* OTA_DATA はフラッシュ消去を伴うので長めに */
#define ESP_IMAGE_MAGIC  0xE9

/* 既定で探すデバイス: ベンダークラスビルド, CDC-NCM ビルド (esp_tinyusb 既定の Espressif VID/PID) */
static const uint16_t k_default_ids[][2] = {
    { USBLAN_DEFAULT_VID, USBLAN_DEFAULT_PID },
    { 0x303A, 0x4000 },
};

static libusb_device_handle *open_device(int vid, int pid)
{
    if (vid >= 0) {
        return libusb_open_device_with_vid_pid(NULL, (uint16_t)vid, (uint16_t)pid);
    }
    for (size_t i = 0; i < sizeof(k_default_ids) / sizeof(k_default_ids[0]); i++) {
        libusb_device_handle *h = libusb_open_device_with_vid_pid(NULL, k_default_ids[i][0], k_default_ids[i][1]);
        if (h) {
            printf("found %04x:%04x\n", k_default_ids[i][0], k_default_ids[i][1]);
            return h;
        }
    }
    return NULL;
}

static int ctrl_in(libusb_device_handle *h, uint8_t req, uint8_t *buf, uint16_t len)
{
    return libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                   req, 0, 0, buf, len, CTRL_TIMEOUT_MS);
}

static int ctrl_out(libusb_device_handle *h, uint8_t req, uint16_t value, uint8_t *buf, uint16_t len)
{
    return libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                   req, value, 0, buf, len, CTRL_TIMEOUT_MS);
}

static void report_device_error(libusb_device_handle *h)
{
    uint8_t st[8];
    if (ctrl_in(h, USBLAN_REQ_OTA_STATUS, st, sizeof(st)) == 8) {
        uint32_t written = st[0] | (st[1] << 8) | (st[2] << 16) | ((uint32_t)st[3] << 24);
        int32_t err = (int32_t)(st[4] | (st[5] << 8) | (st[6] << 16) | ((uint32_t)st[7] << 24));
        fprintf(stderr, "device: written %u bytes, esp_err 0x%x\n", written, (unsigned)err);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-v VID -p PID] firmware.bin\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    int vid = -1, pid = -1, opt;
    while ((opt = getopt(argc, argv, "v:p:h")) != -1) {
        switch (opt) {
        case 'v': vid = (int)strtoul(optarg, NULL, 16); break;
        case 'p': pid = (int)strtoul(optarg, NULL, 16); break;
        default: usage(argv[0]);
        }
    }
    if (optind != argc - 1 || (vid < 0) != (pid < 0)) {
        usage(argv[0]);
    }

    /* イメージ読込み */
    FILE *f = fopen(argv[optind], "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", argv[optind], strerror(errno));
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc(size > 0 ? (size_t)size : 1);
    if (!img || size <= 0 || fread(img, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "failed to read image\n");
        return 1;
    }
    fclose(f);
    if (img[0] != ESP_IMAGE_MAGIC) {
        fprintf(stderr, "not an ESP app image (build/<project>.bin を指定する)\n");
        return 1;
    }

    if (libusb_init(NULL) != 0) {
        return 1;
    }
    libusb_device_handle *h = open_device(vid, pid);
    if (!h) {
        fprintf(stderr, "device not found (権限が無い場合は udev ルールか sudo を使う)\n");
        return 1;
    }

    uint8_t v[2] = {0};
    int r = ctrl_in(h, USBLAN_REQ_GET_VERSION, v, sizeof(v));
    if (r != 2 || (v[0] | (v[1] << 8)) < 2) {
        fprintf(stderr, "device does not support USB update (protocol v2+ required)\n");
        return 1;
    }

    uint8_t sz[4] = { size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff, (size >> 24) & 0xff };
    printf("erasing & preparing %ld bytes...\n", size);
    if ((r = ctrl_out(h, USBLAN_REQ_OTA_BEGIN, 0, sz, 4)) != 4) {
        fprintf(stderr, "OTA_BEGIN failed: %s\n", r < 0 ? libusb_error_name(r) : "short");
        report_device_error(h);
        return 1;
    }

    long off = 0;
    uint16_t seq = 0;
    while (off < size) {
        uint16_t n = (size - off) > USBLAN_OTA_CHUNK ? USBLAN_OTA_CHUNK : (uint16_t)(size - off);
        if ((r = ctrl_out(h, USBLAN_REQ_OTA_DATA, seq++, img + off, n)) != n) {
            fprintf(stderr, "\nOTA_DATA failed at %ld: %s\n", off, r < 0 ? libusb_error_name(r) : "short");
            report_device_error(h);
            return 1;
        }
        off += n;
        printf("\r%ld / %ld (%ld%%)", off, size, off * 100 / size);
        fflush(stdout);
    }
    printf("\n");

    if ((r = ctrl_out(h, USBLAN_REQ_OTA_END, 0, NULL, 0)) != 0) {
        fprintf(stderr, "OTA_END failed (image rejected?): %s\n", libusb_error_name(r));
        report_device_error(h);
        return 1;
    }
    printf("done. device is rebooting into the new firmware.\n");

    libusb_close(h);
    libusb_exit(NULL);
    free(img);
    return 0;
}
