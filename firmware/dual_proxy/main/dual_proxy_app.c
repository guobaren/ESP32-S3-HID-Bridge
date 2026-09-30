#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

#include "bridge_protocol.h"
#include "dual_proxy_runtime_config.h"
#include "dual_proxy_app.h"
#include "dual_status_led.h"
#include "hid_host_mouse.h"
#include "hid_device_profile.h"
#include "link_recovery_logic.h"
#include "onboard_log.h"
#include "pc_hid_output.h"
#include "usb_cdc_control.h"
#include "uart0_control.h"
#include "uart1_link.h"

#define USB_ROLE_PROBE_INTERVAL_MS 3000
#define PROFILE_TRANSFER_TIMEOUT_MS 1000

typedef enum {
    USB_PROBE_NONE = 0,
    USB_PROBE_DEVICE,
    USB_PROBE_HOST,
} usb_probe_mode_t;

static const char *TAG = "dual_proxy";
static uint8_t s_node_id[6];
static volatile uint8_t s_role = DUAL_ROLE_UNRESOLVED;
static volatile uint8_t s_peer_role_claim = DUAL_ROLE_UNRESOLVED;
static usb_probe_mode_t s_probe_mode = USB_PROBE_NONE;
static usb_probe_mode_t s_last_probe_mode = USB_PROBE_NONE;
static int64_t s_probe_started_us;
static int64_t s_role_retry_after_us;
static bool s_host_started;
static bool s_role_initialized;
static bool s_peer_mouse_usb_state_initialized;
static uint8_t s_peer_mouse_usb_state = DUAL_USB_STATE_WAITING;
static bool s_peer_mouse_generation_initialized;
static uint32_t s_peer_mouse_generation;
static hid_profile_receiver_t s_profile_receiver;
static int64_t s_profile_started_us;
static bool s_profile_offer_valid;
static uint32_t s_profile_offer_transfer_id;
static uint32_t s_profile_offer_crc32;
static bool s_profile_schedule_ok;
/*
 * Profile CRC 复用（2026-09-28，按用户决策启用）：本次 clone 会话的 CRC32 与本机
 * 已挂载克隆完全一致时置位——OFFER 阶段不排清理屏障，Profile 收全后不卸载不重装。
 * 每次 OFFER 都会重新赋值，publish 前还会用 dual_pc_hid_installed_profile_matches()
 * 复核，因此不会因为残留标志走进错误路径。
 */
static bool s_profile_reuse_pending;
static uint32_t s_profile_reuse_hits;
/* 鼠标侧对电脑侧本次 Profile 申请的受理状态（按 flow ID 去重）。 */
static uint32_t s_peer_request_flow_id;
static uint32_t s_peer_request_generation;
static bool s_peer_request_accepted;
static bool s_peer_request_waiting_device;
/* 已受理的克隆提议（按 flow ID 去重），重复 OFFER 不得重置接收状态。 */
static uint32_t s_offer_seen_generation;
static uint32_t s_offer_seen_flow_id;
static bool s_offer_seen_valid;
static uint32_t s_duplicate_offer_ignored;
static uint32_t s_mouse_input_errors;
static uint32_t s_duplicate_commit_replays;
static volatile bool s_manual_profile;

static void on_profile_published(const hid_device_profile_t *profile, void *context);

static int16_t read_i16_le(const uint8_t *value)
{
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *value)
{
    return (uint32_t)value[0] |
        ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) |
        ((uint32_t)value[3] << 24);
}

static void on_software_release(void)
{
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        if (dual_uart1_send_software_release() != ESP_OK) {
            ESP_LOGW(TAG, "软件release无法送入板间UART");
        }
    } else if (s_role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_software_release();
    }
}

static void on_software_frame(const dual_frame_t *frame)
{
    if (frame == NULL || frame->type != DUAL_MESSAGE_MOUSE_REPORT ||
        (frame->payload_length != 7 && frame->payload_length != 8)) {
        return;
    }
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        /* 诊断注入不依赖物理鼠标是否在位；后续链路/克隆门控如实决定能否送达。 */
        if (dual_uart1_send_software_mouse(frame->payload, frame->payload_length) != ESP_OK) {
            ESP_LOGW(TAG, "软件MouseReport无法送入板间UART，发送release");
            (void)dual_uart1_send_software_release();
        }
    } else if (s_role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_software_report(
            frame->payload[0],
            read_i16_le(&frame->payload[1]),
            read_i16_le(&frame->payload[3]),
            (int8_t)frame->payload[5],
            (int8_t)frame->payload[6],
            frame->payload_length == 8U ? frame->payload[7] : 0U);
    }
}

static void pc_device_gone(
    const char *reason, uint32_t peer_generation, uint32_t event_id)
{
    ESP_LOGW(TAG, "物理鼠标不可用，断开接收端USB：%s", reason);
    dual_uart1_set_profile_request_ready(false);
    const esp_err_t result = dual_pc_hid_schedule_disconnect(
        peer_generation, event_id,
        event_id != 0U ? DUAL_MESSAGE_DEVICE_GONE : 0U,
        event_id);
    if (result != ESP_OK) {
        dual_status_led_set_flow_error(true);
        ESP_LOGE(TAG, "接收端USB断开排队失败：%s", esp_err_to_name(result));
        if (event_id != 0U) {
            (void)dual_uart1_send_flow_ack(
                DUAL_MESSAGE_DEVICE_GONE, event_id, DUAL_FLOW_STATUS_FAILED);
        }
    }
}

