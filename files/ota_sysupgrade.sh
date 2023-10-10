#!/bin/sh

NEW_OTA_TYPE=$(uci get dcm_props.ota_upgrade.gw_ota_type)
echo $NEW_OTA_TYPE

cleanup() {
        DELETE_DIR=$1
		logger "$DELETE_DIR: delete directory ..."
        rm -rf $DELETE_DIR
logger "clean"
}

exit_success_upgrade() {
        logger "OTA install complete"
        exit 0
}

exit_failure() {
        logger "ERROR source build failed"
        cleanup $1
        exit 1
}

exec_sysupgrade() {
   AYLA_OTA_BUILD_DIR=$1
   if [ $NEW_OTA_TYPE == "1" ]; then
      if [ $(ls $AYLA_OTA_BUILD_DIR/*.rbi 2> /dev/null | wc -l) != "0" ]; then
	     logger "$AYLA_OTA_BUILD_DIR: build directory ..."
	     logger "=====================New OTA============================="
         logger "install  OTA source"
         ota=`(sysupgrade-safe -o $AYLA_OTA_BUILD_DIR/*.rbi 2> /dev/null | wc -l)`
		 logger "$ota : >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
         if [ $ota -eq 40 ]; then
            uci set dcm_props.ota_upgrade.gw_sys_upgrade_status='0'
            uci commit
            logger " new OTA upgrade sucesss "
			exit_success_upgrade
         fi
         if [ $ota -eq 28 ]; then
            uci set dcm_props.ota_upgrade.gw_sys_upgrade_status='1'
            uci commit
            logger " new OTA upgrade failed "
			exit_failure $2
         fi
      fi
else
   if [ $(ls $AYLA_OTA_BUILD_DIR/*.rbi 2> /dev/null | wc -l) != "0" ]; then
      logger "=====================Full Device OTA============================="
      logger "$cmdname: install  OTA source"
      sysupgrade $AYLA_OTA_BUILD_DIR/*.rbi &

      if [ $? -ne 0 ]; then
	     exit_failure $2
      fi
      sleep 20
	  exit_success_upgrade
   fi
fi
}

data=$2
clean=$3
case "$1" in
        -sysupgrade)
                exec_sysupgrade $data $clean
                ;;
        *)
                echo "Usage: ota_sysupgrade.sh [-sysupgrade]"
		exit 1
		;;
esac

