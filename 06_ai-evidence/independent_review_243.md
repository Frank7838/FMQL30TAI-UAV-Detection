# 独立复核意见单：Opus242执行结果

复核日期：2026-10-17。对象：`deliveries/opus242_progress_20261017/PROGRESS.md`（19:16:17版）、`OPUS_PROMPT.txt`及`pc_runs/opus_242_followup_20261017_v1/`原件。接续[[242-独立复核意见单_P1构建_时序例外_告警差异与上板方案_20261017]]。研发助手为GPT。

## 0. 本次实际做了什么

- 我本人读取了：`PROGRESS.md`、`OPUS_PROMPT.txt`、`WORK_STATE.json`、`CHECKPOINT_CHAIN.json`、`EQUIVALENCE_CONTRACT.json`、`SCOPE_CLARIFICATION.json`、`trace_213_verified.json`、`rebuild_v2/result.json`、`closure_audit.json`、`build_source_sha.json`、`regressions_verified.json`、`equivalence_verified.json`/`equivalence_smt_verified.json`/`equivalence_exhaustive_attempt.json`/`equivalence_checker_controls.json`的摘要、`cdc_boundary_verified.json`、`cdc_boundary_qualified.json`、`native_cdc_proof_verified.json`、`connectivity_proof_verified.json`；新`p1_gp_control.sv`全文对比；`p0_cdc.rpt`/`p1_cdc.rpt`中的抽查行；`p0/p1_connectivity_graph.tsv`中的抽查单元；`ps_ai_peripheral.sv`中HP2和`frame_to_ps_largebuf`相关行。在这些JSON上用Python做了分类统计。
- 另派两个只读子审查：①等价证明引擎（`tools/opus242_equivalence/smt/controls/exports*.py`及其输出）；②板端软件（`udma_lease.c`、`p1_mmio.c`、`p1_probe.py`、coexist模板、stream工具、`opus_P1_regression_20261017.py`、`P1_HARDWARE_TEST_PROPOSAL_v2.json`、各软件控制回执）。下文引用它们的结论时会注明，行号由子审查给出，我抽查核实了其中两处（`frame_to_ps_largebuf`是HP2主设备；coexist模板第206行使用`assert`）。
- 没有打开Vivado，没有连接板卡，没有执行SSH、JTAG、NPU、训练，也没有修改任何原件。

## 1. 结论

| 项 | 判定 | 说明 |
|---|---|---|
| 1 等价证明与范围澄清 | **通过（附1项补记）** | 第2节 |
| 2 时序例外差集与布线后复核 | **通过** | 第3节 |
| 3 stat_hp与CDC-6 | **通过（附条件接受，不改RTL）** | 第4节 |
| 4 CDC全量差集与NPU连接 | **通过** | 第5节 |
| 5 重建：时序/MinPeriod/DRC/OOC | **通过** | 第6节 |
| 6 板端软件 | **阻塞：3项，均为软件修改** | 第7节 |
| 7 HP2空闲证据与预算 | **阻塞：缺可执行方法** | 第8节 |

**FPGA部分（候选bit `ad1e5da9…`）可以接受，不需要再改RTL或重建。** 阻塞项都在软件和上板流程里，修改后只需要重做PC侧离线控制，不影响bit。全部关闭后，再由你单独批准一次性下载和条件回退。

## 2. 等价证明（通过，附1项补记）

依子审查结论，并与我读到的摘要一致：

- `ps_ai_i_i_213`是`awwaddr[31:19]!=0x801`的地址译码（8192种赋值穷举），只依赖GP0原有寄存器。0x801<<19=0x400c0000，正是M512告警窗口。没有P1依赖。
- 门槛在证明之前已经冻结（`EQUIVALENCE_CONTRACT.json` 17:55:54，证明18:45），替换的是“LUT=128”，没有自动接受127。
- 引擎语义：LUT的INIT位序（I0为最低位）正确；本网表中只出现BUFG、CARRY4、FD*E、GND/VCC、IBUF、LUT1–6、PS7、RAM64M、RAMD64E、SRL16E，非组合单元一律作切点，遇到未知组合类型会报错。CARRY4把CI与CYINIT做OR，这只在其中之一恒为0时成立；本网表全部76个CARRY4都满足这个条件。
- 切点：630个状态按名称一一映射，类型、INIT和全部参数（包括各IS_*_INVERTED）都要求相同；P0与P1对应切点使用同一变量；任何触及`p1_subsystem_u`的输入锥都会报错。
- 判定：4084项结构相同；1192项不同构中，361项穷举相等，其余831项经83个去重miter由Z3 4.13.3全部判为UNSAT，`unknown`按失败处理。没有任何目标在缺少三种证明之一的情况下被判通过。
- 范围澄清：被排除的`gp_init_reg[2]_srl2_i_1`的输入是一个demo FF和一个P1 FF，负载只有两个P1的CE引脚，不驱动任何原有状态。排除合理。

