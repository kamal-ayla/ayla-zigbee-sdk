#!/bin/bash
if [ "`ping -c 1 8.8.8.8`" ] && [ "`ping -c 1 google.com`" ]
then
  logger "Internet is reachable & system will not perform reboot"
else
  /sbin/reboot
  logger "Internet is not reachable & system going to reboot"
fi
