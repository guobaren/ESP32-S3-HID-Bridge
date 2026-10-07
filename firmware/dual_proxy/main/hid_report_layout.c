#include "hid_report_layout.h"

#include <string.h>

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01U
#define HID_USAGE_PAGE_BUTTON 0x09U
#define HID_USAGE_PAGE_CONSUMER 0x0CU
#define HID_USAGE_MOUSE 0x02U
#define HID_USAGE_X 0x30U
#define HID_USAGE_Y 0x31U
#define HID_USAGE_WHEEL 0x38U
#define HID_USAGE_AC_PAN 0x0238U
#define HID_COLLECTION_APPLICATION 0x01U
#define HID_MAX_GLOBAL_STACK 4U
#define HID_MAX_LOCAL_USAGES 32U
#define HID_MAX_REPORT_STATES 32U

typedef struct {
    uint32_t usage_page;
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
} hid_global_state_t;

typedef struct {
    uint32_t usages[HID_MAX_LOCAL_USAGES];
    uint8_t usage_count;
    uint32_t usage_minimum;
    uint32_t usage_maximum;
    bool has_usage_minimum;
    bool has_usage_maximum;
} hid_local_state_t;

typedef struct {
    bool used;
    uint8_t report_id;
    uint32_t input_bits;
    hid_mouse_report_layout_t layout;
} hid_report_state_t;

static uint32_t read_unsigned(const uint8_t *data, size_t size)
{
    uint32_t value = 0;
    for (size_t index = 0; index < size; ++index) {
        value |= (uint32_t)data[index] << (index * 8U);
    }
    return value;
}

static int32_t read_signed(const uint8_t *data, size_t size)
{
    const uint32_t value = read_unsigned(data, size);
    if (size == 1U) {
        return (int8_t)value;
    }
    if (size == 2U) {
        return (int16_t)value;
    }
    return (int32_t)value;
}

static uint32_t expand_usage(uint32_t usage_page, uint32_t value, size_t size)
{
    if (size == 4U && (value >> 16U) != 0U) {
        return value;
    }
    return (usage_page << 16U) | (value & 0xFFFFU);
}

static void reset_local(hid_local_state_t *local)
{
    memset(local, 0, sizeof(*local));
}

static uint32_t local_usage_at(const hid_local_state_t *local, uint32_t index)
{
    if (index < local->usage_count) {
        return local->usages[index];
    }
    if (local->has_usage_minimum && local->has_usage_maximum &&
        local->usage_maximum >= local->usage_minimum) {
        const uint32_t candidate = local->usage_minimum + index;
        if (candidate <= local->usage_maximum) {
            return candidate;
        }
    }
    return 0U;
}

static hid_report_state_t *report_state_for(
    hid_report_state_t states[HID_MAX_REPORT_STATES],
    uint8_t report_id)
{
    for (size_t index = 0; index < HID_MAX_REPORT_STATES; ++index) {
        if (states[index].used && states[index].report_id == report_id) {
            return &states[index];
        }
    }
    for (size_t index = 0; index < HID_MAX_REPORT_STATES; ++index) {
        if (!states[index].used) {
            states[index].used = true;
            states[index].report_id = report_id;
            states[index].layout.report_id = report_id;
            states[index].layout.buttons_bit_offset = HID_MOUSE_FIELD_INVALID_OFFSET;
            states[index].layout.x.bit_offset = HID_MOUSE_FIELD_INVALID_OFFSET;
            states[index].layout.y.bit_offset = HID_MOUSE_FIELD_INVALID_OFFSET;
            states[index].layout.wheel.bit_offset = HID_MOUSE_FIELD_INVALID_OFFSET;
            states[index].layout.pan.bit_offset = HID_MOUSE_FIELD_INVALID_OFFSET;
            return &states[index];
        }
    }
    return NULL;
}

static void assign_axis(
    hid_mouse_axis_field_t *field,
    uint32_t bit_offset,
    const hid_global_state_t *global)
{
    if (field->bit_offset != HID_MOUSE_FIELD_INVALID_OFFSET ||
        bit_offset > UINT16_MAX || global->report_size == 0U ||
        global->report_size > 32U) {
        return;
    }
    field->bit_offset = (uint16_t)bit_offset;
    field->bit_size = (uint8_t)global->report_size;
    field->logical_minimum = global->logical_minimum;
    field->logical_maximum = global->logical_maximum;
}

