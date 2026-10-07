#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"

#define DUAL_STATS_MAX_COUNTERS 64U
#define DUAL_STATS_MAX_QUEUES 24U
#define DUAL_STATS_UNKNOWN_U16 UINT16_MAX
#define DUAL_STATS_UNKNOWN_U32 UINT32_MAX

typedef enum {
    DUAL_STATS_PAGE_COUNTERS = 1,
    DUAL_STATS_PAGE_QUEUES = 2,
} dual_stats_page_kind_t;

typedef enum {
    DUAL_STATS_UNIT_ITEMS = 1,
    DUAL_STATS_UNIT_BYTES = 2,
} dual_stats_unit_t;

typedef enum {
    DUAL_STAT_REPORTS = 1,
    DUAL_STAT_VENDOR_REPORTS,
    DUAL_STAT_INPUT_FAIL,
    DUAL_STAT_CONTROL,
    DUAL_STAT_CONTROL_FAIL,
    DUAL_STAT_CTRL_RETRY,
    DUAL_STAT_URB_SUB,
    DUAL_STAT_URB_OK,
    DUAL_STAT_URB_TO,
    DUAL_STAT_URB_RETRY,
    DUAL_STAT_RECOVER,
    DUAL_STAT_PORT_CYCLE,
    DUAL_STAT_WHEEL,
    DUAL_STAT_CTRL_LAT_MAX_US,
    DUAL_STAT_SLOW10,
    DUAL_STAT_SLOW100,
    DUAL_STAT_VMIN_GAP_US,
    DUAL_STAT_ERRORS,
    DUAL_STAT_MOTION_RX_DX,
    DUAL_STAT_MOTION_RX_DY,
    DUAL_STAT_MOTION_RX_OK,
    DUAL_STAT_MOTION_RX_BADLEN,
    DUAL_STAT_MOTION_RX_BADPARSE,
    DUAL_STAT_HB_OK,
    DUAL_STAT_HB_FAIL,
    DUAL_STAT_CYCLE_REQ,
    DUAL_STAT_CYCLE_ATTEMPT,
    DUAL_STAT_CYCLE_OK,
    DUAL_STAT_CYCLE_FAIL,
    DUAL_STAT_CYCLE_OFF_FAIL,
    DUAL_STAT_CYCLE_ON_FAIL,
    DUAL_STAT_CYCLE_SUPPRESSED,
    DUAL_STAT_INPUT_RESTORED,
    DUAL_STAT_INPUT_MISSING,
    DUAL_STAT_LATE500,
    DUAL_STAT_DETECT_MAX_US,
    DUAL_STAT_NOT_MOUNTED,
    DUAL_STAT_NOT_READY,
    DUAL_STAT_ATTEMPT,
    DUAL_STAT_SUBMITTED,
    DUAL_STAT_FAILED,
    DUAL_STAT_COMPLETE,
    DUAL_STAT_TRANSFER_FAIL,
    DUAL_STAT_PHYSICAL_RX,
    DUAL_STAT_VENDOR_RX,
    DUAL_STAT_VENDOR_SUBMITTED,
    DUAL_STAT_VENDOR_DROPPED,
    DUAL_STAT_GET_TIMEOUTS,
    DUAL_STAT_UART_TX,
    DUAL_STAT_UART_RX,
    DUAL_STAT_UART_RX_BYTES,
    DUAL_STAT_UART_FRAME_ERR,
    DUAL_STAT_UART_RX_OVERFLOW,
    DUAL_STAT_UART_RX_PENDING_PEAK,
    DUAL_STAT_UART_HEARTBEAT_GAP_PEAK_MS,
    DUAL_STAT_UART_RAW_TX_LATENCY_PEAK_US,
    DUAL_STAT_UART_TX_WRITE_FAIL,
    DUAL_STAT_UART_VENDOR_DROPPED,
    DUAL_STAT_UART_MOTION_DROPPED,
    DUAL_STAT_UART_PROFILE_FAIL,
    DUAL_STAT_UART_GONE_RETRY,
    DUAL_STAT_UART_GONE_FAIL,
    DUAL_STAT_UART_BUDGET_EXHAUSTED,
    DUAL_STAT_UART_FIFO_OVF_EVENTS,
    DUAL_STAT_UART_BUFFER_FULL_EVENTS,
    DUAL_STAT_UART_EVENT_RESET_DROPPED,
    DUAL_STAT_P_USB_STATE,
    DUAL_STAT_M_VENDOR_SESSION_STATE,
} dual_stats_counter_id_t;

