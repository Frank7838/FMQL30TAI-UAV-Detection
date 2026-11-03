"""在有限相机测试正常结束后，对四类事件做一次定向检查。"""
from pathlib import Path
import argparse,json,time,os,sys,fcntl,ctypes
APP=Path('/root/fpga_gaming/model11_standalone_20261025_v1')
sys.path.insert(0,str(APP))
from worker import Hardware
from observability import read_observability

# 本程序会提交合成框并触发告警/超时，不是只读查看入口，也不应随服务自启。
# 必须先确认实际相机任务已正常关闭SDK，再由操作者显式提供boot与一次执行参数。
# 计时是有界验证：保留每种事件的读回值，失败不自动重跑，不修改业务规则。
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--expected-boot',required=True)
    parser.add_argument('--execute-once',action='store_true')
    args=parser.parse_args()
    assert args.execute_once
    assert Path('/proc/sys/kernel/random/boot_id').read_text().strip()==args.expected_boot
    assert not (APP/'UNKNOWN.json').exists()
    active=json.loads((APP/'ACTIVE_RUN.json').read_text())
    result=json.loads((APP/'sessions'/active['session']/'result.json').read_text())
    assert result['device_closed'] and result['completed'] and result['boot']==args.expected_boot
    out=Path(__file__).resolve().parent/'DIRECTED_RESULT.json'
    assert not out.exists(), 'Preserve the preceding attempt'
    started_file=out.with_name('DIRECTED_STARTED.json')
    assert not started_file.exists(), 'Inspect the preceding attempt before doing more'
    lock=os.open('/root/fpga_gaming/ps_session_m53.lock',os.O_RDWR|os.O_NOFOLLOW)
    assert os.fstat(lock).st_ino==943514
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    with started_file.open('x') as record:
        record.write(json.dumps(dict(boot=args.expected_boot,register_writes_max=100,seconds_max=15))+'\n')
    hw=Hardware(lock)
    started=time.monotonic();rows=[]
    try:
        def state(label):
            snap=hw.snapshot();obs=read_observability(hw.read)
            assert obs['supported'] and obs['ready'] and obs['clock_hz']==100_000_000
            assert all(event['cycles']<=obs['now_cycles'] for event in obs['events'].values())
            row=dict(label=label,at_s=time.monotonic()-started,PL=snap,hardware_observability=obs)
            rows.append(row);return row
        initial=state('initial')
        assert initial['PL']['44']==initial['PL']['48']==0
        fid=(initial['PL']['36']+2)&0xffffffff
        def submit(boxes):
            nonlocal fid
            assert time.monotonic()-started<15
            reply=hw.commit(fid,boxes);assert reply['exact']
            sent=fid;fid=(fid+1)&0xffffffff
            return sent
        # Clear an inherited target alarm through normal valid empty frames.
        for _ in range(8):submit([])
        empty=state('cleared_by_empty_frames')
        assert not empty['PL']['32']&1
        positive=[[20000,14000,30000,24000]]
        for _ in range(3):assert_frame=submit(positive)
        asserted=state('alarm_assert')
        assert asserted['PL']['32']&1 and not asserted['PL']['32']&2
        assert_event=asserted['hardware_observability']['events']['alarm_assert']
        assert assert_event['frame']==assert_frame
        time.sleep(1.25)
        fault=state('no_submit_fault')
        assert fault['PL']['32']&3==3
        fault_event=fault['hardware_observability']['events']['stream_fault']
        assert fault_event['frame']==assert_frame
        delay=(fault_event['cycles']-assert_event['cycles'])/100_000_000
        assert .99<=delay<=1.02,delay
        time.sleep(.75)  # 故障灯保持一小段时间，便于现场观察。
        restored_frame=submit(positive)
        restored=state('result_stream_recovered')
        assert restored['PL']['32']&1 and not restored['PL']['32']&2
        recover_event=restored['hardware_observability']['events']['stream_recover']
        assert recover_event['frame']==restored_frame
        time.sleep(.5)
        for _ in range(8):clear_frame=submit([])
        cleared=state('alarm_clear')
        assert not cleared['PL']['32']&3
        clear_event=cleared['hardware_observability']['events']['alarm_clear']
        assert clear_event['frame']==clear_frame
        assert assert_event['cycles']<fault_event['cycles']<recover_event['cycles']<clear_event['cycles']
        assert hw.writes<=100
        report=dict(passed=True,boot=args.expected_boot,steps=rows,register_writes=hw.writes,SDK_loads=0,NPU_calls=0,business_reset_writes=0,timeout_from_last_positive_s=delay,elapsed_s=time.monotonic()-started,physical_LED_verified=False)
        out.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report))
    finally:
        hw.lib.switch_close();os.close(lock)

if __name__=='__main__':main()
