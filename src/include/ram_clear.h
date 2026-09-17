/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Volatile stores are retained even when a temporary's lifetime ends here. */
static inline void ram_clear(void *memory, size_t length) {
    volatile uint8_t *p = memory;
    while (length--) *p++ = 0;
}
