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
bool dual_pc_hid_device_cleanup_pending(void);

/*
 * Profile CRC 复用（2026-09-28，按用户决策启用，替换下面这段早已失效的旧注释）：
 * 当本机已安装且mounted的克隆与本次提议CRC相同，复用内存中的已验证Profile数据；
 * 仍通过唯一重配置worker执行卸载、USB断开、重新安装/枚举，并只在mounted后回最终ACK。
 * 纯判定函数 link_profile_reuse_allowed() 位于 link_recovery_logic.h；前提不满足时
 * 使用刚发布的Profile数据进入同一完整worker路径。
 */
bool dual_pc_hid_installed_profile_matches(uint32_t crc32);
link_profile_replay_result_t dual_pc_hid_profile_result(uint32_t transfer_id, uint32_t crc32);
void dual_pc_hid_note_profile_ack_result(
    uint32_t transfer_id, uint32_t crc32, uint8_t status, esp_err_t result);
/* ESP_OK只表示已排队；Profile重枚举和mounted最终ACK由唯一worker异步完成。 */
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
/* 作废旧操作并等待唯一 worker 静止；失败时禁止切换 USB PHY。 */
esp_err_t dual_pc_hid_disable_reconfigure(void);
bool dual_pc_hid_role_probe_needed(void);
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
