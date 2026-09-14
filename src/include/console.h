/* Read-only USB console. All entry points belong to core 0. */
#pragma once

#include <stdint.h>

void console_init(uint8_t board_role, const char *board_id,
                  uint64_t boot_session, uint32_t image_crc_at_boot);
void console_task(uint64_t now_us);
/* USB unmount / DTR drop discard the terminal session, not boot identity. */
void console_disconnect(void);
