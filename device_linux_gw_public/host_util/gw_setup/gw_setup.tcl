#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#
set version "0.3 2014-10-14"
set cmdname gw_setup

set build_info [file join [file dirname [info script]] build.tcl]
if {[file exists $build_info]} {
	source $build_info
	puts "gw_setup $build_ver"
} else {
	puts "gw_setup version $version"
}

package require Expect
package require xml

set start_time [clock seconds]

# for trace use:
# exp_internal 1
set debug 0

set send_slow {1 0.01}

#
# Include common functions
#
source [file join [file dirname [info script]] ayla_lib.tcl]


#
# Configuration settings
#
set conf(test_mac_addr)	020102030405
set conf(test_wifi_ssid) ""
set conf(test_wifi_key) ""
set conf(test_wifi_sec)	WPA2_Personal
set conf(test_enable) 0
set conf(test_required) 0
set conf(dryrun) 0
set conf(mac_from_mod) 0
set conf(log_user) 0
set conf(product) "gateway"

set conf(rssi_min)	-70
set conf(rssi_max)	-20
set conf(ping_max_loss) 3
set conf(ping_max_rtt)	6

set conf(com_port)	""

set conf(shell_prompt) "root@.*:.*# "

set dl_test_dry_len	1000
set dl_test_min(1000000) 200000

set max_limit 5

# workaround to get forward slashes in DOS home directory path
set home [string map {"\\" "/"} $env(HOME)]

#
# must operate in the parent to gw_setup directory.
#
set pkg_dir "gw_setup"
if {![file exists $pkg_dir]} {
	puts "ayla_setup must run in parent of $pkg_dir directory"
	exit 1
}

#
# Get model and platform specific configuration
#
set model_info_file gw_setup/model_info.tcl
if {![file exists $model_info_file]} {
	puts "missing required $model_info_file"
	exit 1
}
source $model_info_file

#
# AFS config
# If the serial number directory is present, we run offline
#
set afs_data afs_data.txt
set afs_dir "dsns/sn*"
set afs_used_dir "dsns/used_sns"
set afs_err afs_err.txt
set afs_header afs_header.txt
set afs_temp ""
set afs_file ""

if {[llength [glob -type d -nocomplain $afs_dir]]} {
	set afs_offline 1
} else {
	log_err "missing required dir $afs_dir"
	exit 1
}

#
# MAC service temp files
#
set conf(mac_data_file) mac_data.txt
set conf(mac_head_file) mac_head.txt
set conf(mac_err_file) mac_err.txt

#
# more configuration settings - less likely to need changing
#
set conf_file "setup/config"
set conf(log_file) "setup/log.txt"
set path "$pkg_dir"
set null_file NUL
set temp_file "reg_out.txt"

# files used

set conf(curl) "curl/curl.exe"
set conf(ca_bundle) "curl/ca-bundle.crt"

set cmd [file tail $argv0]
set repair_mode 0

set assign(set_id) 0
set assign(repair) 0
set assign(relabel) 0

set console_settings 115200,n,8,1

set assign(serial_arg) 0
set assign(mfg_model) ""
set assign(model) ""
set conf(region) "US"

#
# clear variables that may have data from previous module
#
proc mod_dev_init {} {
	global assign
	global old
	global device_mode

	set device_mode unknown
	foreach n {model mfg_model} {
		set old($n) ""
	}
	foreach n {serial mac_addr mfg_serial hwid} {
		set assign($n) ""
		set old($n) ""
	}
}

#
# Run mod_dev_init before first test
#
mod_dev_init