static void consume_input(
    hid_report_state_t states[HID_MAX_REPORT_STATES],
    const hid_global_state_t *global,
    const hid_local_state_t *local,
    uint32_t flags,
    bool in_mouse_collection)
{
    hid_report_state_t *state = report_state_for(states, global->report_id);
    if (state == NULL || global->report_size > 32U ||
        global->report_count > 256U ||
        global->report_size * global->report_count > UINT16_MAX) {
        return;
    }
    const uint32_t start_bit = state->input_bits;
    state->input_bits += global->report_size * global->report_count;
    const bool data = (flags & 0x01U) == 0U;
    const bool variable = (flags & 0x02U) != 0U;
    const bool relative = (flags & 0x04U) != 0U;
    if (!in_mouse_collection || !data || !variable) {
        return;
    }
    for (uint32_t index = 0; index < global->report_count; ++index) {
        const uint32_t usage = local_usage_at(local, index);
        const uint16_t usage_page = (uint16_t)(usage >> 16U);
        const uint16_t usage_id = (uint16_t)usage;
        const uint32_t bit_offset = start_bit + index * global->report_size;
        if (usage_page == HID_USAGE_PAGE_BUTTON && global->report_size == 1U) {
            if (state->layout.buttons_bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET) {
                state->layout.buttons_bit_offset = (uint16_t)bit_offset;
            }
            if (state->layout.button_count < UINT8_MAX) {
                ++state->layout.button_count;
            }
        } else if (relative && usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            if (usage_id == HID_USAGE_X) {
                assign_axis(&state->layout.x, bit_offset, global);
            } else if (usage_id == HID_USAGE_Y) {
                assign_axis(&state->layout.y, bit_offset, global);
            } else if (usage_id == HID_USAGE_WHEEL) {
                assign_axis(&state->layout.wheel, bit_offset, global);
            }
        } else if (relative && usage_page == HID_USAGE_PAGE_CONSUMER &&
                   usage_id == HID_USAGE_AC_PAN) {
            assign_axis(&state->layout.pan, bit_offset, global);
        }
    }
}

bool hid_report_find_mouse_layout(
    const uint8_t *descriptor,
    size_t length,
    hid_mouse_report_layout_t *layout)
{
    if (descriptor == NULL || layout == NULL || length == 0U) {
        return false;
    }
    hid_global_state_t global = {0};
    hid_global_state_t global_stack[HID_MAX_GLOBAL_STACK];
    size_t global_stack_depth = 0;
    hid_local_state_t local;
    reset_local(&local);
    hid_report_state_t reports[HID_MAX_REPORT_STATES] = {0};
    uint16_t collection_depth = 0;
    uint16_t mouse_collection_depth = 0;

    size_t cursor = 0;
    while (cursor < length) {
        const uint8_t prefix = descriptor[cursor++];
        if (prefix == 0xFEU) {
            if (length - cursor < 2U) {
                return false;
            }
            const uint8_t long_size = descriptor[cursor];
            cursor += 2U;
            if (long_size > length - cursor) {
                return false;
            }
            cursor += long_size;
            continue;
        }
        size_t item_size = prefix & 0x03U;
        if (item_size == 3U) {
            item_size = 4U;
        }
        if (item_size > length - cursor) {
            return false;
        }
        const uint8_t type = (prefix >> 2U) & 0x03U;
        const uint8_t tag = (prefix >> 4U) & 0x0FU;
        const uint8_t *item_data = &descriptor[cursor];
        const uint32_t value = read_unsigned(item_data, item_size);

        if (type == 1U) {
            switch (tag) {
            case 0x0U: global.usage_page = value; break;
            case 0x1U: global.logical_minimum = read_signed(item_data, item_size); break;
            case 0x2U: global.logical_maximum = read_signed(item_data, item_size); break;
            case 0x7U: global.report_size = value; break;
            case 0x8U: global.report_id = (uint8_t)value; break;
            case 0x9U: global.report_count = value; break;
            case 0xAU:
                if (global_stack_depth >= HID_MAX_GLOBAL_STACK) {
                    return false;
                }
                global_stack[global_stack_depth++] = global;
                break;
            case 0xBU:
                if (global_stack_depth == 0U) {
                    return false;
                }
                global = global_stack[--global_stack_depth];
                break;
            default: break;
            }
        } else if (type == 2U) {
            const uint32_t usage = expand_usage(global.usage_page, value, item_size);
            if (tag == 0x0U && local.usage_count < HID_MAX_LOCAL_USAGES) {
                local.usages[local.usage_count++] = usage;
            } else if (tag == 0x1U) {
                local.usage_minimum = usage;
                local.has_usage_minimum = true;
            } else if (tag == 0x2U) {
                local.usage_maximum = usage;
                local.has_usage_maximum = true;
            }
        } else if (type == 0U) {
            if (tag == 0xAU) {
                ++collection_depth;
                const uint32_t usage = local_usage_at(&local, 0);
                if (value == HID_COLLECTION_APPLICATION &&
                    (uint16_t)(usage >> 16U) == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                    (uint16_t)usage == HID_USAGE_MOUSE) {
                    mouse_collection_depth = collection_depth;
                }
            } else if (tag == 0xCU) {
                if (collection_depth == 0U) {
                    return false;
                }
                if (mouse_collection_depth == collection_depth) {
                    mouse_collection_depth = 0U;
                }
                --collection_depth;
            } else if (tag == 0x8U) {
                consume_input(reports, &global, &local, value,
                              mouse_collection_depth != 0U);
            }
            reset_local(&local);
        }
        cursor += item_size;
    }

    for (size_t index = 0; index < HID_MAX_REPORT_STATES; ++index) {
        hid_report_state_t *state = &reports[index];
        if (!state->used ||
            state->layout.x.bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET ||
            state->layout.y.bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET ||
            state->input_bits == 0U || state->input_bits > UINT16_MAX) {
            continue;
        }
        state->layout.report_bytes = (uint16_t)((state->input_bits + 7U) / 8U);
        state->layout.valid = true;
        *layout = state->layout;
        return true;
    }
    return false;
}

