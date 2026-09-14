#pragma once
#include "hardware/sync.h"
typedef struct { spin_lock_t *spin_lock; spin_lock_t storage; } lock_core_t;
static inline void lock_init(lock_core_t *l, uint n) { (void)n; l->storage.held=0; l->spin_lock=&l->storage; }
void lock_internal_spin_unlock_with_notify(lock_core_t *, uint32_t);
void lock_internal_spin_unlock_with_wait(lock_core_t *, uint32_t);
