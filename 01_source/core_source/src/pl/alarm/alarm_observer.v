// 四类事件各保存最近一次时间与帧号；bit0到3依次为置警、清警、断流、恢复。
// Bounded read-only event history. No feedback into the alarm decision.
// Clock is the existing 100MHz business clock. Counter epoch is business_reset,
// not UTC, and is not reset by NPU/bus reset, event W1C or business soft-clear.
module alarm_observer
    (input wire clk, reset,
     input wire [3 : 0] events,
     input wire [31 : 0] event_frame,
     output reg [63 : 0] cycles,
     output reg [31 : 0] event_seq,
     output reg [3 : 0] valid_mask, last_mask,
     output reg [63 : 0] assert_cycles, clear_cycles, fault_cycles, recover_cycles,
     output reg [31 : 0] assert_frame, clear_frame, fault_frame, recover_frame);
    // Core events are registered on the same edge as the state transition.
    // We observe them one edge later; subtract one to identify that transition's
    // pre-edge cycle count. Publication is delayed one clock (10ns), while the
    // recorded timestamp corresponds to the actual core state transition.
    // 事件晚一个时钟送到这里，减1后对应状态实际变化的周期。
    // 只读观察支路：没有反馈到alarm/fault；64位周期数以业务复位为起点。
    // valid_mask区分尚未发生的事件，last_mask保留同周期可能同时出现的事件。
    wire [63 : 0] event_cycles = cycles - 64'd1;
    always @(posedge clk) begin
        if (reset) begin
            cycles <= 0;
            event_seq <= 0;
            valid_mask <= 0;
            last_mask <= 0;
            assert_cycles <= 0;
            clear_cycles <= 0;
            fault_cycles <= 0;
            recover_cycles <= 0;
            assert_frame <= 0;
            clear_frame <= 0;
            fault_frame <= 0;
            recover_frame <= 0;
        end else begin
            cycles <= cycles + 64'd1;
            if (|events) begin
                // One sequence increment per transition clock; recovery and
                // alarm assertion may occur together and retain both records.
                event_seq <= event_seq + 32'd1;
                valid_mask <= valid_mask | events;
                last_mask <= events;
                if (events[0]) begin
                    assert_cycles <= event_cycles;
                    assert_frame <= event_frame;
                end
                if (events[1]) begin
                    clear_cycles <= event_cycles;
                    clear_frame <= event_frame;
                end
                if (events[2]) begin
                    fault_cycles <= event_cycles;
                    fault_frame <= event_frame;
                end
                if (events[3]) begin
                    recover_cycles <= event_cycles;
                    recover_frame <= event_frame;
                end
            end
        end
    end
endmodule