static bool write_bits(
    uint8_t *report,
    size_t report_length,
    uint16_t bit_offset,
    uint8_t bit_size,
    uint32_t value)
{
    if (report == NULL || bit_size == 0U || bit_size > 32U ||
        (uint32_t)bit_offset + bit_size > report_length * 8U) {
        return false;
    }
    for (uint8_t bit = 0; bit < bit_size; ++bit) {
        const uint32_t destination_bit = (uint32_t)bit_offset + bit;
        const uint8_t mask = (uint8_t)(1U << (destination_bit & 7U));
        if ((value & (1UL << bit)) != 0U) {
            report[destination_bit >> 3U] |= mask;
        } else {
            report[destination_bit >> 3U] &= (uint8_t)~mask;
        }
    }
    return true;
}

static bool write_axis(
    uint8_t *report,
    size_t report_length,
    const hid_mouse_axis_field_t *field,
    int32_t value)
{
    if (field->bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET) {
        return value == 0;
    }
    if (field->bit_size == 0U || field->bit_size > 32U ||
        field->logical_minimum >= field->logical_maximum) {
        return false;
    }
    if (value < field->logical_minimum) {
        value = field->logical_minimum;
    } else if (value > field->logical_maximum) {
        value = field->logical_maximum;
    }
    uint32_t encoded = (uint32_t)value;
    if (field->bit_size < 32U) {
        encoded &= (1UL << field->bit_size) - 1U;
    }
    return write_bits(report, report_length, field->bit_offset,
                      field->bit_size, encoded);
}

bool hid_mouse_report_apply_overlay(
    const hid_mouse_report_layout_t *layout,
    uint8_t *report,
    size_t report_length,
    uint8_t software_buttons,
    int32_t x,
    int32_t y,
    int32_t wheel,
    int32_t pan)
{
    if (layout == NULL || report == NULL || !layout->valid ||
        report_length != layout->report_bytes ||
        layout->buttons_bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET ||
        (uint32_t)layout->buttons_bit_offset + layout->button_count >
            report_length * 8U) {
        return false;
    }
    const uint8_t overlay_buttons = layout->button_count < 8U
        ? layout->button_count : 8U;
    for (uint8_t index = 0; index < overlay_buttons; ++index) {
        if ((software_buttons & (uint8_t)(1U << index)) == 0U) {
            continue;
        }
        const uint32_t destination_bit =
            (uint32_t)layout->buttons_bit_offset + index;
        report[destination_bit >> 3U] |=
            (uint8_t)(1U << (destination_bit & 7U));
    }
    return write_axis(report, report_length, &layout->x, x) &&
           write_axis(report, report_length, &layout->y, y) &&
           write_axis(report, report_length, &layout->wheel, wheel) &&
           write_axis(report, report_length, &layout->pan, pan);
}

static bool read_axis(
    const uint8_t *report,
    size_t report_length,
    const hid_mouse_axis_field_t *field,
    int32_t *value)
{
    if (value == NULL) {
        return false;
    }
    if (field->bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET) {
        *value = 0;
        return true;
    }
    if (field->bit_size == 0U || field->bit_size > 32U ||
        (uint32_t)field->bit_offset + field->bit_size > report_length * 8U) {
        return false;
    }
    uint32_t raw = 0U;
    for (uint8_t bit = 0; bit < field->bit_size; ++bit) {
        const uint32_t source_bit = (uint32_t)field->bit_offset + bit;
        if ((report[source_bit >> 3U] & (uint8_t)(1U << (source_bit & 7U))) != 0U) {
            raw |= (1UL << bit);
        }
    }
    /* 有符号字段按位宽做符号扩展（相对轴通常是有符号的）。 */
    if (field->bit_size < 32U && (raw & (1UL << (field->bit_size - 1U))) != 0U) {
        raw |= ~((1UL << field->bit_size) - 1U);
    }
    *value = (int32_t)raw;
    return true;
}

