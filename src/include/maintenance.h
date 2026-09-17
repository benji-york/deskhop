/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAINTENANCE_PROTOCOL_VERSION 1u
#define MAINTENANCE_ADMISSION_TIMEOUT_US 250000ull
#define MAINTENANCE_REPLY_TIMEOUT_US 3000000ull
#define MAINTENANCE_DRAIN_TIMEOUT_US 1000000ull
#define MAINTENANCE_SOURCE_LEASE_US 3000000ull

typedef enum {
    MAINTENANCE_STARTED,
    MAINTENANCE_BUSY,
    MAINTENANCE_UPDATE_ACTIVE,
    MAINTENANCE_REBOOT_PENDING,
    MAINTENANCE_BAD_ARGUMENT,
    MAINTENANCE_ALREADY_CONFIG,
} maintenance_start_t;

typedef enum {
    MAINTENANCE_LOCAL_READY,
    MAINTENANCE_REMOTE_ACCEPTED,
    MAINTENANCE_REMOTE_BUSY,
    MAINTENANCE_REMOTE_UPDATE_ACTIVE,
    MAINTENANCE_REMOTE_REBOOT_PENDING,
    MAINTENANCE_TIMEOUT_NOT_SENT,
    MAINTENANCE_TIMEOUT_UNCONFIRMED,
    MAINTENANCE_ABORTED,
} maintenance_outcome_t;

typedef struct {
    uint32_t token;
    uint8_t target;
    maintenance_outcome_t outcome;
} maintenance_result_t;

/* Core 0 only, except receive(). Targets are physical board roles, A=0/B=1.
 * Initialization occurs before core 1 starts. No callback enters ROM directly. */
void maintenance_init(uint8_t role, uint64_t boot_session);
void maintenance_shutdown(void);
maintenance_start_t maintenance_request(uint8_t target, uint32_t token, uint64_t now_us);
/* Enter configuration mode on this physical board only. Already-active config
 * mode is an idempotent no-op, never the keyboard shortcut's exit toggle.
 * A started request uses the same LOCAL_READY/console-completion fence. */
maintenance_start_t maintenance_request_config(uint32_t token, uint64_t now_us);
bool maintenance_poll(maintenance_result_t *result);
void maintenance_console_reply_complete(uint32_t token, uint64_t now_us);
/* Cancellation cannot retract a remote request already admitted to UART.
 * The console must not report that the remote board remained unchanged. */
void maintenance_cancel(uint32_t token, uint64_t now_us);
bool maintenance_task(uint64_t now_us);
/* Called at UART dequeue, before DMA: invalidate expired/cancelled maintenance
 * packets even if unrelated traffic kept them queued after the deadline. */
bool maintenance_packet_allowed(uint8_t type, const uint8_t payload[8], uint64_t now_us);
/* Core 1 only: bounded handoff; payload is copied, never dispatched here. */
void maintenance_receive(uint8_t type, const uint8_t payload[8], uint64_t now_us);
