#!/bin/sh

##### Global Configurations #####

STABILITY_TEST_FILE=$0
STABILITY_TEST_SCRIPT=$(basename ${STABILITY_TEST_FILE})

NGROK_PORT_NUM=18136   
NGROK_HOSTNAME="0.tcp.ap.ngrok.io"

CONTROLLER_PASSWD='root'
EXTENDER_PASSWD='root'

UPTIME_MINIMUM=1
VERSION=$(cat /etc/config/version | grep "option version" | sed -e "s/ option\ version //g" | sed -e "s/'//g")

CURRENT_DATE_TIME=$(date +"%Y%m%d_%H%M%S")

DEVICE_DSN=$(cat /etc/config/devd.conf | grep dsn | awk '{print $2}' | sed 's/[",]//g')
#echo $DEVICE_DSN

MESH_SANITY_LOG_FILE="owm0131_mesh_sanity"
MESH_SANITY_LOG_TAR_DIR="${CURRENT_DATE_TIME}_${MESH_SANITY_LOG_FILE}"
MESH_SANITY_LOG_TAR_FILE="${CURRENT_DATE_TIME}_${DEVICE_DSN}_${MESH_SANITY_LOG_FILE}.tar"

SANITY_TEST_RESULT_CSV="sanity_test_result.csv"
SANITY_TEST_RESULT_CONSOLE_OUTPUT="sanity_test_result_console_output.log"
#CORE_FILES="CORE"  r\\\\  ,./

OWM0131_FILE_DIR="/root"
OWM0131_LOG_DIR="/root/${DEVICE_DSN}_${MESH_SANITY_LOG_FILE}"

#echo $OWM0131_LOG_DIR

##### Device IDs Configuration

CONTROLLER_SERIAL_NUM=$(rip-create-efu.sh | grep Serial | awk '{print $4}')
#echo $CONTROLLER_SERIAL_NUM

#CONTROLLER_SERIAL_NUM="CP2235RA8VJ"
#DEVICE_NAME="CP2235RA8WM" 

dev_serial=$(ubus call mesh_broker.controller.apdevice get | grep SerialNumber | awk '{print $2}' | sed 's/[",]//g')
DEVICE_NAME="${dev_serial//$CONTROLLER_SERIAL_NUM/}"
#echo $DEVICE_NAME


#DEVICE_NAME_DSN="CP2235RA8WM_AC000W028037584"

################################################################################
# Sanity Test Functions - Start                                                #
################################################################################
Sanity_Report_Version()
{
        if [ "${ENABLE_CONSOLE_LOG}" = true ]
        then
                echo "Sanity_Script_Version: 1.1"
                echo
        else
                echo "Sanity_Script_Version, 1.1"
        fi
}

check_os_version()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " Image version:"
		cat /etc/config/version
		echo
	else
		version=$(cat /etc/config/version | grep "option version" | sed -e "s/ option\ version //g" | sed -e "s/'//g"
)


		if [ $version == $VERSION ]
		then
			echo "check_os_version, PASSED"
		else
			echo "check_os_version, FAILED"
		fi
	fi
}

check_os_details()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " OS Details:"
		uname -a
		echo
	else
		arch_type=$(uname -m)
		kernel_ver=$(uname -r)
		os_name=$(uname -s)
		build_version=$(uname -v)

		if [ ${arch_type} == "armv71" ] & [ ${kernel_ver} == "4.19.208" ]
		then
			echo "check_os_details, PASSED"
		else
			echo "check_os_details, FAILED"
		fi
	fi
}

check_os_uptime()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "System Uptime:"
		uptime
		echo
	else
		sysuptime=`echo $(uptime) | sed 's/^.\+up\ \+\([^,]*\).*/\1/g'`
		sysuptime_min=`echo $(uptime) | sed 's/^.\+up\ \+\([^,]*\).*/\1/g' | cut -d " " -f 1`

		echo $sysuptime | grep -q min

		if [ $? -eq 1 ];
		then
			echo "check_os_uptime, PASSED"
		elif [ $sysuptime_min -gt $UPTIME_MINIMUM ];
		then
			echo "check_os_uptime, PASSED"
		else
			echo "check_os_uptime, FAILED"
		fi
	fi

}

