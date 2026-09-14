#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Runtime-only selection reconciliation; no persistent configuration change.
 * Zero initialization describes an unjoined Pico at the boot default, output A.
 * Serial-number ordering requires fewer than 2^31 outstanding generations.
 */
typedef struct {
    uint32_t counter;
    uint8_t origin;
    uint8_t output;
    bool joined;
    bool joining;
    bool pending_local;
} selection_state_t;

void selection_request(selection_state_t *, uint8_t output, uint8_t origin);
void selection_legacy_request(selection_state_t *, uint8_t output, uint8_t origin);
bool selection_merge(selection_state_t *, const selection_state_t *, uint8_t local_origin);
void selection_encode(const selection_state_t *, uint8_t data[8]);
bool selection_decode(const uint8_t data[8], selection_state_t *);
bool selection_is_legacy(const uint8_t data[8]);
