#!/usr/bin/env bash
# 编译并运行告警模块的仿真。
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$source_root"
output_root=build_output/alarm_sim
mkdir -p "$output_root"
iverilog -g2012 -s tb_fault_transactions -o "$output_root/transactions" -f build/alarm_sources.f tests/pl/tb_fault_transactions.sv
for test_case in {0..8}; do
  vvp "$output_root/transactions" "+CASE=$test_case"
done
iverilog -g2012 -s tb_autonomous_tick -o "$output_root/tick" -f build/alarm_sources.f tests/pl/tb_autonomous_tick.sv
vvp "$output_root/tick"

# 对照旧规则，同时核对四类事件的周期和帧号。
iverilog -g2012 -s tb_observability -o "$output_root/observer" -f build/alarm_sources.f tests/pl/baseline_region_alarm.v tests/pl/tb_observability.sv
vvp "$output_root/observer"
