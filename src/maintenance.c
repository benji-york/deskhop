/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "maintenance.h"

enum { ACK_ACCEPTED, ACK_BUSY, ACK_UPDATE_ACTIVE, ACK_REBOOT_PENDING };
typedef enum { CLIENT_IDLE, CLIENT_LOCAL, CLIENT_ADMISSION, CLIENT_REPLY } client_phase_t;
typedef enum { SERVER_IDLE, SERVER_ACK, SERVER_DRAIN } server_phase_t;
typedef struct { uint8_t type, data[8]; uint64_t received_at_us; } ingress_t;

static queue_t ingress;
static bool initialized;
/* All state below is owned by core 0. Core 1 only copies to ingress. The RAM
 * reservation in global_state is additionally protected by the updater lock. */
static uint8_t local_role;
static uint16_t session_tag;
static client_phase_t client_phase;
static server_phase_t server_phase;
static uint8_t client_payload[8], server_payload[8], last_request[8];
static bool last_request_valid, server_accepted, local_reply_complete;
static bool server_ack_queued, server_ack_sent;
static uint64_t client_started_us, client_sent_us, server_started_us;
static uint32_t client_token;
static uint8_t client_target;
static maintenance_result_t result;
static bool result_pending;

static bool expired(uint64_t now, uint64_t start, uint64_t timeout) {
    return now - start >= timeout;
}

/* Caller holds the updater lock. A source can be idle locally while serving
 * a peer's dirty image, so recent accepted word requests also deny entry. */
static maintenance_start_t safety_locked(uint64_t now_us) {
    if (global_state.reboot_requested)
        return MAINTENANCE_REBOOT_PENDING;
    if (global_state.fw.upgrade_in_progress || global_state.fw.image_dirty
        || (global_state.maintenance_source_seen
            && !expired(now_us, global_state.maintenance_source_last_us,
                        MAINTENANCE_SOURCE_LEASE_US)))
        return MAINTENANCE_UPDATE_ACTIVE;
    if (global_state.config_bootloader_peer_pending || global_state.config_bootloader_local_pending)
        return MAINTENANCE_BUSY;
    return MAINTENANCE_STARTED;
}

static void update_reservation_locked(void) {
    global_state.maintenance_reserved = client_phase != CLIENT_IDLE
        || (server_phase != SERVER_IDLE && server_accepted);
}

static void finish_client(maintenance_outcome_t outcome) {
    result = (maintenance_result_t){client_token, client_target, outcome};
    result_pending = true;
    client_phase = CLIENT_IDLE;
    local_reply_complete = false;
    firmware_update_lock();
    update_reservation_locked();
    firmware_update_unlock();
}

static bool wire_idle(void) {
    return queue_is_empty(&global_state.uart_tx_queue)
        && !dma_channel_is_busy(global_state.dma_tx_channel)
        && !(uart_get_hw(SERIAL_UART)->fr & UART_UARTFR_BUSY_BITS);
}

void maintenance_shutdown(void) {
    if (initialized)
        queue_free(&ingress);
    initialized = false;
}

void maintenance_init(uint8_t role, uint64_t boot_session) {
    maintenance_shutdown();
    local_role = role;
    session_tag = boot_session ^ (boot_session >> 16) ^ (boot_session >> 32)
        ^ (boot_session >> 48);
    client_phase = CLIENT_IDLE;
    server_phase = SERVER_IDLE;
    result_pending = false;
    last_request_valid = false;
    server_accepted = local_reply_complete = false;
    global_state.maintenance_reserved = false;
    global_state.maintenance_source_seen = false;
    global_state.maintenance_source_last_us = 0;
    server_ack_queued = server_ack_sent = false;
    queue_init(&ingress, sizeof(ingress_t), 8);
    initialized = true;
}

maintenance_start_t maintenance_request(uint8_t target, uint32_t token, uint64_t now_us) {
    if (!initialized || local_role > 1 || target > 1 || !token)
        return MAINTENANCE_BAD_ARGUMENT;
    if (client_phase != CLIENT_IDLE || server_phase != SERVER_IDLE || result_pending)
        return MAINTENANCE_BUSY;

    firmware_update_lock();
    maintenance_start_t safe = safety_locked(now_us);
    if (safe != MAINTENANCE_STARTED) {
        firmware_update_unlock();
        return safe;
    }
    client_token = token;
    client_target = target;
    client_started_us = now_us;
    local_reply_complete = false;
    client_payload[0] = MAINTENANCE_PROTOCOL_VERSION;
    client_payload[1] = target;
    for (unsigned i = 0; i < 4; ++i)
        client_payload[2 + i] = token >> (8 * i);
    client_payload[6] = session_tag;
    client_payload[7] = session_tag >> 8;
    client_phase = target == local_role ? CLIENT_LOCAL : CLIENT_ADMISSION;
    update_reservation_locked();
    firmware_update_unlock();
    if (client_phase == CLIENT_LOCAL) {
        result = (maintenance_result_t){token, target, MAINTENANCE_LOCAL_READY};
        result_pending = true;
    }
    return MAINTENANCE_STARTED;
}

