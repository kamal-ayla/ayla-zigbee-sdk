#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

#
# ayla_lib: common functions used in Ayla TCL scripts
# 

set mod_fd ""

#
# error codes
#
set err(stram_timeout)	1
# reserved		2
set err(mtest_time) 	3
set err(usage)		4
set err(mtest_clocks)	7
set err(mtest_gpio)	8
set err(mtest_spi)	9
set err(mtest_log)	10
set err(flash_setup)	11
set err(flash_setup_time) 12
set err(mtest_get_sig)	21
set err(mtest_get_sig_time)	22
set err(load_flash)	30
set err(load_flash_time) 31
set err(mfg_dl) 	32
set err(mfg_dl_time) 	33
set err(ram_start)	34
set err(ram_load)	35
set err(mod_reset_jtag)	41
set err(mod_start_mfg) 	49
set err(mod_start) 	50
set err(mod_cmd) 	51
set err(mod_cmd_time) 	52
set err(mod_test) 	53
set err(gw_setup_agent)	54
set err(gw_setup_cmd)	55
set err(gw_setup_time)	56
set err(serial)		60
set err(serial_old)	61
set err(serial_old_time) 62
set err(serial_old_conf) 63
set err(serial_get)	64
set err(serial_none)	65
set err(serial_write)	66
set err(serial_parse)	67
set err(serial_not_mfg) 68
set err(serial_not_passed) 69
set err(serial_test_time) 70
set err(serial_no_mac)	71
set err(file_crc)	72
set err(console_port)		73
set err(console_connect)	74
set err(mac_from_mod)		75
set err(mac_get)		76
set err(mac_none)		77
set err(mac_parse)		78
set err(flash_erase)		81
set err(flash_erase_time) 	82
set err(flash_init) 		91
set err(flash_init_time) 	92
set err(unknown) 		99

proc log_time {} {
	global start_time

	set elapsed [expr [clock seconds] - $start_time]
	set mins [expr $elapsed / 60]
	set secs [expr $elapsed % 60]
	return [format "%3d:%2.2d" $mins $secs]
}

#
# Log message
#
proc log_msg {level msg} {
	global cmdname 

	puts stderr "[log_time] $cmdname: $level: $msg"
}

proc log_err {msg} {
	log_msg error $msg
}

proc log_info {msg} {
	log_msg info $msg
}

proc log_warn {msg} {
	log_msg warning $msg
}

proc log_debug {msg} {
	global debug

	if {$debug} {
		log_msg debug $msg
	}
}

#
# Find the Windows serial port to use to talk to the Ayla device.
# Searches the registry by running DOS command "reg query".
# Uses temp file from global temp_file
# Returns the name of the port.
#
proc find_port {} {
	global conf
	global cmdname
	global temp_file
	global conf_file

	set ports 0
	set port ""

	if [catch {
		set key {HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM}
		exec reg query $key > $temp_file
	}] {
		puts "$cmdname: error getting com port with regedit"
		exit 1
	}
	if [catch {
		set fd [open $temp_file r]
	}] {
		puts "$cmdname: error getting com port: can't open $temp_file"
		exit 2
	}
	set data [read $fd]
	set lines [split $data "\n\r"]

	foreach line $lines {
		set token [split [regsub -all {[ 	]+} $line " "]]
		set argc [llength $token]
		if {$argc == 4 && [regexp {\\Device\\VCP*} [lindex $token 1]]} {
			set port [lindex $token 3]
			incr ports
		}
	}
	if {$ports > 1} {
		puts "$cmdname: error: found $ports possible com ports to use"
		puts "$cmdname: Please configure com port in $conf_file."
		exit 2
	}
	return $port
}

#
# read conf() array from file
#
proc conf_read {file} {
	global conf
	global cmds
	global cmds_nosave
	global cmdname

	if [catch {
		set fd [open $file r]
	}] {
		puts "$cmdname: error: cannot open configuration file: $file"
		puts "$cmdname: please create $file based on oem_pkg\\$file"
		exit 2
	}
	set data [read $fd]
	set lines [split $data "\n\r"]

	set cmd_mode 0

	foreach line $lines {
		if {[string index "$line" 0] == "#"} {
			continue
		}
		set token [split [regsub -all {[ 	]+} $line " "]]
		set argc [llength $token]
		if {$argc == 0} {
			break
		}
		set cmd [lindex $token 0]

		if {$argc == 1 && $cmd == "commands"} {
			set cmd_mode 1
		} elseif {$argc == 2 && $cmd == "commands" &&
		    [lindex $token 1] == "nosave"} {
			set cmd_mode 2
		} elseif {$argc == 2 && $cmd == "commands" &&
		    [lindex $token 1] == "end"} {
			set cmd_mode 0
		} elseif {$cmd_mode == 1} {
			lappend cmds $line

			#
			# capture expected OEM ID and model from commands
			#
			if {$cmd == "oem" && $argc > 1} {
				set arg1 [lindex $token 1]
				if {$argc == 3 && $arg1 == "model"} {
					set conf(oem_model) [lindex $token 2]
				} elseif {$argc == 2} {
					set conf(oem) $arg1
				}
			}
		} elseif {$cmd_mode == 2} {
			lappend cmds_nosave $line
		} else {
			set conf([lindex $token 0]) [lindex $token 1]
		}
	}
	close $fd
}

