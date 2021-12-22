#!/bin/bash

# Copyright 2015-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

# Ayla setup script for Raspberry Pi.
#
# This script is intended to be run on a Raspberry Pi with a clean installation
# of Raspbian, and should create a working system with core Ayla modules and all dependencies
# needed to build and run them.
# The following modules are installed:
# * devd
# * appd (default demo app)
# * cond (default config works with Wi-Fi adapters supporting the Netlink 802.11 driver interface e.g Ralink RT5370)
# * logd
#
# Additional optional components may be installed on the system for convenience.
# Use the --help command line option to see a listing of optional features.
#
# The goal of this script is to demonstrate the setup procedures necessary to enable
# connectivity to the Ayla cloud on a generic Linux platform.  While it was designed to
# be executed on a running Raspberry Pi, many of the routines can easily be used to set up a
# rootfs image for any embedded Linux device.
#

# Update script info for each change
SCRIPT_VERSION="1.20"
SCRIPT_DATE="March 6, 2018"

cmdname=$(basename "$0")

# Helper directories
working_dir=$(pwd)
temp_dir=$(mktemp -q -d /tmp/ayla_install.XXXX)
ayla_src_dir="$temp_dir/ayla/src"

# Directories to create on the target
ayla_config_dir=/home/pi/ayla/config
ayla_install_dir=/home/pi/ayla

# Options (with defaults assigned)
dry_run=0
upgrade_only=0
build_env_only=0
ayla_package="https://github.com/AylaNetworks/device_linux_public.git"
ayla_package_path=
config_dir=$working_dir
app_to_build="appd"
no_wifi=0
support_gpio=0
support_usbmodem=0
support_ble=0
support_zigbee=0
log_file=""

# Lists of supported JSON requests
JSON_URIS_COMMON="
regtoken.json
local_reg.json
time.json
push_button_reg.json
status.json
lanota.json"

JSON_URIS_WIFI="
wifi_scan.json
wifi_stop_ap.json
wifi_connect.json
wifi_scan_results.json
wps_pbc.json
wifi_profile.json
wifi_profiles.json
wifi_status.json"

# These must be provided, because the defaults are missing required values
REQUIRED_CONFIG_FILES="devd.conf"

# PROD code to pass the Ayla build.  This selects platform-specific source.
PROD_CODE="raspberry_pi"

# Name of dhcpcd event hook script to use on the Raspberry Pi
DHCP_HOOK_SCRIPT="ayla-dhcpcd-hook"

# Path of configuration script that ayla_install.sh creates on a clean install. 
AYLA_INSTALL_OPTS_FILE="$ayla_install_dir/ayla_install.opts"

#
# Utility functions
#
print_banner() {
	echo ""
	echo "****************************************"
	echo " $*"
	echo "****************************************"
	if [ "$log_file" != "" ]; then
		echo "" >> $log_file
		echo "$*" >> $log_file
	fi
}

print_msg() {
	echo "** $*"
	if [ "$log_file" != "" ]; then
		echo " * $*" >> $log_file
	fi
}

cleanup_install() {
	rm -rf $temp_dir
}

success_exit() {
	print_msg "completed successfully"
	cleanup_install
	cd $working_dir
	exit 0
}

error_exit() {
	print_msg "ERROR: $*"
	cleanup_install
	cd $working_dir
	exit 1
}

check_root_user() {
	if [ $(id -u) -eq 0 ]; then
		print_msg "running as root"
		return 1
	else
		print_msg "not running as root"
		return 0
	fi
}

check_config_files() {
	local success=1

	# Skip this check if just setting up the build environment
	[ $build_env_only -eq 1 ] && return $success

	for file in $REQUIRED_CONFIG_FILES
	do
		# Config is not overwritten for an upgrade
		[ $upgrade_only -eq 1 ] && [ -f $ayla_config_dir/$file ] && continue

		if [ ! -f $config_dir/$file ]; then
			print_msg "missing required config file: $file"
			success=0
		fi 
	done
	return $success
}

check_internet_connection() {
	local reliable_url="www.apple.com"
	ping -c 1 -q $reliable_url >/dev/null 2>&1
	if [ $? -ne 0 ]; then
		print_msg "Internet connection DOWN"
		return 0
	else
		print_msg "Internet connection UP"
		return 1
	fi
}