bool hid_mouse_report_read_axes(
    const uint8_t *report,
    size_t report_length,
    const hid_mouse_report_layout_t *layout,
    int32_t *x,
    int32_t *y,
    int32_t *wheel,
    int32_t *pan)
{
    if (report == NULL || layout == NULL || !layout->valid) {
        return false;
    }
    return read_axis(report, report_length, &layout->x, x) &&
        read_axis(report, report_length, &layout->y, y) &&
        read_axis(report, report_length, &layout->wheel, wheel) &&
           read_axis(report, report_length, &layout->pan, pan);
}

bool hid_mouse_report_read_buttons(
    const uint8_t *report,
    size_t report_length,
    const hid_mouse_report_layout_t *layout,
    uint8_t *buttons)
{
    if (report == NULL || layout == NULL || buttons == NULL || !layout->valid ||
        report_length != layout->report_bytes || layout->button_count == 0U ||
        layout->buttons_bit_offset == HID_MOUSE_FIELD_INVALID_OFFSET ||
        (uint32_t)layout->buttons_bit_offset + layout->button_count >
            report_length * 8U) {
        return false;
    }
    const uint8_t count = layout->button_count < 5U ? layout->button_count : 5U;
    uint8_t result = 0U;
    for (uint8_t index = 0U; index < count; ++index) {
        const uint32_t source_bit = (uint32_t)layout->buttons_bit_offset + index;
        if ((report[source_bit >> 3U] & (uint8_t)(1U << (source_bit & 7U))) != 0U) {
            result |= (uint8_t)(1U << index);
        }
    }
    *buttons = result;
    return true;
}

bool hid_mouse_report_apply_physical_masks(
    const hid_mouse_report_layout_t *layout,
    uint8_t *report,
    size_t report_length,
    uint8_t button_mask,
    uint8_t move_mask,
    uint8_t wheel_mask)
{
    if (report == NULL || layout == NULL || !layout->valid ||
        report_length != layout->report_bytes) {
        return false;
    }
    if (layout->buttons_bit_offset != HID_MOUSE_FIELD_INVALID_OFFSET) {
        const uint8_t count = layout->button_count < 5U
            ? layout->button_count : 5U;
        if ((uint32_t)layout->buttons_bit_offset + count > report_length * 8U) {
            return false;
        }
        for (uint8_t button = 0U; button < count; ++button) {
            if ((button_mask & (uint8_t)(1U << button)) == 0U) {
                continue;
            }
            const uint32_t bit = (uint32_t)layout->buttons_bit_offset + button;
            report[bit >> 3U] &= (uint8_t)~(1U << (bit & 7U));
        }
    }

    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
    if (!hid_mouse_report_read_axes(report, report_length, layout,
                                    &x, &y, &wheel, &pan)) {
        return false;
    }
    if ((x < 0 && (move_mask & 0x01U) != 0U) ||
        (x > 0 && (move_mask & 0x02U) != 0U)) {
        x = 0;
    }
    if ((y > 0 && (move_mask & 0x04U) != 0U) ||
        (y < 0 && (move_mask & 0x08U) != 0U)) {
        y = 0;
    }
    if ((wheel < 0 && (wheel_mask & 0x01U) != 0U) ||
        (wheel > 0 && (wheel_mask & 0x02U) != 0U)) {
        wheel = 0;
    }
    return write_axis(report, report_length, &layout->x, x) &&
        write_axis(report, report_length, &layout->y, y) &&
        write_axis(report, report_length, &layout->wheel, wheel) &&
        write_axis(report, report_length, &layout->pan, pan);
}

bool hid_mouse_report_add_axes(
    uint8_t *report,
    size_t report_length,
    const hid_mouse_report_layout_t *layout,
    int32_t delta_x,
    int32_t delta_y,
    int32_t delta_wheel,
    int32_t delta_pan)
{
    if (report == NULL || layout == NULL || !layout->valid) {
        return false;
    }
    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
    if (!hid_mouse_report_read_axes(report, report_length, layout,
                                    &x, &y, &wheel, &pan)) {
        return false;
    }
    return write_axis(report, report_length, &layout->x, x + delta_x) &&
        write_axis(report, report_length, &layout->y, y + delta_y) &&
        write_axis(report, report_length, &layout->wheel, wheel + delta_wheel) &&
        write_axis(report, report_length, &layout->pan, pan + delta_pan);
}