**补记（必做，不阻塞下载）**：有4个组合输出没有成为比较目标，即`led_OBUF[0]_inst_i_1/O`和`rst_d2_ddr_clk_reg_0_buf/O`（驱动`frame_to_ps_inst0`的3个R/CLR引脚）。子审查已逐一手工核对，类型、INIT（4'h8、2'h2）和输入驱动在P0与P1中都相同。要求：把它们加入比较目标重跑，或者在证明回执中明确记为“人工核验”并附上核验内容。

**建议补（不阻塞）**：对照集缺少“引脚交换”“进位错误”“切点映射错误”三类必须被检出的反例；CARRY4对照与引擎使用同一个公式，不算独立验证。

## 3. 时序例外（通过）

`cdc_boundary_verified.json`与`cdc_boundary_qualified.json`：

- GP→HP：P0原厂忽略集11836，新网表11836；新增4个（`ack1_hp`、`enable1_hp`、`req1_hp`、`stop1_hp`的D引脚，都是同步器第一级）；删去的4个都是`u_sync_ddr_clk_rst/reset_2d_reg`的复制体PRE引脚，按复制族归并后为0。opt→routed多出68个，全部是`reset_2d`的复制体。被计时的跨时钟端点只有bundle的161个。
- HP→GP：107→116，新增9个都是P1同步器第一级（`result_t1_gp`、`runtime_hp1_gp`、`stat1_gp[0..6]`），删除0，opt与routed完全相同；被计时端点只有bundle的42个。
- 原始集合不完全相同，但差异全部可以归到“P1新增的合法同步器第一级”和“phys_opt复制体”两类。复制体按类型、INIT和全部输入（含时钟、复位、使能）对应之后才归并，这个归并方法我认为足够。242第2.3节的两项要求已经关闭。

## 4. stat_hp与CDC-6（附条件接受，不改RTL）

新RTL（`rebuild_v2/project/rtl/opus_P1/p1_gp_control.sv`）只改了一处：`stat_hp`在HP时钟下锁存8个状态，GP侧改为两级同步`stat_hp`，符合242的最小改法。P1原有的7条CDC-10已经消失。

Vivado把`stat_hp[6:0]`→`stat1_gp[6:0]`归为**CDC-6**（多位信号用ASYNC_REG同步）。它的意思是：各位分别同步，GP侧在同一拍看到的8位不一定来自HP的同一拍。**我接受它，不要求再改**，理由和条件如下：

1. 释放缓冲依据的“空闲”是`stat_hp[2]`即`fence_hp`**这一位**。它在HP域已经是全部空闲条件（未使能、核心不忙、无读写在途、仲裁空闲、无未应答请求、无故障）的AND，并且先锁存再同步，所以它本身只会是旧值或新值，不会是毛刺。
2. 软件释放谓词（`(status&0xfd9)==0xc0`且enable=0，两次独立采样且epoch相同）在fence之外，还要求busy、pending、quarantine和各个在途位都为0。这些附加位只会让判断更保守，不会让一个“不空闲”的状态被判成空闲。
3. 只要`enable_gp`在GP侧保持为0，P1 DMA不会再启动，所以fence一旦为1，在软件重新使能之前不会因为P1自身而失效。原厂视频主设备的在途位会变化，但它不访问P1的私有缓冲，不影响释放安全。

接受条件（写进`P1_INTERFACE.json`和`p1_mmio.c`注释）：①状态向量不是原子快照，软件只能把它当作“各位分别为0”的AND来用，不能把多位组合解释为一个一致的状态；②释放必须以fence位为主条件；③不得把这条CDC-6扩展成对其他多位总线的豁免。

## 5. CDC全量差集与NPU连接（通过）

**CDC**：P0→新网表原始差集有823条新增、814条删除，其中Critical新增441、删除441。我按（ID、目的端点）统计：

- 441条Critical的新增与删除在每个目的端点上一一抵消，全部来自原厂SDI相机/视频链路（`a2sync_reset_cam0_clk`、`u_vtc`、`frame_trans_inst0`、`frame_to_ps_inst0`等），只是`report_cdc`为同一个目的端点选的“代表源”换了。
- 按目的端点只多出9类：P1的6条CDC-3和1条CDC-6，以及`frame_ctrl_u0/S_AXI_RDATA_reg[0]/[1]`的CDC-15。后者我在`p0_cdc.rpt`第109—110/198—199行和`p1_cdc.rpt`第110—111/202—203行核对过，两边都有，只是代表源不同。
- `native_cdc_proof_verified.json`对199个端点单元的832项完整输入函数、902个源切点的配置和时钟域做了比较，原始270/268条代表源差异保留未删。

