// Native vendor merged write and separate read handshake, not AXI4-Lite.
// Coordinates normalized to 0..65535; PL evaluates box center in half-open ROI.
module baseline_region_alarm
    #(parameter integer TIMEOUT_MS = 1000,
      parameter [15 : 0] ROI_X0 = 8192, ROI_X1 = 57344, ROI_Y0 = 4096, ROI_Y1 = 57344)
    (input wire clk, reset, tick_ms, bus_reset,
     input wire [31 : 0] awaddr, awdata, araddr,
     input wire awvalid, arvalid, rready,
     output wire awready, arready,
     output reg [31 : 0] rdata,
     output reg rvalid,
     output wire alarm, fault);
    reg pending, txn, bad, have_xy;
    reg [31 : 0] shadow_frame, xy;
    reg [4 : 0] boxes;
    reg hit;
    // The inference result is a transaction, not just a fresh-looking frame ID.
    // An open transaction cannot survive a stream fault or remain half-written
    // indefinitely while fault is already set. Business alarm/counters persist.
    reg fault_seen;
    reg [15 : 0] txn_age_ms;
    wire fault_entry = fault && !fault_seen;
    wire txn_expired = txn && tick_ms && txn_age_ms == TIMEOUT_MS - 1;
    wire abort_txn = fault_entry || txn_expired;
    reg pulse, pulse_fresh, pulse_hit, soft_reset;
    reg [31 : 0] pulse_frame, protocol_errors, event_latch;
    wire have_frame;
    wire [3 : 0] positives, empties, events;
    wire [31 : 0] last_frame, accepts, rejects;
    wire [15 : 0] x0 = xy[15 : 0], y0 = xy[31 : 16], x1 = awdata[15 : 0], y1 = awdata[31 : 16];
    wire [16 : 0] sum_x = {1'b0, x0} + {1'b0, x1}, sum_y = {1'b0, y0} + {1'b0, y1};
    wire valid_box = x0 < x1 && y0 < y1;
    wire roi_match = valid_box && sum_x >= {ROI_X0, 1'b0} && sum_x < {ROI_X1, 1'b0} &&
                     sum_y >= {ROI_Y0, 1'b0} && sum_y < {ROI_Y1, 1'b0};
    assign awready = !reset && !bus_reset && !pending && !soft_reset && !abort_txn;
    assign arready = !reset && !bus_reset && !soft_reset && (!rvalid || rready);
    region_alarm_core #(.TIMEOUT_MS(TIMEOUT_MS))
        core(.clk(clk),
             .reset(reset | soft_reset),
             .tick_ms(tick_ms),
             .commit(pulse && !bus_reset && !abort_txn),
             .fresh(pulse_fresh),
             .region_hit(pulse_hit),
             .frame_id(pulse_frame),
             .alarm(alarm),
             .fault(fault),
             .have_frame(have_frame),
             .positive_run(positives),
             .empty_run(empties),
             .last_frame(last_frame),
             .accepted_count(accepts),
             .rejected_count(rejects),
             .events(events));
    always @(posedge clk) begin
        if (reset || soft_reset) begin
            pending <= 0;
            txn <= 0;
            bad <= 0;
            have_xy <= 0;
            shadow_frame <= 0;
            xy <= 0;
            boxes <= 0;
            hit <= 0;
            pulse <= 0;
            pulse_fresh <= 0;
            pulse_hit <= 0;
            pulse_frame <= 0;
            soft_reset <= 0;
            fault_seen <= 0;
            txn_age_ms <= 0;
            protocol_errors <= 0;
            event_latch <= 0;
            rvalid <= 0;
            rdata <= 0;
        end else if (bus_reset) begin
            fault_seen <= fault;
            txn_age_ms <= 0;
            // Abort transport/partial frame while retaining accepted business state.
            pending <= 0;
            txn <= 0;
            bad <= 0;
            have_xy <= 0;
            shadow_frame <= 0;
            xy <= 0;
            boxes <= 0;
            hit <= 0;
            pulse <= 0;
            pulse_fresh <= 0;
            pulse_hit <= 0;
            pulse_frame <= 0;
            soft_reset <= 0;
            rvalid <= 0;
            rdata <= 0;
            event_latch <= event_latch | {28'b0, events};
        end else begin
            fault_seen <= fault;
            if (!txn || abort_txn)
                txn_age_ms <= 0;
            else if (tick_ms)
                txn_age_ms <= txn_age_ms + 1'b1;
            if (abort_txn) begin
                txn <= 0;
                bad <= 0;
                have_xy <= 0;
                shadow_frame <= 0;
                xy <= 0;
                boxes <= 0;
                hit <= 0;
            end
            pulse <= 0;
            pending <= 0;
            soft_reset <= 0;
            event_latch <= event_latch | {28'b0, events};
            if (rvalid && rready)
                rvalid <= 0;
            if (arvalid && arready) begin
                rvalid <= 1;
                case (araddr[17 : 2])
                    16'h0008:
                        rdata <= {19'b0, hit, empties, positives, 1'b0, have_frame, fault, alarm};
                    16'h0009:
                        rdata <= last_frame;
                    16'h000a:
                        rdata <= accepts;
                    16'h000b:
                        rdata <= rejects;
                    16'h000c:
                        rdata <= protocol_errors;
                    16'h000d:
                        rdata <= event_latch;
                    16'h0030:
                        rdata <= 32'hA5050001;
                    default:
                        rdata <= 32'hBAD04960;
                endcase
            end
            if (awvalid && awready) begin
                case (awaddr[17 : 2])
                    16'h0000:
                        if (awdata == 1)
                            soft_reset <= 1;
                    16'h0001: begin
                        if (txn)
                            protocol_errors <= protocol_errors + 1;
                        txn_age_ms <= 0;
                        txn <= 1;
                        bad <= 0;
                        have_xy <= 0;
                        boxes <= 0;
                        hit <= 0;
                        shadow_frame <= awdata;
                    end
                    16'h0002: begin
                        if (!txn || have_xy) begin
                            bad <= 1;
                            protocol_errors <= protocol_errors + 1;
                        end
                        xy <= awdata;
                        have_xy <= 1;
                    end
                    16'h0003: begin
                        if (!txn || !have_xy || boxes == 16 || !valid_box) begin
                            bad <= 1;
                            protocol_errors <= protocol_errors + 1;
                        end else begin
                            boxes <= boxes + 1;
                            hit <= hit | roi_match;
                        end
                        have_xy <= 0;
                    end
                    16'h0004: begin
                        txn <= 0;
                        have_xy <= 0;
                        pending <= 1;
                        if (!txn || bad || have_xy || awdata[15 : 8] != {3'b0, boxes} ||
                            |awdata[31 : 16] || |awdata[7 : 1])
                            protocol_errors <= protocol_errors + 1;
                        else begin
                            pulse <= 1;
                            pulse_frame <= shadow_frame;
                            pulse_fresh <= awdata[0];
                            pulse_hit <= hit;
                        end
                    end
                    16'h000e:
                        event_latch <= (event_latch & ~awdata) | {28'b0, events};
                    default:
                        protocol_errors <= protocol_errors + 1;
                endcase
            end
        end
    end
endmodule
