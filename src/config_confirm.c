/* Bounded, core-0-owned confirmed configuration command service.
 * USB/UART callbacks only enqueue; flash and routing happen in the task.
 * No legacy proxy encapsulation: it truncates the eighth payload byte. */
#include "main.h"
#include "config_confirm.h"

typedef struct {
    uint32_t received_us;
    uint8_t data[8];
    uint16_t usb_epoch;
    uint8_t type;
    bool usb;
} ingress_t;

enum phase { CONFIRM_IDLE, ASSEMBLING, FORWARDING, WAITING, REPLYING };
typedef struct {
    uint64_t value;
    uint32_t token, started_us, result;
    uint16_t usb_epoch;
    uint8_t role, op, key, parts, phase, step, status;
    bool usb, invalid, replay, ack_meta;
} transaction_t;

typedef struct {
    uint64_t value;
    uint32_t token, result;
    uint16_t usb_epoch;
    uint8_t role, op, key, status;
    bool usb, valid;
} cached_t;

static queue_t ingress;
static bool initialized;
static uint16_t usb_epoch;
static transaction_t current;
static cached_t previous;

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static bool request_type(uint8_t type) {
    return type >= CONFIG_CONFIRM_META_MSG && type <= CONFIG_CONFIRM_EXEC_MSG;
}

void config_confirm_init(void) {
    memset(&current, 0, sizeof(current));
    memset(&previous, 0, sizeof(previous));
    usb_epoch = 0;
    queue_init(&ingress, sizeof(ingress_t), 8);
    initialized = true;
}
void config_confirm_shutdown(void) {
    if (initialized) queue_free(&ingress);
    initialized = false;
}
void config_confirm_usb_disconnect(void) {
    ++usb_epoch;
    if (previous.usb) previous.valid = false;
}

static bool enqueue(const uart_packet_t *packet, bool usb) {
    if (!initialized) return false;
    ingress_t item = {.received_us = time_us_32(), .type = packet->type,
                     .usb = usb, .usb_epoch = usb ? usb_epoch : 0};
    memcpy(item.data, packet->data, sizeof(item.data));
    /* Queue overflow is deliberately an unknown/no-confirmation outcome.
     * Never execute a partial transaction or synthesize success. */
    return queue_try_add(&ingress, &item);
}
bool config_confirm_usb_request(const uart_packet_t *packet, device_t *state) {
    return state->config_mode_active && request_type(packet->type) && enqueue(packet, true);
}
void handle_config_confirm_msg(uart_packet_t *packet, device_t *state) {
    (void)state;
    if (request_type(packet->type) || packet->type == CONFIG_CONFIRM_ACK_META_MSG
        || packet->type == CONFIG_CONFIRM_ACK_VALUE_MSG)
        enqueue(packet, false);
}

static void finish(config_confirm_status_t status, uint32_t result) {
    current.status = status;
    current.result = result;
    current.phase = REPLYING;
    current.step = 0;
    current.started_us = time_us_32();
    /* Do not destroy the original receipt after a malformed replay. */
    if (!current.replay) {
        previous = (cached_t){.value = current.value, .token = current.token,
            .result = result, .usb_epoch = current.usb_epoch, .role = current.role,
            .op = current.op, .key = current.key, .status = status,
            .usb = current.usb, .valid = true};
    }
}

static bool mutable_state_busy(const device_t *state) {
    return state->fw.upgrade_in_progress || state->fw.image_dirty || state->reboot_requested
        || state->maintenance_reserved || state->config_bootloader_local_pending
        || state->config_bootloader_peer_pending;
}

