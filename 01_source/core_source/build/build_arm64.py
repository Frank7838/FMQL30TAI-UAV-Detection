"""在配套容器中构建AArch64程序或MMIO库。"""
from pathlib import Path
import argparse
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'sha256:10eeb603dc9c9ab4e305f2f8a1c95c99cedaaaa2a219b721090f604e15da8333'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('component', choices=['npu', 'mmio'])
    args = parser.parse_args()
    output = ROOT/'build_output'/args.component
    output.mkdir(parents=True, exist_ok=False)
    base = ['docker', 'run', '--rm', '--pull=never', '--network=none', '--read-only',
            '--cpus=1', '--memory=2g', '--pids-limit=128', '--tmpfs', '/tmp:rw,size=256m',
            '--mount', f'type=bind,source={ROOT},target=/submission,readonly',
            '--mount', f'type=bind,source={output},target=/out', IMAGE]
    if args.component == 'npu':
        command = ['bash', '-lc',
                   'cmake -S /submission/src/npu -B /out -G Ninja -DCMAKE_BUILD_TYPE=Release '
                   '-DCMAKE_TOOLCHAIN_FILE=/submission/src/npu/aarch64-toolchain.cmake '
                   '&& cmake --build /out --parallel 1 '
                   '&& bash /submission/src/npu/audit_arm64.sh /out/product_stream']
    else:
        command = ['aarch64-linux-gnu-gcc', '-O2', '-shared', '-fPIC', '-Wall', '-Wextra',
                   '-Werror', '-Wl,--build-id=sha1', '/submission/src/board/native/mmio.c',
                   '-o', '/out/libswitch_mmio.so']
    with (output/'build.log').open('wb') as log:
        result = subprocess.run(base+command, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    (output/'BUILD_COMMAND.json').write_text(json.dumps(dict(image=IMAGE, command=base+command,
        exit_code=result.returncode, deployed=False), indent=2), encoding='utf-8')
    if result.returncode:
        raise SystemExit('Build failed; inspect build_output. Existing board was not modified.')
    print('Local build completed:', output)


if __name__ == '__main__':
    main()