void dual_proxy_set_manual_profile(bool enabled)
{
    if (s_role != DUAL_ROLE_PC_DEVICE) {
        return;
    }
    const bool previous = __atomic_exchange_n(&s_manual_profile, enabled,
                                               __ATOMIC_ACQ_REL);
    if (enabled) {
        dual_uart1_set_profile_request_ready(false);
        ESP_LOGW(TAG, "手动Profile模式已启用：暂停接受M侧Profile与输入");
    } else if (previous) {
        ESP_LOGW(TAG, "手动Profile模式已关闭：清理克隆并重新申请真实Profile");
        pc_device_gone("退出手动Profile模式", dual_uart1_peer_generation(), 0U);
    }
}

bool dual_proxy_manual_profile_enabled(void)
{
    return __atomic_load_n(&s_manual_profile, __ATOMIC_ACQUIRE);
}

static void on_link_fault(void)
{
    if (s_role == DUAL_ROLE_PC_DEVICE) {
        if (__atomic_load_n(&s_manual_profile, __ATOMIC_ACQUIRE)) {
            return;
        }
        const uint32_t peer_generation = dual_uart1_peer_generation();
        s_peer_mouse_usb_state_initialized = false;
        s_peer_mouse_usb_state = DUAL_USB_STATE_WAITING;
        /* 保留最后确认的 M generation。UART 超时期间 M 可能复位并生成
         * 新会话；收到新 HELLO 时必须据此识别 generation 变化，重新完成
         * P 本地 USB 清理后再开放唯一 PROFILE_REQUEST。 */
        hid_profile_receiver_init(&s_profile_receiver, on_profile_published, &s_profile_receiver);
        s_profile_started_us = 0;
        s_profile_offer_valid = false;
        pc_device_gone("板间UART超时或故障", peer_generation, 0U);
    } else if (s_role == DUAL_ROLE_MOUSE_HOST) {
        /* 链路故障：克隆已失效，立刻关回"未就绪"，移动重新被抑制，
         * 直到下一次成功的 Profile ACK。 */
        dual_uart1_set_usb_state(DUAL_USB_STATE_DISCONNECTED);
        dual_hid_host_clear_control_queue();
    }
}

