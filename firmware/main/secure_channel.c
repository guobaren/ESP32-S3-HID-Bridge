#include "secure_channel.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_random.h"
#include "lwip/sockets.h"

#define HANDSHAKE_NONCE_LENGTH 16
#define PROOF_LENGTH 32
#define TAG_LENGTH 16
#define HEADER_LENGTH 10

static const uint8_t SERVER_MAGIC[] = {'H', 'B', 'S', '1'};
static const uint8_t CLIENT_MAGIC[] = {'H', 'B', 'C', '1'};
static const uint8_t ACK_MAGIC[] = {'H', 'B', 'A', '1'};

static esp_err_t send_exact(int socket_fd, const uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        int sent = send(socket_fd, data + offset, length - offset, 0);
        if (sent <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)sent;
    }
    return ESP_OK;
}

static esp_err_t receive_exact(int socket_fd, uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        int received = recv(socket_fd, data + offset, length - offset, 0);
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)received;
    }
    return ESP_OK;
}

static void write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void write_u64_be(uint8_t *data, uint64_t value)
{
    for (int index = 7; index >= 0; --index) {
        data[index] = (uint8_t)value;
        value >>= 8;
    }
}

static uint64_t read_u64_be(const uint8_t *data)
{
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

static esp_err_t hmac_sha256(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t output[PROOF_LENGTH])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_length * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attributes, key, key_length, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    size_t output_length = 0;
    status = psa_mac_compute(
        key_id,
        PSA_ALG_HMAC(PSA_ALG_SHA_256),
        data,
        data_length,
        output,
        PROOF_LENGTH,
        &output_length);
    psa_destroy_key(key_id);
    return status == PSA_SUCCESS && output_length == PROOF_LENGTH ? ESP_OK : ESP_FAIL;
}

static esp_err_t compute_proof(
    const char *preshared_key,
    const char *label,
    const uint8_t server_nonce[HANDSHAKE_NONCE_LENGTH],
    const uint8_t client_nonce[HANDSHAKE_NONCE_LENGTH],
    uint8_t output[PROOF_LENGTH])
{
    size_t key_length = strlen(preshared_key);
    size_t label_length = strlen(label);
    if (key_length < 16 || label_length > 16) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t material[16 + HANDSHAKE_NONCE_LENGTH * 2] = {0};
    memcpy(material, label, label_length);
    memcpy(material + label_length, server_nonce, HANDSHAKE_NONCE_LENGTH);
    memcpy(material + label_length + HANDSHAKE_NONCE_LENGTH, client_nonce, HANDSHAKE_NONCE_LENGTH);
    return hmac_sha256(
        (const uint8_t *)preshared_key,
        key_length,
        material,
        label_length + HANDSHAKE_NONCE_LENGTH * 2,
        output);
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

static esp_err_t finish_handshake(
    secure_channel_t *channel,
    int socket_fd,
    const char *preshared_key,
    const uint8_t server_nonce[HANDSHAKE_NONCE_LENGTH],
    const uint8_t client_nonce[HANDSHAKE_NONCE_LENGTH])
{
    uint8_t key[PROOF_LENGTH];
    uint8_t nonce_material[PROOF_LENGTH];
    ESP_RETURN_ON_ERROR(
        compute_proof(preshared_key, "key-v1", server_nonce, client_nonce, key),
        "secure_channel",
        "派生会话密钥失败");
    ESP_RETURN_ON_ERROR(
        compute_proof(preshared_key, "nonce-v1", server_nonce, client_nonce, nonce_material),
        "secure_channel",
        "派生随机数前缀失败");

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attributes, key, sizeof(key), &key_id);
    psa_reset_key_attributes(&attributes);
    memset(key, 0, sizeof(key));
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    memset(channel, 0, sizeof(*channel));
    channel->socket_fd = socket_fd;
    channel->aes_key = key_id;
    memcpy(channel->nonce_prefix, nonce_material, sizeof(channel->nonce_prefix));
    memset(nonce_material, 0, sizeof(nonce_material));
    return ESP_OK;
}

