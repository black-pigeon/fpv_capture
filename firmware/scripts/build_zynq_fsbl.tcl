set hw [lindex $argv 0]
set out_dir [lindex $argv 1]
set repo_dir [lindex $argv 2]
set fsbl_proc [lindex $argv 3]
set fsbl_stdio [lindex $argv 4]
set fsbl_debug_level [lindex $argv 5]

if {$fsbl_proc eq ""} {
	set fsbl_proc "ps7_cortexa9_0"
}

if {$fsbl_stdio eq ""} {
	set fsbl_stdio "ps7_uart_1"
}

if {$fsbl_debug_level eq ""} {
	set fsbl_debug_level "off"
}

proc rewrite_file {path map_list} {
	if {![file exists $path]} {
		return
	}

	set fd [open $path r]
	set data [read $fd]
	close $fd

	set data [string map $map_list $data]

	set fd [open $path w]
	puts -nonewline $fd $data
	close $fd
}

proc uart_baseaddr {inst} {
	switch -- $inst {
		ps7_uart_0 { return 0xE0000000 }
		ps7_uart_1 { return 0xE0001000 }
		default { error "unsupported Zynq-7000 stdio peripheral: $inst" }
	}
}

proc configure_stdio {mss_path xparams_path stdin_inst stdout_inst} {
	set stdin_base [format "0x%08X" [uart_baseaddr $stdin_inst]]
	set stdout_base [format "0x%08X" [uart_baseaddr $stdout_inst]]

	rewrite_file $mss_path [list \
		"PARAMETER stdin = ps7_uart_0" "PARAMETER stdin = $stdin_inst" \
		"PARAMETER stdin = ps7_uart_1" "PARAMETER stdin = $stdin_inst" \
		"PARAMETER stdout = ps7_uart_0" "PARAMETER stdout = $stdout_inst" \
		"PARAMETER stdout = ps7_uart_1" "PARAMETER stdout = $stdout_inst" \
	]

	rewrite_file $xparams_path [list \
		"#define STDIN_BASEADDRESS 0xE0000000" "#define STDIN_BASEADDRESS $stdin_base" \
		"#define STDIN_BASEADDRESS 0xE0001000" "#define STDIN_BASEADDRESS $stdin_base" \
		"#define STDOUT_BASEADDRESS 0xE0000000" "#define STDOUT_BASEADDRESS $stdout_base" \
		"#define STDOUT_BASEADDRESS 0xE0001000" "#define STDOUT_BASEADDRESS $stdout_base" \
	]
}

proc configure_fsbl_debug {cfg_path level} {
	if {![file exists $cfg_path]} {
		return
	}

	switch -- $level {
		off {
			return
		}
		basic {
			rewrite_file $cfg_path [list \
				"#define FSBL_DEBUG_INFO" "#define FSBL_DEBUG_INFO" \
			]
		}
		default {
			error "unsupported Zynq-7000 FSBL debug level: $level"
		}
	}
}

if {$hw eq "" || $out_dir eq ""} {
	puts "usage: xsct build_zynq_fsbl.tcl <hw.hdf|hw.xsa> <out_dir> <embeddedsw_repo> ?<fsbl_proc>? ?<fsbl_stdio>? ?<fsbl_debug_level>?"
	exit 1
}

if {[file exists $out_dir]} {
	file delete -force $out_dir
}
if {$repo_dir ne ""} {
	hsi::set_repo_path $repo_dir
}

file mkdir $out_dir
set hw_dir [file join $out_dir hw_0]
set fsbl_dir [file join $hw_dir zynq_fsbl]

hsi::open_hw_design $hw

hsi::create_sw_design fsbl_sw -proc $fsbl_proc -os standalone
hsi::add_library xilffs
hsi::add_library xilrsa
hsi::generate_app -dir $fsbl_dir -app zynq_fsbl
hsi::close_sw_design [hsi::current_sw_design]
hsi::close_hw_design [hsi::current_hw_design]

configure_fsbl_debug [file join $fsbl_dir fsbl_debug.h] $fsbl_debug_level
configure_stdio \
	[file join $fsbl_dir zynq_fsbl_bsp fsbl_sw.mss] \
	[file join $fsbl_dir zynq_fsbl_bsp $fsbl_proc include xparameters.h] \
	$fsbl_stdio $fsbl_stdio

exit