#
# Update log file
#
proc log_ayla {status {errcode 0} {msg ""}} {
	global conf
	global assign
	global err
	global log_ayla_service_status

	if {![info exists log_ayla_service_status]} {
		set log_ayla_service_status ""
	}
	# replace any commas in the text message with spaces
	regsub -all "," $msg " " msg

	set log_file $conf(log_file)

	set time [clock seconds]
	set timestamp "[clock format $time -gmt 1 -format "20%y/%m/%d %T UTC"]"

	#
	# Comma-separated-values (CSV) log entry.
	# The first field is a format designator in case we want to
	# change it later or have multiple formats.
	#
	set entry "3,$time,$timestamp,$status,$errcode,[assign_get model]"
	set entry "$entry,[assign_get serial],[assign_get mac_addr]"
	set entry "$entry,[assign_get mfg_model],[assign_get mfg_serial]"
	set entry "$entry,[assign_get hwid],$msg"
	set entry "$entry,$conf(oem),$conf(oem_model)"
	set entry "$entry,$log_ayla_service_status,[conf_get odm]"

	if [catch {
		set fd [open $log_file a]
		puts $fd $entry
		close $fd
	} rc errinfo ] {
		send_user "log $log_file write error code $rc\n"
		send_user "log $log_file write error info \
			[dict get $errinfo -errorinfo]"
	}
}

#
# Give pass/fail status on a test.
# stat should be "pass", "fail", or "timeout"
# Optionally, provide a value for tests like RSSI.
#
proc step_status {name stat {value ""}} {
	if {$stat != "start" && $stat != "pass"} {
		send_user "\n"
	}
	set stat [format "%-7s" "$stat:"]
	if {$value != ""} {
		send_user --  "[log_time] $stat $name: $value\n"
	} else {
		send_user --  "[log_time] $stat $name\n"
	}
}

#
# Send a command to the module with a carriage return, using Expect.
#
proc send_mod {cmd} {
	global mod

	while (1) {
		if { [catch { send -i $mod -- "$cmd\r" } rc errinfo] } {
			log_debug "send_mod caught err $rc"
			console_open

		} else {
			break
		}
	}
}

proc assign_get {name} {
	global assign

	if {[info exists assign($name)]} {
		return $assign($name)
	}
	return ""
}

proc conf_get {name} {
	global conf

	if {[info exists conf($name)]} {
		return $conf($name)
	}
	return ""
}

proc conf_get_int {name} {
	global conf

	if {[info exists conf($name)]} {
		return $conf($name)
	}
	return 0
}
 
#
# reset to transition to BC
# wait for module reset by user if it doesn't respond right away
#
proc mod_reset {{mode ""} {recurse 0} {factory 0}} {
	global mod
	global err
	global conf
	global prompt
	global spawn_id

	set spawn_id $mod

	if {$mode != ""} {
		set mode_req "$mode"
	} else {
		set mode_req $conf(req_mode)
	}
	if {!$recurse} {
		step_status mod_reset start
	}
	set version unknown
	log_user $conf(log_user)

	if {[conf_get_int factory_reset] || $factory} {
		set reset_cmd "reset factory"
	} else {
		set reset_cmd "reset"
	}
	set cmd $reset_cmd

	set try 0
	while {$version == "unknown"} {
		if {$try > 1} {
			send_user "\nModule reset timeout: \
			   Try manual reset.\n"
			send_user "\nRetrying reset\n\n"
			set cmd $reset_cmd
			log_user 1
		}
		incr try
		if {$try >= 4} {
			log_user $conf(log_user)
			step_status mod_reset timeout
			error "mod startup timed out" "" $err(mod_start)
		}

		send_mod $cmd
		set spawn_id $mod
		set mod_mode user

		expect_before "$cmd\n"

		set timeout 10
		expect {
			-re {.*bc ([^ ]*) .*mfg mode.*bc init done\n.*} {
				set version $expect_out(1,string)
				set mod_mode mfg
				set prompt "mfg-> "
			} -re {.*bc ([^ ]*) .*setup mode.*bc init done\n.*} {
				set version $expect_out(1,string)
				set mod_mode setup
				set prompt "setup-> "
			} -re {.*bc ([^ ]*) .*bc init done\n.*} {
				set version $expect_out(1,string)
			} eof {
				# console closed, probably due to break cond.
				console_open
				set cmd "show version"
			} timeout {
				set cmd $reset_cmd
			}
		}
	}
	expect_before
	log_user $conf(log_user)

	set req_version [conf_get mod_version]
	if {$version != $req_version && $req_version != ""} {
		send_user "\tmodule has firmware version $version\n"
		send_user "\tconfiguration needs version $req_version\n"
		step_status mod_reset fail "module has wrong version $version\n"
		error "mod has wrong version $version" "" $err(mod_version)
	}

	if {$mod_mode != $mode_req} {
		send_user "\tmodule is in $mod_mode mode\n"
		set key [conf_get oem_setup_key]
		if {!$recurse && $mode_req == "setup" && $mod_mode == "user" &&
		    "$key" != ""} {
			send_user "\tattempting to re-enable setup mode\n"
			send_user "\tresetting to previous factory settings\n"
			mod_reset user 1 1
			mod_cmd ""
			set prompt "setup-> "
			mod_cmd "setup_mode enable $key"
			mod_cmd "save"
			mod_reset setup 1
		} else {
			step_status mod_reset fail \
			    "module is not in $mode_req mode"
			error "mod not in $mode_req mode" "" $err(mod_start_mfg)
		}
	}
	if {!$recurse} {
		step_status mod_reset pass
	}
}

