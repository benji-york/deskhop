"""Standard USB HID keyboard and QMK-style mouse layouts.

These small hand-auditable fixtures are independent of DeskHop's parser.
Keyboard: modifier/reserved/six usages. Mouse: buttons, signed X/Y/wheel/pan.
QMK source reference: tmk_core/protocol/usb_descriptor.c MouseReport descriptor.
"""
KEYBOARD=bytes.fromhex('05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 75 08 95 01 81 01 05 08 19 01 29 05 75 01 95 05 91 02 75 03 95 01 91 01 05 07 19 00 29 65 15 00 25 65 75 08 95 06 81 00 c0')
MOUSE=bytes.fromhex('05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 08 15 00 25 01 75 01 95 08 81 02 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 05 0c 0a 38 02 15 81 25 7f 75 08 95 01 81 06 c0 c0')
def keyboard(mod=0,*keys):return bytes([mod,0,*list(keys)[:6],*([0]*(6-len(keys[:6])))]).hex()
def mouse(buttons=0,x=0,y=0,wheel=0,pan=0):return bytes(v&255 for v in [buttons,x,y,wheel,pan]).hex()
def attach(s):
    for n in (0,1):
        s.do(n,'host',1,0);s.do(n,'set','acceleration',0,0);s.do(n,'set','speed',0,1);s.do(n,'set','speed',1,1)
        s.do(n,'set','os',0,2);s.do(n,'set','os',1,2)
    s.do(0,'mount',1,0,1,KEYBOARD.hex())
    s.do(0,'mount',1,1,0,MOUSE.hex())
    s.do(1,'mount',1,0,2,MOUSE.hex())
    s.advance(5000)
