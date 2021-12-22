#!/bin/sh

#
# Applies OpenWRT-specific changes to system configuration for Ayla setup-mode
#

local script=$(basename "$0")
local setup_mode=0

WEB_UI_FILE="/www/index.html"
WEB_UI_DISABLED_FILE="/www/disabled_index.html"
WEB_UI_ENABLED_FILE="/www/enabled_index.html"

print_usage() {
	echo "Usage: $script <enable|disable>"
}

check_error() {
	local e=$?
	if [ $e -ne 0 ]
	then
		logger -t $script "Error - $1 returned $e"
		exit 1
	fi
}

uci_set() {
	/sbin/uci set "$1=$2"
	check_error "uci_set"
}

uci_commit() {
	/sbin/uci commit $1	# Optional parameter
	check_error "uci_commit"
}

service_start() {
	killall -0 "$1" > /dev/null 2>&1 && return
	/etc/init.d/"$1" start
	check_error "service_start"
}

service_stop() {
	killall -0 "$1" > /dev/null 2>&1 || return
	/etc/init.d/"$1" stop
	check_error "service_stop"
}

set_web_file() {
	cp -f $1 $WEB_UI_FILE
	check_error "copy web file"
	chmod 644 $WEB_UI_FILE
	check_error "set $WEB_UI_FILE permissions"
}


if [ $# -ne 1 ]
then
        print_usage
        exit 1
fi

case "$1" in
        enable)
                setup_mode=1
        ;;
        disable)
                setup_mode=0
        ;;
        *)
        	print_usage
        	exit 1
esac

# Configure SSH service and web interface and update default web file
if [ $setup_mode -eq 1 ]
then
	uci_set "dropbear.general.enable" "on"
	uci_set "luci.main.enable"        "on"
	set_web_file "$WEB_UI_ENABLED_FILE"
else
	uci_set "dropbear.general.enable" "off"
	uci_set "luci.main.enable"        "off"
	set_web_file "$WEB_UI_DISABLED_FILE"
fi

# Save config
uci_commit

# Start/stop SSH service to apply change
if [ $setup_mode -eq 1 ]
then
	service_start "dropbear"
else
	service_stop "dropbear"
fi

exit 0

