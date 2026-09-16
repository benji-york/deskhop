/* Native peripheral boundary for two independently loaded production images.
 * SDK queue.c and every application unit in build.py execute unmodified. */
#include "main.h"
#include <assert.h>
#include <setjmp.h>
#if SIM_HAS_MAINTENANCE
#include "maintenance.h"
#endif

device_t global_state;
uint8_t uart_rxbuf[DMA_RX_BUFFER_SIZE] __attribute__((aligned(DMA_RX_BUFFER_SIZE)));
uint8_t uart_txbuf[DMA_TX_BUFFER_SIZE] __attribute__((aligned(DMA_TX_BUFFER_SIZE)));
uint8_t sim_flash[2*1024*1024], sim_ppb[0x10000];
static sim_watchdog_hw_t wd;
sim_watchdog_hw_t *watchdog_hw = &wd;
static sim_ioqspi_hw_t io;
sim_ioqspi_hw_t *ioqspi_hw = &io;
static sim_sio_hw_t sio;
sim_sio_hw_t *sio_hw = &sio;
static sim_uart_hw_t uart_hw;
static uint64_t now_us, busy_until[3], dma_busy_until, last_kick;
static bool mounted, suspended, stalled[3], fail_report, led, stopped;
static bool uart_stalled, diagnostic_request_accepted, diagnostic_poll_ready;
static bool uart_busy_override;
static int maintenance_start_result;
static bool maintenance_poll_ready;
#if SIM_HAS_MAINTENANCE
static maintenance_result_t maintenance_result;
#endif
#if SIM_HAS_DIAGNOSTIC_PEER
static peer_status_result_t diagnostic_result;
#endif
static bool verify_request_accepted, verify_poll_ready;
#if SIM_HAS_DIAGNOSTIC_VERIFY
static verify_result_t verify_result;
static verify_assessment_t verify_assessment;
#endif
static bool history_request_accepted, history_poll_ready;
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
static const peer_history_result_t *history_borrowed;
#endif
static uint8_t protocol[MAX_DEVICES][MAX_INTERFACES], boot[MAX_DEVICES][MAX_INTERFACES];
static dma_channel_hw_t rx_hw;
static uint32_t rx_write;
static unsigned critical_depth, interrupt_depth;
/* kind: USB=1, UART=2, LED=3, watchdog=4, reset=5, yield=6,
 * flash erase=7, program=8, scheduling boundary=9, remote wake=10. */
typedef void (*event_cb_t)(int, int, int, const void *, int);
static event_cb_t event_cb;
static jmp_buf wait_escape[8];
static uint64_t wait_started[8];
static unsigned wait_depth;
/* Bound waits in virtual time even when the other core is paused. The calling
   C frame catches the escape after the Python scheduler callback has returned. */
