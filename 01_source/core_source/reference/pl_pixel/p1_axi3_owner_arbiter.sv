// 在图像搬运分支间分配AXI读写通道。
`timescale 1ns / 1ps
// AXI3-shaped 2:1 arbiter. One outstanding burst per direction, independent
// read/write owners, retained through all responses. Reset is whole-link only.
// A malformed ID/count/LAST permanently poisons that owner; later traffic cannot
// prove the original request completed. Any protocol fault blocks ALL new grants,
// including legacy, while already presented/committed traffic remains drainable.
module p1_axi3_owner_arbiter
    (input wire clk,
     input wire resetn,
     input wire [58 : 0] s0_aw,
     input wire s0_awvalid,
     output wire s0_awready,
     input wire [77 : 0] s0_w,
     input wire s0_wvalid,
     output wire s0_wready,
     output wire [6 : 0] s0_b,
     output wire s0_bvalid,
     input wire s0_bready,
     input wire [58 : 0] s0_ar,
     input wire s0_arvalid,
     output wire s0_arready,
     output wire [71 : 0] s0_r,
     output wire s0_rvalid,
     input wire s0_rready,
     input wire [58 : 0] s1_aw,
     input wire s1_awvalid,
     output wire s1_awready,
     input wire [77 : 0] s1_w,
     input wire s1_wvalid,
     output wire s1_wready,
     output wire [6 : 0] s1_b,
     output wire s1_bvalid,
     input wire s1_bready,
     input wire [58 : 0] s1_ar,
     input wire s1_arvalid,
     output wire s1_arready,
     output wire [71 : 0] s1_r,
     output wire s1_rvalid,
     input wire s1_rready,
     output wire [58 : 0] m_aw,
     output wire m_awvalid,
     input wire m_awready,
     output wire [77 : 0] m_w,
     output wire m_wvalid,
     input wire m_wready,
     input wire [6 : 0] m_b,
     input wire m_bvalid,
     output wire m_bready,
     output wire [58 : 0] m_ar,
     output wire m_arvalid,
     input wire m_arready,
     input wire [71 : 0] m_r,
     input wire m_rvalid,
     output wire m_rready,
     output wire dma_read_outstanding,
     output wire dma_write_outstanding,
     output wire any_read_outstanding,
     output wire any_write_outstanding,
     output reg [8 : 0] first_protocol_reason,
     output reg [31 : 0] first_protocol_context,
     output wire [31 : 0] first_startup_flags, first_startup_counts, first_startup_cycles,
     output reg protocol_fault);
    localparam IDLE = 0, ADDR = 1, DATA = 2, RESP = 3;
    reg read_poison, write_poison;
    reg [1 : 0] rs, ws;
    reg ro, wo, rlast_owner, wlast_owner;
    reg [4 : 0] rleft, wleft, rid, wid;
    wire [58 : 0] chosen_ar = ro ? s1_ar : s0_ar;
    wire [58 : 0] chosen_aw = wo ? s1_aw : s0_aw;
    wire [77 : 0] chosen_w = wo ? s1_w : s0_w;
    // Detect faults on the same edge as a possible opposite-direction grant.
    // Existing ADDR VALID remains held; only IDLE admission is gated.
    wire protocol_error_now = (m_rvalid && rs == IDLE) || (m_bvalid && ws == IDLE) ||
                              (rs == DATA && m_rvalid && m_rready &&
                               (m_r[71 : 67] != rid || m_r[0] != (rleft == 1) || rleft == 0)) ||
                              (ws == DATA && m_wvalid && m_wready &&
                               (m_w[77 : 73] != wid || m_w[0] != (wleft == 1) || wleft == 0)) ||
                              (ws == RESP && m_bvalid && m_bready && m_b[6 : 2] != wid);
    wire block_new_grant = protocol_fault || protocol_error_now;
    // Diagnostic only: immutable first reason, including simultaneous causes.
    // No diagnostic signal feeds admission, response, poison or reset logic.
    wire [8 : 0] reason_now = {(ws == RESP && m_bvalid && m_bready && m_b[6 : 2] != wid),
                               (ws == DATA && m_wvalid && m_wready && wleft == 0),
                               (ws == DATA && m_wvalid && m_wready && m_w[0] != (wleft == 1)),
                               (ws == DATA && m_wvalid && m_wready && m_w[77 : 73] != wid),
                               (rs == DATA && m_rvalid && m_rready && rleft == 0),
                               (rs == DATA && m_rvalid && m_rready && m_r[0] != (rleft == 1)),
                               (rs == DATA && m_rvalid && m_rready && m_r[71 : 67] != rid),
                               (m_bvalid && ws == IDLE),
                               (m_rvalid && rs == IDLE)};
    // Startup witness is configuration-initialized, NOT cleared by local resetn.
    // Otherwise the pre-open RVALID history would be erased on every init edge.
    // All observation and immutable outputs stay on this same HP clock. None
    // feeds any grant/READY/VALID/poison/reset/control expression.
    reg witness_pre_r = 0, witness_pre_b = 0, witness_opened = 0;
    reg witness_open_r = 0, witness_open_b = 0, witness_frozen = 0;
    reg [15 : 0] witness_ar_count = 0, witness_aw_count = 0;
    reg [31 : 0] witness_cycles = 0;
    reg [31 : 0] witness_flags = 0, witness_counts = 0, witness_first_cycles = 0;
    assign first_startup_flags = witness_flags;
    assign first_startup_counts = witness_counts;
    assign first_startup_cycles = witness_first_cycles;
    always @(posedge clk) begin
        if (!witness_frozen) begin
            if (!resetn) begin
                witness_pre_r <= witness_pre_r | m_rvalid;
                witness_pre_b <= witness_pre_b | m_bvalid;
            end else begin
                if (!witness_opened) begin
                    witness_opened <= 1;
                    witness_open_r <= m_rvalid;
                    witness_open_b <= m_bvalid;
                end
                if (witness_cycles != 32'hffffffff)
                    witness_cycles <= witness_cycles + 1'b1;
                if (m_arvalid && m_arready && witness_ar_count != 16'hffff)
                    witness_ar_count <= witness_ar_count + 1'b1;
                if (m_awvalid && m_awready && witness_aw_count != 16'hffff)
                    witness_aw_count <= witness_aw_count + 1'b1;
                if (protocol_error_now) begin
                    witness_frozen <= 1;
                    witness_flags <= {8'b0,
                                      (m_awvalid && m_awready),
                                      (m_arvalid && m_arready),
                                      m_awready,
                                      m_awvalid,
                                      m_arready,
                                      m_arvalid,
                                      s1_arvalid,
                                      s0_arvalid,
                                      2'b0,
                                      m_bready,
                                      m_rready,
                                      m_bvalid,
                                      m_rvalid,
                                      (witness_cycles == 32'hffffffff),
                                      (witness_aw_count == 16'hffff),
                                      (witness_ar_count == 16'hffff),
                                      (witness_aw_count != 0),
                                      (witness_ar_count != 0),
                                      !witness_opened,
                                      (witness_open_b || (!witness_opened && m_bvalid)),
                                      (witness_open_r || (!witness_opened && m_rvalid)),
                                      witness_pre_b,
                                      witness_pre_r};
                    witness_counts <= {witness_aw_count, witness_ar_count};
                    witness_first_cycles <= witness_cycles;
                end
            end
        end
    end
    always @(posedge clk) begin
        if (!resetn) begin
            first_protocol_reason <= 0;
            first_protocol_context <= 0;
        end else if (first_protocol_reason == 0 && protocol_error_now) begin
            first_protocol_reason <= reason_now;
            first_protocol_context <=
                {m_r[0], m_r[71 : 67], wid, rid, wleft, rleft, wo, ro, ws, rs};
        end
    end

    assign m_ar = chosen_ar;
    assign m_arvalid = (rs == ADDR) && (ro ? s1_arvalid : s0_arvalid);
    assign s0_arready = (rs == ADDR) && !ro && m_arready;
    assign s1_arready = (rs == ADDR) && ro && m_arready;
    assign s0_r = m_r;
    assign s1_r = m_r;
    assign s0_rvalid = (rs == DATA) && !ro && m_rvalid;
    assign s1_rvalid = (rs == DATA) && ro && m_rvalid;
    assign m_rready = (rs == DATA) && (ro ? s1_rready : s0_rready);
    assign m_aw = chosen_aw;
    assign m_awvalid = (ws == ADDR) && (wo ? s1_awvalid : s0_awvalid);
    assign s0_awready = (ws == ADDR) && !wo && m_awready;
    assign s1_awready = (ws == ADDR) && wo && m_awready;
    assign m_w = chosen_w;
    assign m_wvalid = (ws == DATA) && (wo ? s1_wvalid : s0_wvalid);
    assign s0_wready = (ws == DATA) && !wo && m_wready;
    assign s1_wready = (ws == DATA) && wo && m_wready;
    assign s0_b = m_b;
    assign s1_b = m_b;
    assign s0_bvalid = (ws == RESP) && !wo && m_bvalid;
    assign s1_bvalid = (ws == RESP) && wo && m_bvalid;
    assign m_bready = (ws == RESP) && (wo ? s1_bready : s0_bready);
    assign dma_read_outstanding = (rs != IDLE) && ro;
    assign dma_write_outstanding = (ws != IDLE) && wo;
    assign any_read_outstanding = (rs != IDLE) || s0_arvalid || s1_arvalid;
    assign any_write_outstanding =
               (ws != IDLE) || s0_awvalid || s1_awvalid || s0_wvalid || s1_wvalid;
    always @(posedge clk) begin
        if (!resetn) begin
            rs <= IDLE;
            ws <= IDLE;
            ro <= 0;
            wo <= 0;
            rlast_owner <= 1;
            wlast_owner <= 1;
            rleft <= 0;
            wleft <= 0;
            rid <= 0;
            wid <= 0;
            protocol_fault <= 0;
            read_poison <= 0;
            write_poison <= 0;
        end else begin
            if ((m_rvalid && rs == IDLE) || (m_bvalid && ws == IDLE))
                protocol_fault <= 1;
            case (rs)
                IDLE:
                    if (!block_new_grant && !m_rvalid && (s0_arvalid || s1_arvalid)) begin
                        ro <= s0_arvalid ? ((s1_arvalid && !protocol_fault) ? !rlast_owner : 1'b0)
                                         : 1'b1;
                        rs <= ADDR;
                    end
                ADDR:
                    if (m_arvalid && m_arready) begin
                        rid <= m_ar[58 : 54];
                        rleft <= {1'b0, m_ar[21 : 18]} + 1'b1;
                        rs <= DATA;
                    end
                DATA:
                    if (m_rvalid && m_rready) begin
                        if (m_r[71 : 67] != rid || m_r[0] != (rleft == 1) || rleft == 0) begin
                            protocol_fault <= 1;
                            read_poison <= 1;
                        end
                        if (m_r[0] && rleft == 1 && m_r[71 : 67] == rid && !read_poison) begin
                            rs <= IDLE;
                            rlast_owner <= ro;
                            rleft <= 0;
                        end else if (rleft != 0)
                            rleft <= rleft - 1'b1;
                    end
            endcase
            case (ws)
                IDLE:
                    if (!block_new_grant && !m_bvalid && (s0_awvalid || s1_awvalid)) begin
                        wo <= s0_awvalid ? ((s1_awvalid && !protocol_fault) ? !wlast_owner : 1'b0)
                                         : 1'b1;
                        ws <= ADDR;
                    end
                ADDR:
                    if (m_awvalid && m_awready) begin
                        wid <= m_aw[58 : 54];
                        wleft <= {1'b0, m_aw[21 : 18]} + 1'b1;
                        ws <= DATA;
                    end
                DATA:
                    if (m_wvalid && m_wready) begin
                        if (m_w[77 : 73] != wid || m_w[0] != (wleft == 1) || wleft == 0) begin
                            protocol_fault <= 1;
                            write_poison <= 1;
                        end
                        if (m_w[0] && wleft == 1) begin
                            ws <= RESP;
                            wleft <= 0;
                        end else if (wleft != 0)
                            wleft <= wleft - 1'b1;
                    end
                RESP:
                    if (m_bvalid && m_bready) begin
                        if (m_b[6 : 2] != wid) begin
                            protocol_fault <= 1;
                            write_poison <= 1;
                        end
                        if (m_b[6 : 2] == wid && !write_poison) begin
                            ws <= IDLE;
                            wlast_owner <= wo;
                        end
                    end
            endcase
        end
    end
endmodule
