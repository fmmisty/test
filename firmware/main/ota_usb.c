#include "ota_usb.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota_usb";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static uint32_t s_size;
static uint32_t s_written;
static esp_err_t s_last_err = ESP_OK;

static esp_err_t fail(esp_err_t err)
{
    s_last_err = err;
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    return err;
}

esp_err_t ota_usb_begin(uint32_t image_size)
{
    if (s_handle) {
        // 前回の転送が途中で切れていたら捨ててやり直す
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    s_written = 0;
    s_size = image_size;
    s_last_err = ESP_OK;

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        ESP_LOGE(TAG, "no OTA partition (check partitions.csv)");
        return fail(ESP_ERR_NOT_FOUND);
    }
    if (image_size == 0 || image_size > s_part->size) {
        ESP_LOGE(TAG, "image size %lu does not fit in %s (%lu)",
                 (unsigned long)image_size, s_part->label, (unsigned long)s_part->size);
        return fail(ESP_ERR_INVALID_SIZE);
    }
    // イメージサイズ分だけ消去する (パーティション全体より速い)
    esp_err_t err = esp_ota_begin(s_part, image_size, &s_handle);
    if (err != ESP_OK) {
        s_handle = 0;
        return fail(err);
    }
    ESP_LOGI(TAG, "update started: %lu bytes -> %s", (unsigned long)image_size, s_part->label);
    return ESP_OK;
}

esp_err_t ota_usb_write(const void *data, size_t len)
{
    if (!s_handle) {
        return fail(ESP_ERR_INVALID_STATE);
    }
    if (s_written + len > s_size) {
        return fail(ESP_ERR_INVALID_SIZE);
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        return fail(err);
    }
    s_written += len;
    return ESP_OK;
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t ota_usb_end(void)
{
    if (!s_handle) {
        return fail(ESP_ERR_INVALID_STATE);
    }
    if (s_written != s_size) {
        ESP_LOGE(TAG, "incomplete image: %lu / %lu", (unsigned long)s_written, (unsigned long)s_size);
        return fail(ESP_ERR_INVALID_SIZE);
    }
    // esp_ota_end() がイメージのヘッダ・チップ種別・チェックサムを検証する
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image verification failed: %s", esp_err_to_name(err));
        s_last_err = err;
        return err;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        s_last_err = err;
        return err;
    }
    ESP_LOGI(TAG, "update done, rebooting into %s", s_part->label);

    // 制御転送の STATUS ステージをホストに返してから再起動する
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "ota_restart" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 500 * 1000);
    }
    return ESP_OK;
}

void ota_usb_status(uint32_t *written, int32_t *last_err)
{
    *written = s_written;
    *last_err = s_last_err;
}

void ota_usb_mark_app_valid(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new firmware booted fine, cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
