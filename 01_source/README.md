# 全部代码材料阅读说明

这份材料汇集当前成品的项目源码、构建入口和必要测试，同时保留模型训练与转换、IR部署和FPGA像素预处理的参考代码。主运行路径固定为CPU预处理、NPU模型11检测、FPGA告警；参考目录的代码不代表全部已经启用在当前产品中。

## 先读哪些文件

建议按一帧数据经过的顺序阅读：worker.py串联采集、推理与告警；product_stream.cpp管理常驻NPU调用；policy.py定义Q16框与CPU参考状态；region_alarm_regs_m505.v接收完整帧事务；region_alarm_core.v维护告警；alarm_observer.v记录四类事件。最后看Desktop.cs和viewer.py理解结果如何显示。

| 路径 相对core_source | 用途 |
| --- | --- |
| src/board/runtime | 相机主循环、服务入口、预览录像、状态、锁和会话管理 |
| src/npu | 模型装载、输入适配、SDK调用及结果回包 |
| src/pl/alarm | 帧事务、ROI、连续确认、断流与事件观察 |
| src/pl/integration 和 constraints | 接入厂商父工程的顶层、接线与约束 |
| src/desktop | C# WinForms界面、认证控制组件和配置示例 |
| tests | 告警、事件和状态对照；硬件测试入口须显式执行 |
| build | ARM64、Windows及仿真构建入口，硬件覆盖脚本 |
| reference | 模型11训练转换、IR与像素预处理参考 |
| third_party | 必要厂商辅助头文件及原许可 |

## 从启动到退出

run_camera.py由systemd持有，校验运行文件并等待相机，创建带boot的会话；worker.py接管当前设备锁并建立常驻推理连接。每帧检查输入与结果，将完整框事务交给FPGA，再比较CPU参考与硬件读回。预览只保留最新帧，编码与日志均有界。

停止请求由既有服务入口处理，等待当前推理完成并确认SDK关闭后，再结束编码和子进程、释放设备。UNKNOWN或关闭不明时保留所有者与证据，不能把强制结束进程当成正常退出。observability.py沿用主循环持有的设备会话进行有界只读采样，不另开设备，也不改变告警状态。

## 构建与验证

源码构建的完整参数见docs/构建与依赖.md。NPU和MMIO构建入口为build/build_arm64.py；CPU图像及编码库见build/build_native_linux.sh；Windows界面见build/build_desktop.ps1。Python运行依赖NumPy和OpenCV，NPU构建另需Icraft 3.39、ZG330Backend、AArch64工具链及配套C++依赖。

build/alarm_sources.f和build/run_alarm_sim.sh用于告警仿真。tests/pl/tb_observability.sv把超时缩短以覆盖事件转换，并将新旧告警输出并行比较；产品仍使用1000ms超时。tests/board/qualify_observer.py会提交合成框并影响告警，仅供有明确执行条件的板端验收，不能随自启服务运行。

本次收尾只整理说明和注释，已核对Python语法树及RTL去注释内容保持一致，没有重建或重新测试硬件。对应实板功能与一小时运行结果在完整GitHub材料的05_evidence中，代码说明本身不替代实测证据。

## 厂商与第三方依赖

硬件重建需准备匹配厂商父工程、Vivado 2018.3、Procise 2025.1.1及器件工具补丁，再按build/fpga/apply_observer_overlay.tcl接入项目源码。代码材料保留项目接线与约束，不附完整厂商大工程、系统镜像或SDK安装包。

third_party中的厂商辅助代码与根目录第三方许可说明保留来源。模型沿用Ultralytics YOLO11，数据来自公开作者；训练图像、标签全集、模型运行库、个人凭据不属于本代码目录。所有数据链接和许可范围应结合完整GitHub材料中的来源说明阅读。

## 如何查具体实现

docs/代码导读.md按模块说明主运行路径，docs/文件索引.md列出逐文件用途，docs/PL接口与规则.md说明寄存器和事务。完整材料里的代码辅助阅读.html支持按模块和文件搜索。本目录保留展开的完整代码材料，便于逐文件审核。

[代码材料阅读说明](全部代码材料阅读说明.pdf) · [核心源码导读](core_source/docs/代码导读.md) · [逐文件用途](core_source/docs/文件索引.md)

GitHub审核副本清除了上位机源码中的设备专用默认指纹，并重新构建Windows上位机；指纹校验机制保持启用。控制组件已通过离线自检，重新构建的上位机尚未进行实际连板验收。板端运行文件未改动。
