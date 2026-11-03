// 在实际100MHz gp_clk域产生1ms时基；计数分级以减小比较链。
// 不是CPU定时器，因此无新提交时仍能置故障，前提是该时钟与供电仍在。

module alarm_tick_100m
    (input wire clk, reset,
     output wire tick_ms);
    // 两级计数及提前一拍比较形成100MHz→1ms脉冲。lo_last/hi_last是寄存器，
    // 比较常量与非阻塞赋值配套，不能当成组合计数器直接改成999/99；周期由tb_autonomous_tick核对。
    reg [9 : 0] lo = 0;
    reg [6 : 0] hi = 0;
    reg lo_last = 0, hi_last = 0;
    assign tick_ms = lo_last && hi_last;
    always @(posedge clk) begin
        if (reset) begin
            lo <= 0;
            hi <= 0;
            lo_last <= 0;
            hi_last <= 0;
        end else begin
            lo_last <= lo == 998;
            lo <= lo_last ? 0 : lo + 1'b1;
            if (lo_last) begin
                hi_last <= hi == 98;
                hi <= hi_last ? 0 : hi + 1'b1;
            end
        end
    end
endmodule
