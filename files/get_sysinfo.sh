#!/bin/sh

uptime=`uptime|cut -d 'p' -f 2|cut -d 'l' -f 1|tr -d ','`
s1=`uptime |grep -q min`

if [ $? -eq 0 ];then

        echo "`date +%d:%m:%Y:%Z`; Uptime:$uptime"
else
        echo "`date +%d:%m:%Y:%Z`; Uptime:$uptime"Hrs
fi
