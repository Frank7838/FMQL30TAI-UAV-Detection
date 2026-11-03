# 在已打开的厂商父工程中接入当前源码，只准备工程文件，不运行构建或下载。
if {![llength [get_projects -quiet]]} {error "Open the matching vendor parent project first"}
set source_root [file normalize [file join [file dirname [info script]] ../..]]
set parent_root [get_property DIRECTORY [current_project]]
set business_root [file join $parent_root rtl business_m505]
file mkdir $business_root
foreach name {region_alarm_core.v region_alarm_regs_m505.v alarm_tick_100m.v business_reset_sync.v alarm_observer.v} {
    set target [file join $business_root $name]
    file copy -force [file join $source_root src pl alarm $name] $target
    if {![llength [get_files -quiet $target]]} {add_files -norecurse $target}
}
file copy -force [file join $source_root src pl integration ai7030_edif_top.v] [file join $parent_root rtl ai7030_edif_top.v]
update_compile_order -fileset sources_1
puts "Current alarm, observer and LED wiring prepared. No build or device operation performed."
