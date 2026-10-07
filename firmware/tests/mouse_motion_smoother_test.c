#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "mouse_motion_smoother.h"

static void test_supported_slot_counts_preserve_signed_totals(void)
{
    static const uint8_t choices[] = {5U, 10U, 15U, 20U};
    for (size_t choice = 0U; choice < sizeof(choices); ++choice) {
        const uint8_t slots = choices[choice];
        mouse_motion_smoother_t state = {0};
        mouse_motion_smoother_enqueue(
            &state,
            (mouse_motion_delta_t){.x = 23, .y = -17, .wheel = 7, .pan = -3},
            slots);

        mouse_motion_delta_t total = {0};
        for (uint8_t index = 0U; index < slots; ++index) {
            mouse_motion_delta_t part = mouse_motion_smoother_take_next(&state);
            total.x += part.x;
            total.y += part.y;
            total.wheel += part.wheel;
            total.pan += part.pan;
        }
        assert(total.x == 23);
        assert(total.y == -17);
        assert(total.wheel == 7);
        assert(total.pan == -3);
        assert(!mouse_motion_smoother_has_pending(&state));
    }
}

static void test_500hz_commands_overlap_in_one_rolling_window(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 10}, 5U);
    mouse_motion_delta_t first = mouse_motion_smoother_take_next(&state);
    mouse_motion_delta_t second = mouse_motion_smoother_take_next(&state);
    assert(first.x == 2);
    assert(second.x == 2);

    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 5}, 5U);
    int64_t total = first.x + second.x;
    for (uint8_t index = 0U; index < 5U; ++index) {
        total += mouse_motion_smoother_take_next(&state).x;
    }
    assert(total == 15);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_slot_changes_preserve_old_pending_delta(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = -23}, 20U);
    mouse_motion_delta_t emitted = mouse_motion_smoother_take_next(&state);
    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 4}, 10U);
    mouse_motion_delta_t pending = mouse_motion_smoother_pending(&state);
    assert(emitted.x + pending.x == -19);

    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 3}, 0U);
    mouse_motion_delta_t direct = mouse_motion_smoother_take_next(&state);
    assert(direct.x == pending.x + 3);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_invalid_slot_count_is_rejected_without_mutation(void)
{
    mouse_motion_smoother_t state = {0};
    assert(mouse_motion_smoother_valid_slot_count(0U));
    assert(mouse_motion_smoother_valid_slot_count(5U));
    assert(mouse_motion_smoother_valid_slot_count(10U));
    assert(mouse_motion_smoother_valid_slot_count(15U));
    assert(mouse_motion_smoother_valid_slot_count(20U));
    assert(!mouse_motion_smoother_valid_slot_count(1U));
    assert(!mouse_motion_smoother_valid_slot_count(25U));
    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 9}, 1U);
    assert(!mouse_motion_smoother_has_pending(&state));
}

static void test_release_reset_discards_every_slot(void)
{
    mouse_motion_smoother_t state = {0};
    mouse_motion_smoother_enqueue(&state, (mouse_motion_delta_t){.x = 25}, 20U);
    mouse_motion_smoother_reset(&state);
    assert(!mouse_motion_smoother_has_pending(&state));
    assert(mouse_motion_smoother_take_next(&state).x == 0);
}

int main(void)
{
    test_supported_slot_counts_preserve_signed_totals();
    test_500hz_commands_overlap_in_one_rolling_window();
    test_slot_changes_preserve_old_pending_delta();
    test_invalid_slot_count_is_rejected_without_mutation();
    test_release_reset_discards_every_slot();
    puts("固件鼠标平滑测试通过：0/5/10/15/20槽、正负守恒、重叠、设置切换保留与释放清空。");
    return 0;
}
