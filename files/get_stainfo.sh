#!/bin/sh


get_assoclist() {
	interfaces=`iw dev | grep Interface | cut -f 2 -s -d" "`
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

	interfaces=`iw dev | grep Interface | cut -f 2 -s -d" "`

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
	*)
		echo "Usage: get_devinfo.sh [-status|-channel|-mac|-rssi|-noise|-interf|-stationtype|-ssid|-parent|-assoc_ap]" 
		exit 1
		;;
esac