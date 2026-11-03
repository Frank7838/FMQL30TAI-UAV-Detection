# 无人机检测系统核心源码

这份源码对应模型11、CPU预处理、NPU检测和FPGA告警这条主链。电脑端是WinForms程序，通过网络查看画面，通过SSH启动或停止检测。

先看[代码导读](docs/代码导读.md)。如果只想了解核心工作，按下面的顺序读就够了。

|文件|主要做什么|
|---|---|
|src/board/runtime/worker.py|串起采集、推理、FPGA提交和画面发布|
|src/npu/product_stream.cpp|常驻NPU进程，处理图像输入、模型调用和结果回包|
|src/npu/FusedStreamPicPre.hpp|把颜色转换和补边放在一起，减少图像复制|
|src/pl/alarm/region_alarm_regs_m505.v|接收一帧检测框，完成区域判断和寄存器交接|
|src/pl/alarm/region_alarm_core.v|连续命中确认、告警解除和输入超时处理|
|src/pl/alarm/alarm_observer.v|记录置警、清警、断流、恢复的周期数和帧号|
|src/board/runtime/observability.py|从现有MMIO会话读取硬件事件记录|
|src/desktop/Desktop.cs|画面、运行状态、告警记录和操作界面|

src/board还包含相机、跟踪、有界日志、显示编码、MMIO库和自启服务。src/pl/integration是接入厂商工程的顶层代码，constraints保留对应约束。reference中的IR部署、FPGA像素预处理和模型转换是扩展及部署参考，当前RGB运行程序不调用IR和像素预处理分支。

构建方法见[构建与依赖](docs/构建与依赖.md)，硬件接口见[FPGA接口与规则](docs/PL接口与规则.md)。tests保留告警仿真和CPU/FPGA比较程序。reference/model11_training保留训练、样本整理与开发集计分代码。逐个文件的用途见[文件索引](docs/文件索引.md)。

本目录保留详细的流程、单位、接口和资源释放注释。02_runtime保存可运行版本，重新构建时使用这里的源码和构建入口。数据集不放在仓库内，实际来源和使用数量见材料根目录03_model/DATASET_SOURCES.md。第三方头文件及其版权声明放在third_party。

2026年11月1日更新：int8.yaml启用ZG330连续算子合并，减少SDK逐层调度开销；policy.py在Q16转换时处理float32边缘框的舍入误差。这项软件对照没有改模型、阈值、3/8规则或FPGA位流。当晚另完成故障灯及硬件事件版本的上板验收，相关源码和测试已合入。对应实测见../../05_evidence/sdk_scheduling_result.json。

当前完整版本相机连续运行3600.122秒，107396次推理、107396次有效FPGA提交，整段平均29.831 FPS，未知0，SDK正常关闭。提交副本移除了旧查看网页；只读API仍由viewer.py提供。提交副本已更新运行校验清单，尚未重新上板验证。
