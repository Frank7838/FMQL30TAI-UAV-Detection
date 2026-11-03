`timescale 1ns / 1ps
// 仿真中把超时缩为5个tick以覆盖转换；产品超时仍为1000 ms。
// 独立见证器依据alarm/fault实际边沿计时，并与旧告警实现并行比较。
module tb_observability;
    reg clk = 0;
    always
        #5 clk = ~clk;
    reg reset = 1, bus_reset = 0, tick = 0, av = 0, arv = 0, rr = 1;
    reg [31 : 0] aa = 0, ad = 0, ra = 0;
    wire ready, arready, rv, alarm, fault;
    wire bready, barready, brv, balarm, bfault;
    wire [31 : 0] rd, brd;
    region_alarm_regs_m505 #(.TIMEOUT_MS(5))
        dut(.clk(clk),
            .reset(reset),
            .tick_ms(tick),
            .bus_reset(bus_reset),
            .awaddr(aa),
            .awdata(ad),
            .araddr(ra),
            .awvalid(av),
            .arvalid(arv),
            .rready(rr),
            .awready(ready),
            .arready(arready),
            .rdata(rd),
            .rvalid(rv),
            .alarm(alarm),
            .fault(fault));
    baseline_region_alarm #(.TIMEOUT_MS(5))
        reference(.clk(clk),
                  .reset(reset),
                  .tick_ms(tick),
                  .bus_reset(bus_reset),
                  .awaddr(aa),
                  .awdata(ad),
                  .araddr(ra),
                  .awvalid(av),
                  .arvalid(arv),
                  .rready(rr),
                  .awready(bready),
                  .arready(barready),
                  .rdata(brd),
                  .rvalid(brv),
                  .alarm(balarm),
                  .fault(bfault));

    reg [63 : 0] ref_cycles = 0, edge_cycle;
    reg [63 : 0] expected_time[0 : 3];
    reg [31 : 0] expected_frame[0 : 3], expected_seq = 0;
    reg [3 : 0] expected_valid = 0, expected_mask = 0, change;
    reg old_alarm, old_fault, old_soft;
    integer k, checks = 0, transitions = 0;
    // Independent witness uses actual core output edges, not observer events.
    always @(posedge clk) begin
        if (reset) begin
            ref_cycles = 0;
            expected_seq = 0;
            expected_valid = 0;
            expected_mask = 0;
            for (k = 0; k < 4; k = k + 1) begin
                expected_time[k] = 0;
                expected_frame[k] = 0;
            end
        end else begin
            old_alarm = alarm;
            old_fault = fault;
            old_soft = dut.soft_reset;
            edge_cycle = ref_cycles;
            ref_cycles = ref_cycles + 1;
            #1;
            if (dut.observer.cycles !== ref_cycles)
                $fatal(1, "hardware clock counter mismatch");
            if ({alarm, fault, ready, arready, rv} !== {balarm, bfault, bready, barready, brv})
                $fatal(1, "observer changed existing business behavior");
            if (dut.accepts !== reference.accepts || dut.rejects !== reference.rejects ||
                dut.protocol_errors !== reference.protocol_errors ||
                dut.event_latch !== reference.event_latch)
                $fatal(1, "old counters/event latch changed");
            if (rv && ((ra < 32'h40) || ra == 32'hc0) && rd !== brd)
                $fatal(1, "old register read changed");
            change = 0;
            // Explicit soft-clear has never emitted a core clear/recovery event.
            if (!old_soft) begin
                if (alarm && !old_alarm)
                    change[0] = 1;
                if (!alarm && old_alarm)
                    change[1] = 1;
                if (fault && !old_fault)
                    change[2] = 1;
                if (!fault && old_fault)
                    change[3] = 1;
            end
            if (change != 0) begin
                transitions = transitions + 1;
                expected_seq = expected_seq + 1;
                expected_valid = expected_valid | change;
                expected_mask = change;
                for (k = 0; k < 4; k = k + 1)
                    if (change[k]) begin
                        expected_time[k] = edge_cycle;
                        expected_frame[k] = dut.last_frame;
                    end
            end
        end
    end
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
    task read_word
        (input [31 : 0] a,
         output [31 : 0] v);
        begin
            @(negedge clk);
            ra = a;
            arv = 1;
            do
                @(posedge clk);
            while (!arready);
            @(negedge clk);
            arv = 0;
            if (!rv)
                $fatal(1, "read response missing");
            v = rd;
            @(negedge clk);
        end
    endtask
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
    task ticks
        (input integer n);
        integer i;
        begin
            for (i = 0; i < n; i = i + 1) begin
                @(negedge clk);
                tick = 1;
                @(negedge clk);
                tick = 0;
            end
            repeat (4) @(negedge clk)
                ;
        end
    endtask
    reg [31 : 0] lo, hi, v;
    task check_observer;
        integer i;
        begin
            repeat (3) @(negedge clk)
                ;
            read_word('h40, v);
            if (v !== 32'h4f425331)
                $fatal(1, "OBS1 magic missing");
            read_word('h44, v);
            if (v !== 100000000)
                $fatal(1, "clock metadata wrong");
            read_word('hc0, v);
            if (v !== 32'ha5050001)
                $fatal(1, "base ABI changed");
            read_word('h50, v);
            if (v !== expected_seq)
                $fatal(1, "sequence wrong actual=%d expected=%d", v, expected_seq);
            read_word('h54, v);
            if (v !== {28'b0, expected_valid})
                $fatal(1, "valid mask wrong");
            read_word('h58, v);
            if (v !== {28'b0, expected_mask})
                $fatal(1, "last mask wrong");
            for (i = 0; i < 4; i = i + 1) begin
                read_word('h60 + i * 8, lo);
                read_word('h64 + i * 8, hi);
                read_word('h80 + i * 4, v);
                if ({hi, lo} !== expected_time[i] || v !== expected_frame[i])
                    $fatal(1,
                           {"event record wrong event=%d actual=%h expected=%h frame=%d ",
                            "expectedFrame=%d"},
                           i, {hi, lo}, expected_time[i], v, expected_frame[i]);
            end
            checks = checks + 1;
        end
    endtask
    integer i;
    reg [31 : 0] held, old_protocol;
    reg [63 : 0] before_reset;
    initial begin
        repeat (4) @(negedge clk)
            ;
        reset = 0;
        check_observer();
        frame(1, 1);
        frame(2, 1);
        frame(3, 1);
        if (!alarm)
            $fatal(1, "assert setup");
        check_observer();
        for (i = 4; i <= 11; i = i + 1)
            frame(i, 0);
        if (alarm)
            $fatal(1, "clear setup");
        check_observer();
        ticks(5);
        if (!fault || alarm)
            $fatal(1, "fault setup");
        check_observer();
        frame(12, 1);
        if (fault || alarm)
            $fatal(1, "recover setup");
        check_observer();
        frame(13, 1);
        frame(14, 1);
        if (!alarm)
            $fatal(1, "second assert");
        check_observer();
        wr('h38, 15);
        check_observer(); // W1C does not erase history or sequence.
        before_reset = dut.observer.cycles;
        @(negedge clk);
        bus_reset = 1;
        repeat (12) @(negedge clk)
            ;
        bus_reset = 0;
        if (dut.observer.cycles <= before_reset)
            $fatal(1, "bus reset stopped clock history");
        check_observer();
        wr(0, 1);
        check_observer(); // Business clear preserves observer clock/records.
        @(negedge clk);
        dut.observer.cycles = 64'h00000000fffffff0;
        ref_cycles = 64'h00000000fffffff0;
        frame(1, 1);
        frame(2, 1);
        frame(3, 1);
        check_observer();
        if (expected_time[0][63 : 32] !== 1)
            $fatal(1, "64bit low-half rollover not exercised");
        // Live timer read remains latched under bus backpressure.
        @(negedge clk);
        rr = 0;
        ra = 'h48;
        arv = 1;
        do
            @(posedge clk);
        while (!arready);
        @(negedge clk);
        arv = 0;
        held = rd;
        repeat (10) begin
            @(negedge clk);
            if (!rv || rd !== held)
                $fatal(1, "observer read changed while stalled");
        end
        rr = 1;
        repeat (3) @(negedge clk)
            ;
        // Read-only extension writes retain original unknown-write error behavior.
        old_protocol = dut.protocol_errors;
        wr('h40, 1);
        if (dut.protocol_errors !== old_protocol + 1)
            $fatal(1, "extension became writable");
        check_observer();
        @(negedge clk);
        dut.observer.event_seq = 32'hfffffffe;
        expected_seq = 32'hfffffffe;
        for (i = 4; i <= 11; i = i + 1)
            frame(i, 0);
        ticks(5);
        check_observer();
        if (expected_seq !== 0)
            $fatal(1, "sequence rollover not exercised");
        // Hardware business reset is the only epoch reset.
        @(negedge clk);
        reset = 1;
        repeat (4) @(negedge clk)
            ;
        reset = 0;
        check_observer();
        $display(
            "OBSERVABILITY_CYCLE_EXACT_ABI_RESET_ROLLOVER_PASS checkpoints=%0d transitions=%0d",
            checks, transitions);
        $finish;
    end
    initial begin
        #1000000;
        $fatal(1, "testbench deadline");
    end
endmodule
