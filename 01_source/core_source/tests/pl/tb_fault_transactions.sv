// 模拟帧事务超时、重复提交与恢复过程。
`timescale 1ns / 1ps
module tb_fault_transactions;
    reg clk = 0;
    always
        #5 clk = ~clk;
    reg reset = 1, bus_reset = 0, tick = 0, av = 0, arv = 0, rr = 1;
    reg [31 : 0] aa = 0, ad = 0, ra = 0;
    wire ready, arready, rv, alarm, fault;
    wire [31 : 0] rd;
    region_alarm_regs_m505 #(.TIMEOUT_MS(5))
        dut(.clk(clk),
            .reset(reset),
            .bus_reset(bus_reset),
            .tick_ms(tick),
            .awaddr(aa),
            .awdata(ad),
            .awvalid(av),
            .arvalid(arv),
            .rready(rr),
            .awready(ready),
            .arready(arready),
            .rvalid(rv),
            .rdata(rd),
            .alarm(alarm),
            .fault(fault));
    // 按寄存器握手完成一次写入。
    task wr
        (input [31 : 0] a, d);
        begin
            @(negedge clk);
            aa = a;
            ad = d;
            av = 1;
            do
                @(posedge clk);
            while (!ready);
            @(negedge clk);
            av = 0;
            repeat (3) @(negedge clk)
                ;
        end
    endtask
    // 产生n个计时脉冲，仿真中不等待真实毫秒。
    task ticks
        (input integer n);
        integer k;
        begin
            for (k = 0; k < n; k = k + 1) begin
                @(negedge clk);
                tick = 1;
                @(negedge clk);
                tick = 0;
            end
            repeat (3) @(negedge clk)
                ;
        end
    endtask
    // 用帧号、坐标和COMMIT提交完整观测。
    task frame
        (input [31 : 0] id,
         input positive);
        begin
            wr(4, id);
            if (positive) begin
                wr(8, 32'h40004000);
                wr(12, 32'h80008000);
            end
            wr(16, positive ? 32'h101 : 32'h1);
        end
    endtask
    // 连续提交三帧命中，建立已告警的初态。
    task setup_alarm;
        begin
            frame(1, 1);
            frame(2, 1);
            frame(3, 1);
            if (!alarm || fault || dut.accepts != 3)
                $fatal(1, "setup failed");
        end
    endtask
    // 超时后告警保持，过期事务不得增加有效提交数。
    task check_held;
        begin
            repeat (4) @(negedge clk)
                ;
            if (!alarm || !fault || dut.accepts != 3 || dut.last_frame != 3)
                $fatal(1, "STALE_TRANSACTION_ACCEPTED alarm=%b fault=%b count=%d last=%d", alarm,
                       fault, dut.accepts, dut.last_frame);
        end
    endtask
    integer which, i;
    reg [31 : 0] held_read;
    initial begin
        if (!$value$plusargs("CASE=%d", which))
            which = 0;
        repeat (4) @(negedge clk)
            ;
        reset = 0;
        repeat (3) @(negedge clk)
            ;
        case (which)
            0: begin // Transaction starts before stream timeout, completes after it.
                setup_alarm();
                wr(4, 4);
                ticks(5);
                if (!fault)
                    $fatal(1, "fault missing");
                wr(16, 1);
                check_held();
            end
            1: begin // Already faulted: a newly-started transaction cannot stay live forever.
                setup_alarm();
                ticks(5);
                wr(4, 4);
                ticks(5);
                wr(16, 1);
                check_held();
            end
            2: begin // A complete new observation recovers; eight new negatives clear alarm.
                setup_alarm();
                ticks(5);
                frame(4, 0);
                if (fault || !alarm || dut.empties != 1)
                    $fatal(1, "fresh recovery incorrect");
                for (i = 5; i <= 10; i = i + 1) begin
                    frame(i, 0);
                    if (!alarm)
                        $fatal(1, "cleared before eighth negative");
                end
                frame(11, 0);
                if (alarm || fault || dut.accepts != 11)
                    $fatal(1, "eighth negative did not clear");
            end
            3: begin // COMMIT is scheduled on the exact cycle the core enters fault.
                setup_alarm();
                ticks(4);
                wr(4, 4);
                @(negedge clk);
                aa = 16;
                ad = 1;
                av = 1;
                tick = 1;
                @(posedge clk);
                if (!ready)
                    $fatal(1, "collision setup not accepted");
                @(negedge clk);
                tick = 0;
                av = 0;
                repeat (5) @(negedge clk)
                    ;
                check_held();
            end
            4: begin // GP/NPU reset aborts transport; independent business fault/hold survives.
                setup_alarm();
                wr(4, 4);
                wr(8, 32'h40004000);
                bus_reset = 1;
                ticks(5);
                if (!alarm || !fault || ready || arready)
                    $fatal(1, "transport reset disturbed business state");
                bus_reset = 0;
                repeat (4) @(negedge clk)
                    ;
                frame(4, 0);
                if (fault || !alarm || dut.empties != 1)
                    $fatal(1, "recovery after transport reset failed");
            end
            5: begin // Duplicate/unfresh traffic never refreshes watchdog or clears alarm.
                setup_alarm();
                ticks(3);
                frame(3, 0);
                ticks(2);
                check_held();
                frame(3, 0);
                check_held();
                wr(4, 4);
                wr(16, 0);
                check_held();
                if (dut.rejects != 3)
                    $fatal(1, "rejection count changed");
            end
            6: begin // Malformed box/count cannot become a valid empty result.
                setup_alarm();
                wr(4, 4);
                wr(8, 32'h80008000);
                wr(12, 32'h40004000);
                wr(16, 1);
                if (dut.accepts != 3 || !alarm || dut.protocol_errors != 2)
                    $fatal(1, "malformed result accepted");
                ticks(5);
                check_held();
            end
            7: begin // New START backpressured on transaction-expiry edge is not lost.
                setup_alarm();
                ticks(5);
                wr(4, 4);
                ticks(4);
                @(negedge clk);
                aa = 4;
                ad = 5;
                av = 1;
                tick = 1;
                #1;
                if (ready)
                    $fatal(1, "expiry edge should backpressure write");
                @(posedge clk);
                @(negedge clk);
                tick = 0;
                do
                    @(posedge clk);
                while (!ready);
                @(negedge clk);
                av = 0;
                repeat (3) @(negedge clk)
                    ;
                wr(16, 1);
                if (fault || !alarm || dut.last_frame != 5 || dut.accepts != 4 ||
                    dut.protocol_errors != 0)
                    $fatal(1, "held START lost or recovery failed");
            end
            8: begin // Held read data remains stable across the asynchronous business event.
                setup_alarm();
                @(negedge clk);
                ra = 'h20;
                arv = 1;
                rr = 0;
                do
                    @(posedge clk);
                while (!arready);
                @(negedge clk);
                arv = 0;
                held_read = rd;
                ticks(5);
                if (!rv || rd !== held_read || !fault || !alarm)
                    $fatal(1, "stalled read or alarm changed");
                rr = 1;
                repeat (3) @(negedge clk)
                    ;
                frame(4, 0);
                if (fault || !alarm)
                    $fatal(1, "stalled read prevented recovery");
            end
            default:
                $fatal(1, "unknown case");
        endcase
        $display("PL_FAULT_TRANSACTION_CASE_PASS case=%0d", which);
        $finish;
    end
    initial begin
        #1000000;
        $fatal(1, "simulation deadline");
    end
endmodule
