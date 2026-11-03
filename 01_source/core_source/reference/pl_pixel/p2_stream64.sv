// 将像素处理连接到64位输入输出流。
`timescale 1ns / 1ps
// Experimental stream wrapper. All memories/bus ownership remain outside it.
// done denotes the FINAL 64-bit output beat handshake, not pixel-core done.
module p2_stream64
    (input wire clk, init_resetn, start, profile_a,
     input wire [63 : 0] in_data,
     input wire [7 : 0] in_keep,
     input wire in_valid, in_last,
     output wire in_ready,
     output wire [63 : 0] out_data,
     output wire [7 : 0] out_keep,
     output wire out_valid, out_last,
     input wire out_ready,
     output reg busy, done, rejected_start,
     output wire frame_error);
    wire [23 : 0] bgr, rgb;
    wire uv, ul, ur, up_ready, unpack_error, pv, pl, pr, core_ready;
    wire core_busy, core_done, core_rejected;
    reg mode_a, input_done, length_error;
    reg [21 : 0] input_pixels;
    wire accept_start = start && !busy && !frame_error;
    assign frame_error = unpack_error || length_error;
    assign in_ready = busy && !input_done && !frame_error && up_ready;
    p2_unpack64_to24 unpacker(.clk(clk),
                              .init_resetn(init_resetn),
                              .in_data(in_data),
                              .in_keep(in_keep),
                              .in_valid(in_valid && busy && !input_done && !frame_error),
                              .in_last(in_last),
                              .in_ready(up_ready),
                              .out_data(bgr),
                              .out_valid(uv),
                              .out_last(ul),
                              .out_ready(ur),
                              .format_error(unpack_error));
    assign ur = core_ready && !frame_error;
    p2_pixel_stream pixels(.clk(clk),
                           .init_resetn(init_resetn),
                           .start(accept_start),
                           .profile_a(profile_a),
                           .in_bgr(bgr),
                           .in_valid(uv && !frame_error),
                           .in_ready(core_ready),
                           .out_rgb(rgb),
                           .out_valid(pv),
                           .out_last(pl),
                           .out_ready(pr && !frame_error),
                           .busy(core_busy),
                           .done(core_done),
                           .rejected_start(core_rejected));
    p2_pack24_to64 packer(.clk(clk),
                          .init_resetn(init_resetn),
                          .in_data(rgb),
                          .in_valid(pv && !frame_error),
                          .in_last(pl),
                          .in_ready(pr),
                          .out_data(out_data),
                          .out_keep(out_keep),
                          .out_valid(out_valid),
                          .out_last(out_last),
                          .out_ready(out_ready));
    always @(posedge clk) begin
        if (!init_resetn) begin
            busy <= 0;
            done <= 0;
            rejected_start <= 0;
            mode_a <= 0;
            input_done <= 0;
            input_pixels <= 0;
            length_error <= 0;
        end else begin
            done <= 0;
            rejected_start <= 0;
            if (start) begin
                if (busy || frame_error)
                    rejected_start <= 1;
                else begin
                    busy <= 1;
                    mode_a <= profile_a;
                    input_done <= 0;
                    input_pixels <= 0;
                end
            end
            if (uv && ur) begin
                input_pixels <= input_pixels + 1'b1;
                if (ul != (input_pixels == (mode_a ? 2073599 : 307199)))
                    length_error <= 1;
                if (ul)
                    input_done <= 1;
            end
            if (out_valid && out_ready && out_last && !frame_error) begin
                busy <= 0;
                done <= 1;
            end
        end
    end
endmodule