check_os_date()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "System Date:"
		date +"%m-%d-%y"	
		echo
	else
		sysdate=$(date +"%m-%d-%y")

		if [ -z ${sysdate} ]
		then
			echo "check_os_date, FAILED"
		else
			echo "check_os_date, PASSED"
		fi
	fi
}

check_uci_configuration()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "UCI Confiuguration - uci show"
		echo
		uci show
		echo
	fi
}

check_mesh_broker_config()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "mesh broker config"
		echo
		cat /etc/config/mesh_broker
		echo
	fi
}

check_wireless_config()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "wireless config"
		echo
		cat /etc/config/wireless
		echo
	fi
}

check_os_nvram()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Nvram show Command output:"
		nvram show
		echo
	else
		os_nvram=$(nvram show)

		if [ -z $os_nvram ];
		then
			echo "check_os_nvram, FAILED"
		else
			echo "check_os_nvram, PASSED"
		fi
	fi
}

check_os_mesh_broker_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Mesh_broker Status"
		ps | grep mesh_broker
		echo
	else
		controller_status=`pidof mesh_broker`

		if [ $controller_status ]
		then
			echo "Mesh_Broker, ENABLED"
		else
			echo "Mesh_Broker, DISABLED"
		fi
	fi
}
check_os_Master_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Master information status"
		wb_cli -m info
		echo
		
		echo "Master slavelist status"
		
		wb_cli -m slavelist
		echo
		
		
    fi
}

check_os_slave_status()
{
	
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Slave information status"
		wb_cli -s info
		echo
		
		echo "client status"
		wb_cli -s clientlist
		echo
		
		echo "Slave list status"
		
		wb_cli -s slavelist
		echo

	fi
}

check_br_lan_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "IP address: br-lan"
		ifconfig br-lan
		echo
		echo "ifconfig -a output:"
		ifconfig -a
		echo
	else
		bridge_status=$(ifconfig br-lan | grep "inet addr" | awk '{print $2}' | tr -d "addr:")

		if [ -n $bridge_status ]
		then
			echo "check_br_lan_status, PASSED"
		else
			echo "check_br_lan_status, FAILED"
		fi
	fi	
}

list_network_interfaces()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Network Interfaces List:"
		ifconfig -a
		echo
	fi
}

check_os_route()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Route Command Output:"
		route -n
		echo
	else
		ROUTE=`route -n | awk  'NR==3 {print $2}'`

		if [ -n $ROUTE ];
		then
			echo "check_os_route, PASSED"
		else
			echo "check_os_route, FAILED"
		fi
	fi
}

check_internet_connectivity()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Internet Connectivity ping test:"
		ping -c 5 8.8.8.8
		echo
	else
		if ping -c 5 8.8.8.8 &> /dev/null
		then
			echo "check_internet_connectivity, PASSED"
		else
			echo "check_internet_connectivity, FAILED"
		fi
	fi
}

check_internet_connectivity_dns_resolution()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Internet Connectivity DNS Resolution ping test:"
		ping -c 5 google.co.in
		echo
	else
		if ping -c 5 google.co.in &> /dev/null
		then
			echo "check_internet_connectivity_dns_resolution, PASSED"
		else
			echo "check_internet_connectivity_dns_resolution, FAILED"
		fi
	fi
}

check_os_cpu_usage()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "CPU usage"
		top -n 1
		echo
	else
		cpu_usage=`grep 'cpu ' /proc/stat | awk '{usage=($2+$4)*100/($2+$4+$5)} END {print usage ""}'`
		cpu_usage_int=${cpu_usage%.*}

		if [ $cpu_usage_int -lt 20 ]
		then
			echo "check_os_cpu_usage, PASSED"
		else
			echo "check_os_cpu_usage, FAILED"
		fi
	fi
}

check_os_ram_usage()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "RAM usage"
		free -h
		echo
	else
		ram_usage=`free -m | awk '/^Mem/ {print $3}'`

		if [ $ram_usage -gt 100000 ]
		then
			echo "check_os_ram_usage, PASSED"
		else
			echo "check_os_ram_usage, FAILED"
		fi
	fi
}

check_meminfo()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Memory info:"
		cat /proc/meminfo
		echo
	fi
}

check_running_process_list()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "List of process running currently:"
		ps -w
		echo
	fi
}