#define GUARDED(call) do { \
    assert(wait_depth < 8); unsigned slot=wait_depth++; wait_started[slot]=now_us; \
    if (!setjmp(wait_escape[slot])) { call; } \
    wait_depth--; \
} while (0)
static void emit(int kind, int a, int b, const void *p, int n) {
    if (event_cb) event_cb(kind,a,b,p,n);
}
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
bool sim_queue_try_add(queue_t *queue, const void *data) {
    /* Parenthesized function name bypasses the header's function-like alias. */
    bool accepted = (queue_try_add)(queue, data);
    if (queue == &global_state.uart_tx_queue) {
        const uart_packet_t *packet = data;
        if (packet->type >= DIAGNOSTIC_STATUS_REQUEST_MSG
            && packet->type <=
#if SIM_HAS_DIAGNOSTIC_VERIFY
                DIAGNOSTIC_VERIFY_RESPONSE_MSG)
#else
                DIAGNOSTIC_HISTORY_RESPONSE_MSG)
#endif
            emit(17, packet->type, accepted, NULL, 0);
    }
    return accepted;
}
#endif
uint64_t time_us_64(void) { return now_us; }
uint32_t time_us_32(void) { return (uint32_t)now_us; }
void sim_set_time(uint64_t t) { assert(t >= now_us); now_us=t; }
void tight_loop_contents(void) {
    emit(6,0,0,NULL,0);
    if (wait_depth && now_us-wait_started[wait_depth-1] >= 2000000) {
        emit(11,0,0,NULL,0);
        longjmp(wait_escape[wait_depth-1],1);
    }
}
void sleep_us(uint64_t t) { uint64_t end=now_us+t; while(now_us<end) tight_loop_contents(); }
void sleep_ms(uint32_t t) { sleep_us((uint64_t)t*1000); }
uint next_striped_spin_lock_num(void) { return 0; }
uint32_t spin_lock_blocking(spin_lock_t *s) { assert(!s->held); s->held=1; return 0; }
void spin_unlock(spin_lock_t *s, uint32_t n) { (void)n; assert(s->held); s->held=0; }
void lock_internal_spin_unlock_with_notify(lock_core_t *l,uint32_t n) { spin_unlock(l->spin_lock,n); }
void lock_internal_spin_unlock_with_wait(lock_core_t *l,uint32_t n) { spin_unlock(l->spin_lock,n); tight_loop_contents(); }
void critical_section_init(critical_section_t *s) { s->held=0; }
void critical_section_enter_blocking(critical_section_t *s) { assert(!s->held); s->held=1; critical_depth++; }
bool dh_critical_section_try_enter(critical_section_t *s) {
    if (s->held) return false;
    s->held=1; critical_depth++; return true;
}
void critical_section_exit(critical_section_t *s) { assert(s->held); s->held=0; critical_depth--; }
uint32_t save_and_disable_interrupts(void) { return interrupt_depth++; }
void restore_interrupts(uint32_t n) { interrupt_depth=n; }
void flash_range_erase(uint32_t off,size_t n) {
    assert(critical_depth && interrupt_depth && !(off % FLASH_SECTOR_SIZE));
    assert(!(n % FLASH_SECTOR_SIZE) && off+n<=sizeof(sim_flash));
    memset(sim_flash+off,0xff,n); emit(7,off,(int)n,NULL,0);
}
void flash_range_program(uint32_t off,const uint8_t *data,size_t n) {
    assert(critical_depth && interrupt_depth && !(off % FLASH_PAGE_SIZE));
    assert(!(n % FLASH_PAGE_SIZE) && off+n<=sizeof(sim_flash));
    for(size_t i=0;i<n;i++) { assert((sim_flash[off+i] & data[i]) == data[i]); sim_flash[off+i]&=data[i]; }
    emit(8,off,(int)n,NULL,0);
}
void hw_write_masked(uint32_t *p,uint32_t a,uint32_t mask) { *p=(*p&~mask)|(a&mask); }
void watchdog_update(void) { last_kick=now_us; }
/* Reset reason 1 means ROM USB boot; retain the interface-disable mask. */
void reset_usb_boot(uint32_t a,uint32_t b) { stopped=true; emit(5,1,(int)b,NULL,0); }
void gpio_put(uint32_t pin,bool value) { led=value; }
bool gpio_get(uint32_t pin) { return led; }
void pico_get_unique_board_id_string(char *s,uint32_t n) { snprintf(s,n,"SIMULATED-%u",global_state.board_role); }
bool dma_channel_is_busy(uint32_t channel) { return uart_stalled || now_us < dma_busy_until; }
sim_uart_hw_t *uart_get_hw(unsigned uart) {
    (void)uart;
    /* Independent FIFO/shifter control catches DMA-idle-but-not-wire-idle
       resets that the ordinary whole-frame DMA timing model cannot expose. */
    uart_hw.fr = (uart_busy_override || now_us < dma_busy_until) ? UART_UARTFR_BUSY_BITS : 0;
    return &uart_hw;
}
void dma_channel_transfer_from_buffer_now(uint32_t channel,const void *p,uint32_t n) {
    /* 8N1 serial duration, rounded upwards. Copying/serialization belongs to
       the transport model; RX feeds the real production DMA ring parser. */
    dma_busy_until=now_us+((uint64_t)n*10*1000000+SERIAL_BAUDRATE-1)/SERIAL_BAUDRATE;
    emit(2,channel,(int)(dma_busy_until-now_us),p,n);
}
dma_channel_hw_t *dma_channel_hw_addr(uint32_t channel) { return &rx_hw; }
void sim_rx_byte(uint8_t b) {
    uart_rxbuf[rx_write]=b; rx_write=NEXT_RING_IDX(rx_write);
    rx_hw.transfer_count=DMA_RX_BUFFER_SIZE-rx_write;
}
/* Link against real TinyUSB declarations. Stack/transactions are explicit models. */
void tud_task_ext(uint32_t timeout,bool in_isr) { }
void tuh_task_ext(uint32_t timeout,bool in_isr) { }
bool tuh_inited(void) { return true; }
bool tud_mounted(void) { return mounted; }
bool tud_suspended(void) { return suspended; }
bool tud_remote_wakeup(void) { emit(10,0,0,NULL,0); return true; }
bool tud_hid_n_ready(uint8_t instance) {
    return instance<3 && mounted && !suspended && !stalled[instance] && now_us>=busy_until[instance];
}
bool tud_hid_n_report(uint8_t instance,uint8_t id,const void *report,uint16_t len) {
    emit(9,instance,id,NULL,0); /* ready -> send race checkpoint */
    if(!tud_hid_n_ready(instance) || fail_report) return false;
    assert(len+1<=CFG_TUD_HID_EP_BUFSIZE);
    busy_until[instance]=now_us+1000; /* advertised 1 ms poll interval */
    emit(1,instance,id,report,len); return true;
}
bool tud_hid_n_keyboard_report(uint8_t instance,uint8_t id,uint8_t modifier,uint8_t key[6]) {
    hid_keyboard_report_t r={.modifier=modifier}; memcpy(r.keycode,key,6);
    return tud_hid_n_report(instance,id,&r,sizeof(r));
}
uint8_t tuh_hid_interface_protocol(uint8_t addr,uint8_t instance) {
    return addr && addr<=MAX_DEVICES && instance<MAX_INTERFACES ? protocol[addr-1][instance] : 0;
}
uint8_t tuh_hid_get_protocol(uint8_t addr,uint8_t instance) { return boot[addr-1][instance]; }
bool tuh_hid_set_protocol(uint8_t addr,uint8_t instance,uint8_t p) {
    boot[addr-1][instance]=p; tuh_hid_set_protocol_complete_cb(addr,instance,p); return true;
}
bool tuh_hid_receive_report(uint8_t addr,uint8_t instance) { return true; }
bool tuh_hid_set_report(uint8_t addr,uint8_t instance,uint8_t id,uint8_t type,void *p,uint16_t len) {
    emit(3,addr,instance,p,len); return true;
}