结论：分类之后新增的Critical为0，P1的CDC-10为0。“严格只有CDC-3”这句措辞不成立（多了1条CDC-6），按第4节接受。

**NPU连接**：原始连接集合143571→145946，不相同。研发方把差异归为复制体、常量网别名、外部驱动的共享网和名称编码，34个复制族、1537项输入函数检查后剩余新增/删除都是0。我抽查了被删除的那条`s00_regslice/.../m_valid_i_reg/Q`→`U2_n2w_bwc_r/n2w_reg[511]_i_1_replica/I3`：基础单元`n2w_reg[511]_i_1`在P0和P1中类型（LUT4）、INIT（16'h3730）和4个输入完全相同，差异只是phys_opt在P1中少复制了一份。新增连接中有2016条提到`p1_subsystem_u`，都是P1自身的连接；没有新增连接触及S00。仲裁器接在`S_AXI_HP2_1`（S01）上，NPU所在的S00没有变动。我只做了这一处抽查，没有逐条复核34个族。

## 6. 重建（通过）

`rebuild_v2/result.json`：setup WNS 0.030ns、hold WHS 0.029ns，0失败；脉宽失败2处，与M512同一对NPU `AI_CORE_CLK`引脚；DRC类别和计数与P0完全相同，没有新的Critical；布线0错误，无未约束内部端点；bundle裕量4.11/9.082ns，要求5/10ns；24个OOC的SHA不变；源码变更仅限`p1_gp_control.sv`。LUT 36835、FF 52108。旧候选`975e83ee…`作废，记录保留。

## 7. 板端软件

依子审查结论。部署文件与`software_SHA`中的固定值一致；ARM `.so`由对应源码构建，ABI未变。

**通过**：

- `udma_lease.c`：`udma_lease_release`每次都直接现场读两次（间隔1ms）硬件状态，再检查锁，然后按与硬件相同的条件判定，不使用缓存结果；先解除映射和关闭，再删除；所有出错路径都直接返回，不解除映射、不删除；管理器命令只写一次。映射为O_SYNC，要求`sync_mode==1`、`dma_coherent==0`；地址范围限定在`[0x30000000,0x3e000000)`，与`udmabuf0`不重叠；长度上限8MiB−4KiB，尾部至少4KiB保护区，每次访问都检查。目的槽全长0xA5填充、源槽全长回读，都会逐字节比较并计时。
- `p1_mmio.c`：只映射0x80080000处4KiB；偏移4字节对齐且≤0x38；检查同进程对inode943514持锁；快照逐个读寄存器，并重读epoch和签名；空闲判定与硬件谓词一致，两次采样、AND。
- `p1_probe.py`：没有任何finally或析构函数释放缓冲；`failure.json`以`x`模式、0600权限写在任务专属目录里，并且先于输出；stdout断开时被吞掉；预算112帧。
- stream工具：单次SSH，`ConnectionAttempts=1`，不重连；远端目录唯一，原子创建；所有读写都有时限。
- 1000帧回归（`tools/opus_P1_regression_20261017.py`）：17.76FPS是硬门槛；逐帧CPU/PL告警一致由`product_console_m747.validate_records`强制，并与参考逐项精确比较；与第36批的FPS差值以及Hardop/CDMA的差值只作报告，参考值读自SHA固定的文件。

**阻塞（必须改，都是软件修改）**：

- **A. 只靠锁排他，不检查进程。** M53管理器打开锁时不让子进程继承（`m734d`管理器第160、185行），所以如果管理器异常退出，它启动的NPU/视频worker可能仍在使用HP2，而P1可以拿到锁。修改：在`p1_probe.py`的`RealBackend.begin`取锁之后，以及stream工具BOOTSTRAP的`flock`之后，要求不存在argv以`board_video`、`drone_stream`、`drone_yolo11`、`npu_worker`开头的进程（与`active_sessions`同一判定）。
- **B. coexist模板父进程放锁到M53取锁之间有空档（第209—211行）。** M53的`run()`只检查`active_sessions`和M512签名，不检查P1 fence，也不检查`opus_p1_*`租约。修改（二选一）：①在启动M53前的门控子进程里，持锁时再做一次只读的双fence检查和“没有`opus_p1_*`”检查；②启动后读`/proc/locks`，确认锁的持有者就是记录的管理器pid，否则立即停止。我推荐①。
- **C. 门控依赖`assert`（第206行，我已核对）。** 如果用`python3 -O`或设置了PYTHONOPTIMIZE，`assert`会被去掉，父进程即使已经退出，管理器也会照样执行。修改：改成`if not (r and os.read(0,1)==b'1'): os._exit(1)`，并用`python3 -I -E`启动。

