/* Native tests for the v8/v9-to-v10 configuration migration policy. */

#include "config_migration.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                   \
                    __FILE__, __LINE__, #condition);                            \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

enum {
    DISABLED_MODE = 0,
    PONG_MODE = 1,
    JITTER_MODE = 2,
    DEFAULT_TIMEOUT_SEC = 300,
};

static void test_v8_initializes_timeout_and_jitter(void) {
    uint32_t version = CONFIG_VERSION_V8;
    uint32_t timeout = 0xa5a5a5a5;
    uint8_t mode_a = DISABLED_MODE;
    uint8_t mode_b = PONG_MODE;

    CHECK(migrate_config_to_current(version, &version, &timeout, &mode_a, &mode_b,
                                    DEFAULT_TIMEOUT_SEC, JITTER_MODE));
    CHECK(version == CURRENT_CONFIG_VERSION);
    CHECK(timeout == DEFAULT_TIMEOUT_SEC);
    CHECK(mode_a == JITTER_MODE);
    CHECK(mode_b == JITTER_MODE);
}

static void test_v9_preserves_timeout_and_enables_jitter(void) {
    uint32_t version = PREVIOUS_CONFIG_VERSION;
    uint32_t timeout = 1234;
    uint8_t mode_a = DISABLED_MODE;
    uint8_t mode_b = PONG_MODE;

    CHECK(migrate_config_to_current(version, &version, &timeout, &mode_a, &mode_b,
                                    DEFAULT_TIMEOUT_SEC, JITTER_MODE));
    CHECK(version == CURRENT_CONFIG_VERSION);
    CHECK(timeout == 1234);
    CHECK(mode_a == JITTER_MODE);
    CHECK(mode_b == JITTER_MODE);
}

static void test_current_config_is_never_rewritten(void) {
    uint32_t version = CURRENT_CONFIG_VERSION;
    uint32_t timeout = 42;
    uint8_t mode_a = DISABLED_MODE;
    uint8_t mode_b = DISABLED_MODE;

    CHECK(!migrate_config_to_current(version, &version, &timeout, &mode_a, &mode_b,
                                     DEFAULT_TIMEOUT_SEC, JITTER_MODE));
    CHECK(version == CURRENT_CONFIG_VERSION);
    CHECK(timeout == 42);
    CHECK(mode_a == DISABLED_MODE);
    CHECK(mode_b == DISABLED_MODE);
}

static void test_unknown_legacy_config_is_not_migrated(void) {
    uint32_t version = 7;
    uint32_t timeout = 99;
    uint8_t mode_a = PONG_MODE;
    uint8_t mode_b = DISABLED_MODE;

    CHECK(!migrate_config_to_current(version, &version, &timeout, &mode_a, &mode_b,
                                     DEFAULT_TIMEOUT_SEC, JITTER_MODE));
    CHECK(version == 7);
    CHECK(timeout == 99);
    CHECK(mode_a == PONG_MODE);
    CHECK(mode_b == DISABLED_MODE);
}

int main(void) {
    test_v8_initializes_timeout_and_jitter();
    test_v9_preserves_timeout_and_enables_jitter();
    test_current_config_is_never_rewritten();
    test_unknown_legacy_config_is_not_migrated();

    puts("config migration tests passed");
    return EXIT_SUCCESS;
}
