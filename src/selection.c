#include "selection.h"

/* Counter wrap uses serial-number arithmetic. At exactly half the range there
 * is no defined newer value, so retain the current token. Origin B wins ties.
 */
static bool counter_newer(uint32_t incoming, uint32_t current) {
    uint32_t distance = incoming - current;
    return distance != 0 && distance < UINT32_C(0x80000000);
}

static bool token_newer(const selection_state_t *incoming, const selection_state_t *current) {
    return counter_newer(incoming->counter, current->counter)
        || (incoming->counter == current->counter && incoming->origin > current->origin);
}

static void copy_token(selection_state_t *dest, const selection_state_t *source) {
    dest->counter = source->counter;
    dest->origin = source->origin;
    dest->output = source->output;
}

void selection_request(selection_state_t *state, uint8_t output, uint8_t origin) {
    state->counter++;
    state->origin = origin;
    state->output = output;
    state->pending_local = !state->joined && !state->joining;
}

void selection_legacy_request(selection_state_t *state, uint8_t output, uint8_t origin) {
    selection_request(state, output, origin);
    /* Legacy peers cannot synchronize tokens. Preserve their immediate commands
     * without claiming loss/reorder recovery or retaining an older local intent. */
    state->joined = true;
    state->joining = false;
    state->pending_local = false;
}

bool selection_merge(selection_state_t *state, const selection_state_t *incoming,
                     uint8_t local_origin) {
    if (!state->joined && !state->joining) {
        if (incoming->joined) {
            if (state->pending_local) {
                /* Preserve the latest local prejoin intent once, rebasing above
                 * the live peer's established token. No ongoing rebase loop. */
                if (counter_newer(incoming->counter, state->counter))
                    state->counter = incoming->counter;
                state->counter++;
                state->origin = local_origin;
            } else {
                /* A passive reboot rejoins the live state, including counters
                 * near wrap; boot default A must not become a fresh command. */
                copy_token(state, incoming);
            }
            state->joined = true;
        } else {
            /* Seeing a fresh peer starts a handshake, not an established
             * session. Its JOINING reply must not cause the other fresh peer
             * to rebase a losing concurrent intent as if it had rebooted. */
            if (token_newer(incoming, state))
                copy_token(state, incoming);
            state->joined = incoming->joining;
            state->joining = !state->joined;
        }
        state->pending_local = false;
        return true;
    }

    bool established = state->joining && (incoming->joining || incoming->joined);
    if (established) {
        state->joining = false;
        state->joined = true;
    }
    /* Delayed empty boot announcements cannot supersede a joined state. */
    if (!incoming->joined && !incoming->joining && !incoming->pending_local)
        return established;
    if (!token_newer(incoming, state))
        return established;
    copy_token(state, incoming);
    return true;
}

void selection_encode(const selection_state_t *state, uint8_t data[8]) {
    /* Byte zero remains compatible with every legacy OUTPUT_SELECT receiver.
     * Flags: bit0 origin; exclusive phases bit5 joining, bit6 fresh local
     * intent, bit7 joined. Empty fresh boot state has no phase bit. */
    data[0] = state->output;
    data[1] = state->origin | (state->joined ? 0x80 : 0)
            | (state->joining ? 0x20 : 0)
            | (state->pending_local ? 0x40 : 0);
    data[2] = 0x53;
    data[3] = 0x31;
    for (unsigned i = 0; i < 4; i++)
        data[4 + i] = (uint8_t)(state->counter >> (8 * i));
}

bool selection_decode(const uint8_t data[8], selection_state_t *state) {
    unsigned phase = data[1] & 0xe0u;
    if (data[0] > 1 || data[2] != 0x53 || data[3] != 0x31
        || (data[1] & ~0xe1u) != 0 || (phase != 0 && phase != 0x20 && phase != 0x40 && phase != 0x80))
        return false;
    *state = (selection_state_t){
        .output = data[0], .origin = data[1] & 1,
        .joined = (data[1] & 0x80) != 0, .joining = (data[1] & 0x20) != 0,
        .pending_local = (data[1] & 0x40) != 0,
    };
    for (unsigned i = 0; i < 4; i++)
        state->counter |= (uint32_t)data[4 + i] << (8 * i);
    /* The only valid unjoined state without user intent is the boot default. */
    return state->joined || state->joining || state->pending_local
        || (state->counter == 0 && state->origin == 0 && state->output == 0);
}

bool selection_is_legacy(const uint8_t data[8]) {
    if (data[0] > 1)
        return false;
    for (unsigned i = 1; i < 8; i++)
        if (data[i] != 0)
            return false;
    return true;
}