bool maintenance_poll(maintenance_result_t *output) {
    if (!initialized || !output || !result_pending)
        return false;
    *output = result;
    result_pending = false;
    return true;
}

void maintenance_console_reply_complete(uint32_t token, uint64_t now_us) {
    if (initialized && client_phase == CLIENT_LOCAL && token == client_token
        && !expired(now_us, client_started_us, MAINTENANCE_REPLY_TIMEOUT_US))
        local_reply_complete = true;
}

void maintenance_cancel(uint32_t token, uint64_t now_us) {
    (void)now_us;
    if (!initialized || !token || token != client_token)
        return;
    client_phase = CLIENT_IDLE;
    local_reply_complete = false;
    result_pending = false;
    firmware_update_lock();
    update_reservation_locked();
    firmware_update_unlock();
}

void maintenance_receive(uint8_t type, const uint8_t payload[8], uint64_t now_us) {
    if (!initialized || !payload
        || (type != MAINTENANCE_BOOTLOADER_REQUEST_MSG && type != MAINTENANCE_BOOTLOADER_ACK_MSG)
        || payload[0] != MAINTENANCE_PROTOCOL_VERSION)
        return;
    ingress_t event = {.type = type, .received_at_us = now_us};
    memcpy(event.data, payload, sizeof(event.data));
    queue_try_add(&ingress, &event);
}

static void receive_ack(const uint8_t payload[8], uint64_t now_us) {
    if (client_phase != CLIENT_REPLY || payload[1] > ((ACK_REBOOT_PENDING << 1) | 1)
        || (payload[1] & 1) != client_target
        || expired(now_us, client_sent_us, MAINTENANCE_REPLY_TIMEOUT_US)
        || memcmp(payload + 2, client_payload + 2, 6))
        return;
    const maintenance_outcome_t outcomes[] = {
        MAINTENANCE_REMOTE_ACCEPTED, MAINTENANCE_REMOTE_BUSY,
        MAINTENANCE_REMOTE_UPDATE_ACTIVE, MAINTENANCE_REMOTE_REBOOT_PENDING,
    };
    finish_client(outcomes[payload[1] >> 1]);
}

static void receive_request(const uint8_t payload[8], uint64_t received_at, uint64_t now_us) {
    if (payload[1] != local_role || !(payload[2] | payload[3] | payload[4] | payload[5]))
        return;
    /* Duplicates never extend an accepted deadline or trigger another reset.
       A bounded ingress backlog is not permission to execute an old request. */
    if (expired(now_us, received_at, MAINTENANCE_ADMISSION_TIMEOUT_US)
        || (last_request_valid && !memcmp(payload, last_request, 8)))
        return;
    if (server_phase != SERVER_IDLE)
        return; /* Requester gets an honest unconfirmed timeout. */

    uint8_t status = ACK_BUSY;
    firmware_update_lock();
    maintenance_start_t safe = safety_locked(now_us);
    if (client_phase == CLIENT_IDLE && !result_pending && safe == MAINTENANCE_STARTED)
        status = ACK_ACCEPTED;
    else if (safe == MAINTENANCE_UPDATE_ACTIVE)
        status = ACK_UPDATE_ACTIVE;
    else if (safe == MAINTENANCE_REBOOT_PENDING)
        status = ACK_REBOOT_PENDING;
    memcpy(server_payload, payload, 8);
    server_payload[1] |= status << 1;
    memcpy(last_request, payload, 8);
    last_request_valid = true;
    server_phase = SERVER_ACK;
    server_ack_queued = server_ack_sent = false;
    server_started_us = now_us;
    server_accepted = status == ACK_ACCEPTED;
    update_reservation_locked();
    firmware_update_unlock();
}

