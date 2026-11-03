// 提供图像搬运与预处理的控制寄存器。
`timescale 1ns / 1ps
// Native merged-write control port. resetn inputs are CONFIGURATION INIT ONLY.
// Runtime transport resets inhibit and quarantine, never erase bus ownership.
module p1_gp_control
    (input wire gp_clk, gp_init_resetn, hp_clk, hp_init_resetn,
     input wire runtime_reset_gp, runtime_reset_hp,
     input wire [31 : 0] araddr, awwaddr, awwdata,
     input wire arvalid, rready, awwvalid,
     output wire arready, awwready,
     output reg rvalid,
     output reg [31 : 0] rdata,
     output reg [1 : 0] cfg_mode,
     output reg [31 : 0] cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length,
     output reg start, clear_status,
     output wire stop,
     input wire core_busy, core_done, core_error, core_fault,
     input wire [7 : 0] core_error_code,
     input wire [31 : 0] core_bytes,
     input wire core_read_outstanding, core_write_outstanding,
     input wire owner_read_outstanding, owner_write_outstanding, owner_fault,
     input wire [8 : 0] owner_first_reason,
     input wire [31 : 0] owner_first_context,
     input wire [31 : 0] owner_startup_flags, owner_startup_counts, owner_startup_cycles,
     input wire any_read_outstanding, any_write_outstanding);
    (* ASYNC_REG = "TRUE" *) reg runtime_hp1_gp, runtime_hp2_gp;
    wire runtime_reset_local_gp = runtime_reset_gp | runtime_hp2_gp;
    reg [1 : 0] bank_mode;
    reg [31 : 0] bank_base, bank_limit, bank_src, bank_dst, bank_length;
    reg enable_gp, pending_gp, rejected_gp, runtime_fault_gp, runtime_reset_prev_gp;
    reg [31 : 0] epoch, accepted_seq, completed_seq, bytes_gp;
    reg [7 : 0] error_gp;
    reg done_gp, has_error_gp;
    reg request_gp, stop_gp;
    reg [162 : 0] payload_gp;
    reg [1 : 0] result_wait_gp;
    reg result_seen_gp, result_ack_gp;
    (* ASYNC_REG = "TRUE" *) reg result_t1_gp, result_t2_gp;
    (* ASYNC_REG = "TRUE" *) reg [43 : 0] result_d1_gp, result_d2_gp;
    (* ASYNC_REG = "TRUE" *) reg [7 : 0] stat1_gp, stat2_gp;
    // Capture every status expression in its source domain before synchronizing.
    // This removes combinational CDC glitches; the vector is not an atomic bus.
    reg [7 : 0] stat_hp;
    reg result_hp;
    reg [43 : 0] result_payload_hp;
    reg request_ack_hp, active_hp, clear_cmd_hp, runtime_fault_hp;
    reg [2 : 0] request_wait_hp, completion_wait_hp;
    (* ASYNC_REG = "TRUE" *) reg req1_hp, req2_hp, enable1_hp, enable2_hp;
    (* ASYNC_REG = "TRUE" *) reg stop1_hp, stop2_hp, ack1_hp, ack2_hp;
    (* ASYNC_REG = "TRUE" *) reg [162 : 0] payload1_hp, payload2_hp;
    reg stop_seen_hp;
    wire rout = core_read_outstanding | owner_read_outstanding;
    wire wout = core_write_outstanding | owner_write_outstanding;
    wire fault_hp = core_fault | owner_fault | runtime_fault_hp;
    wire fence_hp = !enable2_hp && !core_busy && !rout && !wout && !any_read_outstanding &&
                    !any_write_outstanding && !active_hp && (req2_hp == request_ack_hp) &&
                    !fault_hp;
    wire busy_gp = pending_gp | stat2_gp[0];
    wire quarantine_gp = runtime_fault_gp | stat2_gp[1];
    wire fence_gp = stat2_gp[2] && !pending_gp && !enable_gp && !quarantine_gp;
    // Opus247 root fix: one registered HP-own idle AND, two-stage CDC.
    // Native video activity remains in full fence bit7 solely for diagnostics.
    wire own_idle_hp = !enable2_hp && !core_busy && !rout && !wout && !active_hp &&
                       (req2_hp == request_ack_hp) && !fault_hp;
    wire config_allowed = !enable_gp && !pending_gp && !busy_gp && !quarantine_gp && stat2_gp[7];
    // Diagnostic ABI 0x00030002. The HP bundle freezes until configuration init.
    // GP consumes validity only after the shared bundle synchronizer, then two
    // GP-only validity stages and two settling edges. No parallel HP valid chain.
    // Neither mailbox nor first-reset record affects any control predicate.
    reg [191 : 0] first_hp_bundle;
    (* ASYNC_REG = "TRUE" *) reg [191 : 0] diag1_gp, diag2_gp;
    (* ASYNC_REG = "TRUE" *) reg diag_valid1_gp, diag_valid2_gp;
    reg [1 : 0] diag_wait_gp;
    reg diag_ready_gp;
    reg [191 : 0] diag_bundle_gp;
    reg [31 : 0] first_gp_reset;
    always @(posedge hp_clk) begin
        if (!hp_init_resetn)
            first_hp_bundle <= 0;
        else if (!first_hp_bundle[95] && fault_hp) begin
            first_hp_bundle <=
                {owner_startup_flags, owner_startup_counts, owner_startup_cycles, 1'b1,       27'b0,
                 runtime_reset_hp,    runtime_fault_hp,     owner_fault,          core_fault, 23'b0,
                 owner_first_reason,  owner_first_context};
        end
    end
    always @(posedge gp_clk) begin
        if (!gp_init_resetn) begin
            diag1_gp <= 0;
            diag2_gp <= 0;
            diag_valid1_gp <= 0;
            diag_valid2_gp <= 0;
            diag_wait_gp <= 0;
            diag_ready_gp <= 0;
            diag_bundle_gp <= 0;
            first_gp_reset <= 0;
        end else begin
            diag1_gp <= first_hp_bundle;
            diag2_gp <= diag1_gp;
            diag_valid1_gp <= diag2_gp[95];
            diag_valid2_gp <= diag_valid1_gp;
            if (diag_valid2_gp && !diag_ready_gp) begin
                if (diag_wait_gp == 2) begin
                    diag_bundle_gp <= diag2_gp;
                    diag_ready_gp <= 1;
                end else
                    diag_wait_gp <= diag_wait_gp + 1'b1;
            end
            if (!first_gp_reset[31] && runtime_reset_local_gp &&
                (pending_gp || stat2_gp[0] || stat2_gp[3] || stat2_gp[4] || stat2_gp[5] ||
                 stat2_gp[6] || !fence_gp))
                first_gp_reset <= {1'b1,     18'b0,     runtime_reset_gp, runtime_hp2_gp,
                                   fence_gp, enable_gp, pending_gp,       stat2_gp};
        end
    end

    wire local_write = (awwaddr[31 : 12] == 20'h80080) && (awwaddr[1 : 0] == 0);
    wire local_read = (araddr[31 : 12] == 20'h80080) && (araddr[1 : 0] == 0);
    assign awwready = gp_init_resetn && !runtime_reset_local_gp;
    assign arready = gp_init_resetn && (!rvalid || rready);
    assign stop = !enable2_hp || (stop2_hp != stop_seen_hp) || runtime_reset_hp || runtime_fault_hp;
    always @(posedge gp_clk) begin
        if (!gp_init_resetn) begin
            bank_mode <= 0;
            bank_base <= 0;
            bank_limit <= 0;
            bank_src <= 0;
            bank_dst <= 0;
            bank_length <= 0;
            runtime_hp1_gp <= 0;
            runtime_hp2_gp <= 0;
            enable_gp <= 0;
            pending_gp <= 0;
            rejected_gp <= 0;
            runtime_fault_gp <= 0;
            runtime_reset_prev_gp <= 0;
            epoch <= 0;
            accepted_seq <= 0;
            completed_seq <= 0;
            bytes_gp <= 0;
            error_gp <= 0;
            done_gp <= 0;
            has_error_gp <= 0;
            request_gp <= 0;
            stop_gp <= 0;
            payload_gp <= 0;
            result_wait_gp <= 0;
            result_seen_gp <= 0;
            result_ack_gp <= 0;
            result_t1_gp <= 0;
            result_t2_gp <= 0;
            result_d1_gp <= 0;
            result_d2_gp <= 0;
            stat1_gp <= 0;
            stat2_gp <= 0;
            rvalid <= 0;
            rdata <= 0;
        end else begin
            result_t1_gp <= result_hp;
            result_t2_gp <= result_t1_gp;
            result_d1_gp <= result_payload_hp;
            result_d2_gp <= result_d1_gp;
            stat1_gp <= stat_hp;
            stat2_gp <= stat1_gp;
            runtime_hp1_gp <= runtime_reset_hp;
            runtime_hp2_gp <= runtime_hp1_gp;
            runtime_reset_prev_gp <= runtime_reset_local_gp;
            if (runtime_reset_local_gp) begin
                enable_gp <= 0;
                // A reset already fenced and idle may be re-armed after a fresh fence.
                // Unknown/live state stays quarantined; no pending command is discarded.
                if (pending_gp || stat2_gp[0] || stat2_gp[3] || stat2_gp[4] || stat2_gp[5] ||
                    stat2_gp[6] || !fence_gp)
                    runtime_fault_gp <= 1;
                if (!runtime_reset_prev_gp)
                    epoch <= epoch + 1'b1;
            end
            if (result_t2_gp != result_seen_gp && result_wait_gp == 0)
                result_wait_gp <= 2;
            else if (result_wait_gp != 0) begin
                result_wait_gp <= result_wait_gp - 1'b1;
                if (result_wait_gp == 1) begin
                    {bytes_gp, error_gp, done_gp, has_error_gp} <= result_d2_gp[43 : 2];
                    rejected_gp <= rejected_gp | result_d2_gp[0];
                    pending_gp <= 0;
                    completed_seq <= completed_seq + 1'b1;
                    result_seen_gp <= result_t2_gp;
                    result_ack_gp <= result_t2_gp;
                end
            end
            if (rvalid && rready)
                rvalid <= 0;
            if (arvalid && arready) begin
                rvalid <= 1;
                rdata <= 32'hb0aaaaaa;
                if (local_read)
                    case (araddr[11 : 0])
                        12'h000:
                            rdata <= 32'ha5080001;
                        12'h004:
                            rdata <= 32'h00030002;
                        12'h00c:
                            rdata <=
                                {20'b0,        stat2_gp[6], stat2_gp[5], stat2_gp[4], stat2_gp[3],
                                 fence_gp,     !enable_gp,  rejected_gp, pending_gp,  quarantine_gp,
                                 has_error_gp, done_gp,     busy_gp};
                        12'h010:
                            rdata <= bank_base;
                        12'h014:
                            rdata <= bank_limit;
                        12'h018:
                            rdata <= bank_src;
                        12'h01c:
                            rdata <= bank_dst;
                        12'h020:
                            rdata <= bank_length;
                        12'h024:
                            rdata <= {24'b0, error_gp};
                        12'h028:
                            rdata <= bytes_gp;
                        12'h02c:
                            rdata <= epoch;
                        12'h030:
                            rdata <= {31'b0, enable_gp};
                        12'h03c:
                            rdata <= {30'b0, bank_mode};
                        12'h034:
                            rdata <= accepted_seq;
                        12'h038:
                            rdata <= completed_seq;
                        12'h040:
                            rdata <= diag_bundle_gp[95 : 64];
                        12'h044:
                            rdata <= diag_bundle_gp[63 : 32];
                        12'h048:
                            rdata <= diag_bundle_gp[31 : 0];
                        12'h04c:
                            rdata <= first_gp_reset;
                        12'h050:
                            rdata <= {30'b0, diag_valid2_gp, diag_ready_gp};
                        12'h054:
                            rdata <= diag_bundle_gp[191 : 160];
                        12'h058:
                            rdata <= diag_bundle_gp[159 : 128];
                        12'h05c:
                            rdata <= diag_bundle_gp[127 : 96];
                        default:
                            rdata <= 0;
                    endcase
            end
            if (awwvalid && awwready && local_write) begin
                epoch <= epoch + 1'b1;
                case (awwaddr[11 : 0])
                    12'h008: begin
                        if (awwdata == 2) begin
                            enable_gp <= 0;
                            stop_gp <= !stop_gp;
                        end else if (awwdata == 1 && enable_gp && !busy_gp && !quarantine_gp &&
                                     !runtime_reset_local_gp) begin
                            payload_gp <= {bank_mode, bank_base,   bank_limit, bank_src,
                                           bank_dst,  bank_length, 1'b0};
                            request_gp <= !request_gp;
                            pending_gp <= 1;
                            done_gp <= 0;
                            has_error_gp <= 0;
                            accepted_seq <= accepted_seq + 1'b1;
                        end else if (awwdata == 4 && config_allowed && !quarantine_gp) begin
                            payload_gp <= {bank_mode, bank_base,   bank_limit, bank_src,
                                           bank_dst,  bank_length, 1'b1};
                            request_gp <= !request_gp;
                            pending_gp <= 1;
                            rejected_gp <= 0;
                            accepted_seq <= accepted_seq + 1'b1;
                        end else
                            rejected_gp <= 1;
                    end
                    12'h03c: begin
                        if (config_allowed && awwdata <= 2)
                            bank_mode <= awwdata[1 : 0];
                        else
                            rejected_gp <= 1;
                    end
                    12'h030: begin
                        if (awwdata == 0)
                            enable_gp <= 0;
                        else if (awwdata == 1 && !busy_gp && !quarantine_gp &&
                                 !runtime_reset_local_gp)
                            enable_gp <= 1;
                        else
                            rejected_gp <= 1;
                    end
                    12'h010, 12'h014, 12'h018, 12'h01c, 12'h020: begin
                        if (config_allowed && !quarantine_gp)
                            case (awwaddr[11 : 0])
                                12'h010:
                                    bank_base <= awwdata;
                                12'h014:
                                    bank_limit <= awwdata;
                                12'h018:
                                    bank_src <= awwdata;
                                12'h01c:
                                    bank_dst <= awwdata;
                                12'h020:
                                    bank_length <= awwdata;
                            endcase
                        else
                            rejected_gp <= 1;
                    end
                    default:
                        rejected_gp <= 1;
                endcase
            end
        end
    end
    always @(posedge hp_clk) begin
        if (!hp_init_resetn) begin
            stat_hp <= 0;
            req1_hp <= 0;
            req2_hp <= 0;
            enable1_hp <= 0;
            enable2_hp <= 0;
            stop1_hp <= 0;
            stop2_hp <= 0;
            stop_seen_hp <= 0;
            ack1_hp <= 0;
            ack2_hp <= 0;
            payload1_hp <= 0;
            payload2_hp <= 0;
            request_ack_hp <= 0;
            active_hp <= 0;
            clear_cmd_hp <= 0;
            runtime_fault_hp <= 0;
            request_wait_hp <= 0;
            completion_wait_hp <= 0;
            result_hp <= 0;
            result_payload_hp <= 0;
            cfg_base <= 0;
            cfg_limit <= 0;
            cfg_src <= 0;
            cfg_dst <= 0;
            cfg_length <= 0;
            cfg_mode <= 0;
            start <= 0;
            clear_status <= 0;
        end else begin
            stat_hp <= {own_idle_hp,
                        any_write_outstanding,
                        any_read_outstanding,
                        wout,
                        rout,
                        fence_hp,
                        fault_hp,
                        core_busy};
            req1_hp <= request_gp;
            req2_hp <= req1_hp;
            enable1_hp <= enable_gp;
            enable2_hp <= enable1_hp;
            stop1_hp <= stop_gp;
            stop2_hp <= stop1_hp;
            stop_seen_hp <= stop2_hp;
            ack1_hp <= result_ack_gp;
            ack2_hp <= ack1_hp;
            payload1_hp <= payload_gp;
            payload2_hp <= payload1_hp;
            start <= 0;
            clear_status <= 0;
            if (runtime_reset_hp &&
                (core_busy || rout || wout || any_read_outstanding || any_write_outstanding ||
                 active_hp || req2_hp != request_ack_hp || !fence_hp))
                runtime_fault_hp <= 1;
            if (req2_hp != request_ack_hp && !active_hp && result_hp == ack2_hp &&
                request_wait_hp == 0)
                request_wait_hp <= 2;
            else if (request_wait_hp != 0) begin
                request_wait_hp <= request_wait_hp - 1'b1;
                if (request_wait_hp == 1) begin
                    request_ack_hp <= req2_hp;
                    {cfg_mode, cfg_base, cfg_limit, cfg_src, cfg_dst, cfg_length, clear_cmd_hp} <=
                        payload2_hp;
                    if (!core_busy && !fault_hp && !runtime_reset_hp &&
                        (enable2_hp || payload2_hp[0])) begin
                        if (payload2_hp[0])
                            clear_status <= 1;
                        else
                            start <= 1;
                        active_hp <= 1;
                        completion_wait_hp <= 4;
                    end else begin
                        result_payload_hp <= {32'b0, 8'hf0, 1'b1, 1'b1, fault_hp, 1'b1};
                        result_hp <= !result_hp;
                    end
                end
            end
            if (active_hp) begin
                if (completion_wait_hp != 0)
                    completion_wait_hp <= completion_wait_hp - 1'b1;
                else if (!core_busy && !rout && !wout && (core_done || core_fault || clear_cmd_hp))
                begin
                    result_payload_hp <=
                        {core_bytes, core_error_code, core_done, core_error, fault_hp, 1'b0};
                    result_hp <= !result_hp;
                    active_hp <= 0;
                end
            end
        end
    end
endmodule
