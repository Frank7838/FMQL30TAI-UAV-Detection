// 在64位字节流与24位像素之间转换。
`timescale 1ns / 1ps
// Local stream adapters only: no AXI address channel, ownership, or runtime abort.
// Lane zero contains the earliest byte. Invalid input poisons this frame;
// downstream must discard it. init_resetn is configuration initialization only.
module p2_unpack64_to24
    (input wire clk, init_resetn,
     input wire [63 : 0] in_data,
     input wire [7 : 0] in_keep,
     input wire in_valid, in_last,
     output wire in_ready,
     output wire [23 : 0] out_data,
     output wire out_valid, out_last,
     input wire out_ready,
     output reg format_error);
    reg [127 : 0] reservoir;
    reg [4 : 0] count;
    reg end_seen;
    reg [1 : 0] byte_mod;
    wire [3 : 0] beat_bytes = decode_keep(in_keep);
    wire keep_ok = beat_bytes != 0;
    reg [127 : 0] next_reservoir;
    reg [4 : 0] next_count;
    integer k;
    function [3 : 0] decode_keep
        (input [7 : 0] mask);
        case (mask)
            8'h01:
                decode_keep = 1;
            8'h03:
                decode_keep = 2;
            8'h07:
                decode_keep = 3;
            8'h0f:
                decode_keep = 4;
            8'h1f:
                decode_keep = 5;
            8'h3f:
                decode_keep = 6;
            8'h7f:
                decode_keep = 7;
            8'hff:
                decode_keep = 8;
            default:
                decode_keep = 0;
        endcase
    endfunction
    assign in_ready = !format_error && !end_seen && (count <= 8);
    assign out_valid = !format_error && (count >= 3);
    assign out_data = reservoir[23 : 0];
    assign out_last = end_seen && (count == 3);
    // Conservative space reservation permits simultaneous push/pop but does not
    // depend combinationally on downstream ready. Stalled low bytes never change.
    always @* begin
        next_reservoir = reservoir;
        next_count = count;
        if (out_valid && out_ready) begin
            next_reservoir = reservoir >> 24;
            next_count = count - 3;
        end
        if (in_valid && in_ready) begin
            for (k = 0; k < 8; k = k + 1)
                if (k < beat_bytes)
                    next_reservoir[(next_count + k) * 8 +: 8] = in_data[k * 8 +: 8];
            next_count = next_count + beat_bytes;
        end
    end
    always @(posedge clk) begin
        if (!init_resetn) begin
            reservoir <= 0;
            count <= 0;
            end_seen <= 0;
            byte_mod <= 0;
            format_error <= 0;
        end else begin
            reservoir <= next_reservoir;
            count <= next_count;
            if (out_valid && out_ready && out_last) begin
                end_seen <= 0;
                byte_mod <= 0;
            end
            if (in_valid && in_ready) begin
                byte_mod <= (byte_mod + beat_bytes) % 3;
                if (!keep_ok || (!in_last && in_keep != 8'hff) ||
                    (in_last && ((byte_mod + beat_bytes) % 3 != 0)))
                    format_error <= 1;
                if (in_last)
                    end_seen <= 1;
            end
        end
    end
endmodule

module p2_pack24_to64
    (input wire clk, init_resetn,
     input wire [23 : 0] in_data,
     input wire in_valid, in_last,
     output wire in_ready,
     output wire [63 : 0] out_data,
     output wire [7 : 0] out_keep,
     output wire out_valid, out_last,
     input wire out_ready);
    reg [127 : 0] reservoir;
    reg [4 : 0] count;
    reg end_seen;
    reg [127 : 0] next_reservoir;
    reg [4 : 0] next_count;
    wire [3 : 0] emit_bytes = count >= 8 ? 8 : count[3 : 0];
    assign in_ready = !end_seen && (count <= 8);
    assign out_valid = (count >= 8) || (end_seen && count != 0);
    assign out_data = reservoir[63 : 0];
    assign out_keep = count >= 8 ? 8'hff : ((9'h001 << count) - 1'b1);
    assign out_last = end_seen && (count <= 8);
    always @* begin
        next_reservoir = reservoir;
        next_count = count;
        if (out_valid && out_ready) begin
            next_reservoir = reservoir >> (emit_bytes * 8);
            next_count = count - emit_bytes;
        end
        if (in_valid && in_ready) begin
            next_reservoir[next_count * 8 +: 24] = in_data;
            next_count = next_count + 3;
        end
    end
    always @(posedge clk) begin
        if (!init_resetn) begin
            reservoir <= 0;
            count <= 0;
            end_seen <= 0;
        end else begin
            reservoir <= next_reservoir;
            count <= next_count;
            if (out_valid && out_ready && out_last)
                end_seen <= 0;
            if (in_valid && in_ready && in_last)
                end_seen <= 1;
        end
    end
endmodule
