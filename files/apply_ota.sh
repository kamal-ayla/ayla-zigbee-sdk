#!/bin/sh

# Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

# Ayla OTA apply demo script for Raspberry Pi.

cmdname=$(basename $0)

# Product varient specified when building source
AYLA_PROD="raspberry_pi"

AYLA_OTA_DIR=$(mktemp -q -d /tmp/ayla_ota.XXXXXX)
AYLA_OTA_BUILD_DIR="$AYLA_OTA_DIR/src"
AYLA_OTA_INSTALL_DIR="$AYLA_OTA_DIR/install"

echo $AYLA_OTA_DIR >> /data/ota_log
echo $AYLA_OTA_BUILD_DIR >> /data/ota_log
echo $AYLA_OTA_INSTALL_DIR >> /data/ota_log

logger "==========================ota============================"

cleanup() {
        rm -rf $AYLA_OTA_DIR
echo "clean"
}

exit_success() {
        logger "$cmdname:  OTA install complete"
        cleanup
        exit 0
}

exit_success_upgrade() {
        logger "$cmdname:  OTA install complete"
        exit 0
}

exit_failure() {
        echo "$cmdname:  ERROR $*"
        logger "Install failed $image_src_file \"$*\" @ $(date)" >> $AYLA_OTA_LOG
        cleanup
        exit 1
}


image_path="$1"

# Verify image path parameter
if [ -z $image_path ]; then
        echo "Usage: $cmdname <image path>"
        exit 1
fi
if [ ! -f $image_path ]; then
        exit_failure "image file $image_path missing"
fi


mkdir -p $AYLA_OTA_BUILD_DIR
mkdir -p $AYLA_OTA_INSTALL_DIR

ls $AYLA_OTA_BUILD_DIR  >> /data/ota_log
ls $AYLA_OTA_INSTALL_DIR  >> /data/ota_log

echo "$cmdname: unpacking OTA $image_path to $AYLA_OTA_BUILD_DIR"
#tar -xf $image_path -C $AYLA_OTA_BUILD_DIR
#if [ $? -ne 0 ]; then
#        exit_failure "unpacking failed"
#fi

if [ -n "$(find "$image_path" -prune -size +5000000c)" ]; then
    printf '%s is larger than 5 MB\n' "$image_path"
	mv $image_path $AYLA_OTA_BUILD_DIR/ayla_ota.rbi
else
    echo "fine"
	#mv $image_path $AYLA_OTA_BUILD_DIR/ayla_ota.ipk
	tar -xf $image_path -C $AYLA_OTA_BUILD_DIR
	if [ $? -ne 0 ]; then
		exit_failure "unpacking failed"
	fi
fi

if [ $(ls $AYLA_OTA_BUILD_DIR/*.rbi 2> /dev/null | wc -l) != "0" ]; then
   logger "$cmdname: install  OTA source"
   sysupgrade $AYLA_OTA_BUILD_DIR/*.rbi &

   if [ $? -ne 0 ]; then
        exit_failure "source build failed"
   fi
   sleep 20
   exit_success_upgrade
fi

if [ $(ls $AYLA_OTA_BUILD_DIR/*.ipk 2> /dev/null | wc -l) != "0" ]; then
	if [ $(ls $AYLA_OTA_BUILD_DIR/ayla*.ipk 2> /dev/null | wc -l) == "1" ]; then
		opkg install --nodeps  $AYLA_OTA_BUILD_DIR/ayla*.ipk  >> /data/ota_log
		if [ $? -ne 0 ]; then
        	exit_failure "ayla ipk install failed"
   		fi	
	sleep 10
   	exit_success_upgrade
	fi

   mkdir /www/docroot/js/ViaAyla/
   mv $AYLA_OTA_BUILD_DIR/*.ipk /www/docroot/js/ViaAyla/
   cd /www/docroot/js/ViaAyla/
   FILES=*.ipk
   #echo "$cmdname: install  OTA source"
   #opkg install  --force-reinstall --nodeps  $AYLA_OTA_BUILD_DIR/*  >> /data/ota_logaa
   for f in $FILES
   do
   lcm install -e native -u http://127.0.0.1/js/ViaAyla/$f > OutputInstall
   if [ $? -ne 0 ]; then
        exit_failure "source build failed"
   fi
   var=`head -1 OutputInstall | awk '{print $2}'`;lcm list ID=$var > list
   grep SourceName list > listSource
   var=`head -1 "listSource" | awk '{print $2}'`; echo $var
   $(grep init.d /usr/lib/opkg/info/$var.list) enable
   var=`head -1 OutputInstall | awk '{print $2}'`;lcm start ID=$var;rm OutputInstall
   done
   sleep 10
   rm -rf /www/docroot/js/ViaAyla/*
   exit_success
fi


