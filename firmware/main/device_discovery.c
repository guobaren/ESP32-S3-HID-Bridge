#include "device_discovery.h"

#include <string.h>

static const uint8_t DEVICE_HELLO_SIGNATURE[] = {'H', 'I', 'D', 'B', 'R', 'D', 'G', '2'};

esp_err_t device_discovery_serialize_hello(
    const bridge_frame_t *probe,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (probe == NULL || output == NULL || output_length == NULL ||
        probe->type != BRIDGE_MESSAGE_DEVICE_PROBE ||
        probe->payload_length != DEVICE_PROBE_NONCE_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    bridge_frame_t hello = {
        .version = BRIDGE_PROTOCOL_VERSION,
        .type = BRIDGE_MESSAGE_DEVICE_HELLO,
        .sequence = probe->sequence,
        .payload_length = sizeof(DEVICE_HELLO_SIGNATURE) + DEVICE_PROBE_NONCE_LENGTH,
    };
    memcpy(hello.payload, DEVICE_HELLO_SIGNATURE, sizeof(DEVICE_HELLO_SIGNATURE));
    memcpy(
        hello.payload + sizeof(DEVICE_HELLO_SIGNATURE),
        probe->payload,
        DEVICE_PROBE_NONCE_LENGTH);

    return bridge_frame_serialize(&hello, output, output_capacity, output_length);
}
