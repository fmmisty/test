/*
 * ベンダー制御要求の処理 (protocol/usblan_proto.h)。
 * TinyUSB はベンダー型の要求をクラス構成に関係なく tud_vendor_control_xfer_cb() に渡すので、
 * CDC-NCM ビルドでもベンダークラスビルドでも同じように動く。
 */
#include <string.h>
#include "tinyusb.h"
#include "ota_usb.h"
#include "usb_net.h"
#include "usblan_proto.h"

// DATA ステージが終わるまで有効である必要があるので static
static uint8_t s_buf[USBLAN_OTA_CHUNK];

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static bool device_to_host(uint8_t rhport, tusb_control_request_t const *req)
{
    switch (req->bRequest) {
    case USBLAN_REQ_GET_VERSION:
        // ホスト側ドライバの起動時に呼ばれるので、ここで受信ストリームを仕切り直す
        usb_net_on_driver_start();
        s_buf[0] = USBLAN_PROTO_VERSION & 0xff;
        s_buf[1] = USBLAN_PROTO_VERSION >> 8;
        return tud_control_xfer(rhport, req, s_buf, 2);
    case USBLAN_REQ_GET_MAC:
        usb_net_get_mac(s_buf);
        return tud_control_xfer(rhport, req, s_buf, 6);
    case USBLAN_REQ_GET_LINK:
        s_buf[0] = usb_net_get_link() ? 1 : 0;
        return tud_control_xfer(rhport, req, s_buf, 1);
    case USBLAN_REQ_OTA_STATUS: {
        uint32_t written;
        int32_t err;
        ota_usb_status(&written, &err);
        put_u32(&s_buf[0], written);
        put_u32(&s_buf[4], (uint32_t)err);
        return tud_control_xfer(rhport, req, s_buf, 8);
    }
    default:
        return false;
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    if (req->bmRequestType_bit.direction == TUSB_DIR_IN) {
        return stage == CONTROL_STAGE_SETUP ? device_to_host(rhport, req) : true;
    }

    // ---- Host-to-Device: ファーム更新 ----
    switch (stage) {
    case CONTROL_STAGE_SETUP:
        switch (req->bRequest) {
        case USBLAN_REQ_OTA_BEGIN:
            if (req->wLength != 4) {
                return false;
            }
            return tud_control_xfer(rhport, req, s_buf, 4);   // DATA ステージで受け取る
        case USBLAN_REQ_OTA_DATA:
            if (req->wLength == 0 || req->wLength > sizeof(s_buf)) {
                return false;
            }
            return tud_control_xfer(rhport, req, s_buf, req->wLength);
        case USBLAN_REQ_OTA_END:
            if (ota_usb_end() != ESP_OK) {
                return false;
            }
            return tud_control_status(rhport, req);
        default:
            return false;
        }

    case CONTROL_STAGE_DATA:
        // 受信したデータを処理する。false を返すと STATUS ステージが STALL になりホストに失敗が伝わる
        switch (req->bRequest) {
        case USBLAN_REQ_OTA_BEGIN: {
            uint32_t size = s_buf[0] | (s_buf[1] << 8) | (s_buf[2] << 16) | ((uint32_t)s_buf[3] << 24);
            return ota_usb_begin(size) == ESP_OK;
        }
        case USBLAN_REQ_OTA_DATA:
            // フラッシュの消去・書込みで数十 ms この (TinyUSB) タスクを止めるが、更新中だけなので許容する
            return ota_usb_write(s_buf, req->wLength) == ESP_OK;
        default:
            return true;
        }

    default:
        return true;
    }
}