static void on_link_frame(const dual_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    if (s_role == DUAL_ROLE_PC_DEVICE &&
        __atomic_load_n(&s_manual_profile, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (frame->type == DUAL_MESSAGE_LINK_HELLO) {
        if (s_role == DUAL_ROLE_UNRESOLVED && frame->payload_length == 12U &&
            frame->payload[0] != DUAL_ROLE_UNRESOLVED) {
            s_peer_role_claim = frame->payload[0];
        } else if (frame->payload_length == 12U &&
                   frame->payload[0] == DUAL_ROLE_UNRESOLVED) {
            s_peer_role_claim = DUAL_ROLE_UNRESOLVED;
        }
        if (s_role == DUAL_ROLE_PC_DEVICE && frame->payload_length == 12U &&
            (frame->payload[0] == DUAL_ROLE_MOUSE_HOST ||
             frame->payload[0] == DUAL_ROLE_UNRESOLVED)) {
            const uint8_t peer_usb_state = frame->payload[1];
            const uint32_t peer_generation =
                (uint32_t)frame->payload[8] |
                ((uint32_t)frame->payload[9] << 8) |
                ((uint32_t)frame->payload[10] << 16) |
                ((uint32_t)frame->payload[11] << 24);
            const bool profile_invalidated = dual_peer_profile_invalidated(
                s_peer_mouse_generation_initialized,
                s_peer_mouse_generation,
                peer_generation,
                s_peer_mouse_usb_state_initialized,
                s_peer_mouse_usb_state,
                peer_usb_state);
            s_peer_mouse_usb_state = peer_usb_state;
            s_peer_mouse_usb_state_initialized = true;
            s_peer_mouse_generation = peer_generation;
            s_peer_mouse_generation_initialized = true;
            if (profile_invalidated) {
                /* 首次 WAITING 是启动状态；generation 改变或已连接状态
                 * 变为不可用才表示旧 Profile 需要撤销。 */
                hid_profile_receiver_init(&s_profile_receiver, on_profile_published, &s_profile_receiver);
                s_profile_started_us = 0;
                s_profile_offer_valid = false;
                s_peer_request_accepted = false;
                s_peer_request_waiting_device = false;
                s_peer_request_flow_id = 0U;
                s_peer_request_generation = 0U;
                s_offer_seen_valid = false;
                s_offer_seen_flow_id = 0U;
                s_offer_seen_generation = 0U;
                pc_device_gone("鼠标侧会话或USB状态已失效",
                               peer_generation, 0U);
            }
        }
        return;
    }
    if (s_role == DUAL_ROLE_UNRESOLVED) {
        return;
    }
    if (s_role == DUAL_ROLE_PC_DEVICE) {
        const int64_t now_us = esp_timer_get_time();
        if (s_profile_started_us != 0 &&
            now_us - s_profile_started_us > PROFILE_TRANSFER_TIMEOUT_MS * 1000LL) {
            hid_profile_receiver_init(&s_profile_receiver, on_profile_published, &s_profile_receiver);
            s_profile_started_us = 0;
            pc_device_gone("Profile传输超时", dual_uart1_peer_generation(), 0U);
            if (frame->type != DUAL_MESSAGE_PROFILE_BEGIN) {
                return;
            }
        }
        if (frame->type == DUAL_MESSAGE_PROFILE_BEGIN ||
            frame->type == DUAL_MESSAGE_PROFILE_CHUNK ||
            frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
            if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
                s_profile_schedule_ok = false;
            }
            const bool accepted = hid_profile_receiver_accept_frame(&s_profile_receiver, frame);
            if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT &&
                accepted && s_profile_receiver.last_commit_was_duplicate) {
                /* 同一 transfer 的重放只补发数据接收确认；只有 USB 确实挂载
                 * 才能补发最终成功，安装中绝不能把已发布 Profile 当成已挂载。 */
                const uint32_t transfer_id = read_u32_le(&frame->payload[0]);
                const link_profile_replay_result_t profile_result =
                    dual_pc_hid_profile_result(
                        s_profile_receiver.published_transfer_id,
                        s_profile_receiver.published_crc32);
                s_profile_started_us = 0;
                s_profile_schedule_ok = true;
                ++s_duplicate_commit_replays;
                if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_COMMIT,
                        transfer_id, DUAL_FLOW_STATUS_ACCEPTED) != ESP_OK) {
                    dual_status_led_set_flow_error(true);
                }
                if (profile_result != LINK_PROFILE_REPLAY_PENDING) {
                    const esp_err_t replay_ack = dual_uart1_send_profile_ack_for_generation(
                        s_profile_receiver.published_transfer_id,
                        s_profile_receiver.published_crc32,
                        profile_result == LINK_PROFILE_REPLAY_MOUNTED ? 0U : 1U,
                        dual_uart1_peer_generation());
                    if (replay_ack != ESP_OK) {
                        dual_status_led_set_flow_error(true);
                        ESP_LOGW(TAG, "重复COMMIT的最终确认无法排队：%s",
                                 esp_err_to_name(replay_ack));
                    }
                }
                ESP_LOGW(TAG, "收到重复COMMIT：安装结果=%u，仅补发已有确认；"
                         "transfer=%" PRIu32 " replays=%" PRIu32,
                         (unsigned)profile_result, transfer_id, s_duplicate_commit_replays);
                return;
            }
            if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT &&
                frame->payload_length >= 4U) {
                const uint32_t transfer_id = read_u32_le(&frame->payload[0]);
                if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_COMMIT,
                        transfer_id, accepted && s_profile_schedule_ok ?
                        DUAL_FLOW_STATUS_ACCEPTED :
                        DUAL_FLOW_STATUS_FAILED) != ESP_OK) {
                    dual_status_led_set_flow_error(true);
                }
            }
            if (!accepted || (frame->type == DUAL_MESSAGE_PROFILE_COMMIT &&
                              !s_profile_schedule_ok)) {
                s_profile_started_us = 0;
                pc_device_gone("Profile校验失败", dual_uart1_peer_generation(), 0U);
            } else if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
                s_profile_started_us = 0;
            } else {
                /* 超时按相邻分片的静默时长计算，而不是限制整个低优先级传输时长。 */
                s_profile_started_us = now_us;
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_DEVICE_GONE &&
            frame->payload_length == DUAL_LINK_DEVICE_GONE_LENGTH) {
            const uint32_t sender_generation = read_u32_le(&frame->payload[
                DUAL_LINK_DEVICE_GONE_SENDER_GENERATION_OFFSET]);
            const uint32_t target_generation = read_u32_le(&frame->payload[
                DUAL_LINK_DEVICE_GONE_TARGET_GENERATION_OFFSET]);
            const uint32_t event_id = read_u32_le(&frame->payload[
                DUAL_LINK_DEVICE_GONE_EVENT_ID_OFFSET]);
            if (event_id == 0U || target_generation != dual_uart1_generation() ||
                sender_generation != dual_uart1_peer_generation()) {
                dual_status_led_set_flow_error(true);
                if (event_id != 0U) {
                    (void)dual_uart1_send_flow_ack(
                        DUAL_MESSAGE_DEVICE_GONE, event_id,
                        DUAL_FLOW_STATUS_FAILED);
                }
                ESP_LOGW(TAG, "忽略失效的鼠标拔出事件：event=%" PRIu32
                         " sender_generation=%" PRIu32 " target_generation=%" PRIu32,
                         event_id, sender_generation, target_generation);
                return;
            }
            s_peer_mouse_usb_state_initialized = true;
            s_peer_mouse_usb_state = DUAL_USB_STATE_DISCONNECTED;
            hid_profile_receiver_init(&s_profile_receiver, on_profile_published, &s_profile_receiver);
            s_profile_started_us = 0;
            s_profile_offer_valid = false;
            s_peer_request_accepted = false;
            s_peer_request_waiting_device = false;
            s_peer_request_flow_id = 0U;
            s_peer_request_generation = 0U;
            s_offer_seen_valid = false;
            s_offer_seen_flow_id = 0U;
            s_offer_seen_generation = 0U;
            pc_device_gone("鼠标侧报告物理USB拔出",
                           sender_generation, event_id);
            return;
        }
        if (frame->type == DUAL_MESSAGE_PROFILE_OFFER &&
            frame->payload_length == DUAL_LINK_PROFILE_OFFER_LENGTH) {
            const uint32_t flow_id = (uint32_t)frame->payload[0] |
                ((uint32_t)frame->payload[1] << 8) |
                ((uint32_t)frame->payload[2] << 16) |
                ((uint32_t)frame->payload[3] << 24);
            const uint32_t transfer_id = (uint32_t)frame->payload[4] |
                ((uint32_t)frame->payload[5] << 8) |
                ((uint32_t)frame->payload[6] << 16) |
                ((uint32_t)frame->payload[7] << 24);
            const uint32_t crc32 = (uint32_t)frame->payload[8] |
                ((uint32_t)frame->payload[9] << 8) |
                ((uint32_t)frame->payload[10] << 16) |
                ((uint32_t)frame->payload[11] << 24);
            if (flow_id == 0U || transfer_id == 0U) {
                (void)dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_OFFER,
                                               flow_id, DUAL_FLOW_STATUS_FAILED);
                return;
            }
            const uint32_t offer_generation = dual_uart1_peer_generation();
            if (s_offer_seen_valid && flow_id == s_offer_seen_flow_id &&
                offer_generation == s_offer_seen_generation) {
                /*
                 * M 的重发（同一 generation + flow ID）：不重跑清理、不重置
                 * Profile 接收器，否则会丢掉正在传输的分片并让整份重传。
                 * 已完成的清理会由重配置任务补发确认，这里不重复入队。
                 */
                ++s_duplicate_offer_ignored;
                ESP_LOGI(TAG, "收到同一flow的重复提议：不重置接收状态；flow=%" PRIu32
                         " ignored=%" PRIu32,
                         flow_id, s_duplicate_offer_ignored);
                return;
            }
            /* M 发起的新克隆先撤掉旧设备，再确认接收。Profile 的最终
             * 成败仍由安装挂载后的 PROFILE_ACK 报告。
             * 例外（2026-09-28 用户决策）：本次 Profile 的 CRC32 与本机当前
             * 已挂载克隆完全一致时**复用**它——不卸载、不重装。实测省时间的
             * 正是这一步（卸载→安装→Windows 重新枚举 609~962 ms，尾部 2.7 s），
             * 而板间数据段只有 12~30 ms。判定条件全部在纯函数
             * link_profile_reuse_allowed() 里，任一条不成立就退回完整路径。 */
            const bool reuse_ok = dual_pc_hid_installed_profile_matches(crc32);
            hid_profile_receiver_init(&s_profile_receiver, on_profile_published,
                                      &s_profile_receiver);
            s_profile_started_us = 0;
            dual_uart1_set_profile_request_ready(false);
            s_offer_seen_valid = true;
            s_offer_seen_generation = offer_generation;
            s_offer_seen_flow_id = flow_id;
            s_profile_offer_transfer_id = transfer_id;
            s_profile_offer_crc32 = crc32;
            s_profile_offer_valid = true;
            s_profile_reuse_pending = reuse_ok;
            if (reuse_ok) {
                /* 复用路径没有清理屏障要等，直接确认提议让 M 送完这一小段数据。 */
                const esp_err_t reuse_ack = dual_uart1_send_flow_ack(
                    DUAL_MESSAGE_PROFILE_OFFER, flow_id, DUAL_FLOW_STATUS_ACCEPTED);
                if (reuse_ack == ESP_OK) {
                    ++s_profile_reuse_hits;
                    ESP_LOGW(TAG, "Profile CRC 命中：本机已挂载同一份Profile，"
                             "跳过卸载+重装；flow=%" PRIu32 " transfer=%" PRIu32
                             " crc=%08" PRIX32 " hits=%" PRIu32,
                             flow_id, transfer_id, crc32, s_profile_reuse_hits);
                    return;
                }
                /* 确认排不上队（队列拥塞）：作废复用意图，落到下面的完整清理路径，
                 * 由它在清理完成后按既有重试策略补发同一条确认。 */
                s_profile_reuse_pending = false;
                dual_status_led_set_flow_error(true);
                ESP_LOGW(TAG, "Profile复用的提议确认无法排队(%s)：退回完整卸载+重装路径",
                         esp_err_to_name(reuse_ack));
            }
            const esp_err_t result = dual_pc_hid_schedule_disconnect(
                offer_generation, 0U,
                DUAL_MESSAGE_PROFILE_OFFER, flow_id);
            const uint8_t status = result == ESP_OK ?
                DUAL_FLOW_STATUS_ACCEPTED : DUAL_FLOW_STATUS_FAILED;
            if (result != ESP_OK) {
                s_profile_offer_valid = false;
                dual_status_led_set_flow_error(true);
                if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_OFFER,
                                             flow_id, status) != ESP_OK) {
                    dual_status_led_set_flow_error(true);
                }
            } else {
                ESP_LOGI(TAG,
                         "Profile提议已接收，待完成USB卸载和旧会话清理后确认：flow=%" PRIu32,
                         flow_id);
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_PHYSICAL_RELEASE) {
            dual_pc_hid_physical_release();
            return;
        }
        if (frame->type == DUAL_MESSAGE_SOFTWARE_RELEASE) {
            dual_pc_hid_software_release();
            return;
        }
        if (frame->type == DUAL_MESSAGE_SOFTWARE_MOUSE &&
            (frame->payload_length == 7U || frame->payload_length == 8U)) {
            dual_pc_hid_software_report(
                frame->payload[0],
                read_i16_le(&frame->payload[1]),
                read_i16_le(&frame->payload[3]),
                (int8_t)frame->payload[5],
                (int8_t)frame->payload[6],
                frame->payload_length == 8U ? frame->payload[7] : 0U);
            return;
        }
        if (frame->type == DUAL_MESSAGE_RAW_HID_INPUT ||
            frame->type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE) {
            dual_pc_hid_handle_vendor_frame(frame);
            return;
        }
        if (frame->type != DUAL_MESSAGE_PHYSICAL_MOUSE || frame->payload_length != 9) {
            return;
        }
        /* UART1 reports are already normalized by the mouse-side boot parser. */
        dual_pc_hid_physical_report(
            frame->payload[2],
            read_i16_le(&frame->payload[3]),
            read_i16_le(&frame->payload[5]),
            (int8_t)frame->payload[7],
            (int8_t)frame->payload[8]);
    } else if (s_role == DUAL_ROLE_MOUSE_HOST) {
        if (frame->type == DUAL_MESSAGE_PROFILE_REQUEST &&
            frame->payload_length == DUAL_LINK_PROFILE_REQUEST_LENGTH) {
            const uint32_t request_generation = (uint32_t)frame->payload[0] |
                ((uint32_t)frame->payload[1] << 8) |
                ((uint32_t)frame->payload[2] << 16) |
                ((uint32_t)frame->payload[3] << 24);
            const uint32_t flow_id = (uint32_t)frame->payload[4] |
                ((uint32_t)frame->payload[5] << 8) |
                ((uint32_t)frame->payload[6] << 16) |
                ((uint32_t)frame->payload[7] << 24);
            if (request_generation == 0U ||
                request_generation != dual_uart1_peer_generation() || flow_id == 0U) {
                (void)dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_REQUEST,
                                               flow_id, DUAL_FLOW_STATUS_FAILED);
                return;
            }
            const bool same_flow = s_peer_request_accepted &&
                flow_id == s_peer_request_flow_id &&
                request_generation == s_peer_request_generation;
            if (same_flow) {
                /* 同一 flow 的重放：不重复采集，只重发当前结果。 */
                esp_err_t retry_result = ESP_OK;
                uint8_t status = DUAL_FLOW_STATUS_ACCEPTED;
                if (s_peer_request_waiting_device && dual_hid_host_mouse_present()) {
                    retry_result = dual_hid_host_request_profile_refresh();
                    if (retry_result == ESP_OK) {
                        s_peer_request_waiting_device = false;
                    } else if (retry_result != ESP_ERR_NOT_FOUND) {
                        status = DUAL_FLOW_STATUS_FAILED;
                    }
                }
                if (s_peer_request_waiting_device) {
                    /* 重放时仍在等设备：继续如实告知，让 P 保持等待而不是重试。 */
                    status = DUAL_FLOW_STATUS_WAITING_DEVICE;
                }
                if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_REQUEST,
                                             flow_id, status) != ESP_OK) {
                    dual_status_led_set_flow_error(true);
                }
                ESP_LOGI(TAG, "收到同一flow的重复申请：不重复采集；flow=%" PRIu32
                         " status=%u", flow_id, (unsigned)status);
                return;
            }
            s_peer_request_flow_id = flow_id;
            s_peer_request_generation = request_generation;
            s_peer_request_accepted = true;
            dual_uart1_deferred_refresh();
            if (!dual_hid_host_mouse_present()) {
                /* 物理鼠标尚未枚举：受理申请并等待设备到达，不判永久失败。
                 * 用 WAITING_DEVICE 明确告诉 P 保持等待，别烧重试预算。 */
                s_peer_request_waiting_device = true;
                if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_REQUEST,
                                             flow_id,
                                             DUAL_FLOW_STATUS_WAITING_DEVICE) != ESP_OK) {
                    dual_status_led_set_flow_error(true);
                }
                ESP_LOGW(TAG, "Profile申请已受理，但物理鼠标尚未枚举；等待设备到达后采集："
                         "flow=%" PRIu32, flow_id);
                return;
            }
            const esp_err_t result = dual_hid_host_request_profile_refresh();
            uint8_t status = DUAL_FLOW_STATUS_ACCEPTED;
            if (result == ESP_OK) {
                s_peer_request_waiting_device = false;
            } else if (result == ESP_ERR_NOT_FOUND ||
                       result == ESP_ERR_INVALID_STATE) {
                /* 设备在途/描述符尚未就绪：可重试，先受理并让 P 等待。 */
                s_peer_request_waiting_device = true;
                status = DUAL_FLOW_STATUS_WAITING_DEVICE;
                ESP_LOGW(TAG, "Profile申请受理后暂时无法采集（%s），等待重试：flow=%" PRIu32,
                         esp_err_to_name(result), flow_id);
            } else {
                status = DUAL_FLOW_STATUS_FAILED;
            }
            if (dual_uart1_send_flow_ack(DUAL_MESSAGE_PROFILE_REQUEST,
                                         flow_id, status) != ESP_OK) {
                dual_status_led_set_flow_error(true);
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_HID_SET_REPORT ||
            frame->type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST) {
            dual_hid_host_handle_control_frame(frame);
        }
    }
}