check_device_core_dump()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "List of Core dump Files"
		ls -lh /root/*.gz
		ls -lh /root/*.tgz
		ls -lh /root/*.log
		echo

	fi
}

check_disk_usage()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Disk space:"
		df -h
		echo
	else
		disk_usage=`df | grep ubi0_20 | awk '/^\/dev/ {print $5}' | tr -d '%'`

		if [ $disk_usage -gt 70 ]
		then
			rm -r /root/*.gz
			rm -r /root/*.log
			echo "check_os_disk_usage, FAILED"
		else
			echo "check_os_disk_usage, PASSED"
		fi
	fi
}

check_whitelist_supplicant()
{
    if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " wpa_state "
		ubus call wireless.supplicant get
		echo
		
		echo "Whitelist check"
		ubus call wireless.supplicant.whitelist get
		echo
	else
		wpa_state=$(ubus call wireless.supplicant get | grep wpa_state | awk '{print $2}')
		
		whitelist_state=$(ubus call wireless.supplicant.whitelist get | grep whitelist_state  | awk  '{print $2}')
		
		whitelist_Active=$(ubus call wireless.supplicant.whitelist get | grep whitelist_active  | awk  '{print $2}')
		
		if [ $wpa_state == '"COMPLETED",' ]
		then
			echo "Device is WIFI Onboarding, PASSED"
		else
			echo "Device is WIFI Onboarding, FAILED"
		fi
		
		if [ $whitelist_state == "1," ]
		then
			echo "Device is whitelisted, PASSED"
		else
			echo "Device is whitelisted, FAILED"
		fi
		
		if [ $whitelist_Active == "1" ]
		then
			echo "Device is whitelisted and in active mode, PASSED"
		else
			echo "Device is whitelisted and in active mode, FAILED"
		fi
		
	fi
}  

check_device_ap_scan_mode()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Ethernet and Wireless AP scan status:"
		ubus call wireless.supplicant.apscan get '{"name":"wl0"}'
		echo
		
		echo "Backhaul Sta status:"
		wl -i wl0 bss
		echo
		
		echo "nvram ap scan status"
		nvram get wl0_ap_scan
		echo
		
	else
		ap_scan=$(ubus call wireless.supplicant.apscan get '{"name":"wl0"}' | grep ap_scan_mode | awk '{print $2}')
		mode=$(wl -i wl0 bss)
		nvram=$(nvram get wl0_ap_scan)
		
		if [ $ap_scan == "1" ] 
		then 
			echo "Ubus Call Ap scan mode Status, ENABLED"
		else
			echo "Ubus Call AP scan mode Status, Disabled"
		
		fi 
		
		
		if [ $mode == 'up' ]
		then
			echo "BH Sta status, UP"
		else
			echo "BH Sta Status, DOWN"
		
		fi
		
		
		
		if [ $nvram == "0" ]
		then
			echo " Nvram AP Scan Status, DISABLED"
		else
			echo "Nvram AP Scan Status, ENABLED"
		fi
	fi
	
}

check_device_bh_fh_eth_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Ethernet and Wireless Running status:"
		
		wireless_get_overview.sh
		echo
		ifconfig br-lan
		echo
	else
		ap0=$(wireless_get_overview.sh | grep wl0.1,  | awk '{print $3}')
		ap1=$(wireless_get_overview.sh | grep wl1,  | awk '{print $3}')
		
		ap2=$(wireless_get_overview.sh | grep wl0.2  | awk '{print $3}')

		eth0=$(ifconfig br-lan | grep UP | awk '{print$1}')

		
		if [ $ap1 == "1/1" ]
		then
			echo "check_device_front_haul_2.4G up, PASSED"
		else
			echo "check_device_font_haul_2.4G down, FAILED"
		fi

		if [ $ap0 == "1/1" ]
		then
			echo "check_device_front_haul_5G up, PASSED"
		else
			echo "check_device_font_haul_5G down, FAILED"
		fi


		 if [ $ap2 == "1/1" ]
		 then
			echo "check_device_back_haul_5G up, PASSED"
		 else
		    echo "check_device_back_haul_5G down, FAILED"
		fi

		if [ $eth0 == "UP" ]
		then
			echo "check_device_eth0 up, PASSED"
		else
			echo "check_device_eth0 down, FAILED"
		fi
	fi
}

check_os_mesh_connected()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Mesh Connected Devices"
		wl -i wl0 assoclist	
		echo

		echo "Slave information status"
		wb_cli -s info
		echo
		
		echo "client status"
		wb_cli -s clientlist
		echo
		
		echo "Slave list status"
		
		wb_cli -s slavelist	
		echo 
		
	else
		mesh_devices=`wb_cli -m slavelist	`

		if [ -z $mesh_devices ]
		then
			echo "check_os_mesh_connected, FAILED"
		else
			echo "check_os_mesh_connected, PASSED"
		fi
	fi
}

check_os_ayla_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Devd Process Running:"
		ps | grep devd
		echo

		echo "APPD Process Running:"
		ps | grep appd
		echo
	else
		devd_status=$(pidof devd)
		appd_status=$(pidof appd)

		if [ -n $devd_status ]
		then
			echo "check_ayla_devd, PASSED"
		else
			echo "check_ayla_devd, FAILED"
		fi

		if [ -n $appd_status ]
		then
			echo "check_ayla_appd, PASSED"
		else
			echo "check_ayla_appd, FAILED"
		fi
	fi
}

check_mesh_broker_configuration()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Multiap Confiuguration - /etc/config/multiap"
		echo
		cat /etc/config/mesh_broker
		echo
	fi
}

check_wireless_configuration()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Wireless Confiuguration - /etc/config/wireless"
		echo
		cat /etc/config/wireless
		echo
	fi
}


check_wifi_interface_status()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Wireless interfaces status"
		echo
		echo "wds0.1.1 status"
		wl -i wds0.1.1 status
		echo

		echo "wl0 status"
		echo
		wl -i wl0 status
		echo

		echo "wl0.1 status"
		echo
		wl -i wl0.1 status
		echo

		echo "wl0.2  status"
		echo
		wl -i wl0.2 status
		echo

		echo "wl1 status"
		echo
		wl -i wl1 status
		echo
		echo "i5ctl dm - output:"
		i5ctl dm 
		echo
	fi
}

check_wifi_assoclist()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "WiFi Interface Assoclist"
		echo
		echo "wds0.1.1 assoclist"
		wl -i wds0.1.1 assoclist

		echo "wl0 assoclist"
		echo
		wl -i wl0 assoclist
		echo

		echo "wl0.1 assoclist"
		echo
		wl -i wl0.1 assoclist
		echo
		
		echo "wl0.2 assoclist"
		echo
		wl -i wl0.2 assoclist

		echo "wl1 assoclist"
		echo
		wl -i wl1 assoclist
		echo
	fi
}

check_zigbee_sensors_info()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " Zigbee sensors info:"
		cat /etc/config/appd.conf.startup
	fi
}

check_backbone_list()
{
if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " Hostmanager info:"
		ubus call hostmanager.device get
		echo

		echo "List of Device MAC Addresses are connected with controller"
		ubus call hostmanager.device get | grep "mac-address"
		echo
		
		#echo "List of Device connected with controller"
		#ubus call hostmanager.device get | grep  -w "connected"		
		
		echo
	else
		topo_list=$(ubus call mesh_broker.agent.device_info get | grep RadioNumberOfEntries| awk '{print $2}')

		if [ -z $topo_list ]
		then
			echo "check_topo_list, No child Agent,       "
		else
			echo "check_topo_list, Has child Agent,       "
		fi
	fi
}

check_Agent_info()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " Agent Complete info:"
		ubus call mesh_broker.agent.device_info get
		echo

	fi

}

check_device_serial_number_info()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo " Device serial number info:"
		cat /etc/config/devd.conf.startup
		echo
	else
			Device_number=$(cat /etc/config/devd.conf.startup)
			
			if [ -z $Device_number]
			then 
				echo "check_device_serial_number_info, FAILED"
			else 
				echo "check_device_serial_number_info, PASSED"
		fi
 fi 
}
check_rssi_value()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Rssi value"
		echo "R_value:"  
		wl -i wl0 status | grep RSSI  | awk '{print $3 $4}'	
		echo		
	fi
}

check_noise_value()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Noise value"
		echo "N_value:" 
		wl -i wl0 status | grep RSSI  | awk '{print $9 $10}'		
		echo
	fi
}

check_channel_value()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Channel value"
		echo "C_value:" 
		  wl -i wl0 status | grep -w Primary
		echo
	fi
}
Check_agent_backhaul()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
		then
			echo "Agent_backhaul information:"
			echo "--------------------------" 
			ubus call mesh_broker.agent.device_info get
			echo
	fi
}
ubus_command_check()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Fronthauls, Backhauls, radios and AP information for all connected Agents:"
		echo 
	    ubus call mesh_broker.controller.apdevice get
 		echo
		echo "WiFi easymesh mac addresses of all connected agents:"
		echo
		echo "Station details"
		ubus call mesh_broker.controller.station  get
		echo
		echo "Wifi Mesh MAC address:"
		ubus call mesh_broker.controller.station  get | grep "MACAddress" 
		echo
		echo "Parent Device MAC address:"
		ubus call mesh_broker.controller.apdevice get | grep "X_TCH_ParentAPDevice"
		echo
		echo "Wifi Mesh connected agent address:"
		wb_cli -m slavelist | grep Device
		echo
		echo "To find the List of Eth connected device:"
		echo		 
		ubus call mesh_broker.controller.apdevice get | grep "Eth"
		echo
	else
		topo_list=$(ubus call mesh_broker.controller.apdevice get | grep "AssociatedDevice")

		if [ -z $topo_list ]
		then
			echo "check_topo_list, No child Agent,       "
		else
			echo "check_topo_list, Has child Agent,       "
		echo
	fi
fi	
}
check_device_bh_fh_eth_status_controller_alone()
{
	if [ "${ENABLE_CONSOLE_LOG}" = true ]
	then
		echo "Ethernet and Wireless Running status:"
		
		wireless_get_overview.sh
		echo
		ifconfig br-lan
		echo
	else
		ap0=$(wireless_get_overview.sh | grep wl0,  | awk '{print $3}')
		ap1=$(wireless_get_overview.sh | grep wl1,  | awk '{print $3}')
		
		ap2=$(wireless_get_overview.sh | grep wl0.1  | awk '{print $3}')

		eth0=$(ifconfig br-lan | grep UP | awk '{print$1}')

		
		if [ $ap1 == "1/1" ]
		then
			echo "check_device_front_haul_2.4G up, PASSED"
		else
			echo "check_device_font_haul_2.4G down, FAILED"
		fi

		if [ $ap0 == "1/1" ]
		then
			echo "check_device_front_haul_5G up, PASSED"
		else
			echo "check_device_font_haul_5G down, FAILED"
		fi


		 if [ $ap2 == "1/1" ]
		 then
			echo "check_device_back_haul_5G up, PASSED"
		 else
		    echo "check_device_back_haul_5G down, FAILED"
		fi

		if [ $eth0 == "UP" ]
		then
			echo "check_device_eth0 up, PASSED"
		else
			echo "check_device_eth0 down, FAILED"
		fi
	fi
}

################################################################################
# Sanity Test Functions - END                                                  #
################################################################################

mesh_device()
{
	ubus_command_check
	check_device_bh_fh_eth_status_controller_alone
}
execute_sanity_test_result()
{
	Sanity_Report_Version
	check_os_version
	check_os_details
	check_os_uptime
	check_os_date
	check_uci_configuration
	check_mesh_broker_config
	check_wireless_config
	check_os_nvram
	check_os_mesh_broker_status
	check_os_slave_status
	check_br_lan_status
	list_network_interfaces
	check_os_route
	check_internet_connectivity
	check_internet_connectivity_dns_resolution
	check_os_cpu_usage
	check_os_ram_usage
	check_meminfo
	check_running_process_list
	check_device_core_dump
	check_disk_usage
	check_whitelist_supplicant
	check_device_ap_scan_mode
	check_os_mesh_connected
	check_os_ayla_status
	check_mesh_broker_configuration
	check_wireless_configuration
	check_wifi_interface_status
	check_wifi_assoclist
	check_zigbee_sensors_info
	check_Agent_info
	check_device_serial_number_info
	check_rssi_value
	check_noise_value
	check_channel_value
}

################################################################################
# Sanity test                                                                  #
################################################################################

verify_sshpass_command_availability()
{
	sshpass_location=`which sshpass`

	if [ -z ${sshpass_location} ]
	then
		echo "sshpass command not available"
		exit 1
	fi
}

################################################################################
# Extender Sanity test                                                         #
################################################################################

execute_sanity_test_in_extender_with_console_log()
{
	ENABLE_CONSOLE_LOG=true
	execute_sanity_test_result
	check_device_bh_fh_eth_status
}

execute_sanity_test_in_extender_with_test_result()
{
	ENABLE_CONSOLE_LOG=false
	execute_sanity_test_result
	check_device_bh_fh_eth_status
	
}

execute_sanity_test_in_extender_nodes()
{
	count=`echo ${DEVICE_NAME} | wc -w`
	echo ${count}

	for i in `seq 1 ${count}`
	do
		device_name=`echo $DEVICE_NAME| awk -v dev=$i '{ print $dev }'`
		echo "============================================================"
		echo
		echo "Executing Sanity Script in NG Agent: ${device_name}"
		echo
		echo "============================================================"

		a=`ping -c 1 ${device_name}`

		if [ ${device_name} != ${CONTROLLER_SERIAL_NUM} ]
		then
			cat $0 | sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} "cat > ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT}"

			sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} chmod a+x ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT}

			vv=$(sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} "cat /etc/config/devd.conf|grep dsn")
			agent_dsn=$(echo $vv | awk '{print $2}' | sed 's/[",]//g')
			echo "agent dsn is"
			echo $agent_dsn
			
			AGENT_LOG_DIR="${OWM0131_LOG_DIR}/Agent_${device_name}_${agent_dsn}"
			
			mkdir -p ${OWM0131_LOG_DIR}
			mkdir -p ${AGENT_LOG_DIR}
	
			
			sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT} extender_sanity_with_log >> ${AGENT_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}

			sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT} extender_sanity_with_test_result >> ${AGENT_LOG_DIR}/${SANITY_TEST_RESULT_CSV}
			
		fi
	done
}

################################################################################
# Controller Sanity test                                                       #
################################################################################

execute_sanity_test_in_controller()
{
	verify_sshpass_command_availability

	CONTROLLER_LOG_DIR="${OWM0131_LOG_DIR}/Controller_${CONTROLLER_SERIAL_NUM}_${DEVICE_DSN}"

	mkdir -p ${OWM0131_LOG_DIR}
	mkdir -p ${CONTROLLER_LOG_DIR}

	echo "============================================================"
	echo
	echo "Executing Sanity Script in Controller: ${CONTROLLER_SERIAL_NUM}"
	echo
	echo "============================================================"

	ENABLE_CONSOLE_LOG=false
	execute_sanity_test_result >> ${CONTROLLER_LOG_DIR}/${SANITY_TEST_RESULT_CSV}

	ENABLE_CONSOLE_LOG=true
	execute_sanity_test_result >> ${CONTROLLER_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	ENABLE_CONSOLE_LOG=true
	mesh_device >> ${CONTROLLER_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	check_os_Master_status >> ${CONTROLLER_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	#cp $(ls | grep $STABILITY_TEST_SCRIPT) ${CONTROLLER_LOG_DIR}
}

################################################################################
# Sanity test results from Ngrok SSH                                           #
################################################################################

ngrok_collect_log()
{
	OWM0131_TAR_FILE_DIR=$(sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} pwd)

	sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} mv ${OWM0131_FILE_DIR}/${MESH_SANITY_LOG_FILE} ${OWM0131_FILE_DIR}/${MESH_SANITY_LOG_TAR_DIR}
	sync

	sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} tar -zcf ${MESH_SANITY_LOG_TAR_FILE} -C ${OWM0131_FILE_DIR} ${MESH_SANITY_LOG_TAR_DIR}
	sync

	echo "============================================================"
	echo
	echo "Copying Mesh Sanity Log file to Host PC - ${MESH_SANITY_LOG_TAR_FILE}"
	echo
	echo "============================================================"

	sshpass -p "${CONTROLLER_PASSWD}" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -rP ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME}:${OWM0131_TAR_FILE_DIR}/${MESH_SANITY_LOG_TAR_FILE} .
	if [ $? -eq 0 ]
	then
		echo "Successfully copied Log file to Host PC"
		echo
		echo "============================================================"
		echo
		echo "Cleaning Log files in OWM0131 Controller"
		echo
		echo "============================================================"

		sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} rm -rf ${OWM0131_FILE_DIR}/${MESH_SANITY_LOG_TAR_DIR}
		sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} rm -rf ${OWM0131_TAR_FILE_DIR}/${MESH_SANITY_LOG_TAR_FILE}

	else
		echo "Failed to copy Log file to Host PC"
	fi
	sync
}

ngrok_execute_sanity_test()
{
	# Copy Sanity Test script to Controller through Ngrok
	sshpass -p "${CONTROLLER_PASSWD}" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -rP ${NGROK_PORT_NUM} ${STABILITY_TEST_SCRIPT} root@${NGROK_HOSTNAME}:${OWM0131_FILE_DIR}

	# Give Executable Permission to the Stability Test script
	sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} chmod +x ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT}

	# Execute the Sanity script for the Mesh network
	sshpass -p "${CONTROLLER_PASSWD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p  ${NGROK_PORT_NUM} root@${NGROK_HOSTNAME} ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT} meshsanity

	ngrok_collect_log
}

exec_sanity_in_mesh_network()
{
	execute_sanity_test_in_controller
	execute_sanity_test_in_extender_nodes
	tar -cvf /root/${DEVICE_DSN}_mesh_sanity_test_result.tar -C /root/ ${OWM0131_LOG_DIR}
	rm -rf ${OWM0131_LOG_DIR}
	ls -al /root/
}

################################################################################
# Standalone Device sanity test                                                #
################################################################################

execute_sanity_test_stand_alone()
{
	mkdir -p ${OWM0131_LOG_DIR}
	
	#echo "stand alone called"
	#echo $OWM0131_LOG_DIR

	ENABLE_CONSOLE_LOG=false
	execute_sanity_test_result >> ${OWM0131_LOG_DIR}/${SANITY_TEST_RESULT_CSV}

	ENABLE_CONSOLE_LOG=true
	execute_sanity_test_result >> ${OWM0131_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	ENABLE_CONSOLE_LOG=true
	#mesh_device >> ${CONTROLLER_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	mesh_device >> ${OWM0131_LOG_DIR}/${SANITY_TEST_RESULT_CONSOLE_OUTPUT}
	
	#cp $(ls | grep $STABILITY_TEST_SCRIPT) ${OWM0131_LOG_DIR}
	sync
	#mv ${OWM0131_LOG_DIR} ${OWM0131_FILE_DIR}/${MESH_SANITY_LOG_TAR_DIR}
	tar -cvf /root/${DEVICE_DSN}_sanity_test_result.tar -C /root/ ${OWM0131_LOG_DIR}
	rm -rf ${OWM0131_LOG_DIR}
	ls -al /root/
	sync
}

################################################################################
# Help Function                                                                #
################################################################################

usage()
{
	echo "$0 <stand_alone|ngrok_sanity|meshsanity> <debug>"
	echo "        stand_alone  - runs sanity test locally in the device"
	echo "        ngrok_sanity - collects the sanity through ngrok access"
	echo "        meshsanity   - collects the sanity of total mesh nodes"
	echo
	echo "debug : Enable debug prints in the script"
	echo
}

################################################################################
# Stability Test Execution Starting Point                                      #
################################################################################

if [ $# -eq 0 ]
then
	echo "No arguments supplied"
	echo
	usage
	exit 1
fi

if  [ "$1" = "-h" ] || [ "$1" = "--help" ]
then
        usage
        exit 0
fi

################################################################################
# Enable Debug Mode                                                            #
################################################################################

DEBUG_MODE=$2

if [ "${DEBUG_MODE}" == "debug" ]
then
	echo "Enabling debug mode in stability test script"
	set -x
fi

################################################################################
# SSHPASS command availability check in Host PC                                #
################################################################################

#verify_sshpass_command_availability

################################################################################
# Main Function                                                                #
################################################################################

case "$1" in
	stand_alone)
		execute_sanity_test_stand_alone
		;;
	ngrok_sanity)
		ngrok_execute_sanity_test
		;;
	meshsanity)
		exec_sanity_in_mesh_network
		;;
	extender_sanity_with_log)
		execute_sanity_test_in_extender_with_console_log
		;;
	extender_sanity_with_test_result)
		execute_sanity_test_in_extender_with_test_result
		;;
	*)
		usage
		;;
esac