#
# send a command to BC and look at result
#
proc mod_cmd cmd {
	global err
	global mod
	global prompt

	set spawn_id $mod

	send_mod "$cmd"
	expect {
		"*FAIL:" {
			error "mod cmd $cmd failed" ""  $err(mod_cmd)
		} "usage:" {
			error "mod cmd $cmd failed usage" ""  $err(mod_cmd)
		} "unrecognized command" {
			error "mod cmd $cmd failed" ""  $err(mod_cmd)
		} -re ".*$prompt" {
		} -re "--> " {
		} timeout {
			error "mod cmd $cmd timed out" "" \
				$err(mod_cmd_time)
		}
	}
}

#
# open or re-open the console
#
proc console_open {} {
	global conf
	global mod
	global mod_fd
	global console_settings
	global err

	if { $mod_fd != "" } {
		# try to close, just in case. This is expected to fail.
		if [catch { 
			close -i $mod
		} rc] {
			# send_user "close caught error: $rc\n"
		}
		spawn -noecho -leaveopen $mod_fd
		set mod $spawn_id
		return	
	}
	
	set op "open" 
	if [catch {
		set fd [open "\\\\.\\$conf(com_port)" r+]

		set op "config $console_settings"
		fconfigure $fd -mode $console_settings -handshake none

		set op "set buffers"
		fconfigure $fd -sysbuffer {4096 4096} -pollinterval 10

		set op "spawn"
		spawn -noecho -leaveopen $fd

		set mod $spawn_id
		set mod_fd $fd
	}] {
		error "failed to $op $conf(com_port)" "" $err(console_connect)
	}
}

proc console_start {} {
	global err
	global conf
	global console_settings

	if {$conf(com_port) == ""} {
		set conf(com_port) [find_port]
		if {$conf(com_port) == ""} {
			step_status test_connect fail dev_not_found
			error "no com port found for Ayla board" "" \
				$err(console_port)
		}
	}

	# Pause in case TeraTerm still has serial line open for ayla_update
	after [conf_get_int com_port_delay]
	console_open
	log_info "using port $conf(com_port) settings $console_settings"
	log_user $conf(log_user)
}

proc mod_show_label "" {
	global mod
	global err
	global assign

	step_status show_label start

	set spawn_id $mod
	set cmd "show id"
	send_mod "show id"
	expect_before "show id\n"

	set assign(mac_addr) ""
	set assign(serial) ""

	while 1 {
		expect {
			"*FAIL:" {
				error "mod cmd $cmd failed" ""  $err(mod_cmd)
			} "unrecognized command" {
				error "mod cmd $cmd failed" ""  $err(mod_cmd)
			} -re "id: (\[\^\"\n\]*) \"(\[\^\"\n\]*)\"\n" {
				set name $expect_out(1,string)
				set val $expect_out(2,string)
				set assign($name) $val
			} -re "->" {
				break
			} timeout {
				error "mod cmd $cmd timed out" "" \
					$err(mod_cmd_time)
			}
		}
		expect_before
	}
	expect_before

	set assign(dsn) "$assign(serial)"
	set assign(mac) "$assign(mac_addr)"

	if {$assign(dsn) == ""} {
		step_status show_label fail "expected DSN not seen"
		return
	}
	step_status show_label pass "DSN $assign(dsn) MAC $assign(mac)"
}

proc console_detach "" {
	global mod
	global mod_fd

	if {$mod >= 0} {
		close $mod_fd
		close -i $mod
		wait -i $mod -nowait
		set mod -1
	}
}

# end of ayla_lib.tcl
