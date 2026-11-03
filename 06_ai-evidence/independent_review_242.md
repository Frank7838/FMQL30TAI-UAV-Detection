# 独立复核意见单：P1构建、时序例外、告警差异与上板方案

复核日期：2026-10-17。对象：`deliveries/opus_progress_20261017/PROGRESS.md`（12:49:59版）及其链接原件。接续[[240-独立复核意见单_冷启动性能回退与PL最小路线_20261016]]、[[241-PL端意见补充_依赛题原文_20261016]]。研发助手为GPT。

## 0. 本次实际做了什么

- 读取：`P1_ddr_copy_v1/`下`p1_cdc_guard.tcl`及其上一版的diff、`p1_opt_post_cdc_guard.tsv`、`p1_exact_scope_preflight_verified.json`、`p1_opt_post_chain.tcl`、`plan.json`、`P1_INTERFACE.json`、`result.json`、`audit_routed.tcl`、`audit_check_timing.rpt`、`audit_route_status.rpt`、`audit_cdc_full.rpt`（汇总表＋全部13行p1_subsystem）、`business_delta.tcl`、`business_delta_cone.tcl`、`business_cone_baseline.tsv`、`business_cone_p1_routed.tsv`、`business_delta_diagnostic.json`（routed部分）；`project/constrs_1/ai7030.xdc`；`ps_ai.bd`中HP2的接口网与地址段；RTL `p1_gp_control.sv`（20—100行）、`p1_cdc.xdc`、`p1_axi3_owner_arbiter.sv`（相关行）、`p1_subsystem.sv`（相关行）；`P1_HARDWARE_TEST_PROPOSAL.json`；`p1_probe.py`全文；`opus_P1_stream_20261017.py`第60—200行；`p1_coexist_board_template.py`全文。
- 只在PC上读文件，没有打开Vivado，没有连接板卡，没有执行SDK、NPU、JTAG或训练。
- 未读：ZIP包、`history/`、P0的CDC与例外报告（P0目录里没有）、`udma_lease.c`/`p1_mmio.c`、`p1_copy_dma.sv`、仿真testbench、stream工具第1—60行和200行以后、`stream_controls_v2/verified.json`、`network_diagnosis.json`、`RESUME_STATE.json`。

## 1. 结论

| 项 | 判定 | 是否阻塞下载 |
|---|---|---|
| 时序例外替换（第2节） | 思路正确，对旧端点的覆盖是等价的；但比较基准是P1自身而不是M512基线，而且布线后没有复核 | **阻塞**：需补2项只读检查 |
| business_alarm 128→127（第3节） | 属于跨层次的逻辑重组，不是告警逻辑被改；但影响范围已经扩到GP0接口，需要做等价证明 | **阻塞**：需做等价证明，**不必为它单独重建** |
| 新发现：P1状态位CDC-10（第4节） | 我们自己新加的逻辑，组合信号直接进同步器，与“不确定就不释放缓冲”的安全前提冲突 | **阻塞**：需改RTL并重建 |
| 布线、资源、DRC、MinPeriod（第5节） | 布线0错误，setup/hold 0失败，MinPeriod仍是原来2处，严重DRC与基线相同 | 可继续 |
| 上板方案（第6节） | 预算和回退结构合理 | 可继续，有3处小改 |
| 板端程序（第7节） | fence、epoch、poison、单次协议都正确 | 可继续，有4处小改 |

因为第4节必须改RTL并重建，第2节和第3节的检查都应在**新的routed网表**上完成。当前候选bit `975e83ee…`作废，不下载、不送审批。

## 2. `p1_cdc_guard.tcl`时序例外

### 2.1 原厂例外是什么

`project/constrs_1/ai7030.xdc`第13—14行是两条整组时钟级false_path：`clk_out1_clk_wiz_0`↔`clk_pll_i`。从`ps_ai.bd`看，`clk_pll_i`是HP2互连（`axi_interconnect_4`→`processing_system7_0/S_AXI_HP2`，地址段`HP2_DDR_LOWOCM` 0—1G）的时钟，就是PS DDR一侧，不是PL DDR。GP为`clk_out1_clk_wiz_0`（10ns），HP为`clk_pll_i`（5ns），与guard报告中的5/10ns一致。

