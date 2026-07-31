#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "psa/crypto.h"

#define SECURE_CHANNEL_MAX_PACKET 4096

typedef struct {
    int socket_fd;
    psa_key_id_t aes_key;
    uint8_t nonce_prefix[4];
    uint64_t send_counter;
    uint64_t receive_counter;
} secure_channel_t;

esp_err_t secure_channel_server_handshake(
    secure_channel_t *channel,
    int socket_fd,
    const char *preshared_key);

esp_err_t secure_channel_client_handshake(
    secure_channel_t *channel,
    int socket_fd,
    const char *preshared_key);

esp_err_t secure_channel_send(
    secure_channel_t *channel,
    const uint8_t *plaintext,
    size_t plaintext_length);

esp_err_t secure_channel_receive(
    secure_channel_t *channel,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    size_t *plaintext_length);

void secure_channel_close(secure_channel_t *channel);
