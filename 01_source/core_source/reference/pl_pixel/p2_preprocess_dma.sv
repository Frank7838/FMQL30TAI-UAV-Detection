// 通过DDR读写执行像素预处理。
`timescale 1ns / 1ps
// Experimental PC integration. rst_n = configuration/GSR only.
// STOP/runtime_reset latch quarantine and drain committed/presented beats;
// neither clears owners, local streams, or permanently poisoned responses.
module p2_preprocess_dma
    #(parameter integer ID_WIDTH = 5,
      parameter integer TIMEOUT_CYCLES = 2000000)
    (input wire clk, rst_n, start, stop, clear_status, runtime_reset, cfg_profile_a,
     input wire [31 : 0] cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length,
     output wire busy, read_outstanding, write_outstanding, idle_fenced, abort_drained,
     output reg rejected_start,
     output reg done, error, fault_latched,
     output reg [7 : 0] error_code,
     output reg [31 : 0] bytes_copied,
     output wire [ID_WIDTH - 1 : 0] m_axi_arid,
     output wire [31 : 0] m_axi_araddr,
     output wire [3 : 0] m_axi_arlen,
     output wire [2 : 0] m_axi_arsize,
     output wire [1 : 0] m_axi_arburst,
     output wire [1 : 0] m_axi_arlock,
     output wire [3 : 0] m_axi_arcache,
     output wire [2 : 0] m_axi_arprot,
     output wire [3 : 0] m_axi_arqos,
     output wire m_axi_arvalid,
     input wire m_axi_arready,
     input wire [ID_WIDTH - 1 : 0] m_axi_rid,
     input wire [63 : 0] m_axi_rdata,
     input wire [1 : 0] m_axi_rresp,
     input wire m_axi_rlast, m_axi_rvalid,
     output wire m_axi_rready,
     output wire [ID_WIDTH - 1 : 0] m_axi_awid,
     output wire [31 : 0] m_axi_awaddr,
     output wire [3 : 0] m_axi_awlen,
     output wire [2 : 0] m_axi_awsize,
     output wire [1 : 0] m_axi_awburst,
     output wire [1 : 0] m_axi_awlock,
     output wire [3 : 0] m_axi_awcache,
     output wire [2 : 0] m_axi_awprot,
     output wire [3 : 0] m_axi_awqos,
     output wire m_axi_awvalid,
     input wire m_axi_awready,
     output wire [ID_WIDTH - 1 : 0] m_axi_wid,
     output wire [63 : 0] m_axi_wdata,
     output wire [7 : 0] m_axi_wstrb,
     output wire m_axi_wlast, m_axi_wvalid,
     input wire m_axi_wready,
     input wire [ID_WIDTH - 1 : 0] m_axi_bid,
     input wire [1 : 0] m_axi_bresp,
     input wire m_axi_bvalid,
     output wire m_axi_bready);
    localparam R_IDLE = 0, R_ADDR = 1, R_DATA = 2;
    localparam W_IDLE = 0, W_SEND = 1, W_RESP = 2;
    localparam OUTPUT_BYTES = 32'd1228800;
    reg [1 : 0] rs, ws;
    reg active, read_poison, write_poison, aw_sent, w_sent, last_hold;
    reg [31 : 0] source, destination, remaining, read_timer, write_timer;
    reg [63 : 0] write_data;
    reg [7 : 0] write_keep;
    wire [32 : 0] base_end = {1'b0, cfg_base} + 33'h1000000;
    wire [32 : 0] source_end = {1'b0, cfg_src} + {1'b0, cfg_length};
    wire [32 : 0] dest_end = {1'b0, cfg_dst} + {1'b0, OUTPUT_BYTES};
    wire cfg_ok = (cfg_base >= 32'h30000000) && (base_end <= 33'h3e000000) &&
                  (base_end == {1'b0, cfg_limit}) &&
                  ((cfg_base[2 : 0] | cfg_src[2 : 0] | cfg_dst[2 : 0]) == 0) &&
                  (cfg_length == (cfg_profile_a ? 32'd6220800 : 32'd921600)) &&
                  (cfg_src >= cfg_base) && (source_end <= ({1'b0, cfg_base} + 33'h800000)) &&
                  ({1'b0, cfg_dst} >= ({1'b0, cfg_base} + 33'h800000)) &&
                  (dest_end <= {1'b0, cfg_limit});
    wire r_bad = (m_axi_rid != 0) || (m_axi_rresp != 0) || !m_axi_rlast;
    wire b_bad = (m_axi_bid != 0) || (m_axi_bresp != 0);
    wire r_fault = (rs == R_DATA) && m_axi_rvalid && r_bad;
    wire b_fault = (ws == W_RESP) && m_axi_bvalid && b_bad;
    wire r_timeout = (rs != R_IDLE) && (read_timer >= TIMEOUT_CYCLES - 1);
    wire w_timeout = (ws != W_IDLE) && (write_timer >= TIMEOUT_CYCLES - 1);
    wire stream_error, stream_in_ready, stream_out_valid, stream_out_last;
    wire [63 : 0] stream_out_data;
    wire [7 : 0] stream_out_keep;
    wire stream_busy, stream_done, stream_rejected;
    wire abort_now = fault_latched || stop || runtime_reset || stream_error || r_fault || b_fault ||
                     r_timeout || w_timeout;
    wire stream_start = start && !active && !fault_latched && !stop && !runtime_reset && cfg_ok;
    wire output_accept = active && !abort_now && (ws == W_IDLE);
    wire output_bad = stream_out_valid && output_accept &&
                      ((stream_out_keep != 8'hff) ||
                       (stream_out_last != (bytes_copied == OUTPUT_BYTES - 8)));
    assign busy = active;
    assign read_outstanding = (rs != R_IDLE) || read_poison;
    assign write_outstanding = (ws != W_IDLE) || write_poison;
    assign idle_fenced = !active && !read_outstanding && !write_outstanding && !fault_latched;
    // Diagnostic only. Quarantine still forbids start/free/reuse after abort.
    assign abort_drained = fault_latched && !read_outstanding && !write_outstanding;
    p2_stream64 stream(.clk(clk),
                       .init_resetn(rst_n),
                       .start(stream_start),
                       .profile_a(cfg_profile_a),
                       .in_data(m_axi_rdata),
                       .in_keep(8'hff),
                       .in_valid((rs == R_DATA) && m_axi_rvalid && !abort_now),
                       .in_last(remaining == 8),
                       .in_ready(stream_in_ready),
                       .out_data(stream_out_data),
                       .out_keep(stream_out_keep),
                       .out_valid(stream_out_valid),
                       .out_last(stream_out_last),
                       .out_ready(output_accept),
                       .busy(stream_busy),
                       .done(stream_done),
                       .rejected_start(stream_rejected),
                       .frame_error(stream_error));
    assign m_axi_arid = 0;
    assign m_axi_awid = 0;
    assign m_axi_wid = 0;
    assign m_axi_araddr = source;
    assign m_axi_awaddr = destination;
    assign m_axi_arlen = 0;
    assign m_axi_awlen = 0;
    assign m_axi_arsize = 3;
    assign m_axi_awsize = 3;
    assign m_axi_arburst = 1;
    assign m_axi_awburst = 1;
    assign m_axi_arlock = 0;
    assign m_axi_awlock = 0;
    assign m_axi_arcache = 0;
    assign m_axi_awcache = 0;
    assign m_axi_arprot = 0;
    assign m_axi_awprot = 0;
    assign m_axi_arqos = 0;
    assign m_axi_awqos = 0;
    assign m_axi_arvalid = rs == R_ADDR;
    assign m_axi_rready = (rs == R_DATA) && (abort_now || stream_in_ready);
    assign m_axi_awvalid = (ws == W_SEND) && !aw_sent;
    assign m_axi_wvalid = (ws == W_SEND) && !w_sent;
    assign m_axi_wdata = write_data;
    assign m_axi_wstrb = write_keep;
    assign m_axi_wlast = 1;
    assign m_axi_bready = ws == W_RESP;
    always @(posedge clk) begin
        if (!rst_n) begin
            rs <= R_IDLE;
            ws <= W_IDLE;
            active <= 0;
            done <= 0;
            error <= 0;
            fault_latched <= 0;
            error_code <= 0;
            bytes_copied <= 0;
            rejected_start <= 0;
            source <= 0;
            destination <= 0;
            remaining <= 0;
            read_timer <= 0;
            write_timer <= 0;
            read_poison <= 0;
            write_poison <= 0;
            aw_sent <= 0;
            w_sent <= 0;
            write_data <= 0;
            write_keep <= 0;
            last_hold <= 0;
        end else begin
            rejected_start <= 0;
            if (clear_status && !active && !fault_latched) begin
                done <= 0;
                error <= 0;
                error_code <= 0;
            end
            if (start) begin
                if (active || fault_latched)
                    rejected_start <= 1;
                else if (!cfg_ok || stop || runtime_reset) begin
                    done <= 0;
                    error <= 1;
                    error_code <= !cfg_ok ? 1 : 2;
                end else begin
                    active <= 1;
                    done <= 0;
                    error <= 0;
                    error_code <= 0;
                    bytes_copied <= 0;
                    source <= cfg_src;
                    destination <= cfg_dst;
                    remaining <= cfg_length;
                end
            end
            if (active && (stop || runtime_reset || stream_error || r_fault || b_fault ||
                           r_timeout || w_timeout || output_bad)) begin
                fault_latched <= 1;
                error <= 1;
                done <= 0;
                if (!error) begin
                    if (stop)
                        error_code <= 2;
                    else if (runtime_reset)
                        error_code <= 7;
                    else if (r_timeout || w_timeout)
                        error_code <= 3;
                    else if (r_fault)
                        error_code <= 4;
                    else if (b_fault)
                        error_code <= 5;
                    else
                        error_code <= 6;
                end
            end
            // Progress watchdogs never release an owner or withdraw VALID.
            if (rs == R_IDLE || (m_axi_arvalid && m_axi_arready) || (m_axi_rvalid && m_axi_rready))
                read_timer <= 0;
            else if (!r_timeout)
                read_timer <= read_timer + 1'b1;
            if (ws == W_IDLE || (m_axi_awvalid && m_axi_awready) ||
                (m_axi_wvalid && m_axi_wready) || (m_axi_bvalid && m_axi_bready))
                write_timer <= 0;
            else if (!w_timeout)
                write_timer <= write_timer + 1'b1;
            case (rs)
                R_IDLE:
                    if (active && !abort_now && !output_bad && remaining != 0)
                        rs <= R_ADDR;
                R_ADDR:
                    if (m_axi_arready)
                        rs <= R_DATA;
                R_DATA:
                    if (m_axi_rvalid && m_axi_rready) begin
                        // ID/LAST corruption permanently retains software ownership even
                        // if an apparently valid final response arrives later.
                        if (m_axi_rid != 0 || !m_axi_rlast)
                            read_poison <= 1;
                        if (!read_poison && m_axi_rid == 0 && m_axi_rlast)
                            rs <= R_IDLE;
                        if (!abort_now) begin
                            source <= source + 8;
                            remaining <= remaining - 8;
                        end
                    end
            endcase
            case (ws)
                W_IDLE:
                    if (stream_out_valid && output_accept && !output_bad) begin
                        write_data <= stream_out_data;
                        write_keep <= stream_out_keep;
                        last_hold <= stream_out_last;
                        aw_sent <= 0;
                        w_sent <= 0;
                        ws <= W_SEND;
                    end
                W_SEND: begin
                    if (m_axi_awready && !aw_sent)
                        aw_sent <= 1;
                    if (m_axi_wready && !w_sent)
                        w_sent <= 1;
                    if ((aw_sent || m_axi_awready) && (w_sent || m_axi_wready))
                        ws <= W_RESP;
                end
                W_RESP:
                    if (m_axi_bvalid) begin
                        if (m_axi_bid != 0)
                            write_poison <= 1;
                        if (!write_poison && m_axi_bid == 0) begin
                            ws <= W_IDLE;
                            if (m_axi_bresp == 0) begin
                                bytes_copied <= bytes_copied + 8;
                                destination <= destination + 8;
                                if (last_hold && !abort_now) begin
                                    if (rs == R_IDLE && !read_poison && remaining == 0 &&
                                        bytes_copied == OUTPUT_BYTES - 8) begin
                                        done <= 1;
                                        active <= 0;
                                    end else begin
                                        fault_latched <= 1;
                                        error <= 1;
                                        error_code <= 6;
                                    end
                                end
                            end
                        end
                    end
            endcase
            if (fault_latched && rs == R_IDLE && ws == W_IDLE && !read_poison && !write_poison)
                active <= 0;
        end
    end
endmodule
