#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"
#include "esp_err.h"

#define DUAL_USB_STATE_WAITING 0
#define DUAL_USB_STATE_MOUNTED 1
#define DUAL_USB_STATE_HID_CONNECTED 2
#define DUAL_USB_STATE_DISCONNECTED 3
#define DUAL_USB_STATE_ERROR 4

typedef void (*dual_link_frame_callback_t)(const dual_frame_t *frame);
typedef void (*dual_link_fault_callback_t)(void);

esp_err_t dual_uart1_start(
    uint8_t role,
    const uint8_t node_id[6],
    dual_link_frame_callback_t frame_callback,
    dual_link_fault_callback_t fault_callback);
/* Role may move exactly once from UNRESOLVED to a locked role per boot. */
esp_err_t dual_uart1_lock_role(uint8_t role);
void dual_uart1_set_usb_state(uint8_t usb_state);
bool dual_uart1_peer_online(void);
uint32_t dual_uart1_generation(void);
uint32_t dual_uart1_peer_generation(void);
/* P侧只有在本地USB teardown完成后才能开放本会话的单次Profile申请。 */
void dual_uart1_set_profile_request_ready(bool ready);
/* Drop queued HID++/raw-HID traffic from the previous cloned device session. */
void dual_uart1_cancel_vendor_hid_session(void);
esp_err_t dual_uart1_send_mouse(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
esp_err_t dual_uart1_send_release(uint8_t reason);
esp_err_t dual_uart1_send_software_mouse(const uint8_t *payload, size_t length);
esp_err_t dual_uart1_send_software_release(void);
/* Begins a peer-acknowledged detach barrier and cancels any older cached Profile. */
esp_err_t dual_uart1_send_device_gone(uint8_t reason);
esp_err_t dual_uart1_send_profile_ack(uint32_t transfer_id, uint32_t crc32, uint8_t status);
/*
 * 电脑侧回传的最终确认：同时携带 M/P 双方 generation。
 * expected_peer_generation 必须等于当前对端会话，否则返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t dual_uart1_send_profile_ack_for_generation(
    uint32_t transfer_id,
    uint32_t crc32,
    uint8_t status,
    uint32_t expected_peer_generation);
esp_err_t dual_uart1_send_flow_ack(
    uint8_t acknowledged_type, uint32_t flow_id, uint8_t status);
esp_err_t dual_uart1_send_flow_ack_for_generation(
    uint8_t acknowledged_type,
    uint32_t flow_id,
    uint8_t status,
    uint32_t expected_peer_generation);
esp_err_t dual_uart1_send_raw_hid_input(
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length);
esp_err_t dual_uart1_send_hid_set_report(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    const uint8_t *data,
    size_t data_length);
esp_err_t dual_uart1_send_hid_get_request(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t requested_length);
esp_err_t dual_uart1_send_hid_get_response(
    uint16_t transaction_id,
    uint8_t status,
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length);
/* Copy a complete serialized HID profile for bounded, fair UART1 streaming. */
esp_err_t dual_uart1_queue_profile(
    const uint8_t *blob,
    size_t length,
    uint32_t crc32);
void dual_uart1_cancel_profile(void);
void dual_uart1_deferred_refresh(void);

/* Profile 传输（OFFER→最终 ACK）是否在途：在途期间移动报文应让路，
 * 保证鼠标侧板自己的枚举/采集与整份传输不被 1 kHz 转发干扰。 */
bool dual_uart1_profile_transfer_in_flight(void);

/* 克隆是否已完成（鼠标侧：收到成功 Profile ACK 之后为真）。 */
bool dual_uart1_clone_ready(void);