static void on_profile_published(const hid_device_profile_t *profile, void *context)
{
    const hid_profile_receiver_t *receiver = context;
    if (!s_profile_offer_valid || receiver == NULL ||
        receiver->published_transfer_id != s_profile_offer_transfer_id ||
        receiver->published_crc32 != s_profile_offer_crc32) {
        s_profile_schedule_ok = false;
        dual_status_led_set_flow_error(true);
        ESP_LOGE(TAG, "完整Profile与已确认的克隆提议不匹配");
        return;
    }
    uint16_t vid = 0;
    uint16_t pid = 0;
    if (profile != NULL && profile->device_descriptor.length >= 12U) {
        const uint8_t *descriptor = profile->device_descriptor.data;
        vid = (uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8);
        pid = (uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8);
    }
    ESP_LOGI(TAG,
             "收到物理HID Profile观察快照：VID:PID=%04X:%04X manufacturer_len=%u "
             "product_len=%u serial_present=%s interfaces=%u length_flags=%02X",
             vid, pid, profile != NULL ? profile->manufacturer.length : 0,
             profile != NULL ? profile->product.length : 0,
             profile != NULL && profile->serial.length != 0 ? "yes" : "no",
             profile != NULL ? profile->report_descriptor_count : 0,
             profile != NULL ? profile->flags : 0);
    const uint32_t transfer_id = receiver != NULL ? receiver->published_transfer_id : 0U;
    const uint32_t crc32 = receiver != NULL ? receiver->published_crc32 : 0U;
    /* 初值故意取失败值：只有复用真正完成才跳过下面的完整安装路径。 */
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_profile_reuse_pending) {
        s_profile_reuse_pending = false;
        /*
         * 复用路径（2026-09-28）：Profile 与已挂载克隆完全一致 → 不卸载、不重装。
         * 这里复核一次，避免 OFFER 与 COMMIT 之间克隆被卸载/换掉的竞态；
         * 复核不通过时自动退回下面的完整路径。
         */
        if (dual_pc_hid_installed_profile_matches(crc32)) {
            result = dual_pc_hid_reuse_installed_profile(transfer_id, crc32);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Profile复用复核未通过（%s）：退回完整卸载+重装",
                         esp_err_to_name(result));
            }
        } else {
            ESP_LOGW(TAG, "Profile复用复核未通过（克隆已变化）：退回完整卸载+重装");
        }
    }
    if (result != ESP_OK) {
        result = dual_pc_hid_schedule_reconfigure(profile, transfer_id, crc32);
    }
    s_profile_started_us = 0;
    s_profile_schedule_ok = result == ESP_OK;
    if (result != ESP_OK) {
        dual_status_led_set_flow_error(true);
        ESP_LOGW(TAG, "动态USB克隆排队失败：%s", esp_err_to_name(result));
    }
}

