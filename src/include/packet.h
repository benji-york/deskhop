/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#pragma once

#include <stdint.h>
#include "protocol.h"


/*==============================================================================
 *  Constants
 *==============================================================================*/

/* WebHID configuration preamble. Never accepted by the UART receiver. */
#define START1        0xAA
#define START2        0x55
#define START_LENGTH  2

/* Packet Queue Definitions  */
#define UART_QUEUE_LENGTH  256
#define HID_QUEUE_LENGTH   128
#define KBD_QUEUE_LENGTH   128
#define MOUSE_QUEUE_LENGTH 512

/* Packet Lengths and Offsets */
#define CONFIG_PACKET_LENGTH   12
#define UART_FRAME_VERSION     1
#define UART_FRAME_START       0x7e
#define UART_FRAME_END         0x7f
#define UART_FRAME_BODY_LENGTH 15
#define RAW_PACKET_LENGTH      (2 + 2 * UART_FRAME_BODY_LENGTH)

#define TYPE_LENGTH             1
#define PACKET_DATA_LENGTH      8 // For simplicity, all packet types are the same length

#define KEYARRAY_BIT_OFFSET     16
#define KEYS_IN_USB_REPORT      6
#define KBD_REPORT_LENGTH       8
#define MOUSE_REPORT_LENGTH     8
#define CONSUMER_CONTROL_LENGTH 4
#define SYSTEM_CONTROL_LENGTH   1
#define MODIFIER_BIT_LENGTH     8

/*==============================================================================
 *  Data Structures
 *==============================================================================*/

 typedef struct {
    uint8_t type;     // Enum field describing the type of packet
    union {
        uint8_t data[8];      // Data goes here (type + payload + checksum)
        uint16_t data16[4];   // We can treat it as 4 16-byte chunks
        uint32_t data32[2];   // We can treat it as 2 32-byte chunks
    };
    /* CRC32 of UART version, payload length, command type and all payload bytes.
       This is an internal packet, never a cast of incoming UART or USB bytes. */
    uint32_t checksum;
} __attribute__((packed)) uart_packet_t;
