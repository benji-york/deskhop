/* Pure helpers for upgrading persisted DeskHop configuration values. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_VERSION_V8       8
#define PREVIOUS_CONFIG_VERSION 9
#define CURRENT_CONFIG_VERSION  10

bool migrate_config_to_current(uint32_t stored_version,
                               uint32_t *version,
                               uint32_t *system_timeout_sec,
                               uint8_t *output_a_mode,
                               uint8_t *output_b_mode,
                               uint32_t default_system_timeout_sec,
                               uint8_t jitter_mode);
