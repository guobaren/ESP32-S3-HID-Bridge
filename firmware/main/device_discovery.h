#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"
#include "esp_err.h"

#define DEVICE_PROBE_NONCE_LENGTH 8

esp_err_t device_discovery_serialize_hello(
    const bridge_frame_t *probe,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