#
# Make sure the gateway is at the appropriate prompt
#
proc mod_verify_int {mode} {
	global mod
	global err
	global conf
	global device_mode

	set rc 0

	while {"$device_mode" != "$mode"} {

		set timeout 10
		set retry 0

		set spawn_id $mod

		#
		# Send space as empty command.
		# Without space, uboot repeats last command.
		#
		send_mod " "

		expect {
			-re ".*uboot> " {
				set device_mode uboot
				if {$mode != "uboot"} {
					send_mod "boot"
					exp_continue
				}
			} -re ".*setup_agent: not found\n$conf(shell_prompt)" {
				if {$mode == "gsa"} {
					log_err "gw_setup_agent not installed"
					set rc $err(gw_setup_agent)
					break
				}
			} -re ".*\n$conf(shell_prompt)" {
				# shell prompt indicates device has booted
				set device_mode shell
				switch $mode {
					"uboot" {
						send_mod "reboot"
						exp_continue
					}
					"gsa" {
						send_mod "gw_setup_agent"
						exp_continue
					}
				}
			} -re ".*\ngw_setup_agent-> " {
				set device_mode "gsa"
				if {$mode != "gsa"} {
					gsa_cmd "save factory"
					gsa_cmd "quit"
					send_mod " "
					exp_continue
				}
			} -re ".*Hit any key to stop autoboot:" {
				if {$mode == "uboot"} {
					send_mod " "
				}
				exp_continue
			} "\n> " {
				# maybe prompting for end quote
				send_mod "'"
				exp_continue
			} eof {
				# console closed,
				# probably due to break condition.
				console_open
				exp_continue
			} timeout {
				log_debug "timeout waiting for $mode prompt -\
				   try $retry"
				if {$retry > 3} {
					set rc $err(unknown)
				}
				incr retry
				send_mod " "
				exp_continue
			}
		}
	}
	return $rc

}

proc mod_verify {mode} {
	global err
	global mod

	set spawn_id $mod

	while { [catch "mod_verify_int $mode" rc errinfo] } {

		set err_code [dict get $errinfo -errorcode]
		set err_info [dict get $errinfo -errorinfo]

		log_debug "verify_$mode caught error code $err_code $rc"
		if { $err_code != "" && $err_code > 0 &&
		    $err_code < $err(unknown)} {
			return -options $errinfo $rc
		}
		log_debug "verify_$mode err_info $err_info"
		log_debug "re-opening console"
		console_open
	}

	#
	# If the error is not due to serial port being closed,
	# reraise the error to let our caller catch it.
	#
	if {$rc != 0} {
		if {$mode == "gsa"} {
			set mode "gw_setup_agent"
		}
		error "could not get device into $mode" "" $rc
	}
}

#
# Handle test failure
#
proc test_fail msg {
	global err
	global conf

	log_warn "\n$conf(product) test failed: $msg"
	error "$conf(product) test failed: $msg\n" "" $err(mod_test)
	exit 1
}

#
# AFS XML parser data and routines
#
set afs_token ""
set afs_token_stack [list]

set afs_value(dsn) ""

#
# handle data from AFS Get
#
proc afs_cdata {data args} {
	global afs_value
	global afs_token

	append afs_value($afs_token) "$data"
}

proc afs_elem_start {name attlist args} {
	global afs_value
	global afs_token
	global afs_token_stack

	lappend afs_token_stack $afs_token
	set afs_token $name
	set afs_value($afs_token) ""
}

proc afs_elem_end {name args} {
	global afs_token_stack
	global afs_token

	set afs_token_stack [lrange $afs_token_stack 0 end-1]
	set afs_token [lindex $afs_token_stack end]
}

#
# Check HTTP status.
# Return a string, either: "OK", "open", or HTTP status code
#
proc http_status {file} {

	if [catch {set fd [open "$file" r]}] {
		return "open"
	}
	set lines [split [read $fd] "\n"]
	set line [lindex $lines 0]
	set code [lindex [split $line] 1]

	set status "OK"
	if {$code < 200 || $code >= 300} {
		set status $code
	}
	close $fd
	return $status
}