static void execute(device_t *state) {
    /* A malformed bridge assembly was never sent to the peer. Do not invent
     * a receipt bearing its physical role; the host gets no confirmation. */
    if (current.role != state->board_role && (current.invalid || current.parts != 3)) {
        current.phase = CONFIRM_IDLE;
        return;
    }
    if (current.invalid) { finish(CONFIG_CONFIRM_INVALID, 0); return; }
    if (current.parts != 3) { finish(CONFIG_CONFIRM_INCOMPLETE, 0); return; }
    if (current.replay) {
        bool same = current.role == previous.role && current.op == previous.op
            && current.key == previous.key && current.value == previous.value;
        if (!same && current.role != state->board_role) {
            current.phase = CONFIRM_IDLE;
            return;
        }
        finish(same ? previous.status : CONFIG_CONFIRM_INVALID, same ? previous.result : 0);
        return;
    }
    if (current.role != state->board_role) {
        /* Only USB ingress can route. A UART request can never bounce back. */
        if (!current.usb) { current.phase = CONFIRM_IDLE; return; }
        current.phase = FORWARDING;
        current.step = 0;
        return;
    }
    uint32_t result = 0;
    config_confirm_status_t status = CONFIG_CONFIRM_OK;
    switch (current.op) {
        case CONFIG_CONFIRM_CAP:
            if (current.key || current.value) status = CONFIG_CONFIRM_INVALID;
            else result = CONFIG_CONFIRM_VERSION;
            break;
        case CONFIG_CONFIRM_QUERY_DIGEST:
            if (current.key || current.value) status = CONFIG_CONFIRM_INVALID;
            else result = config_digest(state);
            break;
        case CONFIG_CONFIRM_CHECK_VALUE:
            status = config_check_value64(state, current.key, current.value, &result);
            break;
        case CONFIG_CONFIRM_SAVE:
            if (current.key || current.value > UINT32_MAX) status = CONFIG_CONFIRM_INVALID;
            else status = config_save_confirmed(state, (uint32_t)current.value, &result);
            break;
        case CONFIG_CONFIRM_SET:
        case CONFIG_CONFIRM_BORDER_PAIR:
            if (!firmware_update_try_lock()) { status = CONFIG_CONFIRM_BUSY; break; }
            if (mutable_state_busy(state)) status = CONFIG_CONFIRM_BUSY;
            else if (current.op == CONFIG_CONFIRM_SET) {
                if (!config_set_value64(state, current.key, current.value))
                    status = CONFIG_CONFIRM_INVALID;
            } else {
                border_size_t border = {.top = (int32_t)(uint32_t)current.value,
                                        .bottom = (int32_t)(uint32_t)(current.value >> 32)};
                if (!config_set_border(state, current.key, &border))
                    status = CONFIG_CONFIRM_INVALID;
            }
            if (status == CONFIG_CONFIRM_OK) result = config_digest(state);
            firmware_update_unlock();
            break;
        default: status = CONFIG_CONFIRM_INVALID; break;
    }
    finish(status, result);
}

