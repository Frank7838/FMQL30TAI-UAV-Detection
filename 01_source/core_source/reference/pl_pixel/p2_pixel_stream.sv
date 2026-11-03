// 逐像素完成固定取样、颜色转换和补边。
`timescale 1ns / 1ps
// PC prototype. No memory master or register integration. Reset is configuration
// initialization only; a future DMA adapter must drain bus ownership separately.
// Byte lanes: input [7:0]=B,[15:8]=G,[23:16]=R;
// output [7:0]=R,[15:8]=G,[23:16]=B. All frames are top-left aligned.
module p2_pixel_stream
    (input wire clk, init_resetn, start, profile_a,
     input wire [23 : 0] in_bgr,
     input wire in_valid,
     output wire in_ready,
     output reg [23 : 0] out_rgb,
     output reg out_valid, out_last,
     input wire out_ready,
     output reg busy, done, rejected_start);
    localparam INPUT = 0, PAD = 1, DRAIN = 2;
    reg [1 : 0] phase;
    reg mode_a;
    reg [10 : 0] x, y;
    reg [1 : 0] xm, ym;
    reg [17 : 0] pad_left;
    wire selected = !mode_a || (xm == 1 && ym == 1);
    wire space = !out_valid || out_ready;
    assign in_ready = busy && phase == INPUT && (!selected || space);
    always @(posedge clk) begin
        if (!init_resetn) begin
            phase <= INPUT;
            mode_a <= 0;
            x <= 0;
            y <= 0;
            xm <= 0;
            ym <= 0;
            pad_left <= 0;
            out_rgb <= 0;
            out_valid <= 0;
            out_last <= 0;
            busy <= 0;
            done <= 0;
            rejected_start <= 0;
        end else begin
            done <= 0;
            rejected_start <= 0;
            if (out_valid && out_ready) begin
                out_valid <= 0;
                if (out_last) begin
                    busy <= 0;
                    done <= 1;
                end
            end
            if (start) begin
                if (busy)
                    rejected_start <= 1;
                else begin
                    busy <= 1;
                    phase <= INPUT;
                    mode_a <= profile_a;
                    x <= 0;
                    y <= 0;
                    xm <= 0;
                    ym <= 0;
                    pad_left <= 0;
                    out_valid <= 0;
                    out_last <= 0;
                end
            end
            if (in_valid && in_ready) begin
                if (selected) begin
                    out_rgb <= {in_bgr[7 : 0], in_bgr[15 : 8], in_bgr[23 : 16]};
                    out_valid <= 1;
                    out_last <= 0;
                end
                if (x == (mode_a ? 1919 : 639)) begin
                    x <= 0;
                    xm <= 0;
                    y <= y + 1'b1;
                    ym <= ym == 2 ? 0 : ym + 1'b1;
                    if (y == (mode_a ? 1079 : 479)) begin
                        phase <= PAD;
                        pad_left <= mode_a ? 179200 : 102400;
                    end
                end else begin
                    x <= x + 1'b1;
                    xm <= xm == 2 ? 0 : xm + 1'b1;
                end
            end
            if (busy && phase == PAD && space) begin
                out_rgb <= 24'h727272;
                out_valid <= 1;
                out_last <= pad_left == 1;
                pad_left <= pad_left - 1'b1;
                if (pad_left == 1)
                    phase <= DRAIN;
            end
        end
    end
endmodule