### 2.2 现在的做法是否等价

guard对每个方向依次做：①读出原例外下所有`user_ignored`路径的ENDPOINT_PIN，去掉新bundle的寄存器；②用相同的`-from clk -to clk`做`set_max_delay -reset_path -datapath_only`，清掉这对时钟上所有已有例外；③对①中的端点集合用`set_false_path -from srcclk -through <端点pin> -to dstclk`恢复；④再读一次`user_ignored`端点，要求与①完全相同，并且bundle不在其中；⑤对bundle的D pin另加同值max_delay，并要求有限slack、REQUIREMENT等于目的周期。

判断：

- **对旧端点，路径范围是等价的，不只是端点集合相等**。原例外是时钟对级别，凡是从srcclk出发、终点在dstclk的路径全部忽略；恢复后的例外对同一端点pin同样覆盖“从srcclk出发、经过该pin、到dstclk”的全部路径。同一端点上，不论路径从哪个srcclk寄存器出发、经过什么组合逻辑，都在覆盖范围内。捕获时钟条件`-to dstclk`保留，不会把一个多时钟单元其他时钟域的路径一起放掉。用端点pin而不用`all_registers`，避开了LUTRAM端点漏网的问题，这一点是对的。
- **`-reset_path`的影响面**：它清除的是这对时钟之间**所有**已有例外，不仅是那条false_path。这对时钟上原有的其他例外（例如`proc_sys_reset`的`*cdc_to*/D` false_path、厂商IP的max_delay/multicycle），因为原来都被时钟级false_path盖住，它们覆盖的端点本来就在①的集合里，③会一并恢复为false。所以结果仍然等价。这条推理成立的前提是①的枚举是完整的（见下面第2.3节第1条）。
- **bundle单元的非D引脚**：原来bundle寄存器的CE/R/S等所有引脚都被忽略；现在这些引脚上从srcclk来的路径既不在③里，也不在⑤的`-to D`里，所以受②的5/10ns datapath_only约束。这是**收紧**而不是放宽，安全。
- **没有发现放宽原厂约束的地方**。所有偏差都是更严，最坏结果是多出时序失败，而不是漏掉失败。

### 2.3 仍然缺的证据（阻塞，2项只读检查）

1. **比较基准不对**。①读的是P1设计自己在例外修改之前的忽略集，所以只能证明“修改没有改变P1的旧覆盖”，不能证明P1的忽略集等于M512原厂的忽略集。P1新增了仲裁器、DMA和同步器，它们中凡是跨这对时钟的路径，在原例外下也会被整组忽略，并且会被③原样恢复为false。要求：在P0 routed检查点上用同一段枚举代码导出两个方向的`user_ignored` ENDPOINT_PIN集合，与新routed网表的集合做差。差集里的每个pin必须属于P1，并且逐个说明它是一个合法的同步器第一级（D pin在`p1_cdc.xdc`的`p1_ctrl_first`列表里）；不属于P1、或者是P1中非同步器的端点，都要单独约束。另外把`-max_paths`的截断检查保留。
2. **布线后没有复核**。`audit_routed.tcl`只检查了bundle有有限slack，没有重新枚举忽略集，也没有检查有没有其他新被计时的路径。place/phys_opt的复制可能产生新的端点。要求：在routed检查点上①重新导出两个方向的忽略集并与opt_post的集合对比；②`report_timing -from [get_clocks srcclk] -to [get_clocks dstclk] -max_paths 1000`列出**所有被计时**的跨时钟路径，要求终点只能是bundle的161/43个寄存器（或它们的复制体，复制体要逐个列出）。

两项都是只读Tcl，每项≤10分钟。

## 3. business_alarm 128→127

### 3.1 实际是怎么回事

