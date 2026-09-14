"""Deterministic paired-Pico application simulator (standard library only).

Production C executes in two RTLD_LOCAL libraries, including file/function statics.
The scheduler orders core task polls, DMA bytes, host changes, and HAL checkpoints.
USB is a descriptor/report/endpoint model, not a USB wire or macOS implementation.
"""
from __future__ import annotations
import ctypes as C
import heapq
import hashlib
import itertools
import json
import pathlib
import random
import shutil
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[2]
FIELDS={'output':0,'x':1,'y':2,'buttons':3,'reboot':4,'stopped':5,'kbd_queue':6,
        'mouse_queue':7,'uart_queue':8,'hid_queue':9,'direct':10,'peer':11,'activity':12,
        'gaming':13,'zoom':14,'modifiers':15,'peer_modifiers':16,'led':17,'fw_source':18,
        'fw_address':19,'fw_dirty':20,'blinks':21,'direct_valid':22,'peer_valid':23,'last_kick':24,
        'config_mode':50,'system_timeout':51,'ss_mode':30,'ss_idle':31,'ss_max':32,'ss_inactive':33,'ss_timeout':34,
        'zoom_debt':40,'zoom_overscroll':41,'zoom_pending':42,'zoom_direction':43,'zoom_deadline':44,
        'acceleration':35,'speed':36,'os':37,'led_indicator':38,
        'diagnostic_request_accepted':60,'diagnostic_poll_ready':61,'diagnostic_token':62,
        'diagnostic_outcome':63,'diagnostic_role':64,'diagnostic_major':65,'diagnostic_minor':66,
        'diagnostic_boot_session':67,'diagnostic_uptime_ms':68,'diagnostic_crc':69,'diagnostic_board_id':70}
KINDS={1:'usb',2:'uart_tx',3:'led',4:'watchdog',5:'reset',6:'yield',7:'erase',8:'program',9:'checkpoint',10:'wake',11:'wait_bound',
       12:'diagnostic_request',13:'diagnostic_result'}
CALLBACK=C.CFUNCTYPE(None,C.c_int,C.c_int,C.c_int,C.c_void_p,C.c_int)

class Simulation:
    def __init__(self, seed=1, library=None, quantum=250, background=True, core_order=None):
        self.seed=seed; self.quantum=quantum; self.background=background; self.core_order=core_order
        self.rng=random.Random(seed); self.now=0; self.serial=itertools.count()
        self.events=[]; self.trace=[]; self.steps=[]; self.active=[]; self.error=None
        self.link=[{'delay':0,'drop':0,'xor':0,'truncate':0,'duplicate':False} for _ in range(2)]
        self.pause_until={}; self.callbacks=[]; self.nodes=[]; self.task_ids=[]; self.schedule_count=0
        self.checkpoint_actions=[]
        self.tmp=tempfile.TemporaryDirectory(prefix='deskhop-sim-')
        library=pathlib.Path(library or ROOT/'build/tests/sim/node.so')
        self.library_sha256=hashlib.sha256(library.read_bytes()).hexdigest()
        for role in range(2):
            path=pathlib.Path(self.tmp.name)/f'node-{role}.so'; shutil.copyfile(library,path)
            lib=C.CDLL(str(path), mode=C.RTLD_LOCAL)
            signatures={
                'sim_init':([C.c_uint8,CALLBACK],None),'sim_destroy':([],None),
                'sim_set_time':([C.c_uint64],None),'sim_get':([C.c_int,C.c_int],C.c_int64),
                'sim_set':([C.c_int,C.c_int,C.c_int64],None),'sim_task':([C.c_int],None),
                'sim_core_step':([C.c_int],None),'sim_frequency':([C.c_int],C.c_uint64),'sim_rx_byte':([C.c_uint8],None),
                'sim_task_count':([],C.c_int),'sim_task_core':([C.c_int],C.c_int),
                'sim_task_name':([C.c_int],C.c_char_p),
                'sim_host':([C.c_int,C.c_int],None),'sim_endpoint':([C.c_int,C.c_int,C.c_int],None),
                'sim_mount':([C.c_uint8,C.c_uint8,C.c_uint8,C.c_void_p,C.c_uint16],None),
                'sim_report':([C.c_uint8,C.c_uint8,C.c_void_p,C.c_uint16],None),
                'sim_vendor':([C.c_void_p,C.c_uint16],None),'sim_unmount':([C.c_uint8,C.c_uint8],None),'sim_select':([C.c_uint8],None),
                'sim_led':([C.c_uint8],None),'sim_fill':([C.c_int,C.c_int],None),
                'sim_descriptor':([C.c_int,C.c_int,C.c_void_p],C.c_int),
                'sim_watchdog':([],None),
                'sim_uart_stall':([C.c_int],None),
                'sim_diagnostic_request':([C.c_uint32],None),'sim_diagnostic_poll':([],None),
            }
            for name,(args,ret) in signatures.items():
                f=getattr(lib,name); f.argtypes=args; f.restype=ret
            cb=CALLBACK(lambda kind,a,b,p,n,r=role:self._callback(r,kind,a,b,p,n))
            self.callbacks.append(cb); self.nodes.append(lib); lib.sim_init(role,cb)
            self.task_ids.append({lib.sim_task_name(index).decode('ascii'): index
                                  for index in range(lib.sim_task_count())})
        if background:
            for node in range(2):
                for core in range(2): self._schedule(0,node,core,'core',[core])
                self._schedule(1000,node,2,'watchdog',[])
    def close(self):
        for lib in self.nodes: lib.sim_destroy()
        # Unload explicitly: statics start from their C initializers next scenario.
        import _ctypes
        for lib in self.nodes: _ctypes.dlclose(lib._handle)
        self.nodes=[]; self.callbacks=[]; self.tmp.cleanup()
    def __enter__(self): return self
    def __exit__(self,*args): self.close()
    def _schedule(self,at,node,core,op,args):
        priority=self.rng.getrandbits(64)
        if self.core_order is not None:
            rank=self.core_order.index(node*2+core) if core<2 else -1
            priority+=rank*(1<<64)
        heapq.heappush(self.events,(at,priority,next(self.serial),node,core,op,args))
    def _time(self,at):
        self.now=max(self.now,at)
        for node in self.nodes: node.sim_set_time(self.now)
    def _callback(self,node,kind,a,b,p,n):
        # ctypes otherwise prints and discards exceptions raised in callbacks.
        try:
            if self.error:
                if kind==6:self._time(self.now+250)
                return
            self._handle_callback(node,kind,a,b,p,n)
        except BaseException as exc:
            self.error=f'{type(exc).__name__}: {exc}'
            if kind==6:self._time(self.now+250)
    def _handle_callback(self,node,kind,a,b,p,n):
        data=C.string_at(p,n).hex() if n else ''
        item={'at':self.now,'node':node,'core':self.active[-1][1] if self.active else None,'kind':KINDS[kind],'a':a,'b':b,'data':data}
        # Every visible output/fault is retained; idle polls need not fill traces.
        if kind!=9: self.trace.append(item)
        if kind==2:
            fault=self.link[node]; payload=bytearray.fromhex(data)
            if fault['drop']:
                fault['drop']-=1; self.trace.append(dict(item,kind='fault_drop')); return
            if fault['xor']:
                payload[3]^=fault['xor']; fault['xor']=0
            if fault['truncate']:
                payload=payload[:fault['truncate']]; fault['truncate']=0
            start=self.now+fault['delay']
            for idx,byte in enumerate(payload):
                # Integer ceiling of 8N1 wire serialization. DMA RX is independent.
                when=start+((idx+1)*10*1000000+3686399)//3686400
                self._schedule(when,1-node,2,'rx',[byte])
            if fault['duplicate']:
                for idx,byte in enumerate(payload): self._schedule(start+b+3+idx*3,1-node,2,'rx',[byte])
                fault['duplicate']=False
        elif kind==11:
            self.error='firmware blocking wait exceeded 2 seconds of virtual time'
        elif kind==6:
            if not self._one(until=self.now+250,blocked=set(self.active)):
                self._time(self.now+250)
        elif kind==9 and self.checkpoint_actions:
            target,op,args=self.checkpoint_actions.pop(0)
            self._invoke(target,op,args)
    def _one(self,until,blocked=frozenset()):
        held=[]; chosen=None
        while self.events and self.events[0][0]<=until:
            event=heapq.heappop(self.events)
            at,_,_,node,core,op,args=event
            if (node,core) in blocked: held.append(event); continue
            chosen=event; break
        for event in held: heapq.heappush(self.events,event)
        if chosen is None: return False
        at,_,_,node,core,op,args=chosen; self._time(at); self.schedule_count+=1
        if self.now < self.pause_until.get((node,core),0):
            self._schedule(self.pause_until[(node,core)],node,core,op,args); return True
        self.active.append((node,core))
        try:
            if op=='core':
                self.nodes[node].sim_core_step(core)
                self._schedule(self.now+self.quantum,node,core,op,args)
            elif op=='task':
                task=self.task_id(node,args[0])
                self.nodes[node].sim_task(task)
                freq=self.nodes[node].sim_frequency(task) or self.quantum
                self._schedule(self.now+freq,node,core,op,args)
            elif op=='rx':
                self.nodes[node].sim_rx_byte(args[0])
                self.trace.append({'at':self.now,'node':node,'core':2,'kind':'uart_rx','a':0,'b':0,'data':f'{args[0]:02x}'})
            elif op=='watchdog':
                self.nodes[node].sim_watchdog(); self._schedule(self.now+1000,node,core,op,args)
            else: self._invoke(node,op,args)
        finally: self.active.pop()
        if self.error: raise AssertionError(self.error)
        return True
    def advance(self,us,record=True):
        if record:self.steps.append({'op':'advance','args':[us]})
        end=self.now+us
        while self._one(end): pass
        self._time(end)
    def get(self,node,field,index=0): return self.nodes[node].sim_get(FIELDS[field],index)
    def reports(self,node=None,report_id=None):
        return [x for x in self.trace if x['kind']=='usb' and (node is None or x['node']==node)
                and (report_id is None or x['b']==report_id)]
    def descriptor(self,node,type,index=0):
        out=C.create_string_buffer(1024); n=self.nodes[node].sim_descriptor(type,index,out);return out.raw[:n]
    def task_id(self,node,task):
        """Resolve a stable task name, or validate an image-local numeric index."""
        if isinstance(task,str):return self.task_ids[node][task]
        if not isinstance(task,int) or not 0<=task<self.nodes[node].sim_task_count():
            raise ValueError(f'invalid task index: {task!r}')
        return task
    def _invoke(self,node,op,args):
        lib=self.nodes[node]
        if op in ('report','mount','vendor'):
            raw=bytes.fromhex(args[-1]); data=C.create_string_buffer(raw)
            getattr(lib,'sim_'+op)(*args[:-1],data,len(raw))
        elif op=='set':lib.sim_set(FIELDS[args[0]],args[1],args[2])
        elif op=='task':lib.sim_task(self.task_id(node,args[0]))
        elif op=='fault':self.link[node].update(args[0])
        elif op=='pause': self.pause_until[(node,args[0])]=self.now+args[1]
        elif op=='checkpoint':self.checkpoint_actions.append(tuple(args))
        elif op=='raw':
            for b in bytes.fromhex(args[0]):lib.sim_rx_byte(b)
        else:getattr(lib,'sim_'+op)(*args)
    def do(self,node,op,*args,record=True):
        if record:self.steps.append({'node':node,'op':op,'args':list(args)})
        # Peripheral input and UART handling run on core1; host SET_REPORT on core0.
        core=self.nodes[node].sim_task_core(self.task_id(node,args[0])) if op=='task' else (0 if op in ('host','led','endpoint','vendor','diagnostic_request','diagnostic_poll') else 1)
        self.active.append((node,core))
        try:self._invoke(node,op,list(args))
        finally:self.active.pop()
        if self.error: raise AssertionError(self.error)
    def expect(self,node,field,value,index=0):
        step={'op':'expect','node':node,'args':[field,index,value]}; self.steps.append(step)
        actual=self.get(node,field,index)
        assert actual==value,f'{field}[{index}] on {node}: expected {value}, got {actual} at {self.now} us'
    def expect_report(self,node,report_id,hexdata):
        self.steps.append({'op':'expect_report','node':node,'args':[report_id,hexdata]})
        reports=self.reports(node,report_id)
        assert reports and reports[-1]['data']==hexdata,f'host {node} ID {report_id}: expected {hexdata}, got {reports[-1:]}'
    def check(self,predicate,*args):
        self.steps.append({'op':'check','args':[predicate,*args]})
        if predicate=='key_absent':
            key,=args
            ok=all(key not in bytes.fromhex(r['data'])[2:] for r in self.reports(report_id=1))
        elif predicate=='range':
            node,field,index,lo,hi=args;ok=lo<=self.get(node,field,index)<=hi
        elif predicate=='time_range':
            lo,hi=args;ok=lo<=self.now<=hi
        elif predicate=='usb_count':
            node,report_id,count=args;ok=len(self.reports(node,report_id))==count
        elif predicate=='reset_count':
            node,reason,disable_mask,count=args
            ok=sum(x['kind']=='reset' and x['node']==node
                   and x['a']==reason and x['b']==disable_mask for x in self.trace)==count
        elif predicate=='usb_bytes':
            node,report_id,index,offset,hexdata=args
            reports=self.reports(node,report_id);expected=bytes.fromhex(hexdata)
            ok=(-len(reports)<=index<len(reports) and offset>=0
                and bytes.fromhex(reports[index]['data'])[offset:offset+len(expected)]==expected)
        else:raise ValueError('unknown predicate '+predicate)
        assert ok,f'predicate {predicate} failed: {args}'
    def save(self,path,reason=''):
        pathlib.Path(path).parent.mkdir(parents=True,exist_ok=True)
        pathlib.Path(path).write_text(json.dumps({'schema':1,'seed':self.seed,'quantum':self.quantum,
            'background':self.background,'core_order':self.core_order,'library_sha256':self.library_sha256,'steps':self.steps,'failure':reason,'trace':self.trace},indent=2)+'\n')
    def replay(self,steps):
        for s in steps:
            op=s['op']; args=s['args']
            if op=='advance':self.advance(*args)
            elif op=='expect':self.expect(s['node'],args[0],args[2],args[1])
            elif op=='expect_report':self.expect_report(s['node'],*args)
            elif op=='check':self.check(*args)
            else:self.do(s['node'],op,*args)
