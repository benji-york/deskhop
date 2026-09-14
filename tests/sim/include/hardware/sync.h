#pragma once
#include "pico.h"
typedef struct { unsigned held; } spin_lock_t;
typedef struct { unsigned held; } critical_section_t;
uint32_t spin_lock_blocking(spin_lock_t *);
void spin_unlock(spin_lock_t *, uint32_t);
uint next_striped_spin_lock_num(void);
void critical_section_init(critical_section_t *);
void critical_section_enter_blocking(critical_section_t *);
void critical_section_exit(critical_section_t *);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t);