void sim_init(uint8_t role, event_cb_t cb) {
    event_cb=cb;
    uart_busy_override = false;
    memset(&uart_hw, 0, sizeof(uart_hw));
    history_request_accepted = history_poll_ready = false;
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
    history_borrowed = NULL;
#endif
    memset(&global_state,0,sizeof(global_state));
    memset(sim_flash,0xff,sizeof(sim_flash));
    global_state.board_role=role; global_state.config=default_config;
    global_state.pointer_x=16000; global_state.pointer_y=16000;
    global_state._running_fw.version=192;
    queue_init(&global_state.kbd_queue,sizeof(hid_keyboard_report_t),KBD_QUEUE_LENGTH);
    queue_init(&global_state.mouse_queue,sizeof(mouse_report_t),MOUSE_QUEUE_LENGTH);
    queue_init(&global_state.uart_tx_queue,sizeof(uart_packet_t),UART_QUEUE_LENGTH);
    queue_init(&global_state.hid_queue_out,sizeof(hid_generic_pkt_t),HID_QUEUE_LENGTH);
    firmware_sync_init(); rx_hw.transfer_count=DMA_RX_BUFFER_SIZE;
#if SIM_HAS_FW_BATCH
    firmware_batch_init(&global_state, role + 1);
#endif
#if SIM_HAS_MAINTENANCE
    maintenance_start_result = 0;
    maintenance_poll_ready = false;
    memset(&maintenance_result, 0, sizeof(maintenance_result));
    maintenance_init(role, role + 1);
#endif
#if SIM_HAS_KEYBOARD_SYNC
    keyboard_sync_init(role + 1);
#endif
#if SIM_HAS_DIAGNOSTIC_HISTORY
    /* The store and its cross-core bridge remain production code even when
       this test boundary disables CDC. Initialize before any USB callback. */
    diagnostic_history_init();
#if SIM_HAS_DIAGNOSTIC_RUNTIME
    diagnostic_runtime_init();
#endif
    diagnostic_history_record(HISTORY_BOOT, global_state.active_output, 0, 98);
#endif
#if SIM_HAS_DIAGNOSTIC_PEER
    peer_status_snapshot_t identity = {
        .role = role, .major = 0, .minor = 97, .boot_session = role + 1,
        .image_crc_at_boot = UINT32_C(0x12345678) + role,
    };
    for (unsigned i = 0; i < sizeof(identity.board_id); ++i)
        identity.board_id[i] = (uint8_t)(role * 16 + i);
    diagnostic_peer_init(&identity);
#endif
}
void sim_destroy(void) {
#if SIM_HAS_MAINTENANCE
    maintenance_shutdown();
#endif
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
    history_borrowed = NULL;
#endif
#if SIM_HAS_DIAGNOSTIC_PEER
    diagnostic_peer_shutdown();
#endif
    queue_free(&global_state.kbd_queue); queue_free(&global_state.mouse_queue);
    queue_free(&global_state.uart_tx_queue); queue_free(&global_state.hid_queue_out);
}
void sim_host(int connect,int suspend) {
    if (stopped) return;
    bool changed=mounted!=(bool)connect;
    mounted=connect; suspended=suspend;
    if(changed) { if(connect) tud_mount_cb(); else tud_umount_cb(); }
}
void sim_endpoint(int instance,int stall,int reject) { stalled[instance]=stall; fail_report=reject; }
void sim_uart_stall(int stall) { uart_stalled = stall; }
void sim_uart_busy(int busy) { uart_busy_override = busy; }
void sim_maintenance_request(uint8_t target, uint32_t token) {
#if SIM_HAS_MAINTENANCE
    maintenance_start_result = maintenance_request(target, token, now_us);
#else
    maintenance_start_result = 4;
#endif
    emit(18, token, maintenance_start_result, &target, sizeof(target));
}
void sim_maintenance_poll(void) {
#if SIM_HAS_MAINTENANCE
    maintenance_poll_ready = maintenance_poll(&maintenance_result);
    if (maintenance_poll_ready)
        emit(19, maintenance_result.token, maintenance_result.outcome,
             &maintenance_result.target, sizeof(maintenance_result.target));
#else
    maintenance_poll_ready = false;
#endif
}
void sim_maintenance_complete(uint32_t token) {
#if SIM_HAS_MAINTENANCE
    maintenance_console_reply_complete(token, now_us);
#endif
}
void sim_maintenance_cancel(uint32_t token) {
#if SIM_HAS_MAINTENANCE
    maintenance_cancel(token, now_us);
#endif
}
void sim_maintenance_session(uint64_t session) {
#if SIM_HAS_MAINTENANCE
    maintenance_init(global_state.board_role, session);
#endif
}
/* Core 0's actual bridge API, with scalar introspection instead of relying on
 * ctypes matching either ARM or the host's structure padding. */
