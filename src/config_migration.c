/* Host-independent migration policy for persisted DeskHop configuration. */

#include "config_migration.h"

bool migrate_config_to_current(uint32_t stored_version,
                               uint32_t *version,
                               uint32_t *system_timeout_sec,
                               uint8_t *output_a_mode,
                               uint8_t *output_b_mode,
                               uint32_t default_system_timeout_sec,
                               uint8_t jitter_mode) {
    if (stored_version != CONFIG_VERSION_V8
        && stored_version != PREVIOUS_CONFIG_VERSION)
        return false;

    *version = CURRENT_CONFIG_VERSION;

    if (stored_version == CONFIG_VERSION_V8)
        *system_timeout_sec = default_system_timeout_sec;

    *output_a_mode = jitter_mode;
    *output_b_mode = jitter_mode;
    return true;
}
