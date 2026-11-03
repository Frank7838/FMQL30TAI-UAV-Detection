// 业务复位异步置位、两级同步释放。业务生命周期跟随硬件/PS复位。
// NPU传输soft reset不清alarm与已接受计数；实际接线见integration目录。

module business_reset_sync
    (input wire clk, hardware_reset,
     output wire reset);
    // ASYNC_REG标记两级同步链；异步断言使硬件复位立即生效，释放经过本业务时钟同步。
    (* ASYNC_REG = "TRUE" *) reg [1 : 0] sync = 2'b11;
    always @(posedge clk or posedge hardware_reset)
        if (hardware_reset)
            sync <= 2'b11;
        else
            sync <= {sync[0], 1'b0};
    assign reset = sync[1];
endmodule
