"""Observable, independently encoded pointer/transport regression oracles."""
import struct
from fixtures import attach, keyboard, mouse, MOUSE

def out_mouse(buttons,x,y,wheel=0,pan=0,mode=0):
    return struct.pack('<BhhbbB',buttons,x,y,wheel,pan,mode).hex()

def scenario_pointer(s):
    attach(s)
    for output in (0,1):
        s.do(0,'select',output);s.advance(10000)
        # Place the owner independently, poison the inactive node's cache. A
        # position-neutral click must use the owner's location even without sync.
        owner=output;source=1-output
        s.do(owner,'set','x',0,12345);s.do(owner,'set','y',0,23456)
        s.do(source,'set','x',0,32767);s.do(source,'set','y',0,0)
        itf=1 if source==0 else 0
        s.do(source,'report',1,itf,mouse(buttons=2));s.advance(10000)
        s.expect_report(owner,2,out_mouse(2,12345,23456))
        s.do(source,'report',1,itf,mouse(wheel=-3));s.advance(10000)
        s.expect_report(owner,2,out_mouse(0,12345,23456,-3))
        s.do(owner,'set','gaming',0,1);s.do(source,'set','gaming',0,1)
        s.do(source,'report',1,itf,mouse(buttons=2));s.advance(10000)
        s.expect_report(owner,5,out_mouse(2,0,0,mode=1))
        s.do(source,'report',1,itf,mouse());s.advance(5000)
        s.do(owner,'set','gaming',0,0);s.do(source,'set','gaming',0,0)

def scenario_pointer_sync(s):
    attach(s)
    # B physical trackball -> A; A keyboard composite mouse -> A; back to B.
    s.do(1,'report',1,0,mouse(x=37,y=-19));s.advance(5000)
    for n in (0,1):s.expect(n,'x',16037);s.expect(n,'y',15981)
    s.do(0,'report',1,1,mouse(x=-7,y=11));s.advance(5000)
    for n in (0,1):s.expect(n,'x',16030);s.expect(n,'y',15992)
    s.do(1,'report',1,0,mouse(x=3));s.advance(5000)
    s.expect_report(0,2,out_mouse(0,16033,15992))
    before=s.get(0,'direct',0)
    s.do(0,'report',1,1,mouse());s.advance(5000)
    s.expect(0,'direct',before,0)
    s.expect_report(0,2,out_mouse(0,16033,15992))

def scenario_uart_faults(s):
    attach(s)
    # One corrupted payload must not move the pointer; next intact frame recovers.
    s.do(1,'fault',{'xor':1});s.do(1,'report',1,0,mouse(x=10));s.advance(5000)
    s.expect(0,'x',16000)
    s.do(1,'report',1,0,mouse(x=5));s.advance(5000)
    s.expect(0,'x',16015)
    # Short traffic is held until a full packet exists, then malformed preambles
    # are skipped and the next complete packet re-synchronizes.
    s.do(1,'fault',{'truncate':5});s.do(1,'report',1,0,mouse(x=2));s.advance(5000)
    s.expect(0,'x',16015)
    s.do(1,'report',1,0,mouse(x=3));s.advance(5000)
    # A frame directly after a truncated frame can be consumed in its tail.
    s.do(1,'report',1,0,mouse(x=4));s.advance(5000)
    s.expect(0,'x',16024)
    # Delayed UART remains invisible until its first entire frame arrives.
    s.do(1,'fault',{'delay':20000});s.do(1,'report',1,0,mouse(x=1));s.advance(10000)
    s.expect(0,'x',16024);s.advance(20000);s.expect(0,'x',16025)

def scenario_backpressure(s):
    attach(s)
    s.do(0,'endpoint',0,1,0)
    s.do(0,'report',1,0,keyboard(1,4));s.advance(5000)
    s.expect(0,'kbd_queue',1)
    s.do(0,'endpoint',0,0,1);s.advance(5000)
    s.expect(0,'kbd_queue',1) # failed send must not dequeue
    s.do(0,'endpoint',0,0,0);s.advance(5000)
    s.expect(0,'kbd_queue',0);s.expect_report(0,1,keyboard(1,4))
    # A host can suspend precisely between readiness and submission.
    s.do(0,'checkpoint',0,'host',[1,1])
    s.do(0,'report',1,0,keyboard());s.advance(5000)
    s.expect(0,'kbd_queue',1)
    s.do(0,'host',1,0);s.advance(5000)
    s.expect(0,'kbd_queue',0);s.expect_report(0,1,keyboard())

def scenario_critical_queue(s):
    attach(s)
    s.do(0,'endpoint',0,1,0);s.do(0,'fill',0,128)
    start=s.now;s.do(0,'select',1)
    # Critical release waits exactly the production 100 ms boundary, then
    # requests watchdog reboot instead of reporting success after a drop.
    s.expect(0,'reboot',1)
    s.expect(0,'kbd_queue',128)
    s.check('time_range',start+100000,start+100999)
    s.do(0,'endpoint',0,0,0);s.advance(510000)
    s.expect(0,'stopped',1)

def scenario_uart_queue_switch(s):
    attach(s)
    s.do(0,'fill',1,256)
    s.do(0,'select',1) # the opposite core must make progress during blocking add
    s.advance(150000)
    s.expect(0,'output',1);s.expect(1,'output',1)
    s.expect(0,'uart_queue',0)

