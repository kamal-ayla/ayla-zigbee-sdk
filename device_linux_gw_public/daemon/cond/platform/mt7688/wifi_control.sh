#!/bin/sh
#
# System-specific script to configure the DHCP client, server, and Wi-Fi
# module.  This script is invoked by the Ayla cond daemon when transitioning
# between varioius modes of operation.
# 
# Note: This is a sample script written for a Mediatek MT7688 with proprietary
# iwpriv calls to configure the Wi-Fi kernel driver. This may need to be
# modified or rewritten to provide the desired functionality for the target
# platform.
#


#
# Define utility functions here
#

check_daemon_running() {
	local name="$1"
	local pid=0
	if [ -f /var/run/$name.pid ]; then
		pid=$(cat /var/run/$name.pid)
	elif [ -f /var/run/$name/$name.pid ]; then
		pid=$(cat /var/run/$name/$name.pid)
	fi
	if [ $pid -gt 0 ]; then
		kill -0 $pid 2>/dev/null
		if [ $? -eq 0 ]; then
			return 1
		fi
	fi
	return 0
}

start_daemon() {
	check_daemon_running "$1"
	if [ $? -ne 0 ]; then
		echo "start_daemon: $1 already started"
			return 0
	fi
	echo "start_daemon: starting $1..."
	/etc/init.d/$1 start
	return $?
}

stop_daemon() {
	check_daemon_running "$1"
	if [ $? -eq 0 ]; then
		echo "stop_daemon: $1 already stopped"
		return 0
	fi
	echo "stop_daemon: stopping $1..."
	/etc/init.d/$1 stop
	return $?
}

start_dhcp_client() {
	local interface="$1"

	check_daemon_running udhcpc-$interface
	if [ $? -ne 0 ]; then
		echo "start_dhcp_client: udhcpc-$interface already started"
		return
	fi
	local event_script=/var/run/ayla_dhcp_event.sh
	local default_script=/usr/share/udhcpc/default.script
	echo "#!/bin/sh" > $event_script
	echo "event=\"\$1\"" >> $event_script
	echo "echo \"DHCP event: \$event\"" >> $event_script
	echo "$default_script \$1" >> $event_script
	echo "acli event dhcp_\$event &" >> $event_script
	chmod 755 $event_script

	# Start DHCP client in background, use custom script, try for 3s, retry after 12s
	udhcpc -i $interface -s $event_script -p "/var/run/udhcpc-$interface.pid" -T 3 -A 12 -b
}

stop_dhcp_client() {
	local interface="$1"
	local name="udhcpc-$interface"
	local pid=0

	check_daemon_running $name
	if [ $? -ne 0 ]; then
		if [ -f /var/run/$name.pid ]; then
			pid=$(cat /var/run/$name.pid)
		elif [ -f /var/run/$name/$name.pid ]; then
			pid=$(cat /var/run/$name/$name.pid)
		fi
		kill $pid
	fi
}

print_usage() {
	echo "usage: $0 <module> <action> [interface] [IP addr]|[SSID] [channel] [auth] [encryption] [key] [output file]" >&2
	exit 1
}

if [ $# -lt 2 ]; then
	echo "incorrect arguments" >&2
	print_usage
fi

module="$1"
action="$2"
interface="$3"
ip_address="$4"
ssid="$4"
channel="$5"
auth="$6"
encryption="$7"
key="$8"

case $module-$action in
	station-start)
		echo "$0: station start on $interface"
		ifconfig $interface up
	;;
	station-stop)
		echo "$0: station stop"
		ifconfig $interface down
	;;
	station-scan)
		echo "$0: scan"
	;;
	station-connect)
	;;
	station-disconnect)
		echo "$0: station disconnect"
	;;
	ap-start)
		echo "$0: AP start"
	;;
	ap-stop)
		echo "$0: AP stop"
	;;
	dhcp-client-start)
		# Ensure DHCP client is running (may block for 3s)
		start_dhcp_client $interface
	;;
	dhcp-client-stop)
		echo "$0: DHCP client stop"
		# Terminate DHCP client
		stop_dhcp_client $interface
	;;
	dhcp-server-start)
		echo "$0: DHCP server start"
	;;
	dhcp-server-stop)
		echo "$0: DHCP server stop"
		# Setup interface
		ifconfig $interface 0.0.0.0
	;;
	*)
		echo "$0: invalid command: $module-$action" >&2
		print_usage
	;;
esac