esp_err_t secure_channel_server_handshake(
    secure_channel_t *channel,
    int socket_fd,
    const char *preshared_key)
{
    if (channel == NULL || preshared_key == NULL || strlen(preshared_key) < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, "secure_channel", "PSA 初始化失败");

    uint8_t server_nonce[HANDSHAKE_NONCE_LENGTH];
    uint8_t client_nonce[HANDSHAKE_NONCE_LENGTH];
    uint8_t hello[sizeof(SERVER_MAGIC) + HANDSHAKE_NONCE_LENGTH];
    uint8_t response[sizeof(CLIENT_MAGIC) + HANDSHAKE_NONCE_LENGTH + PROOF_LENGTH];
    esp_fill_random(server_nonce, sizeof(server_nonce));
    memcpy(hello, SERVER_MAGIC, sizeof(SERVER_MAGIC));
    memcpy(hello + sizeof(SERVER_MAGIC), server_nonce, sizeof(server_nonce));
    ESP_RETURN_ON_ERROR(send_exact(socket_fd, hello, sizeof(hello)), "secure_channel", "发送握手失败");
    ESP_RETURN_ON_ERROR(receive_exact(socket_fd, response, sizeof(response)), "secure_channel", "接收握手失败");
    if (memcmp(response, CLIENT_MAGIC, sizeof(CLIENT_MAGIC)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(client_nonce, response + sizeof(CLIENT_MAGIC), sizeof(client_nonce));

    uint8_t expected_proof[PROOF_LENGTH];
    ESP_RETURN_ON_ERROR(
        compute_proof(preshared_key, "client-v1", server_nonce, client_nonce, expected_proof),
        "secure_channel",
        "计算客户端证明失败");
    if (!constant_time_equal(
            response + sizeof(CLIENT_MAGIC) + HANDSHAKE_NONCE_LENGTH,
            expected_proof,
            PROOF_LENGTH)) {
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t ack[sizeof(ACK_MAGIC) + PROOF_LENGTH];
    memcpy(ack, ACK_MAGIC, sizeof(ACK_MAGIC));
    ESP_RETURN_ON_ERROR(
        compute_proof(preshared_key, "server-v1", server_nonce, client_nonce, ack + sizeof(ACK_MAGIC)),
        "secure_channel",
        "计算服务端证明失败");
    ESP_RETURN_ON_ERROR(send_exact(socket_fd, ack, sizeof(ack)), "secure_channel", "发送认证结果失败");
    return finish_handshake(channel, socket_fd, preshared_key, server_nonce, client_nonce);
}

esp_err_t secure_channel_client_handshake(
    secure_channel_t *channel,
    int socket_fd,
    const char *preshared_key)
{
    if (channel == NULL || preshared_key == NULL || strlen(preshared_key) < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, "secure_channel", "PSA 初始化失败");

    uint8_t hello[sizeof(SERVER_MAGIC) + HANDSHAKE_NONCE_LENGTH];
    uint8_t client_nonce[HANDSHAKE_NONCE_LENGTH];
    ESP_RETURN_ON_ERROR(receive_exact(socket_fd, hello, sizeof(hello)), "secure_channel", "接收握手失败");
    if (memcmp(hello, SERVER_MAGIC, sizeof(SERVER_MAGIC)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t *server_nonce = hello + sizeof(SERVER_MAGIC);
    esp_fill_random(client_nonce, sizeof(client_nonce));

    uint8_t response[sizeof(CLIENT_MAGIC) + HANDSHAKE_NONCE_LENGTH + PROOF_LENGTH];
    memcpy(response, CLIENT_MAGIC, sizeof(CLIENT_MAGIC));
    memcpy(response + sizeof(CLIENT_MAGIC), client_nonce, sizeof(client_nonce));
    ESP_RETURN_ON_ERROR(
        compute_proof(
            preshared_key,
            "client-v1",
            server_nonce,
            client_nonce,
            response + sizeof(CLIENT_MAGIC) + HANDSHAKE_NONCE_LENGTH),
        "secure_channel",
        "计算客户端证明失败");
    ESP_RETURN_ON_ERROR(send_exact(socket_fd, response, sizeof(response)), "secure_channel", "发送握手失败");

    uint8_t ack[sizeof(ACK_MAGIC) + PROOF_LENGTH];
    ESP_RETURN_ON_ERROR(receive_exact(socket_fd, ack, sizeof(ack)), "secure_channel", "接收认证结果失败");
    if (memcmp(ack, ACK_MAGIC, sizeof(ACK_MAGIC)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t expected_proof[PROOF_LENGTH];
    ESP_RETURN_ON_ERROR(
        compute_proof(preshared_key, "server-v1", server_nonce, client_nonce, expected_proof),
        "secure_channel",
        "计算服务端证明失败");
    if (!constant_time_equal(ack + sizeof(ACK_MAGIC), expected_proof, PROOF_LENGTH)) {
        return ESP_ERR_INVALID_CRC;
    }
    return finish_handshake(channel, socket_fd, preshared_key, server_nonce, client_nonce);
}

static void build_nonce(const secure_channel_t *channel, uint64_t counter, uint8_t nonce[12])
{
    memcpy(nonce, channel->nonce_prefix, sizeof(channel->nonce_prefix));
    write_u64_be(nonce + 4, counter);
}

esp_err_t secure_channel_send(
    secure_channel_t *channel,
    const uint8_t *plaintext,
    size_t plaintext_length)
{
    if (channel == NULL || plaintext == NULL || plaintext_length == 0 ||
        plaintext_length > SECURE_CHANNEL_MAX_PACKET || plaintext_length > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[HEADER_LENGTH];
    uint64_t counter = ++channel->send_counter;
    write_u16_be(header, (uint16_t)plaintext_length);
    write_u64_be(header + 2, counter);
    uint8_t nonce[12];
    build_nonce(channel, counter, nonce);

    uint8_t encrypted[SECURE_CHANNEL_MAX_PACKET + TAG_LENGTH];
    size_t encrypted_length = 0;
    psa_status_t status = psa_aead_encrypt(
        channel->aes_key,
        PSA_ALG_GCM,
        nonce,
        sizeof(nonce),
        header,
        sizeof(header),
        plaintext,
        plaintext_length,
        encrypted,
        sizeof(encrypted),
        &encrypted_length);
    if (status != PSA_SUCCESS || encrypted_length != plaintext_length + TAG_LENGTH) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(send_exact(channel->socket_fd, header, sizeof(header)), "secure_channel", "发送包头失败");
    return send_exact(channel->socket_fd, encrypted, encrypted_length);
}

esp_err_t secure_channel_receive(
    secure_channel_t *channel,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    size_t *plaintext_length)
{
    if (channel == NULL || plaintext == NULL || plaintext_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t header[HEADER_LENGTH];
    ESP_RETURN_ON_ERROR(receive_exact(channel->socket_fd, header, sizeof(header)), "secure_channel", "接收包头失败");
    size_t length = read_u16_be(header);
    uint64_t counter = read_u64_be(header + 2);
    if (length == 0 || length > SECURE_CHANNEL_MAX_PACKET || length > plaintext_capacity ||
        counter != channel->receive_counter + 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t encrypted[SECURE_CHANNEL_MAX_PACKET + TAG_LENGTH];
    ESP_RETURN_ON_ERROR(
        receive_exact(channel->socket_fd, encrypted, length + TAG_LENGTH),
        "secure_channel",
        "接收密文失败");
    uint8_t nonce[12];
    build_nonce(channel, counter, nonce);
    size_t output_length = 0;
    psa_status_t status = psa_aead_decrypt(
        channel->aes_key,
        PSA_ALG_GCM,
        nonce,
        sizeof(nonce),
        header,
        sizeof(header),
        encrypted,
        length + TAG_LENGTH,
        plaintext,
        plaintext_capacity,
        &output_length);
    if (status != PSA_SUCCESS || output_length != length) {
        return ESP_ERR_INVALID_CRC;
    }
    channel->receive_counter = counter;
    *plaintext_length = output_length;
    return ESP_OK;
}

void secure_channel_close(secure_channel_t *channel)
{
    if (channel == NULL) {
        return;
    }
    if (channel->aes_key != 0) {
        psa_destroy_key(channel->aes_key);
    }
    memset(channel, 0, sizeof(*channel));
    channel->socket_fd = -1;
}