**小改（不阻塞）**：`p1_probe.py`在`except`与进入失败模式之间，如果第二个信号到达，可能跳过失败记录。修改：`stop()`的第一行先屏蔽信号。

**未核验**：其他root进程能否打开`/dev/opus_p1_*`（只有建议锁保护）；`p1_mmio.c`在解码返回EPROTO时无法写STOP，属于失败关闭，但该模块保持未禁止状态；版本寄存器0x04未写入接口文档，请补上。

**PC侧证据范围**：`software_controls_v6` 58/58通过，用的是假后端、假管理器和112个假帧，0次设备/SSH/NPU/JTAG调用；`stream_controls_v4`声明14项，但回执里只记录了10项结果，BOOTSTRAP只做了语法检查，载荷是64字节而不是整帧；`buffer_v3_attempt2`为49项ABI和47项纯主机控制。**这些都没有在真实硬件上运行过。** 请补齐`stream_controls_v4`缺少的4项记录，或者说明为什么是10项。

## 8. HP2空闲证据（阻塞：缺可执行方法）

`P1_HARDWARE_TEST_PROPOSAL_v2.json`的`sequence[0]`和`failure_and_fallback.before_any_JTAG`只写了“需要HP2空闲的肯定证据”，没有对应的脚本或寄存器读取方法。原厂`frame_to_ps_largebuf`（`ps_ai_peripheral.sv`第928行起）是HP2主设备，`m00_axi_awvalid/arvalid`直接接`S_AXI_HP2_AWVALID/ARVALID`，它的读写受`display_en`、`continuous_en`等寄存器控制的帧逻辑驱动，所以“没有进程在运行”并不能证明它空闲。

要求补一个固定、SHA封存的`hp2_idle_audit`，持真实锁时执行，只读：

1. 没有M747/M53/NPU/视频进程（与阻塞A同一判定），没有`opus_p1_*`租约；
2. 对候选bit：P1两次fence一致；
3. 原厂视频主设备：间隔≥100ms的两次采样中，PS可读的状态（例如`display_en`/`continuous_en`、写/读地址索引、`wr_done_pulse`计数、`wr_wfifo_err`/`rd_rfifo_err`）保持不变，并且显示和相机为禁止状态。研发方需要先列出这些信号在M512寄存器表中的地址；
4. 如果第3项在M512 bit上PS读不到，唯一安全的做法是：**断电→新boot审计→在任何会产生HP2流量的程序运行之前下载**。这同样适用于回退M512。

预算本身可执行：候选下载1次、条件回退1次、112帧复制、1次1000帧NPU；不自动重试。

## 9. 关闭顺序（全部在PC侧）

| 步 | 内容 | 预算 | 通过标准 |
|---|---|---|---|
| 1 | 第2节：4个组合输出加入比较目标重跑，或记为人工核验 | ≤30分钟 | 回执中4项都有结论 |
| 2 | 第7节阻塞A/B/C及信号屏蔽小改；`P1_INTERFACE.json`与`p1_mmio.c`写入第4节的CDC-6使用条件；补0x04寄存器说明 | ≤2小时 | 修改后的SHA重新封存，离线控制重跑并新增A/B/C的反例控制（有残留worker时拒绝；门控子进程看到未fence时拒绝；`-O`下门控仍然拒绝） |
| 3 | `stream_controls_v4`补齐14项记录 | ≤30分钟 | 14项全部有结果 |
| 4 | 第8节`hp2_idle_audit`：先列出M512中原厂视频主设备的PS可读状态地址，再写只读脚本和离线控制 | ≤2小时 | 方法写进上板方案v3；读不到时明确采用“断电后先下载” |
| 5 | 上板方案v3交复核 | — | — |

bit不需要重建。第1—5步完成后交给我复核；复核通过后，由你**单独**批准：候选bit `ad1e5da9…`下载一次、原M512条件回退一次、112帧复制、一次1000帧NPU。下载前现场重新核对boot、真实锁、进程、526项保护文件和HP2空闲。

## 10. 我没有核验的内容

- 等价证明与板端软件的逐行审查由子审查完成，我只抽查了两处；`exports*.py`导出范围对PS wrapper `rdata/rvalid`逻辑是否完整，子审查标为未核验。
- 34个复制族中我只抽查了1个；`native_cdc_proof`的199个单元和902个切点没有逐项复核。
- 仿真testbench和日志（62/6/8/160/1501项）的内容，只看了回执。
- 当前boot `78bee972…`的实时状态；所有软件都只在PC假设备上运行过。
- 研发侧所有`passed`字段，凡是我没有用原件复算的，都按“依记录”对待。
