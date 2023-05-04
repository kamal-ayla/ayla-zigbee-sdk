#!/bin/sh

get_assoclist() {

	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`
	
	if [ $product  == $prod_type_gcnt ]; then
		interfaces=`uci show wireless | grep "wireless.wl*" | cut -d "." -f2 | cut -d "=" -f1| uniq`
		cat /dev/null > /tmp/assoclist.txt

			for interface in $interfaces                        
			do
			MODE=`uci get wireless.${interface}.mode`
			if [ $MODE == "ap" ]; then
				wl -i ${interface} assoclist | cut -d " " -f2 >> /tmp/assoclist.txt
			fi
		done
	fi

	if [ $product  == $prod_type_gdnt ]; then
		interfaces=`iw dev | grep Interface | cut -f 2 -s -d" " | grep wl.*`
		cat /dev/null > /tmp/assoclist.txt

			for interface in $interfaces                        
			do
				wl -i ${interface} assoclist | cut -d " " -f2 >> /tmp/assoclist.txt
		done
	fi	
}

get_sta_associated_ap()
{

	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi
		
	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`
	
	if [ $product  == $prod_type_gcnt ]; then
		interfaces=`uci show wireless | grep "wireless.wl*" | cut -d "." -f2 | cut -d "=" -f1| uniq`
			for interface in $interfaces
			do
	                MODE=`uci get wireless.${interface}.mode`
	                if [ $MODE == "ap" ]; then
							wl -i ${interface} assoclist | cut -d " " -f2 | grep -qx $1
				if [ $? -eq 0 ]; then
					echo $interface
					return
				fi
	                fi
			done

		echo "0"
	fi

	if [ $product  == $prod_type_gdnt ]; then
		interfaces=`iw dev | grep Interface | cut -f 2 -s -d" " | grep wl.*`

			for interface in $interfaces
			do
				wl -i ${interface} assoclist | cut -d " " -f2 | grep -qx $1
				if [ $? -eq 0 ]; then
					echo $interface
					return
				fi
			done

		echo "0"
	fi	
}

get_sta_interface()
{

	interfaces=`uci show wireless | grep "wireless.wl*" | cut -d "." -f2 | cut -d "=" -f1| uniq`
	for interface in $interfaces
	do
	MODE=`uci get wireless.${interface}.mode`
	if [ $MODE == "sta" ]; then
		echo $interface
	return

	fi
	done
	echo "0"
}


get_sta_rssi()
{
	sta_interface=`get_sta_interface`
	if [ $sta_interface == "0" ]; then
		echo "0"
	else
		RSSI=`wl -i $sta_interface rssi`
		if [ $RSSI == "32" ]; then
			echo "0"
			return
		fi

		echo $RSSI
		fi

}


get_sta_noise()
{
	sta_interface=`get_sta_interface`
	sta_bssid=`get_sta_bssid`
	if [[ $sta_bssid == "00:00:00:00:00:00" || $sta_interface == "0" ]]; then
		echo "0"
	else
		wl -i $sta_interface noise
	fi

}


get_sta_channel()
{
	sta_interface=`get_sta_interface`
	sta_bssid=`get_sta_bssid`	
	if [[ $sta_bssid == "00:00:00:00:00:00" || $sta_interface == "0" ]]; then
		echo "0"
	else
		wl -i $sta_interface channel | grep "current mac channel" | awk '{print $4}'
	fi

}

get_sta_ssid()
{
	sta_interface=`get_sta_interface`
	if [ $sta_interface == "0" ]; then
		echo " "
	else
		SSID=`wl -i $sta_interface ssid | sed 's/[" ]//g' | cut -d":" -f2`
		if [ -z $SSID ]; then
			echo " "
		else
			echo $SSID
		fi
	fi

}

get_sta_bssid()
{
	sta_interface=`get_sta_interface`
	if [ $sta_interface == "0" ]; then
		echo "00:00:00:00:00:00"
		return
	else
		BSSID=`wl -i $sta_interface bssid`
		if [ -z $BSSID ]; then
			echo "00:00:00:00:00:00"
		else
			echo $BSSID
		fi
	fi

}

