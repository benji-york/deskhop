/* Exact allocation and independent state/output oracles for actual mouse code.
 * This executable runs with ASan/UBSan (separate from the Python-loaded nodes). */
#include "main.h"
#include <assert.h>
void sim_init(uint8_t,void (*)(int,int,int,const void *,int));
void sim_destroy(void);
void sim_host(int,int);
void sim_set_time(uint64_t);
void sim_task(int);
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
int main(void) {
    sim_init(0,observe);sim_host(1,0);sim_set_time(1);
    global_state.config.enable_acceleration=0;
    global_state.config.output[0].speed_x=1;
    report_case(true,0);
    for(unsigned id=0;id<=255;id++)report_case(false,id);
    /* A corrupted/malformed output value must never become an array index. */
    for(unsigned output=2;output<=255;output++) {
        uart_packet_t p={.type=OUTPUT_SELECT_MSG,.data={output}};
        p.checksum=calc_checksum(p.data,PACKET_DATA_LENGTH);
        process_packet(&p,&global_state);
        assert(global_state.active_output==0);
    }
    sim_task(3);assert(outputs==1);
    wide_motion();
    sim_destroy();
    puts("native boundaries: all 256 mouse IDs, exact truncations, 3-byte boot mouse, invalid output indices passed");
}
