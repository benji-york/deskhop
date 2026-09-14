/* Deterministic adversarial tests of production HID units. Reuse only the
 * boundary doubles from the regression executable, never its decode logic. */
#define main hid_regression_main
#include "test_hid_regressions.c"
#undef main

#include <errno.h>
#include <inttypes.h>

static uint32_t rng_state;
static uint32_t next_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static int32_t scalar_oracle(const uint8_t *bytes, int length, unsigned offset, unsigned size) {
    /* Intentionally bit-at-a-time, with arithmetic signed conversion; the
     * production reader gathers bytes and sign-extends with an unsigned mask. */
    if (length <= 0 || size == 0 || size > 32 || offset + size > (unsigned)length * 8)
        return 0;
    uint64_t result = 0;
    for (unsigned bit = 0; bit < size; bit++)
        if (bytes[(offset + bit) / 8] & (1u << ((offset + bit) % 8)))
            result += UINT64_C(1) << bit;
    int64_t signed_result = (int64_t)result;
    if (result & (UINT64_C(1) << (size - 1)))
        signed_result -= INT64_C(1) << size;
    return (int32_t)signed_result;
}

static void scalar_properties(void) {
    for (int length = 0; length <= 8; length++) {
        uint8_t *bytes = length ? malloc((size_t)length) : NULL;
        assert(bytes || !length);
        for (int i = 0; i < length; i++) bytes[i] = (uint8_t)(0xa5u + 19u * i);
        for (unsigned offset = 0; offset <= 72; offset++) {
            for (unsigned size = 0; size <= 40; size++) {
                report_val_t field = {.offset = offset, .size = size};
                assert(get_report_value(bytes, length, &field)
                       == scalar_oracle(bytes, length, offset, size));
            }
        }
        free(bytes);
    }
}

static void parser_invariants(hid_interface_t *iface) {
    assert(parser_state.p_usage >= parser_state.usages);
    assert(parser_state.p_usage < parser_state.usages + HID_MAX_USAGES);
    assert(parser_state.num_report_offsets <= MAX_REPORTS_PER_IFACE);
    for (unsigned i = 0; i < parser_state.num_report_offsets; i++)
        assert(parser_state.report_offsets[i].offset_in_bits <= UINT16_MAX);
    for (unsigned i = 0; i < REPORT_ID_MAP_SIZE; i++)
        assert(iface->report_handler[i] <= REPORT_RECEIVER_SYSTEM);
    assert(iface->num_keyboards <= MAX_KEYBOARDS);
    for (unsigned i = 0; i < MAX_KEYBOARDS; i++)
        assert(iface->keyboards[i].nkro_count <= MAX_NKRO_BLOCKS);
}

static void exercise_descriptor_protocol(const uint8_t *descriptor, size_t length, uint8_t proto) {
    reset();
    host_protocol = proto;
    hid_interface_t *iface = &global_state.iface[0][0];
    iface->protocol = HID_PROTOCOL_REPORT;
    parse_report_descriptor(iface, descriptor, (int)length);
    parser_invariants(iface);
    /* Every dispatch ID and an exact-sized short payload reach the production
     * dispatcher. Unknown IDs must produce no real activity/output. */
    for (unsigned id = 0; id < REPORT_ID_MAP_SIZE; id++) {
        unsigned payload_length = next_random() % 17;
        uint8_t *payload = payload_length ? malloc(payload_length) : NULL;
        assert(payload || !payload_length);
        for (unsigned j = 0; j < payload_length; j++) payload[j] = (uint8_t)next_random();
        if (payload_length && iface->uses_report_id) payload[0] = (uint8_t)id;
        unsigned before = activity_count;
        tuh_hid_report_received_cb(1, 0, payload, (uint16_t)payload_length);
        if (!payload_length || (iface->uses_report_id &&
            (!iface->report_handler[id] || payload_length == 1)))
            assert(activity_count == before);
        free(payload);
    }
}

static void exercise_descriptor(const uint8_t *descriptor, size_t length) {
    exercise_descriptor_protocol(descriptor, length, HID_ITF_PROTOCOL_NONE);
    exercise_descriptor_protocol(descriptor, length, HID_ITF_PROTOCOL_KEYBOARD);
}

