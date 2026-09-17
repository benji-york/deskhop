/* Fixture-only bounded text/pacing tests, with no host clipboard access. */
#include "clipboard_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static bool zero_text(const clipboard_text_t *s) {
    for (unsigned i = 0; i < CLIPBOARD_MAX_TEXT; ++i) if (s->text[i]) return false;
    return true;
}
static void complete(clipboard_text_t *s, const uint8_t *p, size_t n, uint64_t now) {
    CHECK(clipboard_text_begin(s, CLIPBOARD_OK, n, clipboard_crc32(p, n), now));
    for (size_t offset = 0; offset < n;) {
        size_t count = n - offset;
        if (count > 40) count = 40;
        CHECK(clipboard_text_append(s, offset, p + offset, count, now));
        offset += count;
    }
    CHECK(clipboard_text_commit(s, now));
}
static char decode(uint8_t modifier, uint8_t key) {
    static const char normal[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
    static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";
    CHECK(modifier == 0 || modifier == 2);
    CHECK(key >= 4 && key <= 56);
    return (modifier ? shifted : normal)[key - 4];
}
static void ascii(void) {
    for (unsigned c = 0; c <= 255; ++c) {
        uint8_t modifier, key;
        bool expected = (c >= 32 && c <= 126) || c == '\n' || c == '\t';
        CHECK(clipboard_ascii_key(c, &modifier, &key) == expected);
        if (expected) CHECK((uint8_t)decode(modifier, key) == c);
    }
    CHECK(clipboard_crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
}
static void bounds_and_errors(void) {
    clipboard_text_t s = {0};
    uint8_t buffer[1024]; memset(buffer, 'x', sizeof(buffer));
    CHECK(!clipboard_text_begin(&s, 0, 0, 0, 0));
    CHECK(!clipboard_text_begin(&s, 0, 1025, 0, 0));
    CHECK(zero_text(&s));
    for (unsigned status = 1; status <= 255; ++status)
        CHECK(!clipboard_text_begin(&s, status, 1, 0, 0));
    complete(&s, buffer, sizeof(buffer), 0);
    CHECK(s.length == 1024 && s.received == 1024 && s.phase == CLIPBOARD_READY);
    CHECK(!clipboard_text_append(&s, 1024, buffer, 1, 0));
    CHECK(s.phase == CLIPBOARD_IDLE && zero_text(&s));

    for (unsigned failure = 0; failure < 8; ++failure) {
        CHECK(clipboard_text_begin(&s, 0, 8, clipboard_crc32(buffer, 8), 0));
        CHECK(clipboard_text_append(&s, 0, buffer, 4, 0));
        bool result = true;
        switch (failure) {
        case 0: result = clipboard_text_append(&s, 0, buffer, 4, 0); break; /* duplicate */
        case 1: result = clipboard_text_append(&s, 5, buffer, 1, 0); break; /* gap */
        case 2: result = clipboard_text_append(&s, 4, buffer, 5, 0); break; /* overflow */
        case 3: result = clipboard_text_append(&s, 4, NULL, 1, 0); break;
        case 4: result = clipboard_text_append(&s, 4, buffer, 0, 0); break;
        case 5: result = clipboard_text_commit(&s, 0); break; /* incomplete */
        case 6: result = clipboard_text_begin(&s, 0, 1, 0, 0); break; /* second begin */
        case 7: result = clipboard_text_append(&s, 4, buffer, 4, CLIPBOARD_RESPONSE_TIMEOUT_US); break;
        }
        CHECK(!result && s.phase == CLIPBOARD_IDLE && zero_text(&s));
    }
    CHECK(clipboard_text_begin(&s, 0, 8, 0, 0));
    CHECK(clipboard_text_append(&s, 0, buffer, 8, 0));
    CHECK(!clipboard_text_commit(&s, 0));
    CHECK(zero_text(&s));
    for (unsigned byte = 0; byte < 256; ++byte) {
        uint8_t modifier, key, p[2] = {'A', byte};
        if (clipboard_ascii_key(byte, &modifier, &key)) continue;
        CHECK(clipboard_text_begin(&s, 0, sizeof(p), clipboard_crc32(p, sizeof(p)), 0));
        CHECK(clipboard_text_append(&s, 0, p, sizeof(p), 0));
        CHECK(!clipboard_text_commit(&s, 0));
        CHECK(s.phase == CLIPBOARD_IDLE && zero_text(&s));
    }
}
static void release_and_pacing(void) {
    clipboard_text_t s = {0};
    uint8_t p[1024], report[8], previous[8]; memset(p, 'A', sizeof(p));
    complete(&s, p, sizeof(p), 0);
    CHECK(!clipboard_text_report(&s, 100000, report)); /* consumed all-up is not raw release */
    clipboard_text_release(&s);
    uint64_t now = 100000, first = now;
    unsigned accepted = 0;
    while (s.phase != CLIPBOARD_IDLE) {
        CHECK(clipboard_text_report(&s, now, report));
        memcpy(previous, report, 8);
        for (unsigned blocked = 0; blocked < 8; ++blocked) {
            CHECK(clipboard_text_report(&s, now, report));
            CHECK(memcmp(report, previous, 8) == 0);
        }
        if (!(accepted & 1)) CHECK(report[0] == 2 && report[2] == 4);
        else for (unsigned i = 0; i < 8; ++i) CHECK(report[i] == 0);
        clipboard_text_accepted(&s, now);
        ++accepted;
        if (s.phase != CLIPBOARD_IDLE) CHECK(!clipboard_text_report(&s, now + 4999, report));
        now += CLIPBOARD_REPORT_INTERVAL_US;
    }
    CHECK(accepted == 2048 && now - first == UINT64_C(10240000));
    CHECK(zero_text(&s));
    printf("1024-byte fixture: %u accepted reports, %.3f simulated seconds, 100 chars/s\n",
           accepted, (double)(now - first) / 1000000.0);
}
static void cancellation(void) {
    clipboard_text_t s = {0};
    uint8_t report[8]; const uint8_t text[] = "Aa\n\t";
    for (unsigned boundary = 0; boundary < 8; ++boundary) {
        complete(&s, text, sizeof(text) - 1, 0);
        clipboard_text_release(&s);
        uint64_t now = 0;
        for (unsigned i = 0; i < boundary; ++i) {
            CHECK(clipboard_text_report(&s, now, report));
            clipboard_text_accepted(&s, now);
            now += CLIPBOARD_REPORT_INTERVAL_US;
        }
        bool was_down = s.down;
        clipboard_text_cancel(&s);
        CHECK(zero_text(&s));
        CHECK(clipboard_text_report(&s, now, report) == was_down);
        if (was_down) {
            for (unsigned i = 0; i < 8; ++i) CHECK(report[i] == 0);
            clipboard_text_cancel(&s); /* repeated cancel cannot lose pending release */
            CHECK(clipboard_text_report(&s, now, report));
            clipboard_text_accepted(&s, now);
        }
        CHECK(s.phase == CLIPBOARD_IDLE && !s.down && zero_text(&s));
        CHECK(!clipboard_text_report(&s, now + 50000000, report));
    }
    complete(&s, text, sizeof(text) - 1, 0);
    clipboard_text_tick(&s, CLIPBOARD_RESPONSE_TIMEOUT_US);
    CHECK(s.phase == CLIPBOARD_IDLE && zero_text(&s));
    complete(&s, text, sizeof(text) - 1, 0);
    clipboard_text_release(&s);
    CHECK(clipboard_text_report(&s, 0, report));
    clipboard_text_accepted(&s, 0);
    clipboard_text_tick(&s, CLIPBOARD_TYPING_TIMEOUT_US);
    CHECK(s.phase == CLIPBOARD_RELEASING && zero_text(&s));
    CHECK(clipboard_text_report(&s, UINT64_C(99000000), report));
    clipboard_text_accepted(&s, UINT64_C(99000000));
    CHECK(s.phase == CLIPBOARD_IDLE);
}
int main(void) {
    ascii(); bounds_and_errors(); release_and_pacing(); cancellation();
    printf("clipboard text state: all fixture tests passed (%zu bytes state)\n", sizeof(clipboard_text_t));
    return 0;
}
