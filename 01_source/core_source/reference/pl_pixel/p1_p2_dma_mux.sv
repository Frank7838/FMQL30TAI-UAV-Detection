// 按启动配置选择原样搬运或像素预处理。
`timescale 1ns / 1ps
// Original P1 identity-copy engine. AXI3-style HP2, one 64-bit beat outstanding.
// rst_n MUST reset/quiesce the connected interconnect/slave too. Never use a
// software-only local reset to discard an outstanding AXI transaction.
module p1_p2_dma_mux
    #(parameter integer ID_WIDTH = 5,
      parameter integer TIMEOUT_CYCLES = 2000000)
    (input wire clk, rst_n, start, stop, clear_status, runtime_reset,
     input wire [1 : 0] cfg_mode,
     input wire [31 : 0] cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length,
     output wire busy, read_outstanding, write_outstanding,
     output wire idle_fenced, abort_drained, rejected_start,
     output wire done, error, fault_latched,
     output wire [7 : 0] error_code,
     output wire [31 : 0] bytes_copied,
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
    // Mode0=P1 identity copy; mode1=P2 B; mode2=P2 A. Mode is latched
    // at a legal START, so changing the live config cannot switch AXI ownership.
    reg [1 : 0] active_mode;
    reg reject, invalid_done, invalid_error;
    wire can_start = !busy && !read_outstanding && !write_outstanding && !fault_latched && !stop &&
                     !runtime_reset;
    wire select_pixel = active_mode != 0;
    wire cp_busy;
    wire cp_read_outstanding;
    wire cp_write_outstanding;
    wire cp_done;
    wire cp_error;
    wire cp_fault_latched;
    wire [7 : 0] cp_error_code;
    wire [31 : 0] cp_bytes_copied;
    wire [ID_WIDTH - 1 : 0] cp_m_axi_arid;
    wire [31 : 0] cp_m_axi_araddr;
    wire [3 : 0] cp_m_axi_arlen;
    wire [2 : 0] cp_m_axi_arsize;
    wire [1 : 0] cp_m_axi_arburst;
    wire [1 : 0] cp_m_axi_arlock;
    wire [3 : 0] cp_m_axi_arcache;
    wire [2 : 0] cp_m_axi_arprot;
    wire [3 : 0] cp_m_axi_arqos;
    wire cp_m_axi_arvalid;
    wire cp_m_axi_rready;
    wire [ID_WIDTH - 1 : 0] cp_m_axi_awid;
    wire [31 : 0] cp_m_axi_awaddr;
    wire [3 : 0] cp_m_axi_awlen;
    wire [2 : 0] cp_m_axi_awsize;
    wire [1 : 0] cp_m_axi_awburst;
    wire [1 : 0] cp_m_axi_awlock;
    wire [3 : 0] cp_m_axi_awcache;
    wire [2 : 0] cp_m_axi_awprot;
    wire [3 : 0] cp_m_axi_awqos;
    wire cp_m_axi_awvalid;
    wire [ID_WIDTH - 1 : 0] cp_m_axi_wid;
    wire [63 : 0] cp_m_axi_wdata;
    wire [7 : 0] cp_m_axi_wstrb;
    wire cp_m_axi_wlast;
    wire cp_m_axi_wvalid;
    wire cp_m_axi_bready;
    wire px_busy;
    wire px_read_outstanding;
    wire px_write_outstanding;
    wire px_done;
    wire px_error;
    wire px_fault_latched;
    wire [7 : 0] px_error_code;
    wire [31 : 0] px_bytes_copied;
    wire [ID_WIDTH - 1 : 0] px_m_axi_arid;
    wire [31 : 0] px_m_axi_araddr;
    wire [3 : 0] px_m_axi_arlen;
    wire [2 : 0] px_m_axi_arsize;
    wire [1 : 0] px_m_axi_arburst;
    wire [1 : 0] px_m_axi_arlock;
    wire [3 : 0] px_m_axi_arcache;
    wire [2 : 0] px_m_axi_arprot;
    wire [3 : 0] px_m_axi_arqos;
    wire px_m_axi_arvalid;
    wire px_m_axi_rready;
    wire [ID_WIDTH - 1 : 0] px_m_axi_awid;
    wire [31 : 0] px_m_axi_awaddr;
    wire [3 : 0] px_m_axi_awlen;
    wire [2 : 0] px_m_axi_awsize;
    wire [1 : 0] px_m_axi_awburst;
    wire [1 : 0] px_m_axi_awlock;
    wire [3 : 0] px_m_axi_awcache;
    wire [2 : 0] px_m_axi_awprot;
    wire [3 : 0] px_m_axi_awqos;
    wire px_m_axi_awvalid;
    wire [ID_WIDTH - 1 : 0] px_m_axi_wid;
    wire [63 : 0] px_m_axi_wdata;
    wire [7 : 0] px_m_axi_wstrb;
    wire px_m_axi_wlast;
    wire px_m_axi_wvalid;
    wire px_m_axi_bready;
    wire px_rejected;
    assign busy = cp_busy | px_busy;
    assign read_outstanding = cp_read_outstanding | px_read_outstanding;
    assign write_outstanding = cp_write_outstanding | px_write_outstanding;
    assign fault_latched = cp_fault_latched | px_fault_latched;
    assign done = invalid_done | (select_pixel ? px_done : cp_done);
    assign error = invalid_error | (select_pixel ? px_error : cp_error);
    assign error_code = invalid_error ? 8'd1 : (select_pixel ? px_error_code : cp_error_code);
    assign bytes_copied = select_pixel ? px_bytes_copied : cp_bytes_copied;
    assign idle_fenced = !busy && !read_outstanding && !write_outstanding && !fault_latched;
    assign abort_drained = fault_latched && !read_outstanding && !write_outstanding;
    assign rejected_start = reject | px_rejected;
    always @(posedge clk) begin
        if (!rst_n) begin
            active_mode <= 0;
            reject <= 0;
            invalid_done <= 0;
            invalid_error <= 0;
        end else begin
            reject <= 0;
            if (clear_status && !busy && !fault_latched) begin
                invalid_done <= 0;
                invalid_error <= 0;
            end
            if (start) begin
                if (!can_start)
                    reject <= 1;
                else if (cfg_mode == 3) begin
                    reject <= 1;
                    invalid_done <= 1;
                    invalid_error <= 1;
                end else begin
                    active_mode <= cfg_mode;
                    invalid_done <= 0;
                    invalid_error <= 0;
                end
            end
        end
    end
    assign m_axi_arid = select_pixel ? px_m_axi_arid : cp_m_axi_arid;
    assign m_axi_araddr = select_pixel ? px_m_axi_araddr : cp_m_axi_araddr;
    assign m_axi_arlen = select_pixel ? px_m_axi_arlen : cp_m_axi_arlen;
    assign m_axi_arsize = select_pixel ? px_m_axi_arsize : cp_m_axi_arsize;
    assign m_axi_arburst = select_pixel ? px_m_axi_arburst : cp_m_axi_arburst;
    assign m_axi_arlock = select_pixel ? px_m_axi_arlock : cp_m_axi_arlock;
    assign m_axi_arcache = select_pixel ? px_m_axi_arcache : cp_m_axi_arcache;
    assign m_axi_arprot = select_pixel ? px_m_axi_arprot : cp_m_axi_arprot;
    assign m_axi_arqos = select_pixel ? px_m_axi_arqos : cp_m_axi_arqos;
    assign m_axi_arvalid = select_pixel ? px_m_axi_arvalid : cp_m_axi_arvalid;
    assign m_axi_rready = select_pixel ? px_m_axi_rready : cp_m_axi_rready;
    assign m_axi_awid = select_pixel ? px_m_axi_awid : cp_m_axi_awid;
    assign m_axi_awaddr = select_pixel ? px_m_axi_awaddr : cp_m_axi_awaddr;
    assign m_axi_awlen = select_pixel ? px_m_axi_awlen : cp_m_axi_awlen;
    assign m_axi_awsize = select_pixel ? px_m_axi_awsize : cp_m_axi_awsize;
    assign m_axi_awburst = select_pixel ? px_m_axi_awburst : cp_m_axi_awburst;
    assign m_axi_awlock = select_pixel ? px_m_axi_awlock : cp_m_axi_awlock;
    assign m_axi_awcache = select_pixel ? px_m_axi_awcache : cp_m_axi_awcache;
    assign m_axi_awprot = select_pixel ? px_m_axi_awprot : cp_m_axi_awprot;
    assign m_axi_awqos = select_pixel ? px_m_axi_awqos : cp_m_axi_awqos;
    assign m_axi_awvalid = select_pixel ? px_m_axi_awvalid : cp_m_axi_awvalid;
    assign m_axi_wid = select_pixel ? px_m_axi_wid : cp_m_axi_wid;
    assign m_axi_wdata = select_pixel ? px_m_axi_wdata : cp_m_axi_wdata;
    assign m_axi_wstrb = select_pixel ? px_m_axi_wstrb : cp_m_axi_wstrb;
    assign m_axi_wlast = select_pixel ? px_m_axi_wlast : cp_m_axi_wlast;
    assign m_axi_wvalid = select_pixel ? px_m_axi_wvalid : cp_m_axi_wvalid;
    assign m_axi_bready = select_pixel ? px_m_axi_bready : cp_m_axi_bready;
    p1_copy_dma #(.ID_WIDTH(ID_WIDTH),
                  .TIMEOUT_CYCLES(TIMEOUT_CYCLES))
        copy(.clk(clk),
             .rst_n(rst_n),
             .start(start && can_start && cfg_mode == 0),
             .stop(stop || runtime_reset),
             .clear_status(clear_status),
             .cfg_base(cfg_base),
             .cfg_limit(cfg_limit),
             .cfg_src(cfg_src),
             .cfg_dst(cfg_dst),
             .cfg_length(cfg_length),
             .busy(cp_busy),
             .read_outstanding(cp_read_outstanding),
             .write_outstanding(cp_write_outstanding),
             .done(cp_done),
             .error(cp_error),
             .fault_latched(cp_fault_latched),
             .error_code(cp_error_code),
             .bytes_copied(cp_bytes_copied),
             .m_axi_arid(cp_m_axi_arid),
             .m_axi_araddr(cp_m_axi_araddr),
             .m_axi_arlen(cp_m_axi_arlen),
             .m_axi_arsize(cp_m_axi_arsize),
             .m_axi_arburst(cp_m_axi_arburst),
             .m_axi_arlock(cp_m_axi_arlock),
             .m_axi_arcache(cp_m_axi_arcache),
             .m_axi_arprot(cp_m_axi_arprot),
             .m_axi_arqos(cp_m_axi_arqos),
             .m_axi_arvalid(cp_m_axi_arvalid),
             .m_axi_arready(m_axi_arready && !select_pixel),
             .m_axi_rid(m_axi_rid),
             .m_axi_rdata(m_axi_rdata),
             .m_axi_rresp(m_axi_rresp),
             .m_axi_rlast(m_axi_rlast),
             .m_axi_rvalid(m_axi_rvalid && !select_pixel),
             .m_axi_rready(cp_m_axi_rready),
             .m_axi_awid(cp_m_axi_awid),
             .m_axi_awaddr(cp_m_axi_awaddr),
             .m_axi_awlen(cp_m_axi_awlen),
             .m_axi_awsize(cp_m_axi_awsize),
             .m_axi_awburst(cp_m_axi_awburst),
             .m_axi_awlock(cp_m_axi_awlock),
             .m_axi_awcache(cp_m_axi_awcache),
             .m_axi_awprot(cp_m_axi_awprot),
             .m_axi_awqos(cp_m_axi_awqos),
             .m_axi_awvalid(cp_m_axi_awvalid),
             .m_axi_awready(m_axi_awready && !select_pixel),
             .m_axi_wid(cp_m_axi_wid),
             .m_axi_wdata(cp_m_axi_wdata),
             .m_axi_wstrb(cp_m_axi_wstrb),
             .m_axi_wlast(cp_m_axi_wlast),
             .m_axi_wvalid(cp_m_axi_wvalid),
             .m_axi_wready(m_axi_wready && !select_pixel),
             .m_axi_bid(m_axi_bid),
             .m_axi_bresp(m_axi_bresp),
             .m_axi_bvalid(m_axi_bvalid && !select_pixel),
             .m_axi_bready(cp_m_axi_bready));
    p2_burst_preprocess_dma #(.ID_WIDTH(ID_WIDTH),
                              .TIMEOUT_CYCLES(TIMEOUT_CYCLES))
        pixel(.clk(clk),
              .rst_n(rst_n),
              .start(start && can_start && cfg_mode != 0 && cfg_mode != 3),
              .stop(stop),
              .clear_status(clear_status),
              .cfg_base(cfg_base),
              .cfg_limit(cfg_limit),
              .cfg_src(cfg_src),
              .cfg_dst(cfg_dst),
              .cfg_length(cfg_length),
              .busy(px_busy),
              .read_outstanding(px_read_outstanding),
              .write_outstanding(px_write_outstanding),
              .done(px_done),
              .error(px_error),
              .fault_latched(px_fault_latched),
              .error_code(px_error_code),
              .bytes_copied(px_bytes_copied),
              .m_axi_arid(px_m_axi_arid),
              .m_axi_araddr(px_m_axi_araddr),
              .m_axi_arlen(px_m_axi_arlen),
              .m_axi_arsize(px_m_axi_arsize),
              .m_axi_arburst(px_m_axi_arburst),
              .m_axi_arlock(px_m_axi_arlock),
              .m_axi_arcache(px_m_axi_arcache),
              .m_axi_arprot(px_m_axi_arprot),
              .m_axi_arqos(px_m_axi_arqos),
              .m_axi_arvalid(px_m_axi_arvalid),
              .m_axi_arready(m_axi_arready && select_pixel),
              .m_axi_rid(m_axi_rid),
              .m_axi_rdata(m_axi_rdata),
              .m_axi_rresp(m_axi_rresp),
              .m_axi_rlast(m_axi_rlast),
              .m_axi_rvalid(m_axi_rvalid && select_pixel),
              .m_axi_rready(px_m_axi_rready),
              .m_axi_awid(px_m_axi_awid),
              .m_axi_awaddr(px_m_axi_awaddr),
              .m_axi_awlen(px_m_axi_awlen),
              .m_axi_awsize(px_m_axi_awsize),
              .m_axi_awburst(px_m_axi_awburst),
              .m_axi_awlock(px_m_axi_awlock),
              .m_axi_awcache(px_m_axi_awcache),
              .m_axi_awprot(px_m_axi_awprot),
              .m_axi_awqos(px_m_axi_awqos),
              .m_axi_awvalid(px_m_axi_awvalid),
              .m_axi_awready(m_axi_awready && select_pixel),
              .m_axi_wid(px_m_axi_wid),
              .m_axi_wdata(px_m_axi_wdata),
              .m_axi_wstrb(px_m_axi_wstrb),
              .m_axi_wlast(px_m_axi_wlast),
              .m_axi_wvalid(px_m_axi_wvalid),
              .m_axi_wready(m_axi_wready && select_pixel),
              .m_axi_bid(m_axi_bid),
              .m_axi_bresp(m_axi_bresp),
              .m_axi_bvalid(m_axi_bvalid && select_pixel),
              .m_axi_bready(px_m_axi_bready),
              .runtime_reset(runtime_reset),
              .cfg_profile_a(cfg_mode == 2),
              .rejected_start(px_rejected));
endmodule
