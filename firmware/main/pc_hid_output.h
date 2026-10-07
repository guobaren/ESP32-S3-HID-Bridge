#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bridge_protocol.h"
#include "hid_device_profile.h"
#include "link_recovery_logic.h"
#include "stats_snapshot.h"

esp_err_t dual_pc_hid_install_device(void);
esp_err_t dual_pc_hid_start_sender(void);
esp_err_t dual_pc_hid_stop_sender(void);
esp_err_t dual_pc_hid_prepare_for_profile(void);

/*
 * Profile CRC 复用（2026-09-28，按用户决策启用，替换下面这段早已失效的旧注释）：
 * 当本机当前挂载的克隆与本次提议的 Profile 完全一致（CRC32 相同）时，跳过
 * 「卸载 + 重装」。实测省时间的正是这一步——卸载→安装→Windows 重新枚举要
 * 609~962 ms（典型）、尾部到 2.7 s，而板间 Profile 数据段本身只有 12~30 ms。
 * 判定为纯函数 link_profile_reuse_allowed()（见 link_recovery_logic.h），
 * 任何一条前提不成立都退回完整路径，绝不把陈旧克隆留给接收端。
 */
bool dual_pc_hid_installed_profile_matches(uint32_t crc32);
link_profile_replay_result_t dual_pc_hid_profile_result(uint32_t transfer_id, uint32_t crc32);
void dual_pc_hid_note_profile_ack_result(
    uint32_t transfer_id, uint32_t crc32, uint8_t status, esp_err_t result);
esp_err_t dual_pc_hid_reuse_installed_profile(uint32_t transfer_id, uint32_t crc32);
esp_err_t dual_pc_hid_schedule_reconfigure(
    const hid_device_profile_t *profile, uint32_t transfer_id, uint32_t crc32);
/* ack_type is zero for local cleanup, PROFILE_OFFER, or DEVICE_GONE otherwise.
 * The matching flow ACK is sent only after USB teardown and session clearing. */
esp_err_t dual_pc_hid_schedule_disconnect(
    uint32_t sender_generation,
    uint32_t event_id,
    uint8_t ack_type,
    uint32_t ack_flow_id);
void dual_pc_hid_enable_reconfigure(void);
void dual_pc_hid_software_report(
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan,
    uint8_t smoothing_slots);
void dual_pc_hid_software_release(void);
void dual_pc_hid_physical_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void dual_pc_hid_physical_release(void);
void dual_pc_hid_release_all(void);
bool dual_pc_hid_ready(void);
bool dual_pc_hid_usb_attached(void);
void dual_pc_hid_handle_vendor_frame(const dual_frame_t *frame);
void dual_pc_hid_vendor_link_fault(void);
void dual_pc_hid_collect_stats(dual_stats_snapshot_t *snapshot);
