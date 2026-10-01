#pragma once
#include <stddef.h>
#include "driver/spi_common.h"
typedef struct spi_device_t *spi_device_handle_t;
typedef struct { int clock_speed_hz; int mode; int spics_io_num; int queue_size; } spi_device_interface_config_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *h);
esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t);
