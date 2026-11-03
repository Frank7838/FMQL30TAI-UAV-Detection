// 已上板业务状态机：有效新帧、ROI命中3次置位、无命中8次解除。
// 计数的是有效推理观测，不是相机全部帧；3/8为经验防抖常数，属于当前演示配置。
// 有效提交中断1000ms置fault并保留原alarm；新完整有效帧恢复fault。

// Business logic: fresh ROI observations, 3 positive / 8 empty, stream fault.
module region_alarm_core
    #(parameter integer TIMEOUT_MS = 1000)
    (input wire clk, reset, tick_ms, commit, fresh, region_hit,
     input wire [31 : 0] frame_id,
     output reg alarm, fault, have_frame,
     output reg [3 : 0] positive_run, empty_run,
     output reg [31 : 0] last_frame, accepted_count, rejected_count,
     output reg [3 : 0] events);
    // 接口中的commit为一拍脉冲，fresh由事务层提供；本核只接收完整结果的ROI布尔量。
    // 输出events[0/1/2/3]依次为置警、解除、断流、恢复，一拍后由寄存器层锁存。
    // alarm和fault是独立状态：故障不表示无目标，断流不能自动清除先前告警。
    reg [15 : 0] age_ms;
    // 32位序号以模差判断新旧；接受正向半区间，重复或倒退不参与计数。
    wire [31 : 0] delta = frame_id - last_frame;
    // 首帧可接受；后续delta需位于正向半个32位序号空间，重复或倒退只增加拒绝计数。
    wire accept = commit && fresh && (!have_frame || (delta != 0 && !delta[31]));
    // 跳号会打断连续性，避免未知/丢失观测被当作连续命中或连续空帧。
    wire consecutive = have_frame && delta == 1;
    always @(posedge clk) begin
        if (reset) begin
            alarm <= 0;
            fault <= 0;
            have_frame <= 0;
            positive_run <= 0;
            empty_run <= 0;
            last_frame <= 0;
            accepted_count <= 0;
            rejected_count <= 0;
            age_ms <= 0;
            events <= 0;
        end else begin
            // 每个事件仅脉冲一拍；当周期没有新的业务事件就回到0，软件读取锁存寄存器。
            events <= 0;
            if (commit && !accept)
                rejected_count <= rejected_count + 1;
            // 有效新提交优先于同周期时基；接受帧后重新计断流年龄。
            if (accept) begin
                accepted_count <= accepted_count + 1;
                last_frame <= frame_id;
                have_frame <= 1;
                age_ms <= 0;
                if (fault)
                    events[3] <= 1;
                fault <= 0;
                // 非阻塞赋值右侧是上一周期计数，因此positive_run>=2时，当前命中是连续第3次。
                // 计数饱和于3/8，避免运行时间越长计数越大。
                if (region_hit) begin
                    empty_run <= 0;
                    positive_run <=
                        consecutive && positive_run < 3 ? positive_run + 1 : (consecutive ? 3 : 1);
                    if (consecutive && positive_run >= 2 && !alarm) begin
                        alarm <= 1;
                        events[0] <= 1;
                    end
                end else begin
                    positive_run <= 0;
                    empty_run <=
                        consecutive && empty_run < 8 ? empty_run + 1 : (consecutive ? 8 : 1);
                    if (consecutive && empty_run >= 7 && alarm) begin
                        alarm <= 0;
                        events[1] <= 1;
                    end
                end
                // fault与alarm分离：未观测不等于无目标，故障时保持上次告警。
            // 1ms业务时基独立于CPU提交；超时入口清连续计数但保留alarm，下一有效帧清fault。
            end else if (tick_ms && !fault) begin
                if (age_ms == TIMEOUT_MS - 1) begin
                    fault <= 1;
                    positive_run <= 0;
                    empty_run <= 0;
                    events[2] <= 1;
                    // Hold the last alarm during loss of observations; fault is separate.
                end else
                    age_ms <= age_ms + 1;
            end
        end
    end
endmodule
