/*
 * usblan_tap - ESP32-S3 USB-LAN (ベンダークラス) の Linux ユーザー空間ドライバ
 *
 *   ESP32 (bulk IN)  --> パーサ --> TAP (usblanN) --> Linux ネットワークスタック
 *   ESP32 (bulk OUT) <-- ヘッダ付与 <-- TAP
 *
 * プロトコルは ../../protocol/usblan_proto.h。TAP の MAC はデバイスから取得した
 * Wi-Fi STA の MAC に合わせる (L2 ブリッジのため、ホストの送信元 MAC が一致している必要がある)。
 *
 * 使い方: sudo ./usblan_tap [-v VID] [-p PID] [-i ifname]
 * その後:  sudo dhclient usblan0   (または NetworkManager に任せる)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libusb.h>

#include "usblan_proto.h"

#ifndef TUNSETCARRIER
#define TUNSETCARRIER _IOW('T', 226, int)
#endif

#define USB_TIMEOUT_MS   1000
#define CTRL_TIMEOUT_MS  500
#define IN_BUF_SIZE      (16 * 1024)

struct ctx {
    libusb_device_handle *dev;
    int itf;
    uint8_t ep_in;
    uint8_t ep_out;
    int tap_fd;
    char ifname[IFNAMSIZ];
    usblan_parser_t parser;
    volatile bool link_up;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "usblan_tap: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---- USB ----------------------------------------------------------------- */

/* クラス 0xFF のインタフェースと、そのバルク IN/OUT エンドポイントを探す */
static int find_vendor_interface(struct ctx *c)
{
    libusb_device *d = libusb_get_device(c->dev);
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(d, &cfg);
    if (r != 0) {
        return r;
    }
    r = LIBUSB_ERROR_NOT_FOUND;
    for (int i = 0; i < cfg->bNumInterfaces && r != 0; i++) {
        const struct libusb_interface_descriptor *alt = &cfg->interface[i].altsetting[0];
        if (alt->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) {
            continue;
        }
        uint8_t in = 0, out = 0;
        for (int e = 0; e < alt->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                continue;
            }
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                in = ep->bEndpointAddress;
            } else {
                out = ep->bEndpointAddress;
            }
        }
        if (in && out) {
            c->itf = alt->bInterfaceNumber;
            c->ep_in = in;
            c->ep_out = out;
            r = 0;
        }
    }
    libusb_free_config_descriptor(cfg);
    return r;
}

static int vendor_in(struct ctx *c, uint8_t req, uint8_t *buf, uint16_t len)
{
    /* Device-to-Host | Vendor | Interface */
    uint8_t bm = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE;
    return libusb_control_transfer(c->dev, bm, req, 0, (uint16_t)c->itf, buf, len, CTRL_TIMEOUT_MS);
}

/* ---- TAP ----------------------------------------------------------------- */

static int tap_open(char *ifname)
{
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct ifreq ifr = {0};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        close(fd);
        return -1;
    }
    snprintf(ifname, IFNAMSIZ, "%s", ifr.ifr_name);
    return fd;
}

static int tap_configure(const char *ifname, const uint8_t mac[6])
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        return -1;
    }
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);
    int r = ioctl(s, SIOCSIFHWADDR, &ifr);
    if (r == 0) {
        r = ioctl(s, SIOCGIFFLAGS, &ifr);
    }
    if (r == 0) {
        ifr.ifr_flags |= IFF_UP;
        r = ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
    return r;
}

static void tap_set_carrier(struct ctx *c, bool up)
{
    int v = up ? 1 : 0;
    /* 古いカーネル (< 5.0) では未対応。失敗しても致命的ではない */
    if (ioctl(c->tap_fd, TUNSETCARRIER, &v) < 0 && errno != EINVAL && errno != ENOTTY) {
        perror("TUNSETCARRIER");
    }
}

/* ---- データパス ------------------------------------------------------------ */

static void deliver_to_tap(void *arg, const uint8_t *frame, uint16_t len)
{
    struct ctx *c = arg;
    if (write(c->tap_fd, frame, len) < 0 && errno != EIO) {
        perror("tap write");
    }
}

/* ESP32 -> ホスト */
static void *usb_in_thread(void *arg)
{
    struct ctx *c = arg;
    uint8_t *buf = malloc(IN_BUF_SIZE);
    if (!buf) {
        die("out of memory");
    }
    while (!g_stop) {
        int got = 0;
        int r = libusb_bulk_transfer(c->dev, c->ep_in, buf, IN_BUF_SIZE, &got, USB_TIMEOUT_MS);
        if (got > 0) {
            usblan_parser_feed(&c->parser, buf, (size_t)got, deliver_to_tap, c);
        }
        if (r == LIBUSB_ERROR_TIMEOUT || r == 0) {
            continue;
        }
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            fprintf(stderr, "usblan_tap: device disconnected\n");
            g_stop = 1;
            break;
        }
        fprintf(stderr, "usblan_tap: bulk IN: %s\n", libusb_error_name(r));
        usleep(100 * 1000);
    }
    free(buf);
    return NULL;
}

