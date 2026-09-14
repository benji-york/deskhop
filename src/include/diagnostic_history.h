/* Local RAM history. Safe from either application core after startup init. */
#pragma once
#include "history.h"

/* Initialize before USB callbacks or core 1 start; reinitialization is startup-only. */
void diagnostic_history_init(void);
void diagnostic_history_record(history_type_t type, uint8_t a, uint8_t b, uint32_t value);
history_window_t diagnostic_history_window(unsigned limit);
bool diagnostic_history_read(uint64_t seq, history_event_t *event);