static void receive(const ingress_t *item, device_t *state) {
    uint32_t token = get32(item->data);
    if (!token || (item->usb && (item->usb_epoch != usb_epoch || !state->config_mode_active)))
        return;
    if (!request_type(item->type)) {
        /* Only the owner of an outstanding forwarded command may consume a
         * peer receipt. Neither unsolicited replies nor USB ACK injection route. */
        if (item->usb || current.phase != WAITING || !current.usb
            || current.role == state->board_role || token != current.token) return;
        if (item->type == CONFIG_CONFIRM_ACK_META_MSG) {
            if (item->data[4] != current.role || item->data[5] != current.op
                || item->data[6] != current.key || item->data[7] > CONFIG_CONFIRM_INCOMPLETE)
                return;
            if (current.ack_meta && current.status != item->data[7]) {
                /* Contradictory receipts are not confirmation of either
                 * outcome. Do not combine one status with another value. */
                current.phase = CONFIRM_IDLE;
                return;
            }
            current.status = item->data[7];
            current.ack_meta = true;
        } else if (current.ack_meta) {
            finish(current.status, get32(item->data + 4));
        }
        return;
    }
    if (item->type == CONFIG_CONFIRM_META_MSG) {
        if (item->data[4] > OUTPUT_B || (!item->usb && item->data[4] != state->board_role))
            return;
        if (current.phase != CONFIRM_IDLE) {
            if (current.phase == ASSEMBLING && current.token == token && current.usb == item->usb
                && (current.role != item->data[4] || current.op != item->data[5]
                    || current.key != item->data[6] || item->data[7]))
                current.invalid = true;
            return;
        }
        current = (transaction_t){.token = token, .started_us = item->received_us,
            .usb_epoch = item->usb_epoch, .role = item->data[4], .op = item->data[5],
            .key = item->data[6], .phase = ASSEMBLING, .usb = item->usb,
            .invalid = item->data[7] != 0,
            .replay = previous.valid && previous.token == token && previous.usb == item->usb
                && (!item->usb || previous.usb_epoch == item->usb_epoch)};
        reset_config_timer(state);
        return;
    }
    /* Replaying only EXEC is safe for the last completed transaction, including
     * after its receipt was lost. Never run its flash/SET action twice. */
    if (current.phase == CONFIRM_IDLE && item->type == CONFIG_CONFIRM_EXEC_MSG
        && get32(item->data + 4) == 0 && previous.valid && previous.token == token
        && previous.usb == item->usb && (!item->usb || previous.usb_epoch == item->usb_epoch)) {
        current = (transaction_t){.value = previous.value, .token = token,
            .started_us = item->received_us, .usb_epoch = item->usb_epoch,
            .role = previous.role, .op = previous.op, .key = previous.key,
            .usb = item->usb, .replay = true};
        finish(previous.status, previous.result);
        return;
    }
    if (current.phase != ASSEMBLING || current.token != token || current.usb != item->usb)
        return;
    if (item->type == CONFIG_CONFIRM_EXEC_MSG) {
        if (get32(item->data + 4)) current.invalid = true;
        execute(state);
        return;
    }
    unsigned shift = item->type == CONFIG_CONFIRM_HI_MSG ? 32 : 0;
    uint8_t part = shift ? 2 : 1;
    uint64_t value = get32(item->data + 4);
    if ((current.parts & part) && (uint32_t)(current.value >> shift) != value)
        current.invalid = true;
    current.value = (current.value & ~(UINT64_C(0xffffffff) << shift)) | (value << shift);
    current.parts |= part;
}

static void transport(device_t *state) {
    uart_packet_t packet = {0};
    put32(packet.data, current.token);
    if (current.phase == FORWARDING) {
        packet.type = CONFIG_CONFIRM_META_MSG + current.step;
        if (current.step == 0) {
            packet.data[4] = current.role; packet.data[5] = current.op; packet.data[6] = current.key;
        } else if (current.step == 1) put32(packet.data + 4, (uint32_t)current.value);
        else if (current.step == 2) put32(packet.data + 4, (uint32_t)(current.value >> 32));
        if (queue_packet_try(packet.data, packet.type, 8) && ++current.step == 4)
            current.phase = WAITING;
    } else if (current.phase == REPLYING) {
        packet.type = current.step ? CONFIG_CONFIRM_ACK_VALUE_MSG : CONFIG_CONFIRM_ACK_META_MSG;
        if (current.step) put32(packet.data + 4, current.result);
        else {
            packet.data[4] = current.role; packet.data[5] = current.op;
            packet.data[6] = current.key; packet.data[7] = current.status;
        }
        bool sent = current.usb ? queue_cfg_packet_try(&packet, state)
                                : queue_packet_try(packet.data, packet.type, 8);
        if (sent && ++current.step == 2) current.phase = CONFIRM_IDLE;
    }
}

void config_confirm_task(device_t *state) {
    if (!initialized) return;
    if (current.phase != CONFIRM_IDLE && current.usb
        && (current.usb_epoch != usb_epoch || !state->config_mode_active))
        current.phase = CONFIRM_IDLE;
    if (current.phase != CONFIRM_IDLE
        && (uint32_t)(time_us_32() - current.started_us) >= CONFIG_CONFIRM_TIMEOUT_US) {
        /* A missing peer receipt cannot establish whether it already acted.
         * Only the physical target may originate an ACK for that role. */
        if (current.phase == REPLYING || current.role != state->board_role)
            current.phase = CONFIRM_IDLE;
        else finish(CONFIG_CONFIRM_EXPIRED, 0);
    }
    ingress_t item;
    if (queue_try_remove(&ingress, &item)
        && (uint32_t)(time_us_32() - item.received_us) < CONFIG_CONFIRM_TIMEOUT_US)
        receive(&item, state);
    transport(state);
}