/* ホスト -> ESP32 */
static void *tap_thread(void *arg)
{
    struct ctx *c = arg;
    uint8_t buf[USBLAN_HDR_LEN + USBLAN_MAX_FRAME];
    struct pollfd pfd = { .fd = c->tap_fd, .events = POLLIN };

    while (!g_stop) {
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) {
            continue;
        }
        ssize_t n = read(c->tap_fd, buf + USBLAN_HDR_LEN, USBLAN_MAX_FRAME);
        if (n < USBLAN_MIN_FRAME) {
            continue;
        }
        if (!c->link_up) {
            continue;   /* Wi-Fi 未接続中は ESP32 側でも捨てられるので送らない */
        }
        usblan_encode_header(buf, (uint16_t)n);
        int total = (int)n + USBLAN_HDR_LEN, sent = 0;
        int r = libusb_bulk_transfer(c->dev, c->ep_out, buf, total, &sent, USB_TIMEOUT_MS);
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            g_stop = 1;
        } else if (r != 0 || sent != total) {
            /* 途中までしか送れなかった場合もデバイス側パーサがマジックで再同期する */
            fprintf(stderr, "usblan_tap: bulk OUT: %s (%d/%d)\n", libusb_error_name(r), sent, total);
        }
    }
    return NULL;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-v VID] [-p PID] [-i ifname]\n"
            "  default VID:PID = %04x:%04x, ifname = usblan%%d\n",
            prog, USBLAN_DEFAULT_VID, USBLAN_DEFAULT_PID);
    exit(2);
}

int main(int argc, char **argv)
{
    uint16_t vid = USBLAN_DEFAULT_VID, pid = USBLAN_DEFAULT_PID;
    struct ctx c = { .itf = -1 };
    strncpy(c.ifname, "usblan%d", IFNAMSIZ - 1);

    int opt;
    while ((opt = getopt(argc, argv, "v:p:i:h")) != -1) {
        switch (opt) {
        case 'v': vid = (uint16_t)strtoul(optarg, NULL, 16); break;
        case 'p': pid = (uint16_t)strtoul(optarg, NULL, 16); break;
        case 'i': strncpy(c.ifname, optarg, IFNAMSIZ - 1); break;
        default: usage(argv[0]);
        }
    }

    int r = libusb_init(NULL);
    if (r != 0) {
        die("libusb_init: %s", libusb_error_name(r));
    }
    c.dev = libusb_open_device_with_vid_pid(NULL, vid, pid);
    if (!c.dev) {
        die("device %04x:%04x not found (権限が無い場合は udev ルールか sudo を使う)", vid, pid);
    }
    if ((r = find_vendor_interface(&c)) != 0) {
        die("vendor interface not found: %s", libusb_error_name(r));
    }
    libusb_set_auto_detach_kernel_driver(c.dev, 1);
    if ((r = libusb_claim_interface(c.dev, c.itf)) != 0) {
        die("claim interface %d: %s", c.itf, libusb_error_name(r));
    }

    uint8_t v[2] = {0};
    if ((r = vendor_in(&c, USBLAN_REQ_GET_VERSION, v, sizeof(v))) != 2) {
        die("GET_VERSION failed: %s", r < 0 ? libusb_error_name(r) : "short");
    }
    int ver = v[0] | (v[1] << 8);
    if (ver != USBLAN_PROTO_VERSION) {
        die("protocol version mismatch: device %d, driver %d", ver, USBLAN_PROTO_VERSION);
    }
    uint8_t mac[6];
    if ((r = vendor_in(&c, USBLAN_REQ_GET_MAC, mac, sizeof(mac))) != 6) {
        die("GET_MAC failed: %s", r < 0 ? libusb_error_name(r) : "short");
    }

    c.tap_fd = tap_open(c.ifname);
    if (c.tap_fd < 0) {
        die("open TAP: %s (root か CAP_NET_ADMIN が必要)", strerror(errno));
    }
    if (tap_configure(c.ifname, mac) < 0) {
        die("configure %s: %s", c.ifname, strerror(errno));
    }
    printf("usblan_tap: %04x:%04x itf %d (EP IN 0x%02x / OUT 0x%02x) -> %s "
           "%02x:%02x:%02x:%02x:%02x:%02x\n",
           vid, pid, c.itf, c.ep_in, c.ep_out, c.ifname,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pthread_t th_in, th_tap;
    pthread_create(&th_in, NULL, usb_in_thread, &c);
    pthread_create(&th_tap, NULL, tap_thread, &c);

    /* リンク状態を 1 秒ごとに問い合わせて TAP のキャリアに反映 */
    bool last = false;
    tap_set_carrier(&c, false);
    while (!g_stop) {
        uint8_t link = 0;
        if (vendor_in(&c, USBLAN_REQ_GET_LINK, &link, 1) == 1) {
            c.link_up = link != 0;
            if (c.link_up != last) {
                printf("usblan_tap: Wi-Fi link %s\n", c.link_up ? "up" : "down");
                tap_set_carrier(&c, c.link_up);
                last = c.link_up;
            }
        }
        sleep(1);
    }

    pthread_join(th_in, NULL);
    pthread_join(th_tap, NULL);
    printf("usblan_tap: frames rx %u, resync %u bytes, bad len %u\n",
           c.parser.frames, c.parser.resync_bytes, c.parser.bad_len);

    close(c.tap_fd);
    libusb_release_interface(c.dev, c.itf);
    libusb_close(c.dev);
    libusb_exit(NULL);
    return 0;
}