#check if OS release time is later than 2017 Aug 8 16:00:15
#uname -v | awk '{ print $8,$4,$5,$6 }' OFS=' '
#2017 Aug 8 16:00:15
check_release_time() {
	local year=`uname -v | awk '{ print $8 }'`
	local month=`uname -v | awk '{ print $4 }'`
	local mon_num=0
	local day=`uname -v | awk '{ print $5 }'`
	local hour=`uname -v | awk '{ print $6 }'| awk -F ':' '{ print $1 }'`
	local minute=`uname -v | awk '{ print $6 }'| awk -F ':' '{ print $2 }'`
	local second=`uname -v | awk '{ print $6 }'| awk -F ':' '{ print $3 }'`
	if [ $year -gt 2017 ]; then
		return 1
	elif [ $year -lt 2017 ]; then
		return 0
	fi
	case $month in
		'Jan') mon_num=1;;
		'Feb') mon_num=2;;
		'Mar') mon_num=3;;
		'Apr') mon_num=4;;
		'May') mon_num=5;;
		'Jun') mon_num=6;;
		'Jul') mon_num=7;;
		'Aug') mon_num=8;;
		'Sep') mon_num=9;;
		'Oct') mon_num=10;;
		'Nov') mon_num=11;;
		'Dec') mon_num=12;;
	esac
	if [ $mon_num -gt 8 ]; then
		return 1
	elif [ $mon_num -lt 8 ]; then
		return 0
	fi
	if [ $day -gt 8 ]; then
		return 1
	elif [ $day -lt 8 ]; then
		return 0
	fi
	if [ $hour -gt 16 ]; then
		return 1
	elif [ $hour -lt 16 ]; then
		return 0
	fi
	if [ $minute -gt 0 ]; then
		return 1
	elif [ $minute -lt 0 ]; then
		return 0
	fi
	if [ $second -gt 15 ]; then
		return 1
	elif [ $second -lt 15 ]; then
		return 0
	fi
	return 1
}

install_dir() {
	local dir="$1"
	print_msg "creating directory: $dir"
	mkdir -p $dir
	[ $? -ne 0 ] && print_msg "install_dir: mkdir $dir failed"
}