基线中，`business_alarm/pending_reg/Q`经过层次边界一直连到`U0_ps_ai_peripheral/u_gp0_io/gpx_std2ww_u0`，负载为`pulse_frame[31]_i_3`（LUT4）和`business_alarm/ps_ai_i_i_52`（LUT2，`pending|reset`）；后者只驱动GP0里的`ps_ai_i_i_48/I5`。P1中`ps_ai_i_i_52`消失，`pending`直接进入GP0中新的`ps_ai_i_i_205`（LUT4，INIT 16'hDDDF），`pulse_frame[31]_i_3`从LUT4变成了LUT6。

按INIT解码，`ps_ai_i_i_205 = NOT( NOT awwaddr[18] AND NOT i_213 AND (pending OR reset) )`，其中`pending OR reset`正是消失的那个LUT2。所以这是综合把业务模块的一个OR门推进了GP0接口的LUT（跨层次优化），**128→127只是资源归属变化**。

但它同时说明两件事：①**GP0接口逻辑本身也被重新综合了**（`pulse_frame[31]_i_3`的输入换了，`ps_ai_i_i_205`驱动`awaddr_vld`、`wdata_vld`、`bid_fifo`等GP0写握手）。尽管GP0源码没变，但P1替换GP1 RFU后的全局综合改变了它的网表。②`business_alarm`的“128 LUT”从来就不是一个封闭的边界：它的逻辑已经和`u_gp0_io`、`ps_ai_wrapper_u0`（`rdata`/`rvalid`）、`u_demo_clkrst_wrapper`（`shadow_frame`）交织。`input_driver_changes`有2704条，大多是`__0/__1`后缀改名，但不能逐条用眼看。

### 3.2 结论：做等价证明，不为它重建，不豁免

- 不能直接豁免128→127。
- 也**不建议**用KEEP_HIERARCHY/DONT_TOUCH强行重建来凑回128：那样网表会和基线不同，仍然需要证明。
- “LUT数相同”这个门槛原本就是“功能不变”的替代指标。现在应该**在跑证明之前**，书面把这条门槛换成下面的等价门槛并封存，而不是事后宣布通过：

**等价门槛（替代128 LUT）**：以P0 routed网表为参考，P1新routed网表为实现，范围为`business_alarm`、`business_reset_u`、`business_tick`、`core`、`u_gp0_io`以及与它们直接相连的`ps_ai_wrapper_u0`读数据/读有效逻辑和`u_demo_clkrst_wrapper`的`shadow_frame`逻辑。所有FF、LUTRAM、BRAM、DSP、顶层端口和该范围的边界net作为切点；P1子系统与NPU作为黑盒，其输出作为自由输入。要求：
1. 时序单元一一对应（按显式改名映射），类型、INIT、时钟net、复位/置位net对应；
2. 每个对应时序单元的D/CE/R/S输入，以及每个边界输出，都是切点的组合函数，并且两边相等；
3. 新出现的输入（例如P1中`ps_ai_i_i_213`）必须追到切点，并证明它在基线中有对应信号；如果它依赖P1子系统的输出，**判定不等价**。

**做法**：不需要外部商用工具。用Vivado只读导出两份网表的单元、INIT和连接（与现有`business_delta.tcl`同法，但范围扩大到上面的集合），再用Python做：①按改名映射做结构同构比较，绝大多数单元会直接匹配；②对剩下不同构的少数区域（已知是`ps_ai_i_i_52`/`ps_ai_i_i_205`/`pulse_frame[31]_i_3`/`ps_ai_i_i_48`/`ps_ai_i_i_213`附近），在已匹配的边界net处切开，对这一小块的支撑变量做穷举真值表比较。预期支撑变量不超过20个，可以穷举。若某块超过24个变量，就安装一个固定版本的开源等价工具（需要你批准），不改用随机仿真来代替证明。

预算：PC≤半天，零板端。交付：脚本、两份导出、匹配统计、不同构区域清单与逐块真值表结果。失败（任一块不等价，或`ps_ai_i_i_213`依赖P1）：停下，按RTL追根因，不下载。

