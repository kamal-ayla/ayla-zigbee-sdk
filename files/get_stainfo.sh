#!/bin/sh


get_assoclist() {
	interfaces=`uci show wireless | grep "wireless.wl*" | cut -d "." -f2 | cut -d "=" -f1| uniq`
	cat /dev/null > /tmp/assoclist.txt

        for interface in $interfaces                        
        do
		MODE=`uci get wireless.${interface}.mode`
		if [ $MODE == "ap" ]; then
			wl -i ${interface} assoclist | cut -d " " -f2 >> /tmp/assoclist.txt
		fi
	done		
}

get_sta_associated_ap()
{

	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi

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
        front_2g=`ifconfig wl0 | awk '/HWaddr/ {print $5}'`
        f=${#front_2g}
        if [ $f == "17" ]; then
                echo $front_2g
        else
                echo "N/A"
        fi
}



get_wifi_BSSID_fronthaul_5G()
{
	front_5g=`ifconfig wl1 | awk '/HWaddr/ {print $5}'`
	f=${#front_5g}
	if [ $f == "17" ]; then
		echo $front_5g
	else
		echo "N/A"
	fi	
}

get_wifi_BSSID_backhaul()
{
        back=`ifconfig wl1_2 | awk '/HWaddr/ {print $5}'`
        b=${#back}
        if [ $b == "17" ]; then
                echo $back
        else
                echo "N/A"
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
	if [ -z $1 ]; then                                                                 
                echo "0"                                                                   
                return                                                                     
        fi

	ap_interface=`get_sta_associated_ap $1`                                    
        if [ $ap_interface == "0" ]; then                                              
                echo "0"                                        
        else  
		RADIO=`uci get wireless.${ap_interface}.device`
		TYPE=`uci get wireless.${ap_interface}.fronthaul`
		if [ $TYPE == "1" ]; then
			echo -en "Fronthaul"
		else
			echo -en "Backhaul"
		fi

		if [ "$RADIO" == "radio_2G" ]; then
			echo -en "_2G"
		elif [ "$RADIO" == "radio_5G" ]; then
			echo -en "_5G"
		else
			echo -en "_"
		fi
	fi

}

get_parentnode()
{
	cat /etc/config/devd.conf | grep dsn | awk '{print $2}' | sed 's/[",]//g'
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
		
		
	*)
		echo "Usage: get_stainfo.sh [-status|-channel|-mac|-rssi|-noise|-interf|-stationtype|-ssid|-parent|-assoc_ap \
|-sta_rssi|-sta_noise|-sta_bssid|-sta_ssid|-sta_channel|-sta_bssid_fronthaul_2G|-sta_bssid_fronthaul_5G|-sta_bssid_backhaul]"
		exit 1
		;;
esac