install_tar() {
	local file="$1"
	local dir="$2"
	if [ $# -gt 1 ]; then
		print_msg "unpacking $file to $dir"
		tar -xf $file --directory $dir
	else
		print_msg "unpacking $file"
		tar -xf $file
	fi
	[ $? -ne 0 ] && error_exit "extract failed: $file"
}

download_file() {
	local url="$1"
	local dir="$2"
	if [ $# -gt 1 ]; then
		print_msg "downloading $url to $dir"
		wget -P $dir $url
	else
		print_msg "downloading $url"
		wget $url
	fi
	[ $? -ne 0 ] && error_exit "download failed: $url"
}

update_package_manager() {
	print_msg "updating package manager"
	apt-get -y -q update
	apt-get -y -q upgrade
}

install_package() {
	local name="$1"
	print_msg "installing package: $name"
	apt-get install -y -q $name
	[ $? -ne 0 ] && error_exit "package install failed: $name"
}

git_clone_repo() {
	local url="$1"
	local dir="$2"
	# Install GIT if not installed
	which git >/dev/null || install_package "git"
	print_msg "YOU MAY HAVE TO TYPE IN YOUR GITHUB CREDENTIALS"
	if [ -d "$dir" ] && [ -d "$dir/.git" ]; then
		print_msg "pulling existing GIT repository: $url to $dir"
		cd "$dir"
		git pull -q -f origin
		[ $? -ne 0 ] && error_exit "GIT pull failed: $url"
		cd $working_dir
	else
		print_msg "cloning GIT repository: $url to $dir"
		git clone -q "$url" "$dir"
		[ $? -ne 0 ] && error_exit "GIT clone failed: $url"
	fi
}

install_curl_from_source() {
	local version="7.64.1"
	local file="curl-$version"
	local archive="$file.tar.gz"
	# Install dependencies
	print_msg "installing CURL dependencies"
	install_package "libssl-dev"
	# Download CURL source
	download_file "https://curl.haxx.se/download/$archive" $temp_dir
	cd $temp_dir
	install_tar "$archive"
	cd $temp_dir/$file
	print_msg "building CURL"
	./configure
	make --quiet
	make --quiet install
	cd $working_dir
}

install_bluez_from_source() {
	local version="5.49"
	local file="bluez-$version"
	local archive="$file.tar.xz"
	# Install dependencies
	print_msg "installing BlueZ dependencies"
	install_package "libglib2.0-dev"
	install_package "libudev-dev"
	install_package "libreadline6-dev"
	install_package "libical-dev"
	install_package "libdbus-1-dev"
	# Download BlueZ source
	download_file "https://www.kernel.org/pub/linux/bluetooth/$archive" $temp_dir
	cd $temp_dir
	install_tar "$archive"
	cd $temp_dir/$file
	print_msg "building BlueZ"
	./configure
	make --quiet
	make --quiet install
	cd $working_dir
	# Make sure bluetoothd runs at boot
	systemctl enable bluetooth
}

install_ayla_modules() {
	# Unpack specified Ayla source package
	if echo "$ayla_package" | grep -q "\.tar$" || echo "$ayla_package" | grep -q "\.tar.gz$" || echo "$ayla_package" | grep -q "\.tgz$"; then
		install_tar "$ayla_package_path" "$ayla_src_dir"
	elif echo "$ayla_package" | grep -q "\.git$"; then
		git_clone_repo "$ayla_package" "$ayla_src_dir"
	else
		error_exit "unsupported package type: $ayla_package_path"
	fi
	# Build and install.
	# Note: not using TYPE=RELEASE build option here, because we are
	# setting up a dev kit and want to facilitate debugging.
	cd $ayla_src_dir
	# If source package has a rel[ease] directory, build it
	if [ -d rel ]; then
		cd rel
		ayla_src_dir=$ayla_src_dir/rel
	fi
	print_msg "compiling Ayla source at $(pwd)"
	make INSTALL_ROOT=$ayla_install_dir clean
	make PROD=$PROD_CODE APP=$app_to_build INSTALL_ROOT=$ayla_install_dir NO_WIFI=$no_wifi install
	[ $? -ne 0 ] && error_exit "Ayla build failed"

	# Copy the default configs from install directory
	print_msg "copying Ayla default config files to $ayla_config_dir"
	cp --no-clobber -f $ayla_install_dir/etc/config/* $ayla_config_dir/
	# Copy supplied config files into config directory (overwriting default, if not upgrading)
	for file in $REQUIRED_CONFIG_FILES
	do
		# Do not overwrite existing config files for an upgrade
		[ $upgrade_only -eq 1 ] && [ -f $ayla_config_dir/$file ] && continue

		print_msg "installing supplied config: $file"
		cp $config_dir/$file $ayla_config_dir/$file
	done
	# Install platform-specific scripts
	print_msg "installing platform-specific scripts from lib/platform/$PROD_CODE/scripts/"
	cp -rf lib/platform/$PROD_CODE/scripts/* $ayla_install_dir/bin/
	chmod 755 $ayla_install_dir/bin/*.sh
	cd $working_dir
}

install_ayla_certs() {
	local ayla_cert_dir="daemon/devd/certs"
	local system_cert_dir="/etc/ssl/certs"
	cd $ayla_src_dir
	[ -d $ayla_cert_dir ] || error_exit "Ayla certificate directory not found: $ayla_cert_dir"
	print_msg "copying Ayla certificates to $system_cert_dir"
	cp -f $ayla_cert_dir/* $system_cert_dir
	print_msg "rehashing certificate symlinks"
	c_rehash $system_cert_dir
}

append_config_line() {
	local line="$1"
	local file="$2"
	grep -q -x -F "$line" $file && return
	print_msg "appending \"$line\" to $file"
	echo "" >> $file
	echo $line >> $file
	echo "" >> $file
}

generate_init_script() {
	local exec="$1"
	local options="$2"
	local description="$3"
	local file="/etc/init.d/$exec"

	print_msg "generating init script $file for $description"

	[ $upgrade_only -eq 0 ] && [ -f $file ] && error_exit "$file already exists. Remove it, or perform upgrade"
	rm -f $file

	# Create a simple init.d script for our executable
	echo "#! /bin/sh" > $file
	echo "### BEGIN INIT INFO" >> $file
	echo "# Provides:          $exec" >> $file
	echo "# Required-Start:    \$remote_fs \$syslog \$network" >> $file
	echo "# Required-Stop:     \$syslog" >> $file
	echo "# Default-Start:     2 3 4 5" >> $file
	echo "# Default-Stop:      0 1 6" >> $file
	echo "# Description: $description" >> $file
	echo "### END INIT INFO" >> $file
	echo "" >> $file
	echo "BIN=$exec" >> $file
	echo "DIR=$ayla_install_dir" >> $file
	echo "BIN_PATH=\$DIR/bin/\$BIN" >> $file
	echo "OPTIONS=\"$options\"" >> $file
	echo "DESC=\"$description\"" >> $file
	echo "" >> $file
	echo "# PATH should only include /usr/* if it runs after the mountnfs.sh script" >> $file
	echo "PATH=/sbin:/usr/sbin:/bin:/usr/bin:\$DIR/bin" >> $file
	echo "# Exit if the package is not installed" >> $file
	echo "[ -x \"\$BIN_PATH\" ] || exit 0" >> $file
	echo ". /lib/lsb/init-functions" >> $file
	echo "" >> $file
	echo "do_start()" >> $file
	echo "{" >> $file
	echo "        # Return" >> $file
	echo "        #   0 if daemon has been started" >> $file
	echo "        #   1 if daemon was already running" >> $file
	echo "        #   other if daemon could not be started or a failure occurred" >> $file
	echo "        start-stop-daemon --start --quiet --exec \$BIN_PATH -- \$OPTIONS" >> $file
	echo "}" >> $file
	echo "do_stop()" >> $file
	echo "{" >> $file
	echo "        # Return" >> $file
	echo "        #   0 if daemon has been stopped" >> $file
	echo "        #   1 if daemon was already stopped" >> $file
	echo "        #   other if daemon could not be stopped or a failure occurred" >> $file
	echo "        start-stop-daemon --stop --quiet --retry=TERM/30/KILL/5 --exec \$BIN_PATH" >> $file
	echo "}" >> $file
	echo "" >> $file
	echo "case \"\$1\" in" >> $file
	echo "  start)" >> $file
	echo "        log_daemon_msg \"Starting \$DESC \$BIN_PATH\"" >> $file
	echo "        do_start" >> $file
	echo "        ;;" >> $file
	echo "  stop)" >> $file
	echo "        log_daemon_msg \"Stopping \$DESC \$BIN_PATH\"" >> $file
	echo "        do_stop" >> $file
	echo "        ;;" >> $file
	echo "  restart|force-reload)" >> $file
	echo "        \$0 stop" >> $file
	echo "        \$0 start" >> $file
	echo "        ;;" >> $file
	echo "  *)" >> $file
	echo "        echo \"Usage: $file {start|stop|restart|force-reload}\" >&2" >> $file
	echo "        exit 3" >> $file
	echo "        ;;" >> $file
	echo "esac" >> $file
	echo ":" >> $file

	# Fix init script file permissions
	chmod 755 $file
}

configure_init_script() {
	local script="$1"
	local start_order="$2"
	local kill_order="$3"

	update-rc.d $script defaults $start_order $kill_order
	update-rc.d $script enable
}

configure_lighttpd() {
	local server_root="/var/www/html"
	# Enable lighttpd CGI module
	print_msg "enabling lighttpd CGI module"
	lighty-enable-mod cgi
	# Add lighttpd config rule to use acgi as to handle JSON requests
	append_config_line "cgi.assign = ( \".json\" => \"$ayla_install_dir/bin/acgi\" )" /etc/lighttpd/conf-enabled/10-cgi.conf
	# Force MIME type text/html on "client" and "wifi" requests
	append_config_line '$HTTP["url"] == "/client" { mimetype.assign = ( "" => "text/html" ) }' /etc/lighttpd/lighttpd.conf
	if [ $no_wifi -eq 0 ]; then
		append_config_line '$HTTP["url"] == "/wifi" { mimetype.assign = ( "" => "text/html" ) }' /etc/lighttpd/lighttpd.conf
	fi
	# Remove lighttpd default index file
	print_msg "removing lighttpd's default index HTML file"
	rm $server_root/index.lighttpd.html
	# Copy static web files to web server root directory
	cp -rf $ayla_src_dir/dev_kit/raspberry_pi/www/* $server_root/
	# Add stub to web server root directory for each supported .json request
	for file in $JSON_URIS_COMMON
	do
		print_msg "creating stub: $server_root/$file"
		touch "$server_root/$file"
	done
	if [ $no_wifi -eq 0 ]; then
		for file in $JSON_URIS_WIFI
		do
			print_msg "creating stub: $server_root/$file"
			touch "$server_root/$file"
		done
	fi
	# Set permissions of all web files so web server user can access them
	chmod 755 $server_root/*
}

gen_ayla_install_opts_file() {
	print_msg "saving $cmdname options file: $AYLA_INSTALL_OPTS_FILE"
	# Save relevant install options to use for upgrades and OTA updates
	echo "#!/bin/sh" > $AYLA_INSTALL_OPTS_FILE
	echo "# Auto-generated by $cmdname on $(date)" >> $AYLA_INSTALL_OPTS_FILE
	echo "app_to_build=\"$app_to_build\"" >> $AYLA_INSTALL_OPTS_FILE
	echo "no_wifi=$no_wifi" >> $AYLA_INSTALL_OPTS_FILE
}

print_usage() {
	print_banner "Usage"
	echo "$cmdname [OPTIONS]"
	echo "OPTIONS:"
	echo "  -d, --dryrun        Tests script configuration and exits without modifying the system"
	echo "  -u, --upgrade       Modifies install to avoid overwriting existing config"
	echo "  -b, --build_env     Just installs the packages required to compile Ayla modules"
	echo "  -p, --package PATH  Path of Ayla source tarball, or URL to GIT repo (default: device_linux_public.git)"
	echo "  -c, --config DIR    Directory to find required config files (default: $(pwd)/)"
	echo "  -a, --app APP_NAME  Appd to build (default: appd)"
	echo "  -n, --no_wifi       Omits installing and configuring Wi-Fi-specific components"
	echo "  -g, --gpio          Adds Wiring Pi library for Raspberry Pi"
	echo "  -m, --modem         Adds usb-modeswitch library to support USB connected [cellular] modems"
	echo "  -z, --ble           Installs BlueZ Bluetooth daemon from source to enable full BLE support"
	echo "  -e, --zigbee        Installs libreadline-dev/libncurses-dev to enable full ZigBee support"
	echo "  -t, --multi         Installs BlueZ Bluetooth daemon/libreadline-dev/libncurses-dev to enable BLE/ZigBee support"
	echo "  -l, --log PATH      Dump installation details to a log file"
	echo "  -v, --version       Print script version"
	echo "  -h, --help          Print usage"
	cleanup_install
	cd $working_dir
	exit 0
}

print_version() {
	print_banner "$cmdname"
	echo "Version:  $SCRIPT_VERSION"
	echo "Date:     $SCRIPT_DATE"
	cleanup_install
	cd $working_dir
	exit 0
}

#
# On upgrade installs, load the ayla install options file as defaults
#
if [ -f $AYLA_INSTALL_OPTS_FILE ]; then
	print_msg "Loading default options from $AYLA_INSTALL_OPTS_FILE"
	. $AYLA_INSTALL_OPTS_FILE
fi

#
# Get command line options
#
while [ "$1" ]
do
	case $1 in
		-d|--dryrun)
			dry_run=1
		;;
		-u|--upgrade)
			upgrade_only=1
		;;
		-b|--build_env)
			build_env_only=1
		;;
		-p|--package)
			shift
			ayla_package="$1"
			ayla_package_path=$(realpath -q $1)
		;;
		-c|--config)
			shift
			[ -d $1 ] || error_exit "invalid config dir: $1"
			config_dir=$(realpath $1)
		;;
		-a|--app)
			shift
			app_to_build=$1
		;;
		-n|--no_wifi)
			no_wifi=1
		;;
		-g|--gpio)
			support_gpio=1
		;;
		-m|--modem)
			support_usbmodem=1
		;;
		-z|--ble)
			support_ble=1
		;;
		-e|--zigbee)
			support_zigbee=1
		;;
		-t|--multi)
			support_ble=1
			support_zigbee=1
		;;
		-l|--log)
			shift
			log_file=$(realpath $1)
		;;
		-v|--version)
			print_version
		;;
		-h|--help)
			print_usage
		;;
		*)
			echo "invalid option: $1"
			print_usage
		;;
	esac
	shift
done

#
# Initialize log file
#
if [ "$log_file" != "" ]; then
	echo "$cmdname @ $(date)" > $log_file
fi

#
# Set application-specific options
#
if [ "$app_to_build" = "bt_gatewayd" ]; then
	# Require BLE support when building Bluetooth gateway demo
	support_ble=1
elif [ "$app_to_build" = "zb_gatewayd" ]; then
	# Require zigbee support when building ZigBee gateway demo
	support_zigbee=1
elif [ "$app_to_build" = "multi_gatewayd" ]; then
	# Require BLE/zigbee support when building multi protocol gateway demo
	support_ble=1
	support_zigbee=1
fi

#
# Print configuration 
#
print_banner "Configuration"
if [ $dry_run -eq 1 ]; then
	print_msg "script mode:          DRY RUN"
elif [ $build_env_only -eq 1 ]; then
	print_msg "script mode:          BUILD ENVIRONMENT SETUP"
elif [ $upgrade_only -eq 1 ]; then
	print_msg "script mode:          UPGRADE"
else
	print_msg "script mode:          CLEAN INSTALL"
fi
print_msg "package:              $ayla_package"
print_msg "config location:      $config_dir"
print_msg "required config:      $REQUIRED_CONFIG_FILES"
print_msg "application:          $app_to_build"
if [ $no_wifi -eq 1 ]; then
	print_msg "Wi-Fi support:        NO"
else
	print_msg "Wi-Fi support:        YES"
fi
if [ $support_gpio -eq 1 ]; then
	print_msg "GPIO support:         Wiring Pi (for Raspberry Pi)"
fi
if [ $support_usbmodem -eq 1 ]; then
	print_msg "Modem support:        usb-modeswitch"
fi
if [ $support_ble -eq 1 ]; then
	print_msg "BLE support:          BlueZ (bluetoothd)"
fi
if [ $support_zigbee -eq 1 ]; then
	print_msg "zigbee support:       libreadline-dev/libncurses-dev"
fi
print_msg "config directory:     $ayla_config_dir"
print_msg "install directory:    $ayla_install_dir"
if [ "$log_file" != "" ]; then
	print_msg "install log:          $log_file"
fi

#
# Check script prerequisits
#
print_banner "Checking $cmdname prerequisits..."
check_root_user
[ $? -eq 0 ] && error_exit "script must be run as root: use 'sudo'"

check_config_files
[ $? -eq 0 ] && error_exit "required configuration file(s) not found"

check_internet_connection
[ $? -eq 0 ] && error_exit "Network connection required to complete install"

if [ $support_zigbee -eq 1 ]; then
	if [ "$(uname -n)" != "raspberrypi" ]; then
		error_exit "OS is not the raspberrypi"
	fi
	if [ ! -d /home/pi/EmberZNet/v5.7.4.0 ]; then
		error_exit "/home/pi/EmberZNet/v5.7.4.0 not existed, ZigBee gateway need EmberZNet v5.7.4.0 stack"
	fi
	check_release_time
	[ $? -eq 0 ] && error_exit "OS release time `uname -v | awk '{ print $8,$4,$5,$6 }' OFS=' '` is not later than 2017 Aug 8 16:00:15"
	print_msg "check release time OK"
fi

#
# If this is a dry run, then exit now
#
[ $dry_run -eq 1 ] && success_exit

#
# If we are only setting up the build environment, do it here and exit.
#
if [ $build_env_only -eq 1 ]; then
	print_banner "Setting up build environment only..."
	install_dir $temp_dir
	update_package_manager
	install_curl_from_source
	install_package "libjansson-dev"
	if [ $no_wifi -eq 0 ]; then
		install_package "libdbus-1-dev"
	fi
	if [ $support_gpio -eq 1 ];  then
		git_clone_repo "git://git.drogon.net/wiringPi" "$temp_dir/wiringPi"
		cd $temp_dir/wiringPi
		./build
		cd $working_dir
	fi
	if [ $support_ble -eq 1 ]; then
		install_bluez_from_source
	fi
	if [ $support_zigbee -eq 1 ]; then
		install_package "libreadline-dev"
		install_package "libncurses-dev"
	fi
	success_exit
fi

#
# Setup directories
#
print_banner "Creating directories..."
install_dir $temp_dir
install_dir $ayla_src_dir
install_dir $ayla_config_dir
install_dir $ayla_install_dir

#
# On clean installs, create a file to persist selected ayla_install options
#
if [ $upgrade_only -eq 0 ]; then
	print_banner "Generating config files..."
	gen_ayla_install_opts_file
fi

#
# Install required packages
#
print_banner "Installing required packages..."
update_package_manager
install_curl_from_source
install_package "lighttpd"
install_package "libjansson-dev"
if [ $no_wifi -eq 0 ];  then
	install_package "wpasupplicant"
	install_package "hostapd"
	install_package "dnsmasq"
	install_package "libdbus-1-dev"
fi
if [ $support_gpio -eq 1 ];  then
	git_clone_repo "git://git.drogon.net/wiringPi" "$temp_dir/wiringPi"
	cd $temp_dir/wiringPi
	./build
	cd $working_dir
fi
if [ $support_usbmodem -eq 1 ];  then
	install_package "usb-modeswitch"
fi
if [ $support_ble -eq 1 ]; then
	install_bluez_from_source
fi
if [ $support_zigbee -eq 1 ]; then
	install_package "libreadline-dev"
	install_package "libncurses-dev"
fi

#
# Build and install Ayla modules
#
print_banner "Building and installing Ayla modules..."
install_ayla_modules
install_ayla_certs

#
# Update package configuration
#
print_banner "Updating package configuration..."
configure_lighttpd

if [ $no_wifi -eq 0 ]; then
	# Disable dnsmasq and hostapd, in case the package installers enabled them at boot
	print_msg "disabling dnsmasq and hostapd boot scripts"
	update-rc.d dnsmasq disable
	update-rc.d hostapd disable

	# Update /etc/network/interfaces file to disable automatic wpa_supplicant starting (also backup the original file)
	print_msg "disabling wpa_supplicant automatic start in /etc/network/interfaces"
	if [ -f /etc/network/interfaces.orig ]; then
		cp /etc/network/interfaces.orig /etc/network/interfaces
	else
		cp /etc/network/interfaces /etc/network/interfaces.orig
	fi
	sed -i 's/^\s*\(wpa-conf.*\)/\# \1/' /etc/network/interfaces

	# Update dnsmasq's address range and network interface from defaults (also backup the original file)
	print_msg "configuring DHCP server options in /etc/dnsmasq.conf"
	if [ -f /etc/dnsmasq.conf.orig ]; then
		cp /etc/dnsmasq.conf.orig /etc/dnsmasq.conf
	else
		cp /etc/dnsmasq.conf /etc/dnsmasq.conf.orig
	fi
	sed -i s/'#dhcp-range=192.168.0.50,192.168.0.150,255.255.255.0,12h'/'dhcp-range=192.168.0.2,192.168.0.14,255.255.255.240,1h'/ /etc/dnsmasq.conf
	sed -i s/'#interface='/'interface=wlan0'/ /etc/dnsmasq.conf
	
	# Add link to DHCP client event hook script for dhcpcd
	print_msg "creating link to $DHCP_HOOK_SCRIPT in dhcpcd hook directory"
	ln -s $ayla_install_dir/bin/$DHCP_HOOK_SCRIPT /lib/dhcpcd/dhcpcd-hooks/90-$DHCP_HOOK_SCRIPT
fi

#
# Install init.d scripts.
# Note: some Ayla builds may require slightly different command line options to find their config files
#
print_banner "Installing init scripts..."
generate_init_script "devd" "--debug -c $ayla_config_dir/devd.conf" "Ayla cloud client"
configure_init_script "devd" 90 10
generate_init_script "logd" "-c $ayla_config_dir/devd.conf" "Ayla device log server"
configure_init_script "logd" 95 5
if [ $no_wifi -eq 0 ]; then
	generate_init_script "cond" "--debug --wait -c $ayla_config_dir/cond.conf" "Ayla Wi-Fi connection manager"
	configure_init_script "cond" 91 9
fi

print_banner "Installation complete. A reboot may be required to apply all changes."

success_exit
