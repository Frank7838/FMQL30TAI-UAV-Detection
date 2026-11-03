// 通过AXI3读写DDR，完成图像原样搬运。
`timescale 1ns / 1ps
// Original P1 identity-copy engine. AXI3-style HP2, one 64-bit beat outstanding.
// rst_n MUST reset/quiesce the connected interconnect/slave too. Never use a
// software-only local reset to discard an outstanding AXI transaction.
module p1_copy_dma
    #(parameter integer ID_WIDTH = 5,
      parameter integer TIMEOUT_CYCLES = 2000000)
    (input wire clk, rst_n, start, stop, clear_status,
     input wire [31 : 0] cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length,
     output wire busy, read_outstanding, write_outstanding,
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
    localparam IDLE = 0, AR = 1, RD = 2, WR = 3, BR = 4, LOCKED = 5;
    // Codes: 1=range/config,2=stop,3=timeout,4=read/protocol,5=write.
    reg [2 : 0] state;
    reg [31 : 0] src, dst, remaining;
    reg [63 : 0] data_hold;
    reg aw_sent, w_sent, aborted;
    reg [31 : 0] timer;
    wire [3 : 0] beat_bytes = (remaining >= 8) ? 4'd8 : {1'b0, remaining[2 : 0]};
    wire [32 : 0] base_end = {1'b0, cfg_base} + 33'h1000000;
    wire [32 : 0] src_end = {1'b0, cfg_src} + {1'b0, cfg_length};
    wire [32 : 0] dst_end = {1'b0, cfg_dst} + {1'b0, cfg_length};
    // Address ranges are exclusive-end, exactly one independent 16MiB buffer.
    // Source and destination must be contained in separate fixed 8MiB slots.
    // 8-byte starts allow arbitrary byte lengths via the final write strobe.
    wire cfg_ok = (cfg_base >= 32'h30000000) && (base_end <= 33'h3e000000) &&
                  (cfg_base[2 : 0] == 0) && (cfg_src[2 : 0] == 0) && (cfg_dst[2 : 0] == 0) &&
                  (cfg_length != 0) && !base_end[32] && (base_end == {1'b0, cfg_limit}) &&
                  (cfg_src >= cfg_base) && (src_end <= ({1'b0, cfg_base} + 33'h800000)) &&
                  !src_end[32] && ({1'b0, cfg_dst} >= ({1'b0, cfg_base} + 33'h800000)) &&
                  (dst_end <= {1'b0, cfg_limit}) && !dst_end[32] &&
                  ((src_end <= {1'b0, cfg_dst}) || (dst_end <= {1'b0, cfg_src}));
    wire timed_out = (timer >= TIMEOUT_CYCLES - 1);
    wire cancel = aborted || stop || timed_out || fault_latched;
    wire rd_bad = (m_axi_rresp != 0) || (m_axi_rid != {ID_WIDTH{1'b0}}) || !m_axi_rlast;
    wire br_bad = (m_axi_bresp != 0) || (m_axi_bid != {ID_WIDTH{1'b0}});
    assign busy = (state != IDLE && state != LOCKED);
    assign read_outstanding = (state == AR || state == RD);
    assign write_outstanding = (state == WR || state == BR);
    assign m_axi_wid = 0;
    assign m_axi_arid = 0;
    assign m_axi_awid = 0;
    assign m_axi_araddr = src;
    assign m_axi_awaddr = dst;
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
    assign m_axi_arvalid = (state == AR);
    assign m_axi_rready = (state == RD);
    assign m_axi_awvalid = (state == WR && !aw_sent);
    assign m_axi_wvalid = (state == WR && !w_sent);
    assign m_axi_wdata = data_hold;
    assign m_axi_wstrb = (9'h1 << beat_bytes) - 1;
    assign m_axi_wlast = 1;
    assign m_axi_bready = (state == BR);
    always @(posedge clk) begin
        if (!rst_n) begin
            state <= IDLE;
            done <= 0;
            error <= 0;
            error_code <= 0;
            fault_latched <= 0;
            src <= 0;
            dst <= 0;
            remaining <= 0;
            bytes_copied <= 0;
            data_hold <= 0;
            aw_sent <= 0;
            w_sent <= 0;
            aborted <= 0;
            timer <= 0;
        end else begin
            if (state == IDLE) begin
                timer <= 0;
                if (clear_status) begin
                    done <= 0;
                    error <= 0;
                    error_code <= 0;
                end
                if (start) begin
                    done <= 0;
                    error <= 0;
                    error_code <= 0;
                    bytes_copied <= 0;
                    aborted <= 0;
                    if (!cfg_ok) begin
                        done <= 1;
                        error <= 1;
                        error_code <= 1;
                    end else if (stop) begin
                        done <= 1;
                        error <= 1;
                        error_code <= 2;
                    end else begin
                        src <= cfg_src;
                        dst <= cfg_dst;
                        remaining <= cfg_length;
                        state <= AR;
                    end
                end
            end else if (state != LOCKED) begin
                if (!timed_out)
                    timer <= timer + 1;
                if (stop) begin
                    aborted <= 1;
                    error <= 1;
                    if (!error)
                        error_code <= 2;
                end
                if (timed_out) begin
                    aborted <= 1;
                    error <= 1;
                    fault_latched <= 1;
                    if (!error)
                        error_code <= 3;
                end
                case (state)
                    AR:
                        if (m_axi_arready) begin
                            state <= RD;
                            timer <= 0;
                        end
                    RD:
                        if (m_axi_rvalid) begin
                            timer <= 0;
                            if (rd_bad) begin
                                error <= 1;
                                fault_latched <= 1;
                                aborted <= 1;
                                if (!error)
                                    error_code <= 4;
                            end
                            // A broken multi-beat response is drained until RLAST, never reused.
                            if (m_axi_rlast) begin
                                if (cancel || rd_bad) begin
                                    done <= 1;
                                    state <= (fault_latched || timed_out || rd_bad) ? LOCKED : IDLE;
                                end else begin
                                    data_hold <= m_axi_rdata;
                                    aw_sent <= 0;
                                    w_sent <= 0;
                                    state <= WR;
                                end
                            end
                        end
                    WR: begin
                        if (m_axi_awready && !aw_sent) begin
                            aw_sent <= 1;
                            timer <= 0;
                        end
                        if (m_axi_wready && !w_sent) begin
                            w_sent <= 1;
                            timer <= 0;
                        end
                        // Once VALID is asserted it stays asserted, including after stop/timeout.
                        if ((aw_sent || m_axi_awready) && (w_sent || m_axi_wready)) begin
                            state <= BR;
                            timer <= 0;
                        end
                    end
                    BR:
                        if (m_axi_bvalid) begin
                            timer <= 0;
                            if (br_bad) begin
                                error <= 1;
                                fault_latched <= 1;
                                aborted <= 1;
                                if (!error)
                                    error_code <= 5;
                            end
                            if (!br_bad)
                                bytes_copied <= bytes_copied + beat_bytes;
                            if (cancel || br_bad) begin
                                done <= 1;
                                state <= (fault_latched || timed_out || br_bad) ? LOCKED : IDLE;
                            end else if (remaining <= 8) begin
                                done <= 1;
                                state <= IDLE;
                            end else begin
                                src <= src + 8;
                                dst <= dst + 8;
                                remaining <= remaining - 8;
                                state <= AR;
                            end
                        end
                endcase
            end
            // LOCKED ignores start/clear. Only an externally coordinated bus reset
            // may clear this isolation state; timeout does NOT declare bus quiescent.
        end
    end
endmodule