/* P_BOARD_USB_STATE 的位定义；epoch 放在高32位，结果码占 bits 8..15。 */
#define DUAL_P_USB_STATE_ATTACHED (UINT64_C(1) << 0)
#define DUAL_P_USB_STATE_INSTALLED (UINT64_C(1) << 1)
#define DUAL_P_USB_STATE_CLONE_ACTIVE (UINT64_C(1) << 2)
#define DUAL_P_USB_STATE_MOUNTED (UINT64_C(1) << 3)
#define DUAL_P_USB_STATE_RECONFIGURING (UINT64_C(1) << 4)
#define DUAL_P_USB_STATE_WAITING_HOST (UINT64_C(1) << 5)
#define DUAL_P_USB_STATE_FINAL_ACK_FAILED (UINT64_C(1) << 6)
#define DUAL_P_USB_STATE_DISCONNECT_PENDING (UINT64_C(1) << 7)
#define DUAL_P_USB_STATE_RESULT_SHIFT 8U
#define DUAL_P_USB_STATE_RESULT_MASK (UINT64_C(0xFF) << DUAL_P_USB_STATE_RESULT_SHIFT)
#define DUAL_P_USB_STATE_FINAL_ACK_PENDING (UINT64_C(1) << 16)
#define DUAL_P_USB_STATE_EPOCH_SHIFT 32U

typedef enum {
    DUAL_P_USB_RESULT_UNKNOWN = 0,
    DUAL_P_USB_RESULT_WAIT_HOST = 1,
    DUAL_P_USB_RESULT_FINAL_ACK_PENDING = 2,
    DUAL_P_USB_RESULT_MOUNTED_ACKED = 3,
    DUAL_P_USB_RESULT_INSTALL_FAILED = 4,
    DUAL_P_USB_RESULT_FINAL_ACK_FAILED = 5,
    DUAL_P_USB_RESULT_CANCELED = 6,
} dual_p_usb_result_t;

/* M_VENDOR_SESSION_STATE：低位为状态，高32位为当前UART1会话epoch。 */
#define DUAL_M_VENDOR_SESSION_ACTIVE (UINT64_C(1) << 0)
#define DUAL_M_VENDOR_SESSION_FIRST_REQUEST (UINT64_C(1) << 1)
#define DUAL_M_VENDOR_SESSION_PEER_CURRENT (UINT64_C(1) << 2)
#define DUAL_M_VENDOR_SESSION_WAITING_HOST (UINT64_C(1) << 3)
#define DUAL_M_VENDOR_SESSION_EPOCH_SHIFT 32U

typedef enum {
    DUAL_STATS_QUEUE_HOST_HID_EVENT = 1,
    DUAL_STATS_QUEUE_HOST_HID_REPORT,
    DUAL_STATS_QUEUE_HOST_HID_CONTROL,
    DUAL_STATS_QUEUE_UART1_TX,
    DUAL_STATS_QUEUE_UART1_MOTION_TX,
    DUAL_STATS_QUEUE_UART1_SAFETY_TX,
    DUAL_STATS_QUEUE_UART1_SOFTWARE_TX,
    DUAL_STATS_QUEUE_UART1_VENDOR_TX,
    DUAL_STATS_QUEUE_UART1_EVENT,
    DUAL_STATS_QUEUE_PC_VENDOR_INPUT,
    DUAL_STATS_QUEUE_PC_MOTION_INPUT,
    DUAL_STATS_QUEUE_PC_VENDOR_CONTROL,
    DUAL_STATS_QUEUE_UART1_RX_RING_BYTES,
} dual_stats_queue_id_t;

typedef struct {
    uint8_t id;
    uint8_t value_type;
    uint64_t value;
} dual_stats_counter_t;

typedef struct {
    uint8_t id;
    uint8_t unit;
    uint16_t capacity;
    uint16_t depth;
    uint16_t peak;
    uint32_t received;
    uint32_t rejected;
    uint32_t dropped;
} dual_stats_queue_t;

typedef struct {
    uint8_t role;
    uint32_t uptime_ms;
    uint8_t counter_count;
    uint8_t queue_count;
    bool overflow;
    dual_stats_counter_t counters[DUAL_STATS_MAX_COUNTERS];
    dual_stats_queue_t queues[DUAL_STATS_MAX_QUEUES];
} dual_stats_snapshot_t;

void dual_stats_snapshot_add_counter(dual_stats_snapshot_t *snapshot,
                                     uint8_t id, uint64_t value);
void dual_stats_snapshot_add_signed_counter(dual_stats_snapshot_t *snapshot,
                                            uint8_t id, int64_t value);
void dual_stats_snapshot_add_queue(dual_stats_snapshot_t *snapshot,
                                   uint8_t id, uint16_t capacity,
                                   uint16_t depth, uint16_t peak,
                                   uint32_t received, uint32_t rejected,
                                   uint32_t dropped);
void dual_stats_snapshot_add_queue_unit(dual_stats_snapshot_t *snapshot,
                                        uint8_t id, uint8_t unit,
                                        uint16_t capacity, uint16_t depth,
                                        uint16_t peak, uint32_t received,
                                        uint32_t rejected, uint32_t dropped);
void dual_stats_snapshot_add_queue_unknown(dual_stats_snapshot_t *snapshot,
                                           uint8_t id, uint16_t capacity,
                                           uint16_t depth, uint16_t peak);
void dual_stats_snapshot_collect(uint8_t role, dual_stats_snapshot_t *snapshot);
bool dual_stats_snapshot_serialize_page(const dual_stats_snapshot_t *snapshot,
                                        uint8_t kind, uint8_t page_index,
                                        uint8_t page_count, uint8_t status,
                                        uint8_t *payload, size_t capacity,
                                        uint8_t *payload_length);
