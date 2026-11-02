from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PC_SOURCE = (ROOT / "firmware/main/pc_hid_output.c").read_text(encoding="utf-8")
APP_SOURCE = (ROOT / "firmware/main/dual_proxy_app.c").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function signature: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


queue = function_body(PC_SOURCE, "static esp_err_t queue_profile_reconfigure_locked(")
assert "next_reconfigure_epoch_locked()" in queue
assert "s_usb_reconfigure_in_progress = true" in queue
assert "memcpy(&s_reconfigure_profile, profile" in queue
assert "s_reconfigure_profile_pending = true" in queue
assert "s_reconfigure_profile_data_reused = profile_data_reused" in queue

notify = function_body(PC_SOURCE, "static void notify_profile_reconfigure_queued(")
assert "dual_pc_hid_vendor_link_fault()" in notify
assert "dual_pc_hid_release_all()" in notify
assert "xTaskNotifyGive(s_reconfigure_task)" in notify

reuse = function_body(PC_SOURCE, "esp_err_t dual_pc_hid_reuse_installed_profile(")
assert "link_profile_reuse_allowed(" in reuse
assert "s_installed" in reuse
assert "queue_profile_reconfigure_locked(\n        &s_active_profile" in reuse
assert "notify_profile_reconfigure_queued(transfer_id, operation_epoch, true)" in reuse
assert "dual_uart1_send_profile_ack" not in reuse
assert "dual_pc_hid_note_profile_ack_result" not in reuse
assert "s_reconfigure_installed_transfer_id =" not in reuse

schedule = function_body(PC_SOURCE, "esp_err_t dual_pc_hid_schedule_reconfigure(")
assert "queue_profile_reconfigure_locked(\n        profile" in schedule
assert "notify_profile_reconfigure_queued(transfer_id, operation_epoch, false)" in schedule

worker = function_body(PC_SOURCE, "static void reconfigure_task(")
assert "install_profile_attempt(operation_epoch, attempt)" in worker
assert "LINK_PROFILE_HOST_MOUNTED" in worker
assert "if (!current || !tud_mounted()" in worker
assert "dual_uart1_send_profile_ack_for_generation(" in worker

published = function_body(
    APP_SOURCE,
    "static void on_profile_published(const hid_device_profile_t *profile, void *context)\n{",
)
assert "dual_pc_hid_reuse_installed_profile(transfer_id, crc32)" in published
assert "dual_pc_hid_schedule_reconfigure(profile, transfer_id, crc32)" in published
assert "s_profile_schedule_ok = result == ESP_OK" in published
assert "s_offer_seen_generation != dual_uart1_peer_generation()" in published
assert "stale_offer_peer" in published

duplicate_commit = APP_SOURCE[APP_SOURCE.find("if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT &&\n                accepted && s_profile_receiver.last_commit_was_duplicate)"):]
assert "profile_result != LINK_PROFILE_REPLAY_PENDING" in duplicate_commit
assert "profile_status" in duplicate_commit

print("profile_reuse_reenumeration_source_path: PASS")
