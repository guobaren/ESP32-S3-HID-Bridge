#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DUAL_BUTTON_STATE_MASK 0x1FU
#define DUAL_BUTTON_STATE_VALID 0x80U
#define DUAL_BUTTON_STATE_HEARTBEAT_US 5000LL

typedef struct {
    uint8_t buttons;
    bool valid;
    bool subscribed;
    bool dirty;
    bool has_successful_send;
    uint32_t generation;
    uint32_t observation_epoch;
    int64_t last_success_us;
} dual_button_state_t;

void dual_button_state_init(dual_button_state_t *state);
void dual_button_state_set_subscribed(dual_button_state_t *state, bool subscribed);
void dual_button_state_update_physical(dual_button_state_t *state, uint8_t buttons);
bool dual_button_state_update_physical_for_epoch(
    dual_button_state_t *state, uint32_t observation_epoch, uint8_t buttons);
void dual_button_state_invalidate(dual_button_state_t *state);
bool dual_button_state_invalidate_for_epoch(
    dual_button_state_t *state, uint32_t observation_epoch);
uint32_t dual_button_state_observation_epoch(const dual_button_state_t *state);
bool dual_button_state_due(const dual_button_state_t *state, int64_t now_us);
uint8_t dual_button_state_payload(const dual_button_state_t *state);
uint32_t dual_button_state_generation(const dual_button_state_t *state);
void dual_button_state_record_send(
    dual_button_state_t *state,
    uint32_t sent_generation,
    bool success,
    int64_t now_us);
