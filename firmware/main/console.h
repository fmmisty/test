#pragma once

#include "esp_err.h"

/** UART0 (GPIO43/44 テストパッド) 上の設定用 REPL を開始する。 */
esp_err_t console_start(void);
