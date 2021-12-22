#! /bin/sh
echo "dhcp_event $1"

event=$1

acli event dhcp_$event
