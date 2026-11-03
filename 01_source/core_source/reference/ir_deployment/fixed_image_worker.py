"""读取IR固定图像，调用模型并保存检测结果。"""
import base64, fcntl, hashlib, json, mmap, os, queue, shutil, struct, subprocess, sys, threading, time, zlib
from pathlib import Path

def need(ok, why):
    if not ok:
        raise RuntimeError(why)

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''):
            h.update(b)
    return h.hexdigest()

def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)

def exact(fd, n):
    data = bytearray()
    while len(data) < n:
        b = os.read(fd, n - len(data))
        need(bool(b), 'NPU result pipe closed')
        data.extend(b)
    return bytes(data)

def main():
    root = Path(__file__).resolve().parent
    c = json.loads(zlib.decompress((root / 'contract.zlib').read_bytes()))
    need(digest(__file__) == c['worker_SHA'] and c['schema'] == 'IR_full_m841', 'Worker/contract changed')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    need(boot == c['restored_boot'], 'Restored boot changed')
    lock = Path('/root/fpga_gaming/ps_session_m53.lock')
    fd = os.open(str(lock), os.O_RDWR | os.O_NOFOLLOW)
    child = None
    try:
        need(os.fstat(fd).st_ino == lock.stat().st_ino == 943514, 'Real lock changed')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        need(shutil.disk_usage(root).free >= (1 << 30) + (64 << 20), 'Disk floor rejected')
        mem = int(next((l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:'))))
        need(mem >= 131072, 'Memory floor rejected')
        for path, value in c['protected_files'].items():
            need(digest(path) == value, 'Protected SHA differs')
        for path, value in c['model_files'].items():
            need(digest(path) == value, 'Model SHA differs')
        need(digest(root / 'IR_NPU_stream') == c['binary_SHA'], 'Executable differs')
        physical = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
        try:
            with mmap.mmap(physical, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=1074528256) as v:
                signature = struct.unpack_from('<I', v, 192)[0]
        finally:
            os.close(physical)
        need(signature == 2768568321, 'M512 absent')
        binding = dict(boot_id=boot, lock_inode=943514, M512_signature=hex(signature), scope='Frozen40 decoder verification, then all1180 old DEV if decoder alignment passes', NPU_image_budget=c['max_NPU_images'], qualified_monotonic=time.monotonic(), formal_promotion=False, contract_SHA=digest(root / 'contract.zlib'), protected_SHA_count=len(c['protected_files']))
        with (root / 'binding.json').open('x') as f:
            json.dump(binding, f, indent=2)
        with (root / 'running.NPU_m841').open('x') as f:
            json.dump(dict(boot_id=boot, pid=os.getpid()), f)
        emit(dict(type='ready', binding=binding, disk_free_bytes=shutil.disk_usage(root).free, MemAvailable_KiB=mem))
        total = 0
        start = time.monotonic()
        env = dict(os.environ, M834_EXPECTED_BOOT=boot, M834_GUARD_FD=str(fd))
        env.update(c['runtime_env'])
        for phase in ('smoke', 'full'):
            if phase == 'full':
                need(json.loads(sys.stdin.readline()) == dict(continue_full=True), 'Full-stage permission absent')
            output_read, output_write = os.pipe()
            events = queue.Queue()
            results = queue.Queue()
            child = subprocess.Popen([str(root / 'IR_NPU_stream'), c['model_json'], c['model_raw'], str(output_write)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(fd, output_write), env=env, cwd=str(root))
            os.close(output_write)

            def stdout_reader(proc, q):
                for line in iter(proc.stdout.readline, b''):
                    if b'M840_READY' in line:
                        q.put('ready')
                q.put('EOF')

            def stderr_reader(proc, phase_name):
                with (root / (phase_name + '.stderr')).open('xb') as log:
                    for block in iter(lambda: proc.stderr.read(4096), b''):
                        log.write(block)
                        log.flush()

            def result_reader(result_fd, q):
                try:
                    while True:
                        size = struct.unpack('<I', exact(result_fd, 4))[0]
                        need(0 < size < 1048576, 'Result size exceeds budget')
                        q.put(json.loads(exact(result_fd, size)))
                except Exception as error:
                    q.put(dict(error=str(error)))
                finally:
                    os.close(result_fd)
            threading.Thread(target=stdout_reader, args=(child, events), daemon=True).start()
            threading.Thread(target=stderr_reader, args=(child, phase), daemon=True).start()
            threading.Thread(target=result_reader, args=(output_read, results), daemon=True).start()
            need(events.get(timeout=60) == 'ready', 'Persistent NPU startup failed')
            for index, expected in enumerate(c['inputs'][phase]):
                need(time.monotonic() - start < c['wall_seconds'], 'Overall inference budget exceeded')
                request = json.loads(sys.stdin.readline())
                need(request['id'] == expected['id'], 'Input order differs')
                data = base64.b64decode(request['data'])
                need(len(data) == 1228800 and hashlib.sha256(data).hexdigest() == expected['SHA'], 'Input tensor differs')
                child.stdin.write(struct.pack('<I', index) + data)
                child.stdin.flush()
                value = results.get(timeout=30)
                need(value.get('index') == index, 'NPU stream failed: ' + str(value))
                emit(dict(type='frame', phase=phase, id=expected['id'], result=value))
                total += 1
            child.stdin.write(struct.pack('<I', 4294967295))
            child.stdin.flush()
            child.stdin.close()
            need(child.wait(timeout=15) == 0, 'NPU child did not complete')
            child = None
            emit(dict(type='phase_complete', phase=phase, images=len(c['inputs'][phase])))
        completed = dict(actual_NPU_images=total, wall_s=time.monotonic() - start, disk_free_after=shutil.disk_usage(root).free, formal_promotion=False)
        with (root / 'completed.json').open('x') as f:
            json.dump(completed, f, indent=2)
        (root / 'running.NPU_m841').rename(root / 'completed.NPU_m841')
        emit(dict(type='complete', result=completed))
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=10)
        os.close(fd)
if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        emit(dict(type='failed', error=str(error), result_is_unknown=True))
        raise