#
# read assign() values from AFS
#
proc serial_get {} {
	global afs
	global afs_dir
	global afs_data
	global afs_err
	global afs_value
	global afs_key
	global afs_header
	global afs_offline
	global afs_used_dir
	global afs_file
	global afs_temp
	global assign
	global err
	global conf

	if {$assign(serial) != ""} {
		log_err "Serial number already assigned.  Skipping AFS get"
		return
	}

	set afs_value(dsn) ""
	set afs_value(public-key) ""
	set afs_value(mac) ""

	if {$afs_offline} {
		#
		# Find offline serial number file.
		# Use the first one found by glob (will be lowest sn).
		# Setting afs_data will cause file to be deleted below.
		#
		set files [glob -type {f r} -nocomplain $afs_dir/*]
		if {![llength $files]} {
			log_err "No more DSNs (serial numbers) in $afs_dir"
			error "Out of serial numbers in $afs_dir." "" \
				$err(serial_get)
		}

		set afs_data [lindex $files 0]
		log_debug "Using $afs_data"

		#
		# afs_data will be removed.
		# make a copy in case we don't end up using the DSN
		# afs_file will be the original file name
		# afs_temp is the temporary copy.
		#
		set afs_file $afs_data
		file mkdir $afs_used_dir
		set afs_temp $afs_used_dir/[file tail $afs_data]
		log_debug "copying $afs_data to $afs_temp"
		if {[file exists "$afs_temp"]} {
			file delete "$afs_temp"
		}
		file copy "$afs_data" "$afs_temp"
	} else {
		log_debug "\nRequesting AFS info"

		#
		# If a MAC address was not assigned, request it from AFS
		#
		set post_arg "?count=1"
		if {$assign(mac_addr) == "" && !$conf(mac_from_mod) &&
		    [conf_get mac_service] == ""} {
			append post_arg "&mac=true"
		}

		if [catch {
			exec "$conf(curl)" -X POST -o "$afs_data" \
				--stderr "$afs_err" \
				--dump-header "$afs_header" \
				-d "" -H "Content-Type: application/xml" \
				-E "$afs_key" \
				--cacert $conf(ca_bundle) \
				"$afs/certs.xml$post_arg"

		}] {
			file delete "$afs_data" "$afs_err" "$afs_header"
			error "AFS request for new serial number failed." "" \
				$err(serial_get)
		}

		#
		# Check HTTP header
		#
		set status [http_status "$afs_header"]
		if {$status != "OK"} {
			set err_code $err(serial_get)
			if {$status == "nodev"} {
				set err_code $err(serial_none)
			}
			file delete "$afs_data" "$afs_err" "$afs_header"
			error "AFS request for new serial number failed: \
				$status " "" $err_code
		}
	}

	#
	# parse XML data
	#
	set parser [::xml::parser \
		-characterdatacommand afs_cdata \
		-elementstartcommand afs_elem_start \
		-elementendcommand afs_elem_end]

	if [catch {set fd [open "$afs_data" r]}] {
		file delete "$afs_data" "$afs_err" "$afs_header"
		error "read of new serial number failed:\
			empty data" "" $err(serial_get)
	}

	$parser parse [read $fd]
	close $fd

	#
	# use parse results
	#
	if {$afs_value(dsn) == "" || $afs_value(public-key) == ""} {
		error "AFS request for new serial number failed" "" \
			$err(serial_parse)
	}
	set assign(serial) $afs_value(dsn)

	if {$conf(mac_from_mod)} {
		if {$assign(mac_addr) != ""} {
			error "--mac parameter not supported for this model"
			    "" $err(serial_no_mac)
		}
	} elseif {$assign(mac_addr) == ""} {
		if {[conf_get mac_service] != ""} {
			mac_get
		}
		if {$afs_value(mac) == ""} {
			error "no MAC address provided" "" $err(serial_no_mac)
		}
		set assign(mac_addr) [regsub -all : $afs_value(mac) ""]
	}
	set assign(public-key) $afs_value(public-key)

	file delete "$afs_data" "$afs_err" "$afs_header"
}

proc token {name value} {
	set rc "<$name>$value</$name>"
}

proc mac_files_rm {} {
	global conf

	file delete $conf(mac_head_file) $conf(mac_data_file) \
		$conf(mac_err_file)
}

#
# Get new MAC address from factory-floor-based service.
# This service is optionally specified in configuration.
# A sample xml response is:
#	<?xml version="1.0" encoding="utf-8"?>
# 	<string xmlns="http://www.example.com/">1234567890ABC</string>
#
proc mac_get {} {
	global afs_value
	global assign
	global err
	global conf

	if {$afs_value(mac) != ""} {
		log_err "mac_get: mac already assigned.  Skipping MAC get"
		return ""
	}
	if {[conf_get mac_service] == "" && [conf_get mac_test_file] == ""} {
		log_err "mac_get: mac service not configured"
		return
	}
	set afs_value(string) ""

	log_debug "mac_get: Requesting MAC address"

	if {[conf_get mac_test_file] != ""} {
		log_debug "mac_get: getting MAC from $conf(mac_test_file)"
		file copy $conf(mac_test_file) $conf(mac_data_file)
		file copy $conf(mac_test_head) $conf(mac_head_file)
	} else {
		log_debug "mac_get: getting MAC form $conf(mac_service)"
		if [catch {
			exec "$conf(curl)" -X GET -o "$conf(mac_data_file)" \
				--stderr "$conf(mac_err_file)" \
				--dump-header "$conf(mac_head_file)" \
				"$conf(mac_service)"
		}] {
			mac_files_rm
			error "GET request for new MAC address failed." "" \
				$err(mac_get)
		}
	}

	#
	# Check HTTP header
	#
	set status [http_status "$conf(mac_head_file)"]
	if {$status != "OK"} {
		set err_code $err(mac_get)
		if {$status == "nodev"} {
			set err_code $err(mac_none)
		}
		mac_files_rm
		error "request for new MAC addres failed: \
			$status " "" $err_code
	}

	#
	# parse XML data
	#
	set parser [::xml::parser \
		-characterdatacommand afs_cdata \
		-elementstartcommand afs_elem_start \
		-elementendcommand afs_elem_end]

	if [catch {set fd [open "$conf(mac_data_file)" r]}] {
		mac_files_rm
		error "read of new MAC address failed:\
			empty data" "" $err(mac_get)
	}

	$parser parse [read $fd]
	close $fd

	#
	# use parse results
	#
	if {![info exists afs_value(string)] || $afs_value(string) == ""} {
		error "HTTP request for new MAC address failed" "" \
			$err(mac_parse)
	}
	mac_files_rm
	log_debug "got MAC $afs_value(string)"
	set afs_value(mac) $afs_value(string)
}

proc mod_uboot_cmd {cmd} {
	global mod
	global err

	log_debug "uboot cmd: $cmd"
	set prompt "uboot> "

	set spawn_id $mod
	send_mod $cmd

	#
	# convert command to regular expression to match its echo by uboot.
	#
	set echo_patt $cmd
	regsub -all -- "\\$" $echo_patt "\\\\$" echo_patt
	regsub -all -- "\\\\n" $echo_patt "\\\\\\\\n" echo_patt
	regsub -all -- {\+} $echo_patt {\\+} echo_patt

	expect {
		-re ".*Unknown command.*\n+$prompt" {
			log_err "uboot error seen after sending: $cmd"
			log_err "response: $expect_out(0,string)"
			error "mod cmd $cmd failed" "" $err(mod_cmd)
		} -re "$echo_patt.*\n+$prompt" {
			# command complete - just return
		} eof {
			console_open
		} timeout {
			error "mod cmd $cmd timed out" "" $err(mod_cmd_time)
		}
	}
}

#
# Get value for u-boot env variable using fw_printenv
# Undefined variables are returned as empty strings.
#
proc mod_getenv {name} {
	return [shell_cmd "fw_printenv -n $name 2>/dev/null"]
}

#
# Send fw_setenv command
#
proc mod_setenv {name val} {
	shell_cmd "fw_setenv $name \'$val\'"

	#
	# Verify by reading back some or all items
	#
	set readback [mod_getenv $name]
	if { "$readback" != "$val" } {
		log_err "verify for $name failed.  saw \"$readback\" \
		    expected \"$val\""
	} else {
		log_debug "verified $name"
	}
}

#
# Send uboot setenv command for key
#
proc mod_setenv_key {name val} {
	global err
	global mod
	global conf

	mod_verify shell

	set prompt "$conf(shell_prompt)"
	set spawn_id $mod
	set dut $conf(product)

	#
	# send each line of key
	#
	set cmd "fw_setenv $name '"
	foreach line [split "$val" "\n"] {
		send_mod "$cmd$line"
		expect {
			-- "$line\n> " {
			} -re ".*Unknown command.*$prompt" {
				error "$dut: cmd $cmd unknown" \
					"" $err(mod_cmd)
			} -re ".*$prompt" {
				error "$dut: unexpected prompt" \
					"" $err(mod_cmd)
			} eof {
				error "$dut: EOF on serial port" \
					"" $err(mod_cmd)
			} timeout {
				error "$dut: cmd $cmd timed out" \
					"" $err(mod_cmd_time)
			}
		}
		set cmd ""
	}

	#
	# send final quote
	#
	send_mod "'"
	expect {
		-re "'\n$prompt" {
		} -re ".*Unknown command.*$prompt" {
			error "$dut cmd $cmd unknown" "" $err(mod_cmd)
		} -re ".*$prompt" {
			error "$dut unexpected prompt" "" $err(mod_cmd)
		} eof {
			error "$dut EOF on serial port" "" $err(mod_cmd)
		} timeout {
			error "$dut cmd $cmd timed out" "" $err(mod_cmd_time)
		}
	}
}

#
# Add colons between pairs of digits in ethernet address
#
proc add_colons {mac} {
	return [regsub ":$" [regsub -all ".." $mac "&:"] ""]
}

#
# Give the device its DSN, key, and OEM info
#
proc mod_serialize {} {
	global old
	global err
	global assign
	global mod
	global conf
	global afs_file
	global afs_temp

	set spawn_id $mod

	mod_verify shell

	step_status test_assign start

	foreach n {model mfg_model} {
		if {![info exists assign($n)]} {
			set assign($n) ""
		}
		if {$old($n) == ""} {
			set old($n) $assign($n)
		}
	}
	set assign(hwid) "$old(hwid)"

	if {$old(serial) == "" || $assign(relabel)} {

		#
		# Allocate a serial number and get associated data
		#
		set save_serial 0
		if {!$assign(serial_arg) && $assign(serial) == ""} {
			serial_get
			set save_serial 1
		}

		#
		# stop devd if it is running
		#
		shell_cmd "/etc/init.d/devdwatch stop"

		#
		# set time on device for test info and key encoding
		#
		shell_cmd "date -s \
		    [clock format [clock seconds]  -format "%Y%m%d%H%M.%S"]"

		#
		# Set MAC if not already programmed
		#
		if {!$conf(mac_from_mod)} {
			set mac [add_colons $assign(mac_addr)]
			mod_setenv ethaddr $mac
			#shell_cmd "ifconfig eth0 hw ether $mac down up"
		}

		#
		# Set DSN and key in ROM
		#
		gsa_cmd "set_rom id_dsn $assign(serial)"
		gsa_cmd "set_rom id_pubkey '$assign(public-key)'"

		#
		# Set ID and OEM info
		#
		gsa_cmd "set id/dsn $assign(serial)"
		gsa_cmd "set id/rsa_pub_key '$assign(public-key)'"
		# gsa_cmd "set id/model $assign(model)"
		gsa_cmd "set oem/oem $conf(oem)"
		gsa_cmd "set oem/model $conf(oem_model)"
		# oem key generation depends on oem, oem model, and rsa_pub_key
		gsa_cmd "set oem/key $conf(oem_key)"
		# disable setup mode to turn off SSH and the web UI
		gsa_cmd "setup_mode disable"

		gsa_cmd "set client/region $conf(region)"
		if {$conf(dryrun) == 0} {
			gsa_cmd "save factory"
			gsa_cmd "quit"
			if {$afs_temp != ""} {
				file delete $afs_temp
			}
		} else {
			gsa_cmd "quit"
			log_info "dryrun:  skipping save"
			serial_restore
			set save_serial 0
		}

		step_status test_assign pass "DSN $assign(serial)"

		log_ayla label
	} elseif {!$assign(repair)} {
		log_err "device has existing ID $old(serial)"
		step_status test_assign fail $err(serial_old_conf)
		log_ayla fail $err(serial_old_conf) "existing ID"
	} else {
		step_status test_assign skipped OK
		log_ayla label
	}
}

proc serial_restore {} {
	global afs_temp
	global afs_file

	if {$afs_temp != "" && [file exists "$afs_temp"] &&
	    ![file exists $afs_file]} {
		set dir [file dirname $afs_file]
		log_debug "moving $afs_temp to $dir"
		file rename "$afs_temp" "$afs_file"
		set afs_temp ""
	}
}

proc serial_setup {} {
	global assign
	global conf
	global model
	global cmd
	global err


	if {!$assign(set_id)} {
		return
	}
	#
	# check required parameters
	#
	set error 0
	foreach name {mfg_model mfg_serial sku} {
		if {$assign($name) == ""} {
			if {[info exists conf($name)]} {
				if {$conf($name) != ""} {
					set assign($name) $conf($name)
					continue
				}
			}
			log_err "$name not provided in $cmd arguments"
			incr error
		}
	}
	if {$error} {
		error "required parameters missing" "" $err(usage)
	}
	set assign(model) [format "%s%d" $model $assign(sku)]
}

#
# --label: just set ID
#
proc mod_label "" {
	global assign
	global err

	if {!$assign(set_id)} {
		log_err "options --label and --nolabel both specified"
		error "conflicting options --label and --nolabel" "" $err(usage)
	}
	serial_setup
	shell_get_id
	while {1} {
		mod_serialize
		if {![mod_test_loop]} {
			break
		}
	}
}

#
# run command via gw_setup_agent printenv
#
proc gsa_cmd {cmd} {
	global err
	global mod
	global conf

	mod_verify gsa

	set prompt "gw_setup_agent-> "

	send_mod $cmd
	set rval ""
	#
	# parse response like:
	# $name=$val
	#
	set timeout 2
	set spawn_id $mod
	set echo_seen 0

	#
	# convert command to regular expression to match its echo.
	#
	set echo_patt $cmd
	regsub -all -- "\\$" $echo_patt "\\\\$" echo_patt
	regsub -all -- "\\\\n" $echo_patt "\\\\\\\\n" echo_patt
	regsub -all -- {\+} $echo_patt {\\+} echo_patt

	expect {
		-re "$echo_patt\n.*cannot get value: \[^\n\]+\n.*$prompt" {
			set rval ""
			set echo_seen 1
		} -re "$echo_patt\n.*error: command err \(.*\)\n+$prompt" {
			set rval $expect_out(1,string)
			error "$conf(product) cmd error $rval" \
			    "" $err(gw_setup_cmd)
		} -re "$echo_patt\n*\(\[^\n\]*\)\n+$prompt" {
			set rval $expect_out(1,string)
			set echo_seen 1
		} -re "$echo_patt.*\n*$prompt" {
			set rval ""
			set echo_seen 1
		} -re "quit\n+$conf(shell_prompt)" {
			set rval ""
			set echo_seen 1
		} -re ".*Unknown command.*\n+$prompt" {
			error "$conf(product) cmd $cmd unknown" "" $err(mod_cmd)
		} eof {
			console_open
		} timeout {
			error "$conf(product) cmd $cmd timed out" \
				"" $err(gw_setup_time)
		}
	}
	if {!$echo_seen} {
		log_warn "cmd echo not seen for \"$cmd\""
	}
	return $rval
}

#
# send shell command to device
#
proc shell_cmd {cmd} {
	global err
	global mod
	global conf

	mod_verify shell
	set prompt $conf(shell_prompt)

	send_mod $cmd
	set rval ""

	#
	# parse response like:
	# $name=$val
	#
	set timeout 2
	set spawn_id $mod
	expect {
		-re ".*$cmd\n+(\[^\n\]*\).*$prompt" {
			set rval $expect_out(1,string)
		} -re ".*Unknown command.*$prompt" {
			error "$conf(product) cmd $cmd unknown" "" $err(mod_cmd)
		} eof {
			console_open
		} timeout {
			error "$conf(product) cmd $cmd timed out" \
				"" $err(mod_cmd_time)
		}
	}
	return $rval

}

#
# Read label from module into old() array
#
proc shell_get_id {} {
	global old

	mod_verify shell

	set old(hwid) [shell_cmd "cat /sys/devices/platform/hwid/id;echo"]
	set old(mac) [mod_getenv ethaddr]
	set old(serial) [gsa_cmd "get_rom id_dsn"]
}

proc mod_info_elem {name val} {
	send_user -- [format "%15s%-18s %s\n" " " $name $val]
}

#
# --show_label: show the existing HW_ID, DSN and MAC address
#
proc mod_show_label "" {
	global old

	shell_get_id
	while {1} {
		puts ""
		mod_info_elem Name Value
		mod_info_elem "----" "-----"
		mod_info_elem DSN $old(serial)
		mod_info_elem "MAC addr (eth)" $old(mac)
		mod_info_elem HWID $old(hwid)
		puts ""

		if {![mod_test_loop]} {
			break
		}
	}
}

#
# Handle device switch in test loops
# Returns non-zero if test should be repeated with new device
#
proc mod_test_loop {} {
	global old
	global conf
	global loop_flag

	set dev $conf(product)
	set prev_hwid $old(hwid)

	if {!$loop_flag} {
		return 0
	}

	#
	# Wait for board change
	#
	while {1} {
		puts "\n---- Connect next $dev and press \"Enter\" ----\n"
		expect_user {
			"\n" {
			} timeout {
				exp_continue
			}
		}
		mod_dev_init

		#
		# get current configuration (if any) from device
		#
		shell_get_id

		if {$old(hwid) == $prev_hwid} {
			log_warn "$dev has same hwid as previous board"
		} else {
			break
		}
	}
	return 1
}

proc usage "" {
	global cmd

	send_user "usage: $cmd \[options...\] \
		   \n \
	   Specify at most one of the following test options: \n \
	     --label            set ID and MAC address, etc.\n \
	     --show_label       show the ID and MAC\n \
	   \n \
	   The default is --help\n \
	   \n \
	   Options required for --label:\n \
	     --mac <mac-addr>   assign MAC address for gateway \n \
	     --mfg_sn <sn>      provide manufacturer's serial number\n \
	   \n \
	   Options: \n \
	     --debug            show additional debugging messages\n \
	     --log_user         show device interactions for debugging\n \
	     --loop             program multiple units in one run\n \
	     --mfg_model <pn>   provide manufacturer's part number\n \
	     --model <pn>       provide alternate Ayla part number\n \
	     --relabel          replace existing serial number\n \
	     --repair           existing serial number is not an error\n \
	     --sku <0-99>       provide module SKU for Ayla part number\n \
	     --sn <serial>      provide complete Ayla serial number\n \
	   \n \
	   ID refers to serial numbers, part numbers, and MAC address\n \
	"
	exit 1
}

#
# Main procedure
#
set setup_cmd usage
set prev_cmd ""
set skip_console 0
set loop_flag 0

#
# Handle command-line options
#
set argi 0
while {$argi < $argc} {
	set arg [lindex $argv $argi]
	incr argi
	if {$argi < $argc} {
		set optarg [lindex $argv $argi]
	} else {
		set optarg ""
	}
	set cmd_arg ""
	switch -- $arg {
		"--debug" {
			set debug 1
		} "--dryrun" {
			log_err "dryrun option not supported"
			exit 1
			set conf(dryrun) 1
		} "--help" {
			usage
		} "--label" {
			set cmd_arg $arg
			set setup_cmd mod_label
			set assign(set_id) 1
		} "--log_user" {
			exp_internal 1
			set conf(log_user) 1
		} "--loop" {
			set loop_flag 1
		} "--mac" {
			set assign(mac_addr) [regsub -all : $optarg ""]
			incr argi
		} "--mfg_model" {
			set assign(mfg_model) $optarg
			incr argi
		} "--mfg_sn" {
			set assign(mfg_serial) $optarg
			incr argi
		} "--model" {
			set assign(model) $optarg
			incr argi
		} "--nolabel" {
			# nolabel is obsolete, but support it anyway
			set assign(set_id) 0
		} "--relabel" {
			set assign(relabel) 1
		} "--repair" {
			set assign(repair) 1
		} "--show_label" {
			set setup_cmd mod_show_label
		} "--sku" {
			set assign(sku) $optarg
			incr argi
		} "--sn" {
			set assign(serial) $optarg
			set assign(serial_arg) 1
			incr argi
		} default {
			log_err "invalid argument: $arg"
			usage
		}
	}
	if {$cmd_arg != ""} {
		if {$prev_cmd != ""} {
			log_err "$cmd_arg conflicts with $prev_cmd."
			exit 1
		}
		set prev_cmd $cmd_arg
	}
}

conf_read $conf_file

#
# Start terminal session to the module before any download
# otherwise we could miss version message and early prompts
#
set mod -1
if {$skip_console == 0 && [catch console_start rc errinfo]} {
	log_err "$cmd: $rc"
	exit 1
}

if { [catch "$setup_cmd" rc errinfo] } {
	set err_code [dict get $errinfo -errorcode]
	set err_info [dict get $errinfo -errorinfo]

	log_err "$cmd: caught error code $err_code $rc"
	if {$err_code == "NONE"} {
		log_err "$cmd: rc $rc"
		log_err "$cmd: Error info [dict get $errinfo -errorinfo]\n"
	}
	step_status test_summary fail $err_code
	log_ayla fail $err_code
	serial_restore
}

# end of script