void sim_diagnostic_request(uint32_t token) {
#if SIM_HAS_DIAGNOSTIC_PEER
    diagnostic_request_accepted = diagnostic_peer_request(token, now_us);
#else
    diagnostic_request_accepted = false;
#endif
    emit(12, (int)token, diagnostic_request_accepted, NULL, 0);
}
void sim_diagnostic_poll(void) {
#if SIM_HAS_DIAGNOSTIC_PEER
    diagnostic_poll_ready = diagnostic_peer_poll(&diagnostic_result);
    if (diagnostic_poll_ready)
        emit(13, (int)diagnostic_result.token, diagnostic_result.outcome,
             diagnostic_result.snapshot.board_id, sizeof(diagnostic_result.snapshot.board_id));
#else
    diagnostic_poll_ready = false;
#endif
}
void sim_history_request(uint32_t token, unsigned count) {
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
    history_request_accepted = diagnostic_peer_history_request(token, count, now_us);
#else
    history_request_accepted = false;
#endif
    emit(14, (int)token, history_request_accepted, NULL, 0);
}
void sim_history_poll(void) {
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
    const peer_history_result_t *result = diagnostic_peer_history_poll();
    history_poll_ready = result != NULL;
    if (result) {
        assert(!history_borrowed || history_borrowed == result);
        history_borrowed = result;
        emit(15, (int)result->token, result->outcome, NULL, 0);
    }
#else
    history_poll_ready = false;
#endif
}
void sim_history_release(void) {
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
    diagnostic_peer_history_release();
    history_borrowed = NULL;
#endif
    history_poll_ready = false;
    emit(16, 0, 0, NULL, 0);
}
/* Explicit fixture actions: history capture itself is real production code,
   but these records do not purport to be generated by physical input. */
