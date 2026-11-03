set_property SEVERITY {Warning} [get_drc_checks {REQP-44}]
set_property SEVERITY {Warning} [get_drc_checks {REQP-46}]
set_property SEVERITY {Warning} [get_drc_checks {REQP-52}]
set_property SEVERITY {Warning} [get_drc_checks {REQP-56}]

set_property LOC PLLE2_ADV_X1Y3 [get_cells U0_ps_ai_peripheral/ZGolo/u_AI_Mate_top/clkrst_wrapper_u0/AI_CORE_CLK/inst/plle2_adv_inst]

set_false_path -from [get_ports sys_rst]
set_false_path -to [get_ports {led[0]}]
set_false_path -to [get_ports {led[1]}]
set_false_path -from [get_clocks clk_out1_clk_wiz_0] -to [get_clocks clk_out2_clk_wiz_0]
set_false_path -from [get_clocks clk_out2_clk_wiz_0] -to [get_clocks clk_out1_clk_wiz_0]
set_false_path -from [get_clocks clk_out1_clk_wiz_0] -to [get_clocks clk_pll_i]
set_false_path -from [get_clocks clk_pll_i] -to [get_clocks clk_out1_clk_wiz_0]
set_false_path -from [get_clocks clk_out1_sys_clk] -to [get_clocks clk_out1_mgtclk]


create_clock -period 6.734 [get_pins rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK]
create_clock -period 6.734 [get_pins rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLKFABRIC]
create_clock -period 6.734 [get_pins rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/TXOUTCLK]
create_clock -period 6.734 [get_pins rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/TXOUTCLKFABRIC]

## gp_clk and cam_clk
set_false_path -from [get_clocks -of_objects [get_pins U0_ps_ai_peripheral/ps_ai_wrapper_u0/clk_wiz_0_inst/inst/plle2_adv_inst/CLKOUT0]] -to [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK]
set_false_path -from [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK] -to [get_clocks -of_objects [get_pins U0_ps_ai_peripheral/ps_ai_wrapper_u0/clk_wiz_0_inst/inst/plle2_adv_inst/CLKOUT0]]

# 27MHz and RXOUTCLK
set_false_path -from [get_clocks -of_objects [get_pins rx_sdi_inst/sys_clk_inst0/inst/mmcm_adv_inst/CLKOUT0]] -to [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK]
set_false_path -from [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK] -to [get_clocks -of_objects [get_pins rx_sdi_inst/sys_clk_inst0/inst/mmcm_adv_inst/CLKOUT0]]

# from ddr_clk to cam_clk
set_false_path -from [get_clocks -of_objects [get_pins U0_ps_ai_peripheral/mig_ddr3_u0/u_mig_7series_0_mig/u_ddr3_infrastructure/gen_mmcm.mmcm_i/CLKFBOUT]] -to [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK]
set_false_path -from [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK] -to [get_clocks -of_objects [get_pins U0_ps_ai_peripheral/mig_ddr3_u0/u_mig_7series_0_mig/u_ddr3_infrastructure/gen_mmcm.mmcm_i/CLKFBOUT]]

#from cam_clk to gp_clk
set_false_path -from [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/RXOUTCLK] -to [get_clocks -of_objects [get_pins U0_ps_ai_peripheral/ps_ai_wrapper_u0/clk_wiz_0_inst/inst/plle2_adv_inst/CLKOUT0]]

# unknown
set_false_path -from [get_clocks rx_sdi_inst/SDI0/SDI/GTX_i/gtxe2_i/TXOUTCLK] -to [get_clocks -of_objects [get_pins rx_sdi_inst/sys_clk_inst0/inst/mmcm_adv_inst/CLKOUT0]]
