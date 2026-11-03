# IR保留扩展源码

此目录保留已完成IR部署实验的少量源码，独立于模型11的RGB产品链路。原图640×512，上下各补64行114至640×640；六头解码和0.25/0.7阈值保持原实验设置。

|位置|内容|
|---|---|
|IR_stream_NPU_m840|真实NPU持久推理、CPU六头解码和离线解码回放|
|IR_probe_m834|早期单图NPU入口；流式程序复用其boot/设备锁guard|
|IR_stream_m837|持久Host量化仿真，供部署一致性对照|
|host_IR_m824|早期逐图Host原始头导出|
|fixed_image_worker.py|608张IR开发图像轮次使用的有限实验驱动，仅保留查阅|

CMake入口保留原Icraft依赖。编译需要已有AArch64工具链和专有SDK；阅读或普通校验不会运行这些程序。NPU入口需要当前boot、已持有的真实设备锁和冻结模型/输入合同，历史驱动不能代替产品run_camera.py。

IR权重、校准清单与原始结果仍位于原项目deliveries/model_rc1_lowlight_IR_20261020_v1；提交包中的结果说明位于05_evidence/ir_board_result.json，数据来源见03_model/DATASET_SOURCES.md。该扩展未接入真实IR相机或RGB告警主链路。
