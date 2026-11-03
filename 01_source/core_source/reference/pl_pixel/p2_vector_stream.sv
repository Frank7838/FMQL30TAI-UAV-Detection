// 按字节流组织固定模式的向量像素处理。
`timescale 1ns / 1ps
// PC candidate only. AXI/DDR/ownership live outside this component.
// A ingress: BGR rows y=3*j+1 (j=0..359), all1920 pixels per row.
// Future row-stride DMA must enforce that contract. B ingress:640x480 BGR.
// Input/output:64-bit little-endian byte stream. Configuration reset only.
module p2_vector_stream
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
     output reg frame_error);
    reg mode_a, padding;
    reg [255 : 0] ibuf, obuf;
    reg [5 : 0] icount, ocount;
    reg [31 : 0] accepted, consumed, produced, emitted;
    reg [1 : 0] group_mod;
    reg [255 : 0] next_ibuf, next_obuf;
    reg [5 : 0] next_icount, next_ocount;
    reg [191 : 0] converted;
    integer bytes_out, k;
    wire [31 : 0] expected_input = mode_a ? 32'd2073600 : 32'd921600;
    wire [31 : 0] image_bytes = mode_a ? 32'd691200 : 32'd921600;
    wire emit = out_valid && out_ready;
    wire convert = busy && !frame_error && !padding && icount >= 24 && ocount <= 8;
    wire pad = busy && !frame_error && padding && produced < 1228800 && ocount <= 24;
    wire receive = in_valid && in_ready;
    assign in_ready = busy && !frame_error && !padding && accepted < expected_input && icount <= 24;
    assign out_valid = busy && !frame_error && ocount >= 8;
    assign out_data = obuf[63 : 0];
    assign out_keep = 8'hff;
    assign out_last = emitted == 32'd1228792;
    function [23 : 0] swap
        (input [23 : 0] p);
        swap = {p[7 : 0], p[15 : 8], p[23 : 16]};
    endfunction
    always @* begin
        converted = 0;
        bytes_out = 0;
        if (!mode_a) begin
            for (k = 0; k < 8; k = k + 1)
                converted[k * 24 +: 24] = swap(ibuf[k * 24 +: 24]);
            bytes_out = 24;
        end else
            case (group_mod)
                0: begin
                    converted[71 : 0] = {swap(ibuf[168 +: 24]), swap(ibuf[96 +: 24]),
                                         swap(ibuf[24 +: 24])};
                    bytes_out = 9;
                end
                2: begin
                    converted[47 : 0] = {swap(ibuf[120 +: 24]), swap(ibuf[48 +: 24])};
                    bytes_out = 6;
                end
                1: begin
                    converted[71 : 0] = {swap(ibuf[144 +: 24]), swap(ibuf[72 +: 24]),
                                         swap(ibuf[0 +: 24])};
                    bytes_out = 9;
                end
                default: begin
                    converted = 0;
                    bytes_out = 0;
                end
            endcase
        next_ibuf = ibuf;
        next_icount = icount;
        next_obuf = obuf;
        next_ocount = ocount;
        if (emit) begin
            next_obuf = obuf >> 64;
            next_ocount = ocount - 8;
        end
        if (convert) begin
            next_ibuf = ibuf >> 192;
            next_icount = icount - 24;
            next_obuf = next_obuf | ({64'd0, converted} << (next_ocount * 8));
            next_ocount = next_ocount + bytes_out;
        end
        if (pad) begin
            next_obuf = next_obuf | ({192'd0, 64'h7272727272727272} << (next_ocount * 8));
            next_ocount = next_ocount + 8;
        end
        if (receive) begin
            next_ibuf = next_ibuf | ({192'd0, in_data} << (next_icount * 8));
            next_icount = next_icount + 8;
        end
    end
    always @(posedge clk) begin
        if (!init_resetn) begin
            busy <= 0;
            done <= 0;
            rejected_start <= 0;
            frame_error <= 0;
            mode_a <= 0;
            padding <= 0;
            ibuf <= 0;
            obuf <= 0;
            icount <= 0;
            ocount <= 0;
            accepted <= 0;
            consumed <= 0;
            produced <= 0;
            emitted <= 0;
            group_mod <= 0;
        end else begin
            done <= 0;
            rejected_start <= 0;
            ibuf <= next_ibuf;
            obuf <= next_obuf;
            icount <= next_icount;
            ocount <= next_ocount;
            if (start) begin
                if (busy || frame_error)
                    rejected_start <= 1;
                else begin
                    busy <= 1;
                    mode_a <= profile_a;
                    padding <= 0;
                    ibuf <= 0;
                    obuf <= 0;
                    icount <= 0;
                    ocount <= 0;
                    accepted <= 0;
                    consumed <= 0;
                    produced <= 0;
                    emitted <= 0;
                    group_mod <= 0;
                end
            end
            if (receive) begin
                if (in_keep != 8'hff || in_last != (accepted == expected_input - 8))
                    frame_error <= 1;
                accepted <= accepted + 8;
            end
            if (convert) begin
                consumed <= consumed + 24;
                produced <= produced + bytes_out;
                group_mod <= group_mod == 0 ? 2 : group_mod == 2 ? 1 : 0;
                if (consumed == expected_input - 24) begin
                    if (produced + bytes_out != image_bytes || accepted != expected_input ||
                        icount != 24)
                        frame_error <= 1;
                    padding <= 1;
                end
            end
            if (pad)
                produced <= produced + 8;
            if (emit) begin
                emitted <= emitted + 8;
                if (out_last) begin
                    if (produced != 1228800 || icount != 0 || ocount != 8 || !padding)
                        frame_error <= 1;
                    else begin
                        busy <= 0;
                        done <= 1;
                    end
                end
            end
        end
    end
endmodule