def scenario_disconnect(s):
    attach(s)
    s.do(0,'report',1,0,keyboard(8,4));s.advance(5000)
    s.expect_report(0,1,keyboard(8,4))
    s.do(0,'unmount',1,0);s.advance(5000)
    s.expect_report(0,1,keyboard());s.expect(0,'modifiers',0)
    s.do(0,'host',0,0)
    s.do(1,'report',1,0,mouse(x=4));s.advance(5000)
    s.expect(0,'mouse_queue',0)
    s.do(0,'host',1,0);s.do(1,'report',1,0,mouse(x=3));s.advance(5000)
    s.expect_report(0,2,out_mouse(0,16007,16000))

def scenario_core_watchdog(s):
    attach(s)
    s.do(1,'pause',1,1200000);s.advance(1100000)
    s.expect(1,'stopped',1);s.expect(0,'stopped',0)

SCENARIOS={
    'pointer':scenario_pointer,'pointer_sync':scenario_pointer_sync,
    'uart_faults':scenario_uart_faults,'backpressure':scenario_backpressure,
    'critical_queue':scenario_critical_queue,'uart_queue_switch':scenario_uart_queue_switch,
    'disconnect':scenario_disconnect,'core_watchdog':scenario_core_watchdog,
}
# Retain explicit expected-counterexample support for future findings. The two
# original gaps are now passing regressions in mouse_buttons and selection.
KNOWN_GAPS={}

def scenario_generated(s,steps=100):
    """Independent host oracle: accumulate integer deltas in a safe interior.

    Input generation uses its own PRNG so transport scheduler choices cannot
    accidentally define the expected result. Every event is saved for replay.
    """
    import random
    r=random.Random(s.seed ^ 0x44534b48);attach(s)
    x=y=16000;active=0
    for i in range(steps):
        if i%11==0:
            active=1-active
            s.do(0,'report',1,0,keyboard(0,0x73));s.advance(5000) # bare F24
            s.do(0,'report',1,0,keyboard());s.advance(5000)
            s.expect(0,'output',active);s.expect(1,'output',active)
        source=r.randrange(2);dx=r.randint(-12,12);dy=r.randint(-12,12)
        if dx==dy==0:dx=1
        x+=dx;y+=dy
        s.do(source,'report',1,1 if source==0 else 0,mouse(x=dx,y=dy));s.advance(r.randint(2000,6000))
        s.expect_report(active,2,out_mouse(0,x,y))
        for n in (0,1):s.expect(n,'x',x);s.expect(n,'y',y)

def frame(kind,data=b''):
    # Independent protocol oracle; no calls to production write_raw_packet.
    data=bytes(data).ljust(8,b'\0');assert len(data)==8
    checksum=0
    for byte in data:checksum^=byte
    return (b'\xaa\x55'+bytes([kind])+data+bytes([checksum])).hex()

def scenario_vendor_config(s):
    attach(s)
    request=frame(21,b'\x53'+(123).to_bytes(4,'little')) # SET timeout API field83
    before=s.get(0,'system_timeout')
    s.do(0,'vendor',request);s.expect(0,'system_timeout',before)
    s.do(0,'set','config_mode',0,1)
    s.do(0,'vendor',request[:-2]);s.expect(0,'system_timeout',before)
    bad=request[:-2]+f'{int(request[-2:],16)^1:02x}'
    s.do(0,'vendor',bad);s.expect(0,'system_timeout',before)
    s.do(0,'vendor',request);s.expect(0,'system_timeout',123)
    s.do(0,'vendor',frame(20,b'\x53'));s.advance(3000)
    s.expect_report(0,6,frame(20,b'\x53'+(123).to_bytes(4,'little')))
    # Read-only active output and unsupported API IDs cannot be written.
    s.do(0,'vendor',frame(21,b'\x00\x01'));s.expect(0,'output',0)
    count=len(s.reports(0,6));s.do(0,'vendor',frame(20,b'\xff'));s.advance(2000)
    s.check('usb_count',0,6,count)
    # Config endpoint may not inject input-routing packet types.
    s.do(0,'vendor',frame(3,b'\x01'));s.expect(0,'output',0)
    s.do(0,'endpoint',2,1,0);s.do(0,'vendor',frame(20,b'\x53'));s.advance(3000)
    s.expect(0,'hid_queue',1)
    s.do(0,'endpoint',2,0,0);s.advance(3000);s.expect(0,'hid_queue',0)
    # GET_ALL walks production API map, returning every mapped field once.
    count=len(s.reports(0,6));s.do(0,'vendor',frame(22));s.advance(100000)
    s.check('usb_count',0,6,count+44)


def scenario_consumer_system(s):
    attach(s)
    consumer='050c0901a101150026ff0319002aff03751095028100c0'
    system='05010980a1011981298315012503750895018100c0'
    s.do(0,'mount',2,0,0,consumer);s.do(0,'mount',2,1,0,system);s.advance(5000)
    for output in (0,1):
        s.do(0,'select',output);s.advance(5000)
        s.do(0,'report',2,0,'e900ea00');s.advance(5000);s.expect_report(output,3,'e900ea00')
        s.do(0,'report',2,1,'82');s.advance(5000);s.expect_report(output,4,'82')
        s.do(0,'report',2,0,'00000000');s.advance(5000);s.expect_report(output,3,'00000000')
        prior=s.get(0,'direct',output);count=len(s.reports(output))
        s.do(0,'report',2,0,'');s.do(0,'report',2,1,'');s.advance(5000)
        s.expect(0,'direct',prior,output);s.check('usb_count',output,None,count)

SCENARIOS.update(vendor_config=scenario_vendor_config,consumer_system=scenario_consumer_system)
