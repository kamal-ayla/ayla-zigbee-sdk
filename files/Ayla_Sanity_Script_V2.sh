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
                echo "Sanity_Script_Version: 1.0"
                echo
        else
                echo "Sanity_Script_Version, 1.0"
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

################################################################################
# Sanity Test Functions - END                                                  #
################################################################################

execute_sanity_test_result()
{
	Sanity_Report_Version
	check_os_version
	check_os_details
	check_os_uptime
	check_os_date
	check_uci_configuration
	check_os_nvram
	check_device_serial_number_info
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
	#check_device_bh_fh_eth_status
}

execute_sanity_test_in_extender_with_test_result()
{
	ENABLE_CONSOLE_LOG=false
	execute_sanity_test_result
	#check_device_bh_fh_eth_status

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
			cat ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT} | sshpass -p "${EXTENDER_PASSWD}" ssh -y root@${device_name} "cat > ${OWM0131_FILE_DIR}/${STABILITY_TEST_SCRIPT}"

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

