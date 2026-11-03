#!/usr/bin/env bash
# 检查AArch64产物的架构、解释器和动态库依赖。
set -euo pipefail
binary=${1:-/workspace/output/build_arm64/drone_yolo11_arm64}
file "$binary"
aarch64-linux-gnu-readelf -h "$binary"
aarch64-linux-gnu-readelf -l "$binary" | grep 'interpreter'
declare -A checked
check_elf() {
    local path="$1" needed found directory
    if [[ -n "${checked[$path]:-}" ]]; then return; fi
    checked[$path]=1
    file -L "$path" | grep -q 'ARM aarch64'
    echo "CHECKED_ELF: $path"
    aarch64-linux-gnu-readelf --version-info "$path" \
        | awk '/Version needs section/{inside=1;next} inside{print}'
    while read -r needed; do
        found=''
        for directory in /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/aarch64-linux-gnu/lib; do
            if [[ -f "$directory/$needed" ]]; then found="$directory/$needed"; break; fi
        done
        if [[ -z "$found" ]]; then echo "MISSING: $needed" >&2; exit 1; fi
        echo "DEPENDENCY: $needed => $found"
        check_elf "$found"
    done < <(aarch64-linux-gnu-readelf -d "$path" | awk '/NEEDED/{gsub(/\[|\]/,"",$5); print $5}')
}
check_elf "$binary"
echo 'ALL_ARM64_DEPENDENCIES_RESOLVED_IN_CONTAINER'
