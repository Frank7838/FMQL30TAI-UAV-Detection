#####################  hdmi ############################
set_property PACKAGE_PIN AC17 [get_ports HDMI_CLK_P]
set_property PACKAGE_PIN AA15 [get_ports {HDMI_TX_P[0]}]
set_property PACKAGE_PIN Y16 [get_ports {HDMI_TX_P[1]}]
set_property PACKAGE_PIN W16 [get_ports {HDMI_TX_P[2]}]
set_property IOSTANDARD TMDS_33 [get_ports HDMI_CLK_P]
set_property IOSTANDARD TMDS_33 [get_ports {HDMI_TX_P[*]}]
#####################  hdmi ############################

set_property PACKAGE_PIN F8 [get_ports sys_rst]
set_property IOSTANDARD LVCMOS15 [get_ports sys_rst]
set_property PACKAGE_PIN E7 [get_ports rx_gtx_full_reset]
set_property IOSTANDARD LVCMOS15 [get_ports rx_gtx_full_reset]

#LED1, BANK13
set_property IOSTANDARD LVCMOS15 [get_ports {led[0]}]
set_property PACKAGE_PIN J1 [get_ports {led[0]}]
#LED2, BANK13
set_property IOSTANDARD LVCMOS15 [get_ports {led[1]}]
set_property PACKAGE_PIN M6 [get_ports {led[1]}]

create_clock -period 5.000 [get_ports sys_clk_p]
set_property IOSTANDARD DIFF_SSTL15 [get_ports sys_clk_p]
set_property IOSTANDARD DIFF_SSTL15 [get_ports sys_clk_n]
set_property PACKAGE_PIN C8 [get_ports sys_clk_p]
set_property PACKAGE_PIN C7 [get_ports sys_clk_n]

set_property PACKAGE_PIN AB3 [get_ports FMC_HPC_DP0_M2C_N]
set_property PACKAGE_PIN AB4 [get_ports FMC_HPC_DP0_M2C_P]
create_clock -period 6.734 [get_ports FMC_HPC_GBTCLK0_M2C_C_P]
set_property PACKAGE_PIN U6 [get_ports FMC_HPC_GBTCLK0_M2C_C_P]; # WUJING BOARD
set_property PACKAGE_PIN U5 [get_ports FMC_HPC_GBTCLK0_M2C_C_N]
#set_property PACKAGE_PIN R6 [get_ports FMC_HPC_GBTCLK0_M2C_C_P] ## WUJING LITE BOARD
#set_property PACKAGE_PIN R5 [get_ports FMC_HPC_GBTCLK0_M2C_C_N]

#pjtag:7030 Demo
set_property PACKAGE_PIN AD16 [get_ports PJTAG_TDO]
set_property PACKAGE_PIN AB14 [get_ports PJTAG_TMS]
set_property PACKAGE_PIN AB15 [get_ports PJTAG_TCK]
set_property PACKAGE_PIN AD15 [get_ports PJTAG_TDI]
set_property IOSTANDARD LVCMOS33 [get_ports PJTAG_TCK]
set_property IOSTANDARD LVCMOS33 [get_ports PJTAG_TMS]
set_property IOSTANDARD LVCMOS33 [get_ports PJTAG_TDI]
set_property IOSTANDARD LVCMOS33 [get_ports PJTAG_TDO]

set_property PULLUP true [get_ports PJTAG_TDO]
create_clock -period 20.000 -name jtag_clk -waveform {0.000 10.000} [get_ports PJTAG_TCK]

########################
#set_property SEVERITY {Warning} [get_drc_checks {REQP-44}]
#set_property SEVERITY {Warning} [get_drc_checks {REQP-46}]
#set_property SEVERITY {Warning} [get_drc_checks {REQP-52}]
#set_property SEVERITY {Warning} [get_drc_checks {REQP-56}]