get_wifi_BSSID_fronthaul_2G()
{
	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`
	
	if [ $product  == $prod_type_gcnt ]; then
        front_2g=`ifconfig wl0 | awk '/HWaddr/ {print $5}'`
        f=${#front_2g}
        if [ $f == "17" ]; then
                echo $front_2g
        else
                echo "N/A"
        fi
	fi

	if [ $product  == $prod_type_gdnt ]; then
		front_2g=`wb_cli -s info | grep "Fronthaul" | grep 2G | awk '{print $2}' | sed -e $'s/,/\\\n/g'`		
        f=${#front_2g}
        if [ $f == "17" ]; then
                echo $front_2g
        else
                echo "N/A"
        fi
	fi	
}



get_wifi_BSSID_fronthaul_5G()
{

	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`
	
	if [ $product  == $prod_type_gcnt ]; then	
		front_5g=`ifconfig wl1 | awk '/HWaddr/ {print $5}'`
		f=${#front_5g}
		if [ $f == "17" ]; then
			echo $front_5g
		else
			echo "N/A"
		fi
	fi

	if [ $product  == $prod_type_gdnt ]; then	
		front_5g=`ifconfig wl0 | awk '/HWaddr/ {print $5}'`
		f=${#front_5g}
		if [ $f == "17" ]; then
			echo $front_5g
		else
			echo "N/A"
		fi
	fi	
}

get_wifi_BSSID_backhaul()
{

	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`

	if [ $product  == $prod_type_gdnt ]; then
		back=`wb_cli -s info | grep "Backhaul" | awk '{print $2}' | sed -e $'s/,/\\\n/g'`
		b=${#back}
		if [ $b == "17" ]; then
				echo $back
		else
				echo "N/A"
		fi
	fi

	if [ $product  == $prod_type_gcnt ]; then
		back=`ifconfig wl1_2 | awk '/HWaddr/ {print $5}'`
		b=${#back}
		if [ $b == "17" ]; then
				echo $back
		else
				echo "N/A"
		fi
	fi
}

get_rssi()
{
	if [ -z $1 ]; then                         
		echo "0"  
		return                                               
     	fi
	
	ap_interface=`get_sta_associated_ap $1`
	if [ $ap_interface == "0" ]; then
		echo "0"
	else
		wl -i $ap_interface rssi $1
	fi
}


get_noise()                                                       
{ 
	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi
                                                                    
        ap_interface=`get_sta_associated_ap $1`                           
        if [ $ap_interface == "0" ]; then                                     
                echo "0" 
        else                                                                              
 		wl -i $ap_interface noise
	fi                                                
}



get_ssid()
{
	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi

	ap_interface=`get_sta_associated_ap $1`
        if [ $ap_interface == "0" ]; then          
                echo "0"                       
        else
		uci get wireless.${ap_interface}.ssid
	fi
}


get_channel()
{
	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi

	ap_interface=`get_sta_associated_ap $1`
        if [ $ap_interface == "0" ]; then          
               echo "0" 
        else
		wl -i $ap_interface channel | grep "current mac channel" | awk '{print $4}'
	fi
}



get_active()
{
	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi

	ap_interface=`get_sta_associated_ap $1`
        if [ $ap_interface == "0" ]; then          
                echo "0"                       
        else
		echo "1" 
	fi
}


get_stationtype()
{

	prod_type_gdnt=gdnt-r_extender
	prod_type_gcnt=gcnt-5_extender_orion
	product=`uci get version.@version[0].product`

        if [ -z $1 ]; then
                echo "0"
                return
        fi

        ap_interface=`get_sta_associated_ap $1`
        if [ $ap_interface == "0" ]; then
                echo "0"
        else
		ap_interface="${ap_interface//./_}"
                RADIO=`uci get wireless.${ap_interface}.device`
           if [ $product  == $prod_type_gcnt ]; then
                TYPE=`uci get wireless.${ap_interface}.fronthaul`
                if [ $TYPE == "1" ]; then
                        echo -en "Fronthaul"
                else
                        echo -en "Backhaul"
                fi
          fi
          if [ $product  == $prod_type_gdnt ]; then
                TYPE=`ubus call wireless.accesspoint get | sed -n '/"'${ap_interface}'": {/,/}/p' | awk '/"map"/ {print $2}' | sed 's/"//g' | sed 's/-BSS//g' | sed 's/,//g'`
               if [ $TYPE == "Fronthaul" ]; then
		     echo -en "$TYPE"
               else
                     echo -en "Backhaul"
                fi
           fi

		if [ $product  == $prod_type_gcnt ]; then
			if [ "$RADIO" == "radio_2G" ]; then
	                        echo -en "_2G"
			elif [ "$RADIO" == "radio_5G" ]; then
        	                echo -en "_5G"
			else
                        	echo -en "_"
			fi
		fi
		if [ $product  == $prod_type_gdnt ]; then
			if [ "$RADIO" == "radio1" ]; then
                        	echo -en "_2G"
			elif [ "$RADIO" == "radio0" ]; then
                        	echo -en "_5G"
			else
                        	echo -en "_"
			fi
		fi				
        fi
}

get_parentnode()
{
	cat /etc/config/devd.conf | grep dsn | awk '{print $2}' | sed 's/[",]//g'
}

get_deviceMac()
{
   agent=`uci get mesh_broker.mesh_common.agent_almac`
   echo $agent

}

get_agent_almac()
{
   agent=`uci get mesh_broker.mesh_common.agent_almac`
   echo $agent

}

get_controller_almac()
{
   controller=`uci get mesh_broker.mesh_common.controller_almac`
   echo $controller

}


get_parent_mac()
{	
	status=`ubus call mesh_broker.agent.device_info get | awk '/"X_TCH_ParentAPDevice"/ {print $2}' | sed 's/"//g' | sed 's/,//g'`

	echo $status
}

get_bh_type()
{
	status=`ubus call mesh_broker.agent.device_info get | awk '/"BackhaulLinkType"/ {print $2}' | sed 's/"//g' | sed 's/,//g'`

	echo $status
}

get_backhaul_network_up_time()
{
	status=`wl -i wl0 sta_info all | grep network | awk '{ print substr ($0, 14 ) }' | awk '{ print substr( $0, 1, length($0)-8 ) }'`

	echo $status
}


Macaddr=$2

case "$1" in

	-status)
		get_active $Macaddr
		;; 
	-channel)
		get_channel $Macaddr
		;;
	-mac)
		get_assoclist
		;;
	-rssi)
		get_rssi $Macaddr
		;;
	-noise)                                                                                         
                get_noise $Macaddr                                                                     
                ;; 
	-stationtype)
		get_stationtype $Macaddr
		;;
	-ssid)                                                                                      
                get_ssid $Macaddr                                                                  
                ;;
	-parent)                                                                                      
                get_parentnode                                                                   
                ;;
	-assoc_ap)
		get_sta_associated_ap $Macaddr
		;;
	-sta_rssi)
		get_sta_rssi
		;;
	-sta_noise)
		get_sta_noise
		;;
	-sta_channel)
		get_sta_channel
		;;
	-sta_ssid)
		get_sta_ssid
		;;
	-sta_bssid)
		get_sta_bssid
		;;	
        -sta_bssid_fronthaul_2G)
                get_wifi_BSSID_fronthaul_2G
                ;;
        -sta_bssid_fronthaul_5G)
                get_wifi_BSSID_fronthaul_5G
                ;;
		
        -sta_bssid_backhaul)
                get_wifi_BSSID_backhaul
                ;;
        -sta_device_mac)
                get_deviceMac
                ;;
        -sta_agent_almac)
		get_agent_almac
                ;;
        -sta_controller_almac)
                get_controller_almac
                ;;	
        -sta_parent_mac)
                get_parent_mac
                ;;
        -sta_bh_type)
                get_bh_type
                ;;
	-backhaul_nw_up_time)
                get_backhaul_network_up_time
                ;;
	*)
		echo "Usage: get_stainfo.sh [-status|-channel|-mac|-rssi|-noise|-interf|-stationtype|-ssid|-parent|-assoc_ap \
|-sta_rssi|-sta_noise|-sta_bssid|-sta_ssid|-sta_channel|-sta_bssid_fronthaul_2G|-sta_bssid_fronthaul_5G|-sta_bssid_backhaul|-sta_device_mac|-sta_agent_almac|-sta_controller_almac|-sta_parent_mac|-sta_bh_type|-backhaul_nw_up_time]"
		exit 1
		;;
esac
