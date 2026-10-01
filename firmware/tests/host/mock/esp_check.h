#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { esp_err_t e_ = (x); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, msg) do { if (!(a)) return (err); } while (0)
