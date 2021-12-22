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

# Directories to use when unpacking and building source (/tmp is in RAM)
AYLA_OTA_DIR=$(mktemp -q -d /tmp/ayla_ota.XXXX)
AYLA_OTA_BUILD_DIR="$AYLA_OTA_DIR/src"
AYLA_OTA_INSTALL_DIR="$AYLA_OTA_DIR/install"

# Directories used by Ayla modules. Change to accomodate your system, if needed.
AYLA_INSTALL_ROOT="/home/pi/ayla"
AYLA_CONFIG_DIR="/home/pi/ayla/config"

# Path of configuration script that ayla_install.sh creates on a clean install. 
AYLA_INSTALL_OPTS_FILE="$AYLA_INSTALL_ROOT/ayla_install.opts"

# Log file to write OTA status to
AYLA_OTA_LOG="$AYLA_INSTALL_ROOT/ota_install.log"

# App to build in project (default is appd)
app_to_build="appd"

# Flag to omit building Wi-Fi setup components
no_wifi=0

cleanup() {
	rm -rf $AYLA_OTA_DIR
}

exit_success() {
	echo "$cmdname:  OTA install complete"
	echo "Installed $image_src_file @ $(date)" >> $AYLA_OTA_LOG
	cleanup
	exit 0
}

exit_failure() {
	echo "$cmdname:  ERROR $*"
	echo "Install failed $image_src_file \"$*\" @ $(date)" >> $AYLA_OTA_LOG
	cleanup
	exit 1
}

# Source opts script to update app_to_build and no_wifi vars
if [ -f $AYLA_INSTALL_OPTS_FILE ]; then
	echo "$cmdname: loading install options from $AYLA_INSTALL_OPTS_FILE"
	. "$AYLA_INSTALL_OPTS_FILE"
fi

image_path="$1"

# Verify image path parameter
if [ -z $image_path ]; then
	echo "Usage: $cmdname <image path>"
	exit 1
fi
if [ ! -f $image_path ]; then
	exit_failure "image file $image_path missing"
fi

image_src_file=$(basename $image_path)

mkdir -p $AYLA_OTA_BUILD_DIR
mkdir -p $AYLA_OTA_INSTALL_DIR

echo "$cmdname: unpacking OTA $image_path to $AYLA_OTA_BUILD_DIR"
tar -xf $image_path --directory $AYLA_OTA_BUILD_DIR
if [ $? -ne 0 ]; then
	exit_failure "unpacking failed"
fi

echo "$cmdname: entering OTA source dir $AYLA_OTA_BUILD_DIR"
cd $AYLA_OTA_BUILD_DIR

echo "$cmdname: building OTA source"
make INSTALL_ROOT=$AYLA_OTA_INSTALL_DIR PROD=$AYLA_PROD APP=$app_to_build NO_WIFI=$no_wifi install
if [ $? -ne 0 ]; then
	exit_failure "source build failed"
fi

echo "$cmdname: installing OTA binaries to $AYLA_INSTALL_ROOT/bin"
cp -f $AYLA_OTA_INSTALL_DIR/bin/* $AYLA_INSTALL_ROOT/bin/
if [ $? -ne 0 ]; then
	exit_failure "binary install failed"
fi

echo "$cmdname: installing OTA config to $AYLA_CONFIG_DIR"
cp --no-clobber -f $AYLA_OTA_INSTALL_DIR/etc/config/* $AYLA_CONFIG_DIR/
if [ $? -ne 0 ]; then
	exit_failure "config install failed"
fi

exit_success
