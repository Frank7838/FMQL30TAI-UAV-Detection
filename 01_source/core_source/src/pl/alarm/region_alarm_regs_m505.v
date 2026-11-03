// 已上板事务寄存器层，厂商地址/数据合并写握手，不是完整AXI4-Lite。
// BEGIN→最多16对坐标→COMMIT组成一次推理结果，半开ROI使用Q16框中心。
// 关键修复：故障入口或半事务超时使事务失效，旧COMMIT不能解除fault。

// Native vendor merged write and separate read handshake, not AXI4-Lite.
// Coordinates normalized to 0..65535; PL evaluates box center in half-open ROI.
module region_alarm_regs_m505
    #(parameter integer TIMEOUT_MS = 1000,
      parameter [15 : 0] ROI_X0 = 8192, ROI_X1 = 57344, ROI_Y0 = 4096, ROI_Y1 = 57344)
    (input wire clk, reset, tick_ms, bus_reset,
     input wire [31 : 0] awaddr, awdata, araddr,
     input wire awvalid, arvalid, rready,
     output wire awready, arready,
     output reg [31 : 0] rdata,
     output reg rvalid,
     output wire alarm, fault);
    // 寄存器偏移以字节计：
    // 0x00业务软复位(产品禁写)，0x04 BEGIN(frame_id)，0x08 XY0，0x0C XY1，0x10 COMMIT。
    // 0x20状态，0x24末帧，0x28接受数，0x2C拒绝数，0x30协议错，0x34事件，0x38事件W1C。
    // 0xC0返回签名0xA5050001。地址译码取[17:2]，不要把case常量误当成字节偏移。
    // txn表示事务已打开，have_xy表示只收到第一半坐标，bad在当前事务内保持错误。
    reg pending, txn, bad, have_xy;
    reg [31 : 0] shadow_frame, xy;
    reg [4 : 0] boxes;
    reg hit;
    // The inference result is a transaction, not just a fresh-looking frame ID.
    // An open transaction cannot survive a stream fault or remain half-written
    // indefinitely while fault is already set. Business alarm/counters persist.
    reg fault_seen;
    reg [15 : 0] txn_age_ms;
    // 同时覆盖故障前已开事务，以及故障已置位后新开但未完成的事务。
    wire fault_entry = fault && !fault_seen;
    wire txn_expired = txn && tick_ms && txn_age_ms == TIMEOUT_MS - 1;
    wire abort_txn = fault_entry || txn_expired;
    reg pulse, pulse_fresh, pulse_hit, soft_reset;
    reg [31 : 0] pulse_frame, protocol_errors, event_latch;
    wire have_frame;
    wire [3 : 0] positives, empties, events;
    wire [31 : 0] last_frame, accepts, rejects;
    // 记录告警、断流与恢复发生时的周期数和帧号；不参与告警判断。
    wire [63 : 0] obs_cycles, obs_assert, obs_clear, obs_fault, obs_recover;
    wire [31 : 0] obs_seq, obs_assert_frame, obs_clear_frame, obs_fault_frame, obs_recover_frame;
    wire [3 : 0] obs_valid, obs_last_mask;
    alarm_observer observer(.clk(clk),
                            .reset(reset),
                            .events(events),
                            .event_frame(last_frame),
                            .cycles(obs_cycles),
                            .event_seq(obs_seq),
                            .valid_mask(obs_valid),
                            .last_mask(obs_last_mask),
                            .assert_cycles(obs_assert),
                            .clear_cycles(obs_clear),
                            .fault_cycles(obs_fault),
                            .recover_cycles(obs_recover),
                            .assert_frame(obs_assert_frame),
                            .clear_frame(obs_clear_frame),
                            .fault_frame(obs_fault_frame),
                            .recover_frame(obs_recover_frame));
    wire [15 : 0] x0 = xy[15 : 0], y0 = xy[31 : 16], x1 = awdata[15 : 0], y1 = awdata[31 : 16];
    // 坐标端点先扩到17位相加，避免16位溢出；直接比较2×ROI边界，不除法截断中心。
    wire [16 : 0] sum_x = {1'b0, x0} + {1'b0, x1}, sum_y = {1'b0, y0} + {1'b0, y1};
    wire valid_box = x0 < x1 && y0 < y1;
    wire roi_match = valid_box && sum_x >= {ROI_X0, 1'b0} && sum_x < {ROI_X1, 1'b0} &&
                     sum_y >= {ROI_Y0, 1'b0} && sum_y < {ROI_Y1, 1'b0};
    // 超时边界不接受新写，并屏蔽尚未提交的pulse，防止旧事务穿透。
    // awvalid/awready是厂商合并地址与数据握手，不能按标准AXI-Lite分离AW/W通道使用。
    // pending让刚发出的COMMIT脉冲留出消费周期；复位或事务过期边界不接受写。
    assign awready = !reset && !bus_reset && !pending && !soft_reset && !abort_txn;
    assign arready = !reset && !bus_reset && !soft_reset && (!rvalid || rready);
    region_alarm_core #(.TIMEOUT_MS(TIMEOUT_MS))
        core(.clk(clk),
             .reset(reset | soft_reset),
             .tick_ms(tick_ms),
             .commit(pulse && !bus_reset && !abort_txn),
             .fresh(pulse_fresh),
             .region_hit(pulse_hit),
             .frame_id(pulse_frame),
             .alarm(alarm),
             .fault(fault),
             .have_frame(have_frame),
             .positive_run(positives),
             .empty_run(empties),
             .last_frame(last_frame),
             .accepted_count(accepts),
             .rejected_count(rejects),
             .events(events));
    always @(posedge clk) begin
        if (reset || soft_reset) begin
            pending <= 0;
            txn <= 0;
            bad <= 0;
            have_xy <= 0;
            shadow_frame <= 0;
            xy <= 0;
            boxes <= 0;
            hit <= 0;
            pulse <= 0;
            pulse_fresh <= 0;
            pulse_hit <= 0;
            pulse_frame <= 0;
            soft_reset <= 0;
            fault_seen <= 0;
            txn_age_ms <= 0;
            protocol_errors <= 0;
            event_latch <= 0;
            rvalid <= 0;
            rdata <= 0;
            // 传输复位只终止半帧、清总线应答，不复位告警规则核；已接受业务计数/告警继续保留。
        end else if (bus_reset) begin
            fault_seen <= fault;
            txn_age_ms <= 0;
            // Abort transport/partial frame while retaining accepted business state.
            pending <= 0;
            txn <= 0;
            bad <= 0;
            have_xy <= 0;
            shadow_frame <= 0;
            xy <= 0;
            boxes <= 0;
            hit <= 0;
            pulse <= 0;
            pulse_fresh <= 0;
            pulse_hit <= 0;
            pulse_frame <= 0;
            soft_reset <= 0;
            rvalid <= 0;
            rdata <= 0;
            event_latch <= event_latch | {28'b0, events};
        end else begin
            fault_seen <= fault;
            if (!txn || abort_txn)
                txn_age_ms <= 0;
            else if (tick_ms)
                txn_age_ms <= txn_age_ms + 1'b1;
            // fault入口和已开事务超时都使旧事务失效，即使fault早已置位也不能无限保留半帧。
            if (abort_txn) begin
                txn <= 0;
                bad <= 0;
                have_xy <= 0;
                shadow_frame <= 0;
                xy <= 0;
                boxes <= 0;
                hit <= 0;
            end
            pulse <= 0;
            pending <= 0;
            soft_reset <= 0;
            event_latch <= event_latch | {28'b0, events};
            if (rvalid && rready)
                rvalid <= 0;
            if (arvalid && arready) begin
                rvalid <= 1;
                // 0x20状态，0x24末帧，0x28接受，0x2C拒绝，0x30协议错，0x34事件，0xC0签名。
                case (araddr[17 : 2])
                    16'h0008:
                        rdata <= {19'b0, hit, empties, positives, 1'b0, have_frame, fault, alarm};
                    16'h0009:
                        rdata <= last_frame;
                    16'h000a:
                        rdata <= accepts;
                    16'h000b:
                        rdata <= rejects;
                    16'h000c:
                        rdata <= protocol_errors;
                    16'h000d:
                        rdata <= event_latch;
                    16'h0010:
                        rdata <= 32'h4F425331;
                    16'h0011:
                        rdata <= 32'd100000000;
                    16'h0012:
                        rdata <= obs_cycles[31 : 0];
                    16'h0013:
                        rdata <= obs_cycles[63 : 32];
                    16'h0014:
                        rdata <= obs_seq;
                    16'h0015:
                        rdata <= {28'b0, obs_valid};
                    16'h0016:
                        rdata <= {28'b0, obs_last_mask};
                    16'h0018:
                        rdata <= obs_assert[31 : 0];
                    16'h0019:
                        rdata <= obs_assert[63 : 32];
                    16'h001a:
                        rdata <= obs_clear[31 : 0];
                    16'h001b:
                        rdata <= obs_clear[63 : 32];
                    16'h001c:
                        rdata <= obs_fault[31 : 0];
                    16'h001d:
                        rdata <= obs_fault[63 : 32];
                    16'h001e:
                        rdata <= obs_recover[31 : 0];
                    16'h001f:
                        rdata <= obs_recover[63 : 32];
                    16'h0020:
                        rdata <= obs_assert_frame;
                    16'h0021:
                        rdata <= obs_clear_frame;
                    16'h0022:
                        rdata <= obs_fault_frame;
                    16'h0023:
                        rdata <= obs_recover_frame;

                    16'h0030:
                        rdata <= 32'hA5050001;
                    default:
                        rdata <= 32'hBAD04960;
                endcase
            end
            if (awvalid && awready) begin
                // 写寄存器：0x00受控业务复位，0x04 BEGIN，0x08/0x0C框，0x10 COMMIT，0x38事件W1C。
                case (awaddr[17 : 2])
                    16'h0000:
                        if (awdata == 1)
                            soft_reset <= 1;
                    // BEGIN重建影子事务；上一事务未完成时增加协议错，不假装其已经提交成功。
                    16'h0001: begin
                        if (txn)
                            protocol_errors <= protocol_errors + 1;
                        txn_age_ms <= 0;
                        txn <= 1;
                        bad <= 0;
                        have_xy <= 0;
                        boxes <= 0;
                        hit <= 0;
                        shadow_frame <= awdata;
                    end
                    // XY0与XY1必须成对顺序写入；重复XY0会标bad，后续COMMIT不能转成正常空帧。
                    16'h0002: begin
                        if (!txn || have_xy) begin
                            bad <= 1;
                            protocol_errors <= protocol_errors + 1;
                        end
                        xy <= awdata;
                        have_xy <= 1;
                    end
                    // 每对合法框更新boxes并累积hit；最多16框，整个事务只要任一框中心命中ROI即可。
                    16'h0003: begin
                        if (!txn || !have_xy || boxes == 16 || !valid_box) begin
                            bad <= 1;
                            protocol_errors <= protocol_errors + 1;
                        end else begin
                            boxes <= boxes + 1;
                            hit <= hit | roi_match;
                        end
                        have_xy <= 0;
                    end
                    // COMMIT编码：[15:8]框数，[0]fresh；其余位必须0且不得缺少半对坐标。
                    // COMMIT先终止事务并检查编码、框数和半对坐标；成功才产生下一拍pulse交给规则核。
                    // 空框事务允许boxes=0，但必须有新BEGIN并fresh=1，不能用缺失输入伪装成空帧。
                    16'h0004: begin
                        txn <= 0;
                        have_xy <= 0;
                        pending <= 1;
                        if (!txn || bad || have_xy || awdata[15 : 8] != {3'b0, boxes} ||
                            |awdata[31 : 16] || |awdata[7 : 1])
                            protocol_errors <= protocol_errors + 1;
                        else begin
                            pulse <= 1;
                            pulse_frame <= shadow_frame;
                            pulse_fresh <= awdata[0];
                            pulse_hit <= hit;
                        end
                    end
                    // 事件写1清除；同周期的新events再OR回来，避免清旧事件时丢失新事件。
                    16'h000e:
                        event_latch <= (event_latch & ~awdata) | {28'b0, events};
                    default:
                        protocol_errors <= protocol_errors + 1;
                endcase
            end
        end
    end
endmodule