/* 上次按键字节：仅用于按键边沿打点（点击延迟测量）。 */
static uint8_t s_last_buttons;

static void on_mouse_report(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    if (s_role != DUAL_ROLE_MOUSE_HOST) {
        return;
    }
    /*
     * 按键边沿打点：用于测量"点击延迟"（用户当前的验收标准）。
     * 主机侧高频采样左键状态并同时抓取本行日志，两者按时间配对即可算出
     * 从"鼠标侧看到按键"到"电脑侧看到按键"的真实延迟。
     * 只在边沿打印，人工点击频率下不会造成日志洪峰。
     */
    if (buttons != s_last_buttons) {
        s_last_buttons = buttons;
        ESP_LOGI(TAG, "按键边沿：buttons=0x%02X（点击延迟测量打点）", buttons);
    }
    /*
     * 2026-09-27：按用户要求**移除全部纯移动抑制**。这里原先在「Profile 传输在途」或
     * 「厂商事务在途」时丢弃无按键的纯移动；现已删除——纯移动在任何阶段都照常转发。
     * 提醒：本回调在物理路径上不会被调用（生效路径是 `hid_host_mouse.c` 的
     * `raw_report_task`），此处保留仅为记录转发语义，避免将来误以为让它路仍在生效。
     */
    if (dual_uart1_send_mouse(interface_number, report_id, buttons, x, y, wheel, pan) != ESP_OK) {
        (void)dual_uart1_send_release(1);
    }
}

