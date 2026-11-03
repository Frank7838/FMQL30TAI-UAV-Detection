# Vivado2018.3 restricted XDC syntax: pure query/constraint commands only.
# Endpoint/clock assertions run after vendor OPT POST creates PS clocks.
# Held-bundle maxdelay is applied in true Tcl after actual PS/DDR clocks exist,
# before placement. Both constrained path groups are audited again after route.
set p1_ctrl_first [get_cells -hier -regexp {.*p1_subsystem_u/control/(result_t1_gp_reg|stat1_gp_reg.*|req1_hp_reg|enable1_hp_reg|stop1_hp_reg|ack1_hp_reg|runtime_hp1_gp_reg)}]
set_false_path -to [get_pins -of_objects $p1_ctrl_first -filter {REF_PIN_NAME == D}]
