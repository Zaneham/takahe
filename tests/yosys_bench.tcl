read_liberty -lib /lib/sky130_fd_sc_hd__tt_025C_1v80.lib
read_verilog /work/picorv32_clean.v
hierarchy -top picorv32
proc; opt; flatten; opt
techmap
dfflibmap -liberty /lib/sky130_fd_sc_hd__tt_025C_1v80.lib
abc -liberty /lib/sky130_fd_sc_hd__tt_025C_1v80.lib
clean
stat