**最先做的一步（≤30分钟）**：追`ps_ai_i_i_213`的输入锥到切点。如果它只依赖`ww_gpx.awwaddr`等GP0原有寄存器，基本可以预期证明会通过；如果它依赖`p1_subsystem_u`，说明GP0写接口的行为真的被改了，这比LUT数严重得多。

## 4. 新发现：P1状态位跨时钟前有组合逻辑（阻塞）

`audit_cdc_full.rpt`第146—152行，P1子系统有7条**CDC-10 Critical**（Combinational logic detected before a synchronizer），终点是`control/stat1_gp_reg[0..6]`，源包括`core/FSM_sequential_state_reg`、`core/fault_latched_reg`、`arbiter/ro_reg`、`arbiter/wo_reg`、`arbiter/FSM_onehot_rs_reg[1]`，以及原厂`frame_to_ps_inst0/u_aq_axi_master/reg_wvalid_reg`。

对照RTL `p1_gp_control.sv`第64行：

```verilog
stat1_gp<={1'b0,any_write_outstanding,any_read_outstanding,wout,rout,fence_hp,fault_hp,core_busy};
```

其中`fence_hp`（第39—41行）是十来个HP域信号的组合AND；`rout`/`wout`是两个寄存器的OR；`any_read/write_outstanding`（`p1_axi3_owner_arbiter.sv`第62—63行）是仲裁状态与两个主设备ARVALID/AWVALID/WVALID的组合OR，直接来自原厂视频主设备。这些组合信号**没有在HP时钟下先打一拍**，就被GP时钟采样。

为什么要改：输入在HP时钟沿附近变化时，组合输出可能出现毛刺，GP侧可能把毛刺采成一个稳定的错误值。最危险的情况是在“写0禁止启动→等待排空”这段时间里，`rout`/`wout`在变化，`fence_hp`可能瞬间为1。释放私有缓冲依据的正是`fence_ack`、`busy=0`、`outstanding=0`。软件要求两次读一致并且epoch相同，概率已经非常低，但这是我们自己新加的逻辑，违背了“不确定就不释放”的设计原则，修起来也很便宜。

**最小修改**：在HP时钟域加一个8位寄存器`stat_hp`，每拍锁存上述8个信号，GP侧改为同步`stat_hp`。`stat1_gp`的ASYNC_REG和`p1_cdc.xdc`中的第一级false_path保持不变。这会让GP侧看到的状态晚1个HP周期，`fence_hp`的意义不变。仿真回归需要重跑，此前的62场景和仲裁6/8场景都要重跑。

另外，CDC报告里共有CDC-10 125条、CDC-1 10201条等，绝大多数是原厂的。P0目录里没有CDC报告，无法区分新旧。要求在P0 routed检查点上导出同一份`report_cdc -details`，按（ID、源、目的）做差。差集中只允许出现P1的已知同步器（CDC-3），任何新出现的Critical都要逐条说明。

## 5. 布线、资源、DRC、MinPeriod与NPU SI0

依`result.json`和我读到的报告：

- 布线：85441个可布线net全部布通，0错误（`audit_route_status.rpt`）。
- `check_timing`：没有无时钟寄存器，没有未约束的内部端点。11个输入端口没有input delay，与原工程相同（PJTAG、DDR3 DQS等）。
- setup WNS 0.030ns、hold WHS 0.029ns，0失败；脉宽WPWS -0.409，失败2处，都是原NPU `AI_CORE_CLK`的BUFG/I和PLLE2 CLKOUT0，与M512同一对pin。没有新增MinPeriod豁免。**可接受**；WNS只有30ps，新的重建必须重新满足，不能放宽。
- 资源：LUT 36012→36848（+836），FF 50578→52115（+1537），BRAM/DSP不变。
- DRC：类别和计数与基线完全相同，没有新的Critical。
- bundle：两组有限slack 4.060/8.928ns，REQUIREMENT 5/10ns。**可接受**。
- **NPU SI0未变**这一项只有声明，我没有看到结构证据。要求和第3节合并：把`S_AXI_HP2_0`→`axi_interconnect_4/S00_AXI`这条路径以及`u_AI_Mate_top`的边界net一起导出并比较，证明连接不变。24个OOC检查点复用可以证明内部不变，但证明不了顶层连线。
- Tcl日志通道故障未复现：保留现有的标准通道拒绝检查即可，不阻塞。

