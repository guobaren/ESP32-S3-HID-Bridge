#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_MOUSE_FIELD_INVALID_OFFSET UINT16_MAX

typedef struct {
    uint16_t bit_offset;
    uint8_t bit_size;
    int32_t logical_minimum;
    int32_t logical_maximum;
} hid_mouse_axis_field_t;

typedef struct {
    bool valid;
    uint8_t report_id;
    uint16_t report_bytes;
    uint16_t buttons_bit_offset;
    uint8_t button_count;
    hid_mouse_axis_field_t x;
    hid_mouse_axis_field_t y;
    hid_mouse_axis_field_t wheel;
    hid_mouse_axis_field_t pan;
} hid_mouse_report_layout_t;

/*
 * Parse one HID report descriptor and select a relative mouse input report.
 * Offsets are relative to the report body and therefore exclude Report ID.
 * The parser is bounded and independent from USB/FreeRTOS so it can be tested
 * on the host.  It never assumes a Logitech VID/PID or a boot-mouse layout.
 */
bool hid_report_find_mouse_layout(
    const uint8_t *descriptor,
    size_t length,
    hid_mouse_report_layout_t *layout);

/*
 * Build one software overlay report from the latest physical report template.
 * Relative axes in the template are cleared before software deltas are written,
 * while physical button bits and unrelated fields are preserved. Software
 * buttons are ORed into the first eight dynamically discovered button bits.
 */
bool hid_mouse_report_apply_overlay(
    const hid_mouse_report_layout_t *layout,
    uint8_t *report,
    size_t report_length,
    uint8_t software_buttons,
    int32_t x,
    int32_t y,
    int32_t wheel,
    int32_t pan);

/*
 * 读取/累加一条鼠标报表的相对轴值。用于把积压的多条移动合并成一条提交：
 * 逐条提交会变成"停止后还在动"（旧采样延迟到达），逐条丢弃又会丢位移。
 */
bool hid_mouse_report_read_axes(
    const uint8_t *report,
    size_t report_length,
    const hid_mouse_report_layout_t *layout,
    int32_t *x,
    int32_t *y,
    int32_t *wheel,
    int32_t *pan);
bool hid_mouse_report_add_axes(
    uint8_t *report,
    size_t report_length,
    const hid_mouse_report_layout_t *layout,
    int32_t delta_x,
    int32_t delta_y,
    int32_t delta_wheel,
    int32_t delta_pan);