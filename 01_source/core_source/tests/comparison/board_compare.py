"""向CPU与FPGA提交相同输入并比较状态及耗时。"""
from pathlib import Path
import ctypes as C
import fcntl
import hashlib
import json
import os
import select
import signal
import struct
import subprocess
import time
import traceback

W=Path(__file__).resolve().parent
P=Path('/root/fpga_gaming/model11_standalone_20261025_v1')
PLAN=json.loads((W/'PLAN.json').read_text())
OFFSETS=['192','32','36','40','44','48','52']
REPLY=struct.Struct('<8I6Q')
REQUEST=struct.Struct('<68I')


def save(name,value):
    (W/name).write_text(json.dumps(value,indent=2)+'\n')


def svc():
    return dict(s.split('=',1) for s in subprocess.check_output(
        ['systemctl','show','model11-detect.service','-p','MainPID','-p','ActiveState','-p','UnitFileState'],text=True).splitlines())


def main():
    assert not (W/'RESULT.json').exists(),'Preserve prior execution'
    assert Path('/proc/sys/kernel/random/boot_id').read_text().strip()==PLAN['boot']
    assert svc()['MainPID']=='0' and svc()['ActiveState']=='inactive'
    assert not (P/'UNKNOWN.json').exists()
    assert hashlib.sha256((P/'libswitch_mmio.so').read_bytes()).hexdigest()==PLAN['driver_SHA256']
    lease=os.open(str(P/'launcher.lock'),os.O_RDWR|os.O_NOFOLLOW)
    fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
    fd=os.open('/root/fpga_gaming/ps_session_m53.lock',os.O_RDWR|os.O_NOFOLLOW)
    fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
    assert os.fstat(fd).st_ino==943514
    lib=C.CDLL(str(W/'paired.so'))
    lib.paired_open.argtypes=[C.c_char_p,C.c_int]
    lib.paired_submit.argtypes=[C.c_uint32,C.c_void_p,C.c_uint32,C.c_void_p,C.c_void_p]
    lib.paired_snapshot.argtypes=[C.c_void_p]
    hw_open=False;cpu=None;paused=False;rows=[];gaps=[];error=None;clean=False
    started=time.monotonic();cpu_calls=0;submissions=0;fid=0
    rowfile=(W/'PAIRED_ROWS.jsonl').open('w')
    gapfile=(W/'GAP_SAMPLES.jsonl').open('w')
    def sample():
        a=(C.c_uint32*7)();t0=time.monotonic_ns()
        assert lib.paired_snapshot(a)==0
        return dict(zip(OFFSETS,a)),t0,time.monotonic_ns()
    def command(op=2,frame=0,boxes=(),fresh=1,count=None):
        nonlocal cpu_calls
        assert time.monotonic()-started<PLAN['max_seconds']
        values=[op,frame,fresh,len(boxes) if count is None else count]
        values.extend(x for b in boxes for x in b);values.extend([0]*(68-len(values)))
        t0=time.monotonic_ns();cpu.stdin.write(REQUEST.pack(*values));cpu.stdin.flush()
        assert select.select([cpu.stdout],[],[],2)[0],'CPU participant response timeout'
        raw=cpu.stdout.read(80);assert len(raw)==80
        t1=time.monotonic_ns();x=REPLY.unpack(raw);cpu_calls+=1
        return dict(zip(['status','last','accepted','rejected','events','protocol','sequence','reserved',
                         'compute_ns','now_ns','last_accept_ns','fault_ns','alarm_ns','clear_ns'],x)),t1-t0
    def sync():
        s,_,_=sample()
        command(3,s['36'],[(s['44'],0,0,0)],s['32'],s['40'])
    def send(boxes,label,index):
        nonlocal fid,submissions
        assert submissions<PLAN['max_commits']
        fid=(fid+1)&0xffffffff
        # Alternate transport order to avoid always giving one participant an
        # earlier dispatch. Algorithm timers exclude Python/pipe dispatch.
        def pl():
            a=(C.c_uint32*7)();b=(C.c_uint32*(4*len(boxes)))(*(v for z in boxes for v in z))
            ns=C.c_uint64();t0=time.monotonic_ns()
            assert lib.paired_submit(fid,b,len(boxes),a,C.byref(ns))==0
            return dict(zip(OFFSETS,a)),ns.value,t0,time.monotonic_ns()
        t0=time.monotonic_ns()
        if index%2:
            state,hw_ns,p0,p1=pl();cs,ipc=command(1,fid,boxes)
        else:
            cs,ipc=command(1,fid,boxes);state,hw_ns,p0,p1=pl()
        # bit12 is uncommitted transaction shadow in RTL. Compare it here only
        # immediately after a successful complete frame; not in timeout polls.
        assert state['32']==cs['status'],(state,cs)
        assert state['36']==cs['last'] and state['40']==cs['accepted']
        assert state['44']==cs['rejected'] and state['48']==0
        assert state['52']==cs['events'],(state,cs)
        row=dict(label=label,index=index,frame=fid,boxes=boxes,cpu=cs,PL=state,
                 cpu_IPC_roundtrip_ns=ipc,PL_guarded_path_ns=hw_ns,
                 PL_call_begin_ns=p0,PL_call_end_ns=p1,dispatch_begin_ns=t0,
                 dispatch_end_ns=time.monotonic_ns(),order='PL_first' if index%2 else 'CPU_first',exact=True)
        rowfile.write(json.dumps(row)+'\n');rows.append(row);submissions+=1
        return row
    def gap(alarm,stop_cpu):
        nonlocal paused
        label=('cpu_process_paused' if stop_cpu else 'watchdog_running')+'_alarm'+str(alarm)
        for i in range(3 if alarm else 8):
            r=send([[25000,25000,35000,35000]] if alarm else [],'gap_prepare',len(rows))
            time.sleep(.004)
        assert bool(r['PL']['32']&1)==bool(alarm)
        last_pl_interval=[r['PL_call_begin_ns'],r['PL_call_end_ns']]
        last_cpu_accept=r['cpu']['last_accept_ns']
        last_nonfault=None;first_fault=None;samples=0
        if stop_cpu:
            os.kill(cpu.pid,signal.SIGSTOP);paused=True
            for _ in range(20):
                if Path('/proc/'+str(cpu.pid)+'/status').read_text().split('State:')[1].lstrip().startswith('T'):break
                time.sleep(.001)
            else:raise RuntimeError('Known CPU participant did not enter stopped state')
        # Leave the independent C watchdog running for normal input-loss cases.
        # 暂停对照进程，观察FPGA的独立断流计时。
        until=r['PL_call_end_ns']+int(PLAN['gap_seconds']*1e9)
        while time.monotonic_ns()<until:
            state,p0,p1=sample();cr=None
            if not stop_cpu:cr,_=command()
            assert bool(state['32']&1)==bool(alarm),'Alarm changed during input loss'
            if state['32']&2:
                if first_fault is None:first_fault=[p0,p1]
            else:last_nonfault=[p0,p1]
            if cr:assert bool(cr['status']&1)==bool(alarm)
            gapfile.write(json.dumps(dict(label=label,PL=state,begin_ns=p0,end_ns=p1,cpu=cr))+'\n')
            samples+=1;time.sleep(.001)
        assert first_fault is not None
        unavailable=None
        if stop_cpu:
            # No request can complete while the whole alarm process is stopped.
            cpu.stdin.write(REQUEST.pack(2,0,1,0,*([0]*64)));cpu.stdin.flush()
            unavailable=not bool(select.select([cpu.stdout],[],[],.02)[0]);assert unavailable
            resume_ns=time.monotonic_ns();os.kill(cpu.pid,signal.SIGCONT);paused=False
            assert select.select([cpu.stdout],[],[],2)[0]
            cr=dict(zip(['status','last','accepted','rejected','events','protocol','sequence','reserved',
                         'compute_ns','now_ns','last_accept_ns','fault_ns','alarm_ns','clear_ns'],REPLY.unpack(cpu.stdout.read(80))))
        else:resume_ns=None;cr,_=command()
        assert cr['status']&2 and bool(cr['status']&1)==bool(alarm)
        result=dict(label=label,alarm_held=alarm,PL_last_accept_interval_ns=last_pl_interval,
                    cpu_last_accept_ns=last_cpu_accept,PL_last_nonfault_sample=last_nonfault,
                    PL_first_fault_sample=first_fault,cpu_fault_ns=cr['fault_ns'],
                    CPU_resume_ns=resume_ns,CPU_unavailable_while_stopped=unavailable,samples=samples)
        recovery=send([[25000,25000,35000,35000]] if alarm else [],'gap_recovery',len(rows))
        assert not (recovery['PL']['32']&2) and not (recovery['cpu']['status']&2)
        result['recovered']=True;gaps.append(result)
    try:
        assert lib.paired_open(str(P/'libswitch_mmio.so').encode(),fd)==0;hw_open=True
        initial,_,_=sample();assert initial['192']==0xA5050001
        assert not initial['32']&1,'Preserve inherited alarm'
        assert initial['44']==initial['48']==0
        fid=(initial['36']+1)&0xffffffff
        cpu=subprocess.Popen([str(W/'cpu_baseline')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                             stderr=(W/'CPU.stderr').open('w'),bufsize=0)
        save('STARTED.json',dict(boot=PLAN['boot'],PID=os.getpid(),CPU_participant_PID=cpu.pid,initial=initial))
        sync()
        # Throughput-independent overhead, common Q16 inputs and alternating order.
        for n in [0,1,4,16]:
            for i in range(128):
                b=[[100,100,500,500] for _ in range(n)]
                if n and i%3:b[-1]=[25000,25000,35000,35000]
                if n and i%7==0:b[0]=[8000,5000,8384,6000] # center exactly left ROI edge
                if n and i%11==0:b[-1]=[57000,5000,57688,6000] # center exactly excluded right edge
                send(b,'overhead_'+str(n),len(rows))
        start=time.monotonic_ns();target=start
        for i in range(220):
            target+=int((1/28+[0,-.004,.006,0][i%4])*1e9)
            delay=(target-time.monotonic_ns())/1e9
            if delay>0:time.sleep(delay)
            b=[[25000,25000,35000,35000]] if i%11<3 else []
            r=send(b,'paced_28Hz_with_fixed_jitter',len(rows));r['target_ns']=target
            # Persist scheduling target separately without rewriting source rows.
            gapfile.write(json.dumps(dict(label='dispatch_target',frame=r['frame'],target_ns=target))+'\n')
        for stop_cpu in [False,True]:
            for alarm in [0,1]:gap(alarm,stop_cpu)
        for i in range(8):send([],'normal_cleanup',len(rows))
        final,_,_=sample();assert final['32']&3==0;clean=True
        save('FINAL_REGISTERS.json',final)
    except Exception:error=traceback.format_exc()
    finally:
        if cpu:
            if paused:os.kill(cpu.pid,signal.SIGCONT)
            if cpu.poll() is None:
                try:cpu.stdin.write(REQUEST.pack(9,0,1,0,*([0]*64)));cpu.stdin.flush();cpu.wait(timeout=3)
                except Exception:pass
        if hw_open:lib.paired_close()
        os.close(fd);os.close(lease);rowfile.close();gapfile.close()
        save('RESULT.json',dict(completed=error is None,error=error,boot=PLAN['boot'],
             elapsed_s=time.monotonic()-started,paired_commits=submissions,all_rows_exact=all(r['exact'] for r in rows),
             gaps=gaps,cleanup_alarm_fault_zero=clean,CPU_exit=cpu.poll() if cpu else None,
             SDK_loads=0,NPU_calls=0,business_resets=0,BOOT_writes=0,service=svc(),
             response_scope='Software state/readback observations; no physical LED edge timestamp',
             hardware_timestamp_available=False,independent_accuracy=False))
    print((W/'RESULT.json').read_text(),flush=True)
    return 0 if error is None else 1


if __name__=='__main__':raise SystemExit(main())
