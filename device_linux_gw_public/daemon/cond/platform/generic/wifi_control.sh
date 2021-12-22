#!/bin/sh
#
# System-specific script to configure the DHCP client, server, and Wi-Fi
# module.  This script is invoked by the Ayla cond daemon when transitioning
# between varioius modes of operation.
# 
# Note: This is a sample script written as a template and should be
# modified as needed to provide the desired functionality for the target
# platform.
#


#
# Define utility functions here
#


print_usage() {
        echo "usage: $0 <module> <action> [interface] [IP addr]|[SSID] [channel] [auth] [encryption] [key]" >&2
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
                #
                # Perform any actions needed to handle a station mode started event
                #
	;;
	station-stop)
		echo "$0: station stop"
                #
                # Perform any actions needed to handle a station mode stopped event
                #
	;;
	station-scan)
		echo "$0: scan"
                #
                # Perform any actions needed to handle a scan request
                #
	;;
	station-connect)
		echo "$0: connect to $ssid"
                #
                # Perform any actions needed to handle a connect request
                #
	;;
	station-disconnect)
		echo "$0: station disconnect"
		#
                # Perform any actions needed to handle a disconnect request
                #
	;;
	ap-start)
		echo "$0: AP start"
                #
                # Perform any actions needed to handle an AP mode started event
                #
	;;
	ap-stop)
		echo "$0: AP stop"
                #
                # Perform any actions needed to handle an AP mode stopped event
                #
	;;
	dhcp-client-start)
		echo "$0: DHCP client start"
		#
                # Perform any actions needed to start the DHCP client for $interface
                #
	;;
	dhcp-client-stop)
		echo "$0: DHCP client stop"
		#
                # Perform any actions needed to stop the DHCP client for $interface
                #
	;;
	dhcp-server-start)
		echo "$0: DHCP server start"
		#
                # Perform any actions needed to start the DHCP server for $interface
                # with static IP $ip_address
                #
	;;
	dhcp-server-stop)
		echo "$0: DHCP server stop"
                #
                # Perform any actions needed to stop the DHCP server for $interface
                #
	;;
	*)
		echo "$0: invalid command: $module-$action" >&2
		print_usage
	;;
esac