void sim_history_clear(void) {
#if SIM_HAS_DIAGNOSTIC_HISTORY
    diagnostic_history_init();
#endif
}
void sim_history_record(uint8_t type, uint8_t a, uint8_t b, uint32_t value) {
#if SIM_HAS_DIAGNOSTIC_HISTORY
    diagnostic_history_record((history_type_t)type, a, b, value);
#endif
}
/* Verification-only fixture: establish identical boot images before querying.
 * This is test setup, not a firmware update or a physical reboot model. */
void sim_verify_prepare(void) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    for (unsigned i = 0; i < STAGING_IMAGE_SIZE; ++i)
        sim_flash[i] = (uint8_t)((i * 73u + i / 127u) ^ 0x31);
    firmware_metadata_t metadata = {.magic=FIRMWARE_METADATA_MAGIC, .version=201,
        .checksum=calc_crc32(sim_flash, STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)};
    memcpy(sim_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE, &metadata, sizeof(metadata));
    global_state._running_fw = metadata;
    peer_status_snapshot_t fixture = {.role=global_state.board_role, .minor=101,
        .boot_session=global_state.board_role + 1, .image_crc_at_boot=metadata.checksum};
    for (unsigned i=0;i<8;++i) fixture.board_id[i]=global_state.board_role*16+i;
    diagnostic_peer_init(&fixture);
#endif
}
/* Independent fixture images for the real updater. This does not implement
 * transfer, program, metadata validation, or reboot: those stay production C.
 * The configuration sentinel makes accidental writes beyond the slot visible. */
void sim_fw_prepare(uint16_t version, uint8_t salt) {
    assert(version >= 100);
    for (unsigned i = 0; i < STAGING_IMAGE_SIZE; ++i)
        sim_flash[i] = (uint8_t)((i * 73u + i / 127u) ^ salt);
    firmware_metadata_t metadata = {.magic=FIRMWARE_METADATA_MAGIC, .version=version,
        .checksum=calc_crc32(sim_flash, STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)};
    memcpy(sim_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE, &metadata, sizeof(metadata));
    memset(sim_flash + sizeof(sim_flash) - FLASH_SECTOR_SIZE, 0xa5, FLASH_SECTOR_SIZE);
    global_state._running_fw = metadata;
#if SIM_HAS_DIAGNOSTIC_PEER
    peer_status_snapshot_t fixture = {.role=global_state.board_role,
        .major=(version - 100) / 1000, .minor=(version - 100) % 1000,
        .boot_session=global_state.board_role + 1, .image_crc_at_boot=metadata.checksum};
    for (unsigned i=0;i<8;++i) fixture.board_id[i]=global_state.board_role*16+i;
    diagnostic_peer_init(&fixture);
#endif
}
void sim_flash_read(uint32_t offset, uint8_t *out, uint32_t length) {
    assert(out && offset <= sizeof(sim_flash) && length <= sizeof(sim_flash) - offset);
    memcpy(out, sim_flash + offset, length);
}
void sim_verify_request(uint32_t token) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    verify_request_accepted=diagnostic_verify_request(token,now_us);
#else
    verify_request_accepted=false;
#endif
}
void sim_verify_poll(void) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    verify_poll_ready=diagnostic_verify_poll(&verify_result);
#else
    verify_poll_ready=false;
#endif
}
void sim_verify_assess(uint16_t version, uint32_t crc) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    verify_assessment=verification_assess(&verify_result,version,crc);
#endif
}
void sim_verify_recheck(void) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    diagnostic_verify_recheck_local(&verify_result);
#endif
}
void sim_verify_mutate(uint32_t offset, uint8_t keep_bits) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    assert(offset < sizeof(sim_flash));
    uint32_t page_offset=offset & ~(FLASH_PAGE_SIZE-1);
    uint8_t page[FLASH_PAGE_SIZE];
    memcpy(page,sim_flash+page_offset,sizeof(page));
    page[offset-page_offset] &= keep_bits;
    write_flash_page_erasing(page_offset,page,false);