## 6. 上板方案`P1_HARDWARE_TEST_PROPOSAL.json`

合理之处：候选bit最多下载1次，原M512条件回退最多1次；112帧（100真实＋12合成）逐字节比较；NPU会话1次、1000帧；不自动重试；私有16MiB放在CMA `[0x30000000,0x3e000000)`内，不与`udmabuf0`（0x30100000，128MiB）重叠，禁止借用`udmabuf0`；O_SYNC非缓存映射；状态未知时保留缓冲，先断电再回退；明确声明不宣称加速，112帧是已曝光的开发输入。这些我都同意。

需要改的3处：

1. **下载前确认HP总线空闲**。回退或候选下载都会重写PL，而HP2上的主设备（原视频主设备、P1 DMA、NPU）如果有未完成事务，PS互连可能挂死。序列第1步（下载候选）之前，要求：没有M747会话在运行、没有NPU进程。回退下载之前，要求：P1两次fence一致；如果做不到，就断电而不是JTAG（现有方案已经这样写，只需把“下载候选前”也写进去）。
2. **cache口径固定为一种**。112帧全部使用`explicit_sync=false`＋O_SYNC非缓存，作为正确性门槛。显式同步加缓存映射的成本对比留到P3，不在P1里混用两种模式。
3. **1000帧回归加NPU与告警的对照项**。除了FPS≥17.76和逐帧一致，还要记录SDK profile里的`Hardop_HardTime`和CDMA memcpy时间，与10/16第36批的5.958ms/4.494ms对照，作为“HP2加了仲裁器但NPU不受影响”的直接证据（仅记录，不设门槛，因为240第1节说明开机状态会影响CDMA）。因为告警逻辑已经被重新综合（第3节），1000帧中的PL告警字段与CPU影子逐帧一致这一项**必须是门槛**，不能只当参考。

## 7. 板端程序

`p1_probe.py`做得对的地方：执行前核验plan、代码与库的SHA、新bit证明、boot；先取真实锁inode943514再核对526项SHA；开始时写0禁止启动，然后等待两次一致的fence（掩码0xfd9=0xc0、enable=0、epoch相同）才创建私有租约；每帧先比对头部和数据SHA才交给硬件；描述符写入后回读比较；CLEAR作为独立命令等待完成；每帧只启动一次，计数accepted/completed序号；结束后再fence再reclaim；失败时只尽力发一次STOP和禁止，不释放、不重试；没有用析构函数释放缓冲。stream工具的就绪门槛（uptime≥600、system running、无jobs、load1≤0.3、无apt/dpkg进程）、远端目录唯一且不允许重连、5个文件逐个核验SHA，也都到位。

需要改的4处：

