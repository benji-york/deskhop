/* Exact allocation and independent state/output oracles for actual mouse code.
 * This executable runs with ASan/UBSan (separate from the Python-loaded nodes). */
#include "main.h"
#include "config_packet.h"
#include <assert.h>
void sim_init(uint8_t,void (*)(int,int,int,const void *,int));
void sim_destroy(void);
void sim_host(int,int);
void sim_set_time(uint64_t);
void sim_task(int);
void sim_mount(uint8_t,uint8_t,uint8_t,const uint8_t *,uint16_t);
void sim_unmount(uint8_t,uint8_t);
float calculate_mouse_acceleration_factor(int32_t,int32_t);
int32_t move_and_keep_on_screen(int,int);
static unsigned outputs;
static void observe(int kind,int a,int b,const void *data,int len) { if(kind==1) outputs++; }
static const uint8_t descriptor[]={
  0x05,1,0x09,2,0xa1,1,0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
  0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,
  0x05,0x0c,0x0a,0x38,2,0x95,1,0x81,6,0xc0
};
static void report_case(bool boot_mode,unsigned id) {
    hid_interface_t iface={.protocol=boot_mode?HID_PROTOCOL_BOOT:HID_PROTOCOL_REPORT};
    uint8_t desc[sizeof(descriptor)+2];
    memcpy(desc,descriptor,sizeof(descriptor));
    unsigned desc_len=sizeof(descriptor);
    if(id) { memmove(desc+8,desc+6,sizeof(descriptor)-6);desc[6]=0x85;desc[7]=id;desc_len+=2; }
    parse_report_descriptor(&iface,desc,desc_len);
    unsigned needed=boot_mode?3:5+(id!=0);
    for(unsigned n=0;n<needed;n++) {
        uint8_t *raw=n?calloc(n,1):NULL;
        if(n&&id)raw[0]=id;
        global_state.mouse_buttons=2;
        uint64_t before=global_state.direct_activity[0];
        unsigned queue_before=queue_get_level(&global_state.mouse_queue);
        process_mouse_report(raw,n,0,&iface);
        assert(global_state.mouse_buttons==2);
        assert(global_state.direct_activity[0]==before);
        assert(queue_get_level(&global_state.mouse_queue)==queue_before);
        free(raw);
    }
    uint8_t *raw=calloc(needed,1);
    unsigned start=id?1:0;
    if(id)raw[0]=id;
    raw[start]=2;raw[start+1]=1;
    global_state.mouse_buttons=0;
    int16_t x=global_state.pointer_x;
    process_mouse_report(raw,needed,0,&iface);
    assert(global_state.pointer_x>x);
    assert(global_state.mouse_buttons==2);
    free(raw);
}
static void wide_motion(void) {
    const unsigned widths[]={16,24,32};
    for(unsigned j=0;j<3;j++) {
        unsigned bits=widths[j], bytes=bits/8;
        hid_interface_t iface={.protocol=HID_PROTOCOL_REPORT};
        uint8_t desc[]={0x05,1,0x09,2,0xa1,1,0x05,9,0x19,1,0x29,8,
            0x75,1,0x95,8,0x81,2,0x05,1,0x09,0x30,0x09,0x31,
            0x75,bits,0x95,2,0x81,6,0xc0};
        parse_report_descriptor(&iface,desc,sizeof(desc));
        uint8_t raw[9]={0};
        /* Largest positive and negative source axes. Config can also contain
           extreme signed speed values; no conversion/addition may overflow. */
        for(unsigned k=0;k<bytes;k++) raw[1+k]=0xff;
        raw[bytes]=0x7f;raw[2*bytes]=0x80;
        global_state.pointer_x=global_state.pointer_y=16000;
        global_state.gaming_mode=true;
        global_state.config.enable_acceleration=1;
        global_state.config.output[0].speed_x=INT32_MAX;
        global_state.config.output[0].speed_y=INT32_MIN;
        process_mouse_report(raw,1+2*bytes,0,&iface);
        assert(global_state.pointer_x==MAX_SCREEN_COORD);
        assert(global_state.pointer_y==MAX_SCREEN_COORD);
        mouse_report_t report;
        while(queue_try_remove(&global_state.mouse_queue,&report)) { }
        assert(report.mode==RELATIVE && report.x==INT16_MAX && report.y==INT16_MIN);
        /* Direct helper extremes must also remain defined. */
        assert(calculate_mouse_acceleration_factor(INT32_MIN,INT32_MAX)==4.0f);
        assert(move_and_keep_on_screen(32767,INT32_MAX)==32767);
        assert(move_and_keep_on_screen(0,INT32_MIN)==0);
    }
}
static void vendor_diagnostics_cannot_enter_core1(void) {
    const uint8_t kinds[] = {DIAGNOSTIC_STATUS_REQUEST_MSG, DIAGNOSTIC_STATUS_RESPONSE_MSG,
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
        DIAGNOSTIC_HISTORY_REQUEST_MSG, DIAGNOSTIC_HISTORY_RESPONSE_MSG,
#endif
    };
    for (unsigned proxy = 0; proxy < 2; ++proxy) {
        for (unsigned i = 0; i < sizeof(kinds); ++i) {
            uart_packet_t packet = {.type = proxy ? PROXY_PACKET_MSG : kinds[i]};
            packet.data[0] = proxy ? kinds[i] : 1;
            packet.data[1] = 0x12;
            packet.data[4] = 1;
            packet.checksum = calc_packet_checksum(&packet);
            assert(verify_checksum(&packet));
            assert(!validate_packet(&packet));
        }
        /* An otherwise valid configuration read still crosses this boundary. */
        uart_packet_t allowed = {.type = proxy ? PROXY_PACKET_MSG : GET_VAL_MSG};
        allowed.data[0] = proxy ? GET_VAL_MSG : 83;
        allowed.checksum = calc_packet_checksum(&allowed);
        assert(verify_checksum(&allowed));
        assert(validate_packet(&allowed));
    }
}