static bool service_server(uint64_t now_us) {
    if (server_phase == SERVER_IDLE)
        return false;
    firmware_update_lock();
    /* Guard again even though normal updater admission obeys the reservation.
       Never enter ROM over a dirty image if another legacy path changes state. */
    if (server_accepted && safety_locked(now_us) != MAINTENANCE_STARTED) {
        server_phase = SERVER_IDLE;
        update_reservation_locked();
        firmware_update_unlock();
        return false;
    }
    uint64_t timeout = server_phase == SERVER_ACK ? MAINTENANCE_ADMISSION_TIMEOUT_US
                                                : MAINTENANCE_DRAIN_TIMEOUT_US;
    if (expired(now_us, server_started_us, timeout)) {
        server_phase = SERVER_IDLE;
        update_reservation_locked();
        firmware_update_unlock();
        return false;
    }
    if (server_phase == SERVER_ACK
        && queue_packet_try(server_payload, MAINTENANCE_BOOTLOADER_ACK_MSG, 8)) {
        server_ack_queued = true;
        server_phase = server_accepted ? SERVER_DRAIN : SERVER_IDLE;
        server_started_us = now_us;
        update_reservation_locked();
    }
    bool reset = server_phase == SERVER_DRAIN && server_ack_sent && wire_idle();
    if (reset) {
        server_phase = SERVER_IDLE;
        /* Hold updater exclusion through ROM entry. Tests return from this. */
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1);
        update_reservation_locked();
    }
    firmware_update_unlock();
    return reset;
}

bool maintenance_packet_allowed(uint8_t type, const uint8_t payload[8], uint64_t now_us) {
    if (type != MAINTENANCE_BOOTLOADER_REQUEST_MSG && type != MAINTENANCE_BOOTLOADER_ACK_MSG)
        return true;
    if (!initialized || !payload)
        return false;
    if (type == MAINTENANCE_BOOTLOADER_REQUEST_MSG)
        return client_phase == CLIENT_REPLY && !memcmp(payload, client_payload, 8)
            && !expired(now_us, client_sent_us, MAINTENANCE_REPLY_TIMEOUT_US);
    if (!server_ack_queued || memcmp(payload, server_payload, 8)
        || (server_accepted && server_phase != SERVER_DRAIN)
        || expired(now_us, server_started_us, MAINTENANCE_DRAIN_TIMEOUT_US))
        return false;
    server_ack_queued = false;
    server_ack_sent = server_accepted;
    return true;
}

bool maintenance_task(uint64_t now_us) {
    if (!initialized)
        return false;
    ingress_t event;
    for (unsigned i = 0; i < 8 && queue_try_remove(&ingress, &event); ++i) {
        if (event.type == MAINTENANCE_BOOTLOADER_ACK_MSG)
            receive_ack(event.data, now_us);
        else
            receive_request(event.data, event.received_at_us, now_us);
    }
    if (service_server(now_us))
        return true;
    if (client_phase == CLIENT_IDLE)
        return false;

    firmware_update_lock();
    maintenance_start_t safe = safety_locked(now_us);
    if (safe != MAINTENANCE_STARTED) {
        firmware_update_unlock();
        finish_client(client_phase == CLIENT_REPLY ? MAINTENANCE_TIMEOUT_UNCONFIRMED
                                                  : MAINTENANCE_ABORTED);
        return false;
    }
    if (client_phase == CLIENT_LOCAL) {
        if (expired(now_us, client_started_us, MAINTENANCE_REPLY_TIMEOUT_US)) {
            firmware_update_unlock();
            finish_client(MAINTENANCE_ABORTED);
            return false;
        }
        bool reset = local_reply_complete && wire_idle();
        if (reset) {
            client_phase = CLIENT_IDLE;
            reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1);
            update_reservation_locked();
        }
        firmware_update_unlock();
        return reset;
    }
    if (client_phase == CLIENT_ADMISSION) {
        if (expired(now_us, client_started_us, MAINTENANCE_ADMISSION_TIMEOUT_US)) {
            firmware_update_unlock();
            finish_client(MAINTENANCE_TIMEOUT_NOT_SENT);
            return false;
        }
        if (queue_packet_try(client_payload, MAINTENANCE_BOOTLOADER_REQUEST_MSG, 8)) {
            client_phase = CLIENT_REPLY;
            client_sent_us = now_us;
        }
    }
    bool timed_out = client_phase == CLIENT_REPLY
        && expired(now_us, client_sent_us, MAINTENANCE_REPLY_TIMEOUT_US);
    firmware_update_unlock();
    if (timed_out)
        finish_client(MAINTENANCE_TIMEOUT_UNCONFIRMED);
    return false;
}