static size_t generate_descriptor(uint32_t seed, uint32_t iteration, uint8_t *bytes) {
    rng_state = seed ^ (iteration * UINT32_C(0x9e3779b9));
    if (!rng_state) rng_state = 1;
    /* Mix arbitrary bytes with mutations of representative keyboard/mouse
     * descriptors. Exact truncation is a distinct operator, not zero padding. */
    static const uint8_t keyboard[] = {
        0x05,1, 0x09,6, 0xa1,1, 0x85,255,
        0x05,7, 0x19,0xe0, 0x29,0xe7, 0x75,1, 0x95,8, 0x81,2,
        0x75,8, 0x95,1, 0x81,1, 0x19,0, 0x29,0xff, 0x95,6, 0x81,0, 0xc0
    };
    static const uint8_t mouse[] = {
        0x05,1, 0x09,2, 0xa1,1, 0x85,24, 0x05,9, 0x19,1, 0x29,5,
        0x75,1, 0x95,5, 0x81,2, 0x95,3, 0x81,1,
        0x05,1, 0x09,0x30, 0x09,0x31, 0x75,8, 0x95,2, 0x81,6, 0xc0
    };
    unsigned mode = iteration % 4;
    size_t length;
    if (mode < 2) {
        length = mode ? sizeof(mouse) : sizeof(keyboard);
        memcpy(bytes, mode ? mouse : keyboard, length);
        unsigned changes = 1 + next_random() % 4;
        while (changes--) bytes[next_random() % length] = (uint8_t)next_random();
        if (iteration % 3 == 0) length = next_random() % (length + 1);
    } else {
        length = next_random() % 257;
        for (size_t j = 0; j < length; j++) bytes[j] = (uint8_t)next_random();
    }
    return length;
}

static void save_hex(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "w");
    assert(file);
    for (size_t i = 0; i < length; i++) fprintf(file, "%02x%s", bytes[i], i + 1 == length ? "\n" : " ");
    assert(fclose(file) == 0);
}

static void replay_hex(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) { perror(path); exit(2); }
    uint8_t bytes[4096];
    unsigned value;
    size_t length = 0;
    while (fscanf(file, "%x", &value) == 1) {
        assert(value <= 255 && length < sizeof(bytes));
        bytes[length++] = (uint8_t)value;
    }
    fclose(file);
    uint8_t *exact = length ? malloc(length) : NULL;
    assert(exact || !length);
    if (length) memcpy(exact, bytes, length);
    rng_state = 1;
    exercise_descriptor(exact, length);
    free(exact);
}

int main(int argc, char **argv) {
    uint32_t seed = 0x484944u, iterations = 2000, first = 0;
    const char *artifact = NULL;
    bool one_case = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scalar")) { scalar_properties(); return 0; }
        if (!strcmp(argv[i], "--replay") && i + 1 < argc) { replay_hex(argv[++i]); return 0; }
        if (!strcmp(argv[i], "--artifact") && i + 1 < argc) artifact = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) { first = (uint32_t)strtoul(argv[++i], NULL, 0); one_case = true; }
        else { fprintf(stderr, "usage: %s [--seed N] [--iterations N] [--case N] [--artifact FILE] [--replay FILE] [--scalar]\n", argv[0]); return 2; }
    }
    fprintf(stderr, "HID properties seed=%" PRIu32 " first=%" PRIu32 " cases=%" PRIu32 "\n", seed, first, one_case ? 1 : iterations);
    if (!one_case) { hid_regression_main(); scalar_properties(); }
    for (uint32_t i = 0; i < (one_case ? 1 : iterations); i++) {
        uint8_t bytes[256];
        size_t length = generate_descriptor(seed, first + i, bytes);
        if (artifact) save_hex(artifact, bytes, length);
        uint8_t *exact = length ? malloc(length) : NULL;
        assert(exact || !length);
        if (length) memcpy(exact, bytes, length);
        exercise_descriptor(exact, length);
        free(exact);
    }
    puts("HID adversarial properties passed (exact buffers, scalar oracle, parser and all-ID dispatch)");
    return 0;
}