static void on_mouse_release(bool device_gone)
{
    if (s_role != DUAL_ROLE_MOUSE_HOST) {
        return;
    }
    const link_release_plan_t plan = link_release_plan(device_gone);
    if (plan.record_input_error) {
        /* 输入/报告异常（device_gone=false）：只释放按钮并记录输入错误，
         * 不触发 DEVICE_GONE、不清缓存 Profile、不引发 USB 重枚举。 */
        ++s_mouse_input_errors;
        if (dual_uart1_send_release(2) != ESP_OK) {
            ESP_LOGW(TAG, "输入异常时的release无法送入板间UART");
        }
        ESP_LOGW(TAG, "实体输入异常：已释放按钮，保持克隆会话；errors=%" PRIu32,
                 s_mouse_input_errors);
        return;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_DISCONNECTED);
    (void)dual_uart1_send_release(2);
    dual_uart1_cancel_profile();
    (void)dual_uart1_send_device_gone(1);
}

static esp_err_t start_pc_role(void)
{
    __atomic_store_n(&s_manual_profile, false, __ATOMIC_RELEASE);
    s_peer_mouse_usb_state_initialized = false;
    s_peer_mouse_usb_state = DUAL_USB_STATE_WAITING;
    s_peer_mouse_generation_initialized = false;
    s_peer_mouse_generation = 0;
    s_profile_offer_valid = false;
    s_profile_schedule_ok = false;
    s_peer_request_accepted = false;
    s_peer_request_waiting_device = false;
    s_peer_request_flow_id = 0U;
    s_peer_request_generation = 0U;
    s_offer_seen_valid = false;
    s_offer_seen_flow_id = 0U;
    s_offer_seen_generation = 0U;
    dual_uart1_set_profile_request_ready(false);
    hid_profile_receiver_init(&s_profile_receiver, on_profile_published, &s_profile_receiver);
    esp_err_t result = dual_pc_hid_prepare_for_profile();
    if (result != ESP_OK) {
        return result;
    }
    dual_pc_hid_enable_reconfigure();
    /* 电脑侧板的调试口只跑板载日志服务：目标电脑的排障常常拿不到它的控制台。 */
    const esp_err_t log_service = dual_uart0_log_service_start(DUAL_ROLE_PC_DEVICE);
    if (log_service != ESP_OK) {
        ESP_LOGW(TAG, "UART0 板载日志服务启动失败：%s", esp_err_to_name(log_service));
    }
    /* Prepare the receive path before advertising PC_DEVICE to the peer. */
    s_role = DUAL_ROLE_PC_DEVICE;
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    result = dual_uart1_lock_role(DUAL_ROLE_PC_DEVICE);
    if (result != ESP_OK) {
        s_role = DUAL_ROLE_UNRESOLVED;
        return result;
    }
    /* 上电身份确认前已同步卸载探测 USB；在此之后才允许 TX task 发出唯一 REQUEST。 */
    dual_uart1_set_profile_request_ready(true);
    s_probe_mode = USB_PROBE_NONE;
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_PC_DEVICE);
    dual_status_led_set_peer_connected(dual_uart1_peer_online());
    dual_status_led_set_pc_mounted(tud_mounted());
    ESP_LOGI(TAG, "角色锁定：PC_DEVICE；原生USB=严格鼠标克隆，UART1=实体+软件输入链路");
    s_role_initialized = true;
    return ESP_OK;
}

