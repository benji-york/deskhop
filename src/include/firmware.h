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

 #include "structs.h"

 /*==============================================================================
  *  Firmware Update Functions
  *  Functions for managing firmware updates, CRC calculation, and related tasks.
  *==============================================================================*/

 uint32_t calculate_firmware_crc32(void);
 void     enter_firmware_recovery(void);
 void     firmware_sync_init(void);
 void     firmware_update_lock(void);
 void     firmware_update_unlock(void);
 bool     firmware_image_is_valid(uint16_t, uint32_t, bool);
 void     read_flash_bytes(const uint8_t *, void *, size_t);
 bool     read_running_firmware_word(uint32_t, uint32_t *);
 void     reboot(void);
 void     write_flash_page(uint32_t, uint8_t *);
 void     write_flash_page_erasing(uint32_t, uint8_t *, bool);

 /*==============================================================================
  *  UART Packet Fetching
  *  Functions to handle incoming UART packets, especially for firmware updates.
  *==============================================================================*/
 void     fetch_packet(device_t *);
 uint32_t get_ptr_delta(uint32_t, device_t *);
 bool     is_start_of_packet(device_t *);
 bool     request_byte(device_t *, uint32_t);

 /*==============================================================================
  *  Button Interaction
  *  Functions interacting with the button, e.g. checking if pressed.
  *==============================================================================*/

 bool is_bootsel_pressed(void);