/* Polynomial long division is independent of the production CRC8 routine. */
static uint8_t config_crc_oracle(const uint8_t *data, unsigned length) {
    unsigned remainder = 0;
    for (unsigned i = 0; i < length * 8 + 8; ++i) {
        unsigned bit = i < length * 8 ? (data[i / 8] >> (7 - i % 8)) & 1u : 0;
        remainder = (remainder << 1) | bit;
        if (remainder & 0x100u)
            remainder ^= 0x107u;
    }
    return (uint8_t)remainder;
}

static void vendor_transport_integrity(void) {
    assert(config_crc_oracle((const uint8_t *)"123456789", 9) == 0xf4);
    uint8_t valid[CONFIG_PACKET_LENGTH] = {0xaa, 0x55, SET_VAL_MSG, 83, 123};
    valid[11] = config_crc_oracle(valid, 11);
    const uint32_t before = global_state.config.screensaver_system_timeout_sec;
    const bool was_enabled = global_state.config_mode_active;
    global_state.config_mode_active = true;
    for (unsigned bit = 0; bit < sizeof(valid) * 8; ++bit) {
        uint8_t corrupt[sizeof(valid)];
        memcpy(corrupt, valid, sizeof(valid));
        corrupt[bit / 8] ^= 1u << (bit % 8);
        tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                              HID_REPORT_TYPE_OUTPUT, corrupt, sizeof(corrupt));
        assert(global_state.config.screensaver_system_timeout_sec == before);
        assert(!global_state.reboot_requested);
    }
    for (unsigned length = 0; length < sizeof(valid); ++length)
        tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                              HID_REPORT_TYPE_OUTPUT, valid, length);
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_FEATURE, valid, sizeof(valid));
    assert(global_state.config.screensaver_system_timeout_sec == before);

    /* Even CRC-correct malformed preambles and nested proxies are refused. */
    uint8_t malformed[sizeof(valid)];
    memcpy(malformed, valid, sizeof(valid));
    malformed[0] = 0;
    malformed[11] = config_crc_oracle(malformed, 11);
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_OUTPUT, malformed, sizeof(malformed));
    assert(global_state.config.screensaver_system_timeout_sec == before);
    uart_packet_t nested = {.type = PROXY_PACKET_MSG, .data = {PROXY_PACKET_MSG, WIPE_CONFIG_MSG}};
    assert(!validate_packet(&nested));

    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_OUTPUT, valid, sizeof(valid));
    assert(global_state.config.screensaver_system_timeout_sec == 123);

    /* Real response queue preserves descriptor length and independently
       verified CRC, even when UART frames have a different size. */
    hid_generic_pkt_t queued;
    while (queue_try_remove(&global_state.hid_queue_out, &queued)) { }
    valid[2] = GET_VAL_MSG;
    memset(valid + 4, 0, 7);
    valid[11] = config_crc_oracle(valid, 11);
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_OUTPUT, valid, sizeof(valid));
    assert(queue_try_remove(&global_state.hid_queue_out, &queued));
    assert(queued.instance == ITF_NUM_HID_VENDOR && queued.report_id == REPORT_ID_VENDOR);
    assert(queued.len == CONFIG_PACKET_LENGTH && queued.data[2] == GET_VAL_MSG);
    assert(queued.data[3] == 83 && queued.data[4] == 123);
    assert(queued.data[11] == config_crc_oracle(queued.data, 11));
    global_state.config.screensaver_system_timeout_sec = before;
    global_state.config_mode_active = was_enabled;
    puts("WebHID transport: independent CRC8, all 96 single-bit errors, lengths/types/preamble, nested proxy and real response queue passed");
}