1. **断链时失败证据会丢**。SSH断开后，probe写stdout会抛BrokenPipe；`run_probe`的except会先执行`backend.failure()`（STOP/禁止已经发出，这一点没问题），但随后`emit`再次失败，失败记录无法到达PC。要求：`failure()`的结果同时写一份到已有的专属远端目录`<remote_root>/failure.json`（用`x`模式，只写一次）。这只是在本次任务自己的目录里多写一个小文件，不违反“不写板端日志”的原意。另外在`signal`中加入SIGHUP和SIGPIPE，处理方式与SIGTERM相同。
2. **源槽和目的槽的完整性**。现在只比较目的槽等于输入。还要求：①每帧开始复制前，把目的槽写成固定填充值（例如0xA5），这样DMA什么都没做的情况就不会因为上一帧残留而被误判；②复制完成后回读源槽，确认没有被改写。成本大约每帧多一次6MB非缓存写和一次非缓存读，按现有deadline足够。
3. **`p1_coexist_board_template.py`的`check()`缺就绪门槛**。stream工具里有，但NPU回归是在另一个时间点启动的。要求在`check()`里加入同样的uptime/system running/jobs/load/包管理器检查，并且在`start()`远端脚本中，持锁时再核对一次P1两次fence一致（现在只在`check()`里查过，`check()`和`start()`之间锁是放开的）。
4. **回归门槛里的FPS基线**。`NPU_acceptance.FPS_min`沿用17.76。按240第1.5节，同boot通过就绪门槛的CPU路径才是PL比较的基线；P1不替换NPU输入，所以17.76作为“没有退步”的下限可以接受，但结果里要同时报告与第36批19.857FPS的差值，不要只报“过线”。

`udma_lease.c`和`p1_mmio.c`我没有读。fence谓词、租约状态机和尾部guard的C实现，请研发方在下一份材料中附上，供复核。

## 8. 修改顺序与预算

| 步 | 内容 | 预算 | 通过标准 | 失败止损 |
|---|---|---|---|---|
| 1 | 追`ps_ai_i_i_213`的输入锥（在当前网表上） | ≤30分钟，只读 | 只依赖GP0原有寄存器 | 依赖P1：停下，交我和你看RTL |
| 2 | RTL：`stat_hp`寄存器；重跑全部仿真 | ≤2小时 | 62场景、仲裁6/8、GP/CDC 158项全部通过 | 不通过就修，不跳过 |
| 3 | 重建（沿用24个OOC缓存、同一guard） | ≤30分钟 | setup/hold 0失败；MinPeriod仍是原2处；无新DRC Critical | 时序不收敛就停，不放宽约束 |
| 4 | 第2.3节两项只读检查（对P0基线、布线后） | ≤30分钟 | 差集只含P1同步器第一级；跨时钟被计时路径只有bundle | 差集出现其他端点就单独约束后再建 |
| 5 | 第4节P0/P1的CDC差集 | ≤30分钟 | 新增只有P1已知同步器，CDC-10在P1中为0 | 新Critical逐条说明，说明不了就改 |
| 6 | 第3节等价证明（含NPU SI0连接比较） | ≤半天 | 全部切点对应，不同构区域穷举相等 | 不等价就停 |
| 7 | 修改probe/stream/template（第7节）；更新上板方案（第6节） | ≤2小时 | 离线自测通过，SHA重新封存 | — |
| 8 | 交复核，然后你单独确认新bit下载和条件回退 | — | — | — |

第1—7步全部在PC上完成，预计10/18内可以结束。即使用掉一整天，到10/19还剩P2、P3约一周，10/26 18:00止损不变。

## 9. 需要你决定或配合的事项

1. 是否同意把“business_alarm LUT=128”这条门槛，在跑证明之前书面换成第3.2节的等价门槛。
2. 如果第6步出现超过24个支撑变量的区域，是否允许安装一个固定版本的开源等价检查工具。
3. 第8步之后的新bit下载与条件回退，届时单独确认。

研发方可以直接做：第1—7步（全部PC侧，零板端）。

## 10. 我没有核验的内容

- `udma_lease.c`、`p1_mmio.c`、`p1_copy_dma.sv`、全部testbench与仿真日志；stream工具的其余部分；`stream_controls_v2/verified.json`中的断链控制结果。
- P0基线的CDC和例外报告（目录里没有），因此第2.3节和第4节的“新旧差集”我只能列为要求，不能给出结论。
- `audit_exceptions.rpt`（9.5MB）全文；我只根据guard代码和输出、xdc和RTL推理例外的效果。
- `ps_ai_i_i_213`的来源；GP0接口其余改名单元是否全部等价。
- 当前boot `78bee972…`的实时状态、网口诊断与`RESUME_STATE.json`；研发侧所有`passed`字段。