/*
 * 上电后延迟启动（克隆相关的）USB 操作。用户要求：上电后先等 1 秒再开始，
 * 让供电与上电瞬间的电气状态先稳定，避免与克隆建立流程相互干扰。
 * 以 esp_timer_get_time()（上电起算）为准，若已经过了 1 秒则不额外等待。
 */
#define CLONE_USB_START_DELAY_MS 1000
static void wait_clone_usb_start_delay(void)
{
    /*
     * 无条件等待：上电/复位（含 RTS 复位、软复位）后，克隆相关的 USB 操作
     * 一律延后 CLONE_USB_START_DELAY_MS 再开始，让供电与上电瞬间的电气状态先稳定。
     * 注意不能写成“等到上电满 N 毫秒”——板子自身初始化（状态灯、板载日志挂载、
     * 描述符打印）就要 1 秒以上，那样判断会直接被跳过（首版就是这么失效的，
     * 日志里连一行都没有）。
     */
    ESP_LOGI(TAG, "上电/复位延迟：先等 %d ms 再启动克隆相关 USB 操作",
             CLONE_USB_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CLONE_USB_START_DELAY_MS));
}
static esp_err_t start_mouse_role(void)
{
    esp_err_t result = ESP_OK;
    /*
     * 鼠标侧同样必须接收来自电脑侧的 HID++ SET/GET_REPORT。
     * 这里只注册断开回调会导致描述符虽然克隆成功，但 G HUB 的控制请求
     * 在 UART 解析后被静默丢弃。
     */
    if (!s_host_started) {
        wait_clone_usb_start_delay();
        result = dual_hid_host_start(on_mouse_report, on_mouse_release);
        if (result != ESP_OK) {
            return result;
        }
        s_host_started = true;
    }
    result = dual_uart0_control_start(
        DUAL_ROLE_MOUSE_HOST, on_software_frame, on_software_release);
    if (result != ESP_OK) {
        return result;
    }
    /* Both input paths are ready before the role claim can trigger peer traffic. */
    s_role = DUAL_ROLE_MOUSE_HOST;
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    result = dual_uart1_lock_role(DUAL_ROLE_MOUSE_HOST);
    if (result != ESP_OK) {
        s_role = DUAL_ROLE_UNRESOLVED;
        return result;
    }
    s_probe_mode = USB_PROBE_NONE;
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    dual_status_led_set_peer_connected(dual_uart1_peer_online());
    /* A mouse HID interface may finish enumerating during the unresolved
     * Host probe. Setting the final role clears the LED readiness state, so
     * restore it from the Host's current state after the role transition. */
    dual_status_led_set_host_mouse_ready(dual_hid_host_mouse_present());
    ESP_LOGI(TAG, "角色锁定：MOUSE_HOST；原生USB=真实鼠标Host，UART0=电脑A软件控制，UART1=透明代理链路");
    s_role_initialized = true;
    return ESP_OK;
}

static esp_err_t start_device_probe(void)
{
    s_last_probe_mode = USB_PROBE_DEVICE;
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_NONE);
    dual_status_led_set_pc_mounted(false);
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    const esp_err_t result = dual_pc_hid_install_device();
    if (result == ESP_OK) {
        s_probe_started_us = esp_timer_get_time();
        s_probe_mode = USB_PROBE_DEVICE;
        ESP_LOGI(TAG, "USB身份未定：Device栈就绪，开始完整 %d ms 探测窗口",
                 USB_ROLE_PROBE_INTERVAL_MS);
    } else {
        s_probe_mode = USB_PROBE_NONE;
    }
    return result;
}

static esp_err_t start_host_probe(void)
{
    s_last_probe_mode = USB_PROBE_HOST;
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_NONE);
    dual_status_led_set_pc_mounted(false);
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    wait_clone_usb_start_delay();
    const esp_err_t result = dual_hid_host_start(on_mouse_report, on_mouse_release);
    if (result == ESP_OK) {
        s_probe_started_us = esp_timer_get_time();
        s_host_started = true;
        s_probe_mode = USB_PROBE_HOST;
        ESP_LOGI(TAG, "USB身份未定：Host栈就绪，开始完整 %d ms 探测窗口",
                 USB_ROLE_PROBE_INTERVAL_MS);
    } else {
        s_probe_mode = USB_PROBE_NONE;
    }
    return result;
}

static uint8_t locally_confirmed_role(void)
{
    /* ATTACHED is raised before host SET_CONFIGURATION; green LED reflects
     * this physical Device-side evidence even while tud_mounted() is false. */
    if (s_probe_mode == USB_PROBE_DEVICE &&
        (dual_pc_hid_usb_attached() || tud_mounted())) {
        return DUAL_ROLE_PC_DEVICE;
    }
    if (s_probe_mode == USB_PROBE_HOST && dual_hid_host_device_present()) {
        return DUAL_ROLE_MOUSE_HOST;
    }
    return DUAL_ROLE_UNRESOLVED;
}

static esp_err_t stop_current_probe(void)
{
    if (s_probe_mode == USB_PROBE_DEVICE) {
        if (dual_pc_hid_usb_attached() || tud_mounted()) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t result = dual_pc_hid_prepare_for_profile();
        if (result == ESP_OK) {
            s_probe_mode = USB_PROBE_NONE;
        }
        return result;
    }
    if (s_probe_mode == USB_PROBE_HOST) {
        if (dual_hid_host_device_present()) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t result = dual_hid_host_stop();
        if (result == ESP_OK) {
            s_host_started = false;
            s_probe_mode = USB_PROBE_NONE;
        }
        return result;
    }
    return ESP_OK;
}

