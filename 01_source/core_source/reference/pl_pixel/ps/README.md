# FPGA预处理的CPU侧代码

hotpath保存同输入、同ARM核的CPU/FPGA耗时比较程序；functional保存模式A单帧、模式B连续8帧的功能比较程序。两套cached_mmio接口对应各自实验版本，分别保留。

cached_lease负责连续缓冲区、映射和缓存交接；cached_mmio负责预处理寄存器与帧提交；cpu_reference完成固定取样、颜色转换和补边，供功能核对及CPU计时使用。输入是当时生成的合成BGR图像，数据本身没有放入源码包。

在匹配环境的AArch64 Linux中，进入hotpath或functional目录编译：

```sh
gcc -std=gnu11 -O2 -Wall -Wextra -c cached_lease.c cached_mmio.c
g++ -std=gnu++17 -O3 -march=armv8-a -Wall -Wextra paired_cost.cpp cached_lease.o cached_mmio.o -o paired_cost_arm64
```

functional目录把最后一行的paired_cost.cpp替换为functional_followup.cpp。硬件运行需要对应位流、u-dma-buf图像缓冲驱动及其/dev/u-dma-buf-mgr接口、设备锁和配置；这些程序与日常相机检测互斥。

原始输入、输出与执行记录分别保存在pc_runs/pl_acceleration_delegate_20261023_v1和pc_runs/boot24_v2下。测得的640×480平均耗时为CPU 0.648 ms、FPGA全通路11.243 ms；成品采用CPU预处理。