#endif
}
void sim_verify_update_state(unsigned state) {
#if SIM_HAS_DIAGNOSTIC_VERIFY
    assert(state <= 3);
    global_state.fw.upgrade_in_progress=state==1;
    global_state.fw.image_dirty=state==2;
    global_state.reboot_requested=state==3;
#endif
}
void sim_mount(uint8_t addr,uint8_t instance,uint8_t proto,const uint8_t *desc,uint16_t len) {
    if (stopped) return;
    assert(addr && addr<=MAX_DEVICES && instance<MAX_INTERFACES);
    protocol[addr-1][instance]=proto; boot[addr-1][instance]=HID_PROTOCOL_REPORT;
    tuh_hid_mount_cb(addr,instance,desc,len);
}
void sim_report(uint8_t addr,uint8_t instance,const uint8_t *p,uint16_t n) { if (stopped) return; GUARDED(tuh_hid_report_received_cb(addr,instance,p,n)); }
void sim_unmount(uint8_t addr,uint8_t instance) { if (stopped) return; tuh_hid_umount_cb(addr,instance); }
void sim_vendor(const uint8_t *data,uint16_t len) { if(stopped)return; GUARDED(tud_hid_set_report_cb(ITF_NUM_HID_VENDOR,REPORT_ID_VENDOR,HID_REPORT_TYPE_OUTPUT,data,len)); }
void sim_led(uint8_t value) { if (stopped) return; tud_hid_set_report_cb(0,REPORT_ID_KEYBOARD,HID_REPORT_TYPE_OUTPUT,&value,1); }
void sim_select(uint8_t output) { if (stopped) return; GUARDED(set_active_output(&global_state,output)); }
/* Task IDs, names, frequencies and core split come from production main.c.
   This boundary leaves CDC disabled: the console task is a no-op here. Real
   CDC/console transactions are exercised by the separate USB stack harness. */
