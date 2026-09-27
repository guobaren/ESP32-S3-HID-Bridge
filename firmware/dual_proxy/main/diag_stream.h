#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#define DUAL_DIAG_EVENT_DATA_MAX 64U

/* UART driver must be installed before this starts the low-priority TX worker. */
esp_err_t dual_diag_stream_start(uart_port_t uart_port);
void dual_diag_stream_set_enabled(bool enabled);
void dual_diag_stream_record(uint8_t source, uint8_t kind,
                             const uint8_t *data, uint8_t length);
void dual_diag_stream_send_status(uint16_t sequence);
