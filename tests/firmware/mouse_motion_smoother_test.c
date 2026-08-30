#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "mouse_motion_smoother.h"

static void test_five_slots_preserve_signed_totals(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 20, .y = -7, .wheel = 3, .pan = -2},
        MOUSE_MOTION_SMOOTHING_SLOTS);

    mouse_motion_delta_t total = {0};
    for (uint8_t index = 0; index < MOUSE_MOTION_SMOOTHING_SLOTS; ++index) {
        mouse_motion_delta_t part = mouse_motion_smoother_take_next(&state);
        total.x += part.x;
        total.y += part.y;
        total.wheel += part.wheel;
        total.pan += part.pan;
    }
    assert(total.x == 20);
    assert(total.y == -7);
    assert(total.wheel == 3);
    assert(total.pan == -2);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_500hz_commands_overlap_in_one_rolling_window(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 10},
        MOUSE_MOTION_SMOOTHING_SLOTS);
    mouse_motion_delta_t first = mouse_motion_smoother_take_next(&state);
    mouse_motion_delta_t second = mouse_motion_smoother_take_next(&state);
    assert(first.x == 2);
    assert(second.x == 2);

    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 5},
        MOUSE_MOTION_SMOOTHING_SLOTS);
    int64_t remaining = first.x + second.x;
    for (uint8_t index = 0; index < MOUSE_MOTION_SMOOTHING_SLOTS; ++index) {
        remaining += mouse_motion_smoother_take_next(&state).x;
    }
    assert(remaining == 15);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_direct_mode_drains_existing_plan_without_loss(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 9, .y = -4},
        MOUSE_MOTION_SMOOTHING_SLOTS);
    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 3, .y = 2},
        0);

    mouse_motion_delta_t immediate = mouse_motion_smoother_take_next(&state);
    assert(immediate.x == 12);
    assert(immediate.y == -2);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_reset_discards_every_slot(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(
        &state,
        (mouse_motion_delta_t){.x = 25},
        MOUSE_MOTION_SMOOTHING_SLOTS);
    mouse_motion_smoother_reset(&state);
    assert(!mouse_motion_smoother_has_pending(&state));
    assert(mouse_motion_smoother_take_next(&state).x == 0);
}

int main(void)
{
    assert(mouse_motion_smoother_valid_slot_count(0));
    assert(mouse_motion_smoother_valid_slot_count(MOUSE_MOTION_SMOOTHING_SLOTS));
    assert(!mouse_motion_smoother_valid_slot_count(1));
    test_five_slots_preserve_signed_totals();
    test_500hz_commands_overlap_in_one_rolling_window();
    test_direct_mode_drains_existing_plan_without_loss();
    test_reset_discards_every_slot();
    puts("固件鼠标平滑测试通过：5 槽守恒、500 Hz 重叠、直通排空与重置均符合预期。");
    return 0;
}
