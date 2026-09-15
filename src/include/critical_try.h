/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pico/critical_section.h"

/* This SDK predates a critical-section try-enter API. Reading an RP2040
 * hardware spin lock claims it atomically, or returns zero if already owned.
 * Match the SDK's acquire fence and IRQ handling without its retry loop.
 * Native tests replace this hardware boundary through their include path. */
static inline bool dh_critical_section_try_enter(critical_section_t *section) {
    uint32_t saved_irq = save_and_disable_interrupts();
    if (!*section->spin_lock) {
        restore_interrupts(saved_irq);
        return false;
    }
    __mem_fence_acquire();
    section->save = saved_irq;
    return true;
}
