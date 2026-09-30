#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * USB のベンダー制御要求で受け取ったイメージを OTA パーティションへ書き込む。
 * 呼び出しはすべて TinyUSB タスクから (usb_ctrl.c)。
 */
esp_err_t ota_usb_begin(uint32_t image_size);
esp_err_t ota_usb_write(const void *data, size_t len);
/** 検証して起動パーティションを切り替え、少し待ってから再起動する */
esp_err_t ota_usb_end(void);
void ota_usb_status(uint32_t *written, int32_t *last_err);

/** 起動が正常に進んだら呼ぶ。呼ばずに再起動すると前のファームに戻る (ロールバック) */
void ota_usb_mark_app_valid(void);