#include "task_tables.inc"
int sim_task_count(void) { return (int)ARRAY_SIZE(sim_tasks); }
int sim_task_core(int id) {
    assert(id>=0 && id<sim_task_count());
    return id>=SIM_CORE0_TASK_COUNT;
}
const char *sim_task_name(int id) {
    assert(id>=0 && id<sim_task_count());
    return sim_task_names[id];
}
void sim_task(int id) {
    assert(id>=0 && id<(int)ARRAY_SIZE(sim_tasks));
    if(stopped) return;
    if(sim_task_core(id)) global_state.core1_last_loop_pass=time_us_32();
    GUARDED(task_scheduler(&global_state,&sim_tasks[id]));
}
void sim_core_step(int core) {
    if (stopped) return;
    assert(core==0 || core==1);
    int begin=core ? SIM_CORE0_TASK_COUNT : 0;
    int end=core ? (int)ARRAY_SIZE(sim_tasks) : SIM_CORE0_TASK_COUNT;
    if(core) global_state.core1_last_loop_pass=time_us_32();
    GUARDED(for(int id=begin;id<end;id++) task_scheduler(&global_state,&sim_tasks[id]));
}
uint64_t sim_frequency(int id) { assert(id>=0 && id<sim_task_count()); return sim_tasks[id].frequency; }
void sim_watchdog(void) {
    if(!stopped && (now_us-last_kick>=WATCHDOG_TIMEOUT*1000 || *(uint32_t *)(sim_ppb+0xed0c))) {
        stopped=true; emit(5,0,0,NULL,0);
    }
}
/* Numeric introspection keeps ctypes independent of host/ARM structure padding. */
int64_t sim_get(int field,int index) {
    device_t *s=&global_state;
    switch(field) {
      case 0:return s->active_output; case 1:return s->pointer_x; case 2:return s->pointer_y;
      case 3:return s->mouse_buttons; case 4:return s->reboot_requested; case 5:return stopped;
      case 6:return queue_get_level(&s->kbd_queue); case 7:return queue_get_level(&s->mouse_queue);
      case 8:return queue_get_level(&s->uart_tx_queue); case 9:return queue_get_level(&s->hid_queue_out);
      case 10:return s->direct_activity[index]; case 11:return s->peer_activity[index];
      case 12:return s->last_activity[index]; case 13:return s->gaming_mode;
      case 14:return s->zoom_assist[index].active; case 15:return s->local_modifiers;
      case 16:return s->peer_modifiers; case 17:return s->keyboard_leds_actual[index];
      case 18:return s->fw.source; case 19:return s->fw.address;
      case 20:return s->fw.image_dirty; case 21:return s->blinks_left;
      case 22:return s->direct_activity_valid; case 23:return s->peer_activity_valid;
      case 24:return last_kick;
#if SIM_HAS_CONFIG_BOOTLOADER
      case 25:return s->config_bootloader_peer_pending;
      case 26:return s->config_bootloader_local_pending;
#else
      case 25:case 26:return 0;
#endif
      case 27:return dma_channel_is_busy(s->dma_tx_channel);
      case 28:return !!(uart_get_hw(SERIAL_UART)->fr & UART_UARTFR_BUSY_BITS);
      case 29:return s->fw.upgrade_in_progress;
      case 40:return s->zoom_assist[index].debt;
      case 41:return s->zoom_assist[index].overscroll;
      case 42:return s->zoom_assist[index].exit_pending;
      case 43:return s->zoom_assist[index].zoom_in_direction;
      case 44:return s->zoom_assist[index].exit_deadline;
      case 50:return s->config_mode_active;
      case 51:return s->config.screensaver_system_timeout_sec;
      case 60:return diagnostic_request_accepted;
      case 61:return diagnostic_poll_ready;
      case 130:return maintenance_start_result;
      case 131:return maintenance_poll_ready;
#if SIM_HAS_MAINTENANCE
      case 132:return maintenance_result.token;
      case 133:return maintenance_result.target;
      case 134:return maintenance_result.outcome;
      case 135:return s->maintenance_reserved;
      case 136:return s->maintenance_source_seen;
      case 137:return s->maintenance_source_last_us;
#else
      case 132:case 133:case 134:case 135:case 136:case 137:return 0;
#endif
      case 110:return verify_request_accepted;
      case 111:return verify_poll_ready;
#if SIM_HAS_DIAGNOSTIC_VERIFY
      case 112:return verify_result.token;
      case 113:return verify_result.remote;
      case 114:return verify_result.transport;
      case 115:return verify_result.snapshot.outcome;
      case 116:return verify_result.snapshot.slot_crc32;
      case 117:return verify_result.snapshot.bytes_read;
      case 118:return verify_result.snapshot.started_us;
      case 119:return verify_result.snapshot.completed_us;
      case 120:return verify_result.snapshot.generation_start;
      case 121:return verify_result.snapshot.generation_end;
      case 122:assert(index>=0 && index<2);return verify_result.snapshot.start.core_ticks[index];
      case 123:assert(index>=0 && index<2);return verify_result.snapshot.end.core_ticks[index];
      case 124:assert(index>=0 && index<2);return verify_result.snapshot.end.core_age_ms[index];
      case 125:return verify_assessment.verdict;
      case 126:return verify_assessment.reason;
      case 127:return verify_result.snapshot.minor;
      case 128:return verify_result.snapshot.boot_session;
      case 129:return verify_result.snapshot.metadata_crc32;
#endif
      case 80:return history_request_accepted;
      case 81:return history_poll_ready;
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
      case 82:return history_borrowed != NULL;
      case 83:assert(history_borrowed);return history_borrowed->token;
      case 84:assert(history_borrowed);return history_borrowed->outcome;
      case 85:assert(history_borrowed);return history_borrowed->snapshot.role;
      case 86:assert(history_borrowed);return history_borrowed->snapshot.boot_session;
      case 87:assert(history_borrowed);return history_borrowed->requested_at_us;
      case 88:assert(history_borrowed);return history_borrowed->first_response_us;
      case 89:assert(history_borrowed);return history_borrowed->snapshot.sampled_at_us;
      case 90:assert(history_borrowed);return history_borrowed->snapshot.window.first_seq;
      case 91:assert(history_borrowed);return history_borrowed->snapshot.window.end_seq;
      case 92:assert(history_borrowed);return history_borrowed->snapshot.window.oldest_seq;
      case 93:assert(history_borrowed);return history_borrowed->snapshot.window.overwritten;
      case 94:assert(history_borrowed);return history_borrowed->snapshot.window.count;
      case 95:assert(history_borrowed);return history_borrowed->snapshot.gap_mask;
      case 96:case 97:case 98:case 99:case 100:case 101:case 102: {
        assert(history_borrowed && index >= 0 && index < PEER_HISTORY_MAX_COUNT);
        const history_event_t *event = &history_borrowed->snapshot.events[index];
        switch (field) {
          case 96:return event->seq;case 97:return event->time_us;case 98:return event->value;
          case 99:return event->type;case 100:return event->a;case 101:return event->b;
          default:return event->reserved;
        }
      }
#endif
#if SIM_HAS_DIAGNOSTIC_PEER
      case 62:return diagnostic_result.token;
      case 63:return diagnostic_result.outcome;
      case 64:return diagnostic_result.snapshot.role;
      case 65:return diagnostic_result.snapshot.major;
      case 66:return diagnostic_result.snapshot.minor;
      case 67:return diagnostic_result.snapshot.boot_session;
      case 68:return diagnostic_result.snapshot.uptime_ms;
      case 69:return diagnostic_result.snapshot.image_crc_at_boot;
      case 70:assert(index >= 0 && index < 8);return diagnostic_result.snapshot.board_id[index];
#if SIM_HAS_DIAGNOSTIC_RUNTIME
      case 71:return diagnostic_result.snapshot.protocol;
      case 72:return diagnostic_result.snapshot.runtime.core_valid;
      case 73:assert(index >= 0 && index < 2);return diagnostic_result.snapshot.runtime.core_ticks[index];
      case 74:assert(index >= 0 && index < 2);return diagnostic_result.snapshot.runtime.core_age_ms[index];
      case 75:return diagnostic_result.observation.boot;
      case 76:return diagnostic_result.observation.progress;
      case 77:return diagnostic_result.observation.update;
      case 78:return diagnostic_result.snapshot.runtime.phase;
      case 79:return diagnostic_result.snapshot.runtime.received_bytes;
#endif
#endif
      default:assert(false);return 0;
    }
}
void sim_set(int field,int index,int64_t value) {
    device_t *s=&global_state;
    switch(field) {
      case 1:s->pointer_x=value;break; case 2:s->pointer_y=value;break;
      case 13:s->gaming_mode=value;break;
      case 30:s->config.output[index].screensaver.mode=value;break;
      case 31:s->config.output[index].screensaver.idle_time_us=value;break;
      case 32:s->config.output[index].screensaver.max_time_us=value;break;
      case 33:s->config.output[index].screensaver.only_if_inactive=value;break;
      case 34:s->config.screensaver_system_timeout_sec=value;break;
      case 35:s->config.enable_acceleration=value;break;
      case 36:s->config.output[index].speed_x=value;s->config.output[index].speed_y=value;break;
      case 37:s->config.output[index].os=value;break;
      case 38:s->config.kbd_led_as_indicator=value;break;
      case 50:s->config_mode_active=value;break;
      default:assert(false);
    }
}
void sim_fill(int queue_id,int count) {
    queue_t *q= queue_id==0?&global_state.kbd_queue:queue_id==1?&global_state.uart_tx_queue:&global_state.mouse_queue;
    uint8_t data[32]={0}; if(queue_id==0) data[0]=KEYBOARD_MODIFIER_LEFTCTRL;
    for(int i=0;i<count;i++) queue_try_add(q,data);
}
int sim_descriptor(int type,int index,uint8_t *out) {
    const uint8_t *p;
    int n=0;
    if(type==0) { p=tud_descriptor_device_cb(); n=p[0]; }
    else if(type==1) { p=tud_descriptor_configuration_cb(index); n=p[2]|(p[3]<<8); }
    else { p=tud_hid_descriptor_report_cb(index); const uint8_t *cfg=tud_descriptor_configuration_cb(0);
        for(int off=0;off<(cfg[2]|(cfg[3]<<8));off+=cfg[off])
            if(cfg[off+1]==0x21 && index--==0) { n=cfg[off+7]|(cfg[off+8]<<8); break; }
    }
    assert(n>=0 && n<=1024); memcpy(out,p,n); return n;
}