#if SIM_HAS_DIAGNOSTIC_HISTORY
/* Unlike the isolated HID/host executables, this uses the actual history
 * store, SDK-lock bridge, and production event hooks with CDC disabled. */
static uint64_t history_next(void) {
    return diagnostic_history_window(HISTORY_CAPACITY).end_seq;
}

static void expect_history(uint64_t seq, history_type_t type,
                           uint8_t a, uint8_t b, uint32_t value) {
    history_event_t event;
    assert(diagnostic_history_read(seq, &event));
    assert(event.seq == seq && event.time_us == time_us_64());
    assert(event.type == type && event.a == a && event.b == b);
    assert(event.value == value && event.reserved == 0);
}

static void history_hooks(void) {
    sim_destroy();
    sim_init(OUTPUT_A, observe);
    history_window_t initial = diagnostic_history_window(HISTORY_CAPACITY);
    assert(initial.count == 1 && initial.first_seq == 1 && initial.end_seq == 2);
    expect_history(1, HISTORY_BOOT, OUTPUT_A, 0, 98);
    sim_set_time(10);

    /* The hotkey mutates active_output before set_active_output sees it. */
    uint64_t seq = history_next();
    output_toggle_hotkey_handler(&global_state, NULL);
    assert(global_state.active_output == OUTPUT_B && history_next() == seq + 1);
    expect_history(seq, HISTORY_OUTPUT_LOCAL, OUTPUT_A, OUTPUT_B, 0);
    seq = history_next();
    set_active_output(&global_state, OUTPUT_B);
    assert(history_next() == seq); /* No fabricated change for same output. */

    selection_state_t incoming = global_state.selection;
    selection_request(&incoming, OUTPUT_A, OUTPUT_B);
    uart_packet_t selected = {.type = OUTPUT_SELECT_SYNC_MSG};
    selection_encode(&incoming, selected.data);
    selected.checksum = calc_packet_checksum(&selected);
    process_packet(&selected, &global_state);
    assert(global_state.active_output == OUTPUT_A && history_next() == seq + 1);
    expect_history(seq, HISTORY_OUTPUT_PEER, OUTPUT_B, OUTPUT_A, 0);
    seq = history_next();
    process_packet(&selected, &global_state);
    assert(history_next() == seq); /* Repeated peer reconciliation is quiet. */

    sim_host(0, 0);
    expect_history(seq++, HISTORY_USB_UNMOUNT, 0, 0, 0);
    sim_host(1, 0);
    expect_history(seq++, HISTORY_USB_MOUNT, 0, 0, 0);
    assert(history_next() == seq);

    global_state.config.enforce_ports = false;
    sim_mount(1, 0, HID_ITF_PROTOCOL_MOUSE, descriptor, sizeof(descriptor));
    expect_history(seq++, HISTORY_HID_MOUNT, 1, 0, HID_ITF_PROTOCOL_MOUSE | 512u);
    sim_unmount(1, 0);
    expect_history(seq++, HISTORY_HID_UNMOUNT, 1, 0, HID_ITF_PROTOCOL_MOUSE | 512u);
    assert(history_next() == seq);

    /* Enumeration is observable even if enforced-port policy declines input
       from this interface. It must not claim that reports were accepted. */
    global_state.config.enforce_ports = true;
    sim_mount(3, 1, HID_ITF_PROTOCOL_MOUSE, descriptor, sizeof(descriptor));
    expect_history(seq++, HISTORY_HID_MOUNT, 3, 1, HID_ITF_PROTOCOL_MOUSE | 512u);
    assert(!global_state.mouse_connected && history_next() == seq);
    sim_unmount(3, 1);
    expect_history(seq++, HISTORY_HID_UNMOUNT, 3, 1, HID_ITF_PROTOCOL_MOUSE | 512u);
    global_state.config.enforce_ports = false;

    /* A rejected report descriptor is recorded once at enumeration, rather
       than once per subsequently ignored report. */
    const uint8_t truncated[] = {0x75};
    sim_mount(2, 3, HID_ITF_PROTOCOL_NONE, truncated, sizeof(truncated));
    expect_history(seq++, HISTORY_HID_MOUNT, 2, 3, HID_ITF_PROTOCOL_NONE);
    expect_history(seq++, HISTORY_DESCRIPTOR_REJECTED, 2, 3, 0);
    assert(global_state.iface[1][3].descriptor_invalid);
    const uint8_t report[] = {0, 1, 0, 0, 0};
    tuh_hid_report_received_cb(2, 3, report, sizeof(report));
    assert(history_next() == seq);
    tuh_hid_mount_cb(0, 0, descriptor, sizeof(descriptor));
    tuh_hid_umount_cb(MAX_DEVICES + 1, 0);
    assert(history_next() == seq); /* Invalid addresses never become events. */

    uart_packet_t bad = {.type = MOUSE_REPORT_MSG, .checksum = 1};
    process_packet(&bad, &global_state);
    expect_history(seq++, HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, MOUSE_REPORT_MSG);

    /* The shared packet dispatcher also validates USB configuration packets.
       A checksum error must not falsely attribute this traffic to UART. */
    global_state.config_mode_active = true;
    const config_t saved_config = global_state.config;
    const unsigned hid_before = queue_get_level(&global_state.hid_queue_out);
    uart_packet_t config_read = {.type = GET_VAL_MSG, .data = {83}};
    uint8_t raw[CONFIG_PACKET_LENGTH];
    write_config_packet(raw, &config_read);
    raw[CONFIG_PACKET_LENGTH - 1] ^= 1;
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_OUTPUT, raw, sizeof(raw));
    expect_history(seq++, HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, GET_VAL_MSG);
    assert(history_next() == seq);
    assert(queue_get_level(&global_state.hid_queue_out) == hid_before);
    assert(memcmp(&global_state.config, &saved_config, sizeof(saved_config)) == 0);
    raw[CONFIG_PACKET_LENGTH - 1] ^= 1;
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                          HID_REPORT_TYPE_OUTPUT, raw, sizeof(raw));
    assert(queue_get_level(&global_state.hid_queue_out) == hid_before + 1);
    assert(history_next() == seq); /* Valid read reaches the same USB path. */
    global_state.config_mode_active = false;

    uart_packet_t filler = {.type = FLASH_LED_MSG};
    while (queue_try_add(&global_state.uart_tx_queue, &filler)) { }
    const uint8_t enabled = ENABLE;
    assert(!queue_packet_try(&enabled, FLASH_LED_MSG, sizeof(enabled)));
    assert(history_next() == seq); /* A retryable refusal is not a drop. */
    queue_packet(&enabled, FLASH_LED_MSG, sizeof(enabled));
    expect_history(seq++, HISTORY_UART_DROPPED, 0, 0, FLASH_LED_MSG);
    assert(history_next() == seq);

    /* Snapshot bounds stay fixed when subsequent real error hooks overwrite
       old slots; the console can identify lost rows without stale copies. */
    const history_window_t before = diagnostic_history_window(HISTORY_CAPACITY);
    for (unsigned i = 0; i < HISTORY_CAPACITY; ++i)
        process_packet(&bad, &global_state);
    const history_window_t after = diagnostic_history_window(HISTORY_CAPACITY);
    assert(before.end_seq == seq && after.end_seq == seq + HISTORY_CAPACITY);
    assert(after.count == HISTORY_CAPACITY && after.oldest_seq == seq);
    assert(after.overwritten == before.end_seq - 1);
    history_event_t retained = {.seq = UINT64_C(0xfeedface)};
    assert(!diagnostic_history_read(before.first_seq, &retained));
    assert(retained.seq == UINT64_C(0xfeedface));
    expect_history(after.first_seq, HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, MOUSE_REPORT_MSG);

    diagnostic_history_init();
    assert(diagnostic_history_window(HISTORY_CAPACITY).count == 0);
    diagnostic_history_record(HISTORY_BOOT, OUTPUT_A, 0, 98);
    expect_history(1, HISTORY_BOOT, OUTPUT_A, 0, 98);
    puts("history hooks: real store/bridge, output changes, USB/HID callbacks, ignored-port enumeration, descriptor rejection, USB/UART checksum errors, retry/drop distinction, overwrite and reset passed");
}
#endif
int main(void) {
    sim_init(0,observe);sim_host(1,0);sim_set_time(1);
    global_state.config.enable_acceleration=0;
    global_state.config.output[0].speed_x=1;
    report_case(true,0);
    for(unsigned id=0;id<=255;id++)report_case(false,id);
    /* A corrupted/malformed output value must never become an array index. */
    for(unsigned output=2;output<=255;output++) {
        uart_packet_t p={.type=OUTPUT_SELECT_MSG,.data={output}};
        p.checksum=calc_packet_checksum(&p);
        process_packet(&p,&global_state);
        assert(global_state.active_output==0);
    }
    sim_task(3);assert(outputs==1);
    wide_motion();
    /* The malformed-motion fixture intentionally poisons speed fields. The
       following transport cases require an independently valid config. */
    global_state.config = default_config;
    vendor_diagnostics_cannot_enter_core1();
    vendor_transport_integrity();
#if SIM_HAS_DIAGNOSTIC_HISTORY
    history_hooks();
#endif
    sim_destroy();
    puts("native boundaries: all 256 mouse IDs, exact truncations, 3-byte boot mouse, invalid output indices, USB diagnostic core ownership passed");
}