static esp_err_t enter_locked_role(uint8_t role)
{
    dual_status_led_set_role(role == DUAL_ROLE_PC_DEVICE
        ? DUAL_STATUS_LED_ROLE_PC_DEVICE : DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    dual_status_led_set_peer_connected(dual_uart1_peer_online());
    if (role == DUAL_ROLE_PC_DEVICE && s_probe_mode == USB_PROBE_HOST) {
        const esp_err_t stopped = stop_current_probe();
        if (stopped != ESP_OK) {
            dual_status_led_set_flow_error(true);
            return stopped;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    } else if (role == DUAL_ROLE_MOUSE_HOST && s_probe_mode == USB_PROBE_DEVICE) {
        const esp_err_t stopped = stop_current_probe();
        if (stopped != ESP_OK) {
            dual_status_led_set_flow_error(true);
            return stopped;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    const esp_err_t result = role == DUAL_ROLE_PC_DEVICE
        ? start_pc_role() : start_mouse_role();
    if (result != ESP_OK) {
        dual_status_led_set_flow_error(true);
    }
    return result;
}

static esp_err_t rotate_unresolved_probe(void)
{
    const usb_probe_mode_t last_mode = s_probe_mode != USB_PROBE_NONE
        ? s_probe_mode : s_last_probe_mode;
    const usb_probe_mode_t next_mode = last_mode == USB_PROBE_DEVICE
        ? USB_PROBE_HOST : USB_PROBE_DEVICE;
    esp_err_t result = stop_current_probe();
    if (result != ESP_OK) {
        s_probe_started_us = esp_timer_get_time();
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    result = next_mode == USB_PROBE_HOST ? start_host_probe() : start_device_probe();
    if (result != ESP_OK) {
        s_probe_mode = USB_PROBE_NONE;
    }
    return result;
}

void app_main(void)
{
    /* 板载日志要最先起来：后面的身份探测、USB 重枚举和线序测试都可能失败，
     * 那些现场恰恰需要“上电至今”的完整记录。失败只停用该功能，不阻塞启动。 */
    const esp_err_t log_result = dual_onboard_log_start();
    if (log_result != ESP_OK) {
        ESP_LOGW(TAG, "板载日志不可用：%s；继续运行输入链路", esp_err_to_name(log_result));
    }
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(s_node_id));
    const esp_err_t led_result = dual_status_led_init();
    if (led_result != ESP_OK) {
        ESP_LOGW(TAG, "板载状态灯初始化失败：%s；继续运行输入链路", esp_err_to_name(led_result));
    }
    ESP_LOGI(TAG, "dual_proxy启动 node=%02X%02X%02X%02X%02X%02X UART1 TX=GPIO17 RX=GPIO18 baud=921600",
             s_node_id[0], s_node_id[1], s_node_id[2],
             s_node_id[3], s_node_id[4], s_node_id[5]);
    ESP_LOGI(TAG, "诊断开关：hidpp_timeout=%d link_gone_retry=%d periodic_stats=%d",
             DUAL_PROXY_ENABLE_HIDPP_TIMEOUT_DIAGNOSTIC,
             DUAL_PROXY_ENABLE_LINK_GONE_RETRY_DIAGNOSTIC,
             DUAL_PROXY_ENABLE_PERIODIC_STATS_LOG);
    esp_err_t result = dual_uart1_start(
        DUAL_ROLE_UNRESOLVED, s_node_id, on_link_frame, on_link_fault);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UART1身份协商通道启动失败：%s；停止输入", esp_err_to_name(result));
        dual_status_led_set_role(DUAL_STATUS_LED_ROLE_NONE);
        dual_pc_hid_release_all();
        return;
    }

    result = start_device_probe();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "首次 Device 探测启动失败：%s；3 秒后轮换重试", esp_err_to_name(result));
    }

    while (true) {
        if (s_role != DUAL_ROLE_UNRESOLVED) {
            if (s_role_initialized) {
                return;
            }
            result = s_role == DUAL_ROLE_PC_DEVICE ? start_pc_role() : start_mouse_role();
            if (result == ESP_OK) {
                return;
            }
            ESP_LOGE(TAG, "角色已锁定但初始化失败 role=%u error=%s；1 秒后重试",
                     (unsigned)s_role, esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const uint8_t local_role = locally_confirmed_role();
        const uint8_t peer_role = s_peer_role_claim;
        const uint8_t resolved_role = dual_role_resolve(peer_role, local_role);
        bool transition_attempted = false;
        if (resolved_role != DUAL_ROLE_UNRESOLVED &&
            esp_timer_get_time() >= s_role_retry_after_us) {
            transition_attempted = true;
            result = enter_locked_role(resolved_role);
            if (result == ESP_OK) {
                if (peer_role != DUAL_ROLE_UNRESOLVED) {
                    ESP_LOGI(TAG, "依据UART1对端身份锁定互补角色：peer=%u local=%u",
                             (unsigned)peer_role, (unsigned)resolved_role);
                } else {
                    ESP_LOGI(TAG, "依据本地USB连接证据锁定角色：local=%u",
                             (unsigned)resolved_role);
                }
                return;
            }
            ESP_LOGW(TAG, "启动已判定角色失败 role=%u error=%s",
                     (unsigned)resolved_role, esp_err_to_name(result));
        }

        const int64_t now_us = esp_timer_get_time();
        if (transition_attempted && s_role == DUAL_ROLE_UNRESOLVED) {
            s_role_retry_after_us = now_us + 1000000LL;
        }
        if (s_role == DUAL_ROLE_UNRESOLVED &&
            now_us - s_probe_started_us >= USB_ROLE_PROBE_INTERVAL_MS * 1000LL) {
            result = rotate_unresolved_probe();
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "未锁定身份轮换失败：%s；下个探测周期重试",
                         esp_err_to_name(result));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
