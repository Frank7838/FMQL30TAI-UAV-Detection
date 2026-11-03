// 连接控制寄存器、跨时钟信号和图像处理通道。
`timescale 1ns / 1ps
// Only GSR configuration initialization resets ownership. Runtime GP/HP resets
// inhibit and fence through p1_gp_control; no live AXI state is discarded.
module p1_subsystem
    #(parameter integer STARTUP_QUIET_HP_CYCLES = 20000000)
    (input wire gp_clk, hp_clk, runtime_reset_gp, runtime_reset_hp,
     input wire [31 : 0] araddr, awwaddr, awwdata,
     input wire arvalid, rready, awwvalid,
     output wire arready, awwready, rvalid,
     output wire [31 : 0] rdata,
     input wire [58 : 0] s0_aw, s0_ar,
     input wire [77 : 0] s0_w,
     input wire s0_awvalid, s0_wvalid, s0_arvalid, s0_bready, s0_rready,
     output wire s0_awready, s0_wready, s0_arready, s0_bvalid, s0_rvalid,
     output wire [6 : 0] s0_b,
     output wire [71 : 0] s0_r,
     output wire [58 : 0] m_aw, m_ar,
     output wire [77 : 0] m_w,
     output wire m_awvalid, m_wvalid, m_arvalid, m_bready, m_rready,
     input wire m_awready, m_wready, m_arready, m_bvalid, m_rvalid,
     input wire [6 : 0] m_b,
     input wire [71 : 0] m_r);
    reg [3 : 0] gp_init = 0, hp_init = 0;
    // One-time startup qualification only. At the audited200MHz HP clock,
    // 20,000,000 consecutive reset-inactive edges are100ms. Before it opens,
    // the P1 arbiter/core remain initialized and cannot forward native requests.
    // Every observed startup reset restarts the interval. Once open, initialization
    // NEVER falls on runtime reset: live ownership and quarantine rules stay intact.
    localparam integer STARTUP_COUNT_WIDTH = $clog2(STARTUP_QUIET_HP_CYCLES);
    reg [STARTUP_COUNT_WIDTH - 1 : 0] startup_quiet_hp = 0;
    (* ASYNC_REG = "TRUE" *) reg hp_ready1_gp = 0, hp_ready2_gp = 0;
    always @(posedge hp_clk)
        if (!hp_init[3]) begin
            if (runtime_reset_hp)
                startup_quiet_hp <= 0;
            else if (startup_quiet_hp == STARTUP_QUIET_HP_CYCLES - 1)
                hp_init <= 4'hf;
            else
                startup_quiet_hp <= startup_quiet_hp + 1'b1;
        end
    always @(posedge gp_clk) begin
        hp_ready1_gp <= hp_init[3];
        hp_ready2_gp <= hp_ready1_gp;
        if (!gp_init[3] && !runtime_reset_gp && hp_ready2_gp)
            gp_init <= {gp_init[2 : 0], 1'b1};
    end
    wire [1 : 0] cfg_mode;
    wire [31 : 0] cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length, core_bytes;
    wire start, stop, clear_status, core_busy, core_done, core_error, core_fault;
    wire [7 : 0] core_error_code;
    wire core_read_outstanding, core_write_outstanding;
    wire owner_read_outstanding, owner_write_outstanding, owner_fault;
    wire any_read_outstanding, any_write_outstanding;
    wire [8 : 0] owner_first_reason;
    wire [31 : 0] owner_first_context;
    wire [31 : 0] owner_startup_flags, owner_startup_counts, owner_startup_cycles;
    p1_gp_control control(.gp_clk(gp_clk),
                          .gp_init_resetn(gp_init[3]),
                          .hp_clk(hp_clk),
                          .hp_init_resetn(hp_init[3]),
                          .runtime_reset_gp(runtime_reset_gp),
                          .runtime_reset_hp(runtime_reset_hp),
                          .araddr(araddr),
                          .awwaddr(awwaddr),
                          .awwdata(awwdata),
                          .arvalid(arvalid),
                          .rready(rready),
                          .awwvalid(awwvalid),
                          .arready(arready),
                          .awwready(awwready),
                          .rvalid(rvalid),
                          .rdata(rdata),
                          .cfg_mode(cfg_mode),
                          .cfg_base(cfg_base),
                          .cfg_limit(cfg_limit),
                          .cfg_src(cfg_src),
                          .cfg_dst(cfg_dst),
                          .cfg_length(cfg_length),
                          .start(start),
                          .stop(stop),
                          .clear_status(clear_status),
                          .core_busy(core_busy),
                          .core_done(core_done),
                          .core_error(core_error),
                          .core_fault(core_fault),
                          .core_error_code(core_error_code),
                          .core_bytes(core_bytes),
                          .core_read_outstanding(core_read_outstanding),
                          .core_write_outstanding(core_write_outstanding),
                          .owner_read_outstanding(owner_read_outstanding),
                          .owner_write_outstanding(owner_write_outstanding),
                          .owner_fault(owner_fault),
                          .owner_first_reason(owner_first_reason),
                          .owner_first_context(owner_first_context),
                          .owner_startup_flags(owner_startup_flags),
                          .owner_startup_counts(owner_startup_counts),
                          .owner_startup_cycles(owner_startup_cycles),
                          .any_read_outstanding(any_read_outstanding),
                          .any_write_outstanding(any_write_outstanding));
    wire [4 : 0] awid, arid, wid;
    wire [31 : 0] awaddr, araddr_dma;
    wire [3 : 0] awlen, arlen, awcache, arcache, awqos, arqos;
    wire [2 : 0] awsize, arsize, awprot, arprot;
    wire [1 : 0] awburst, arburst, awlock, arlock;
    wire [63 : 0] wdata;
    wire [7 : 0] wstrb;
    wire awvalid, awready, wlast, wvalid, wready, bvalid, bready, arvalid_dma, arready_dma,
         rvalid_dma, rready_dma;
    wire [6 : 0] b;
    wire [71 : 0] r;
    p1_p2_dma_mux core(.runtime_reset(runtime_reset_hp),
                       .clk(hp_clk),
                       .rst_n(hp_init[3]),
                       .start(start),
                       .stop(stop),
                       .clear_status(clear_status),
                       .cfg_mode(cfg_mode),
                       .cfg_base(cfg_base),
                       .cfg_limit(cfg_limit),
                       .cfg_src(cfg_src),
                       .cfg_dst(cfg_dst),
                       .cfg_length(cfg_length),
                       .busy(core_busy),
                       .done(core_done),
                       .error(core_error),
                       .fault_latched(core_fault),
                       .error_code(core_error_code),
                       .bytes_copied(core_bytes),
                       .read_outstanding(core_read_outstanding),
                       .write_outstanding(core_write_outstanding),
                       .m_axi_awid(awid),
                       .m_axi_awaddr(awaddr),
                       .m_axi_awlen(awlen),
                       .m_axi_awsize(awsize),
                       .m_axi_awburst(awburst),
                       .m_axi_awlock(awlock),
                       .m_axi_awcache(awcache),
                       .m_axi_awprot(awprot),
                       .m_axi_awqos(awqos),
                       .m_axi_awvalid(awvalid),
                       .m_axi_awready(awready),
                       .m_axi_wid(wid),
                       .m_axi_wdata(wdata),
                       .m_axi_wstrb(wstrb),
                       .m_axi_wlast(wlast),
                       .m_axi_wvalid(wvalid),
                       .m_axi_wready(wready),
                       .m_axi_bid(b[6 : 2]),
                       .m_axi_bresp(b[1 : 0]),
                       .m_axi_bvalid(bvalid),
                       .m_axi_bready(bready),
                       .m_axi_arid(arid),
                       .m_axi_araddr(araddr_dma),
                       .m_axi_arlen(arlen),
                       .m_axi_arsize(arsize),
                       .m_axi_arburst(arburst),
                       .m_axi_arlock(arlock),
                       .m_axi_arcache(arcache),
                       .m_axi_arprot(arprot),
                       .m_axi_arqos(arqos),
                       .m_axi_arvalid(arvalid_dma),
                       .m_axi_arready(arready_dma),
                       .m_axi_rid(r[71 : 67]),
                       .m_axi_rdata(r[66 : 3]),
                       .m_axi_rresp(r[2 : 1]),
                       .m_axi_rlast(r[0]),
                       .m_axi_rvalid(rvalid_dma),
                       .m_axi_rready(rready_dma));
    // Packed address: ID5 ADDR32 LEN4 SIZE3 BURST2 LOCK2 CACHE4 PROT3 QOS4.
    p1_axi3_owner_arbiter arbiter(.clk(hp_clk),
                                  .resetn(hp_init[3]),
                                  .s0_aw(s0_aw),
                                  .s0_awvalid(s0_awvalid),
                                  .s0_awready(s0_awready),
                                  .s0_w(s0_w),
                                  .s0_wvalid(s0_wvalid),
                                  .s0_wready(s0_wready),
                                  .s0_b(s0_b),
                                  .s0_bvalid(s0_bvalid),
                                  .s0_bready(s0_bready),
                                  .s0_ar(s0_ar),
                                  .s0_arvalid(s0_arvalid),
                                  .s0_arready(s0_arready),
                                  .s0_r(s0_r),
                                  .s0_rvalid(s0_rvalid),
                                  .s0_rready(s0_rready),
                                  .s1_aw({awid, awaddr, awlen, awsize, awburst, awlock, awcache,
                                          awprot, awqos}),
                                  .s1_awvalid(awvalid),
                                  .s1_awready(awready),
                                  .s1_w({wid, wdata, wstrb, wlast}),
                                  .s1_wvalid(wvalid),
                                  .s1_wready(wready),
                                  .s1_b(b),
                                  .s1_bvalid(bvalid),
                                  .s1_bready(bready),
                                  .s1_ar({arid, araddr_dma, arlen, arsize, arburst, arlock, arcache,
                                          arprot, arqos}),
                                  .s1_arvalid(arvalid_dma),
                                  .s1_arready(arready_dma),
                                  .s1_r(r),
                                  .s1_rvalid(rvalid_dma),
                                  .s1_rready(rready_dma),
                                  .m_aw(m_aw),
                                  .m_awvalid(m_awvalid),
                                  .m_awready(m_awready),
                                  .m_w(m_w),
                                  .m_wvalid(m_wvalid),
                                  .m_wready(m_wready),
                                  .m_b(m_b),
                                  .m_bvalid(m_bvalid),
                                  .m_bready(m_bready),
                                  .m_ar(m_ar),
                                  .m_arvalid(m_arvalid),
                                  .m_arready(m_arready),
                                  .m_r(m_r),
                                  .m_rvalid(m_rvalid),
                                  .m_rready(m_rready),
                                  .dma_read_outstanding(owner_read_outstanding),
                                  .dma_write_outstanding(owner_write_outstanding),
                                  .any_read_outstanding(any_read_outstanding),
                                  .any_write_outstanding(any_write_outstanding),
                                  .first_protocol_reason(owner_first_reason),
                                  .first_protocol_context(owner_first_context),
                                  .first_startup_flags(owner_startup_flags),
                                  .first_startup_counts(owner_startup_counts),
                                  .first_startup_cycles(owner_startup_cycles),
                                  .protocol_fault(owner_fault));
endmodule
