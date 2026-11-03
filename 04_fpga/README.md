# FPGA部分

current_BOOT.bin和current_converted.bit对应11月1日已上板的故障灯、硬件事件记录版本。

当前FPGA完成框中心的区域判断、3/8连续确认、结果超时监测、告警保持，以及LED1故障和LED2目标告警输出。新增的观察模块用100MHz时钟记录置警、清警、断流、恢复的最近一次周期数和帧号。画框、轨迹和录像由CPU完成。

RTL、顶层接线和约束见01_source/core_source/src/pl。告警、事件记录及时基合计270 LUT、830 FF，不使用DSP或BRAM；路由报告在reports/observer_20261101。父工程仍有原来的最小周期及厂商DRC限制，详见05_evidence/实测结果.md。

新版本通过人工冷启动和90秒相机验收：2647次推理及提交，逐帧CPU/FPGA状态一致，未知0，SDK正常关闭。四类事件定向测试通过。LED1及LED2的故障与告警状态已完成现场观察。

完整厂商父工程和器件工具补丁保留在本机pc_runs/submission_cleanup_20261031_v1/local_fpga_assets/04_fpga/parent_project_sources.zip。它包含上一版工程，重建当前版时应用build/fpga/apply_observer_overlay.tcl，把提交源码覆盖到该工程，并加入alarm_observer.v；无需复制整套厂商工程到Git。

重建需要匹配的厂商工程、Vivado 2018.3和Procise 2025.1.1环境。build/fpga保留转换及启动镜像模板。部署使用current_BOOT.bin，只替换匹配启动卡上的BOOT.bin，保留Linux、uEnv、其他启动文件和分区。

当前完整版本相机连续运行3600.122秒，107396次推理、107396次有效FPGA提交，整段平均29.831 FPS，未知0，SDK正常关闭。硬件功能和资源不变，长测摘要见05_evidence/camera_1h_final_result.json。
