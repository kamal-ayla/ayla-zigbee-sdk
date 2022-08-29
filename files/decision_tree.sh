#!/bin/sh
cleanup_ayla_env()
{
	# Delete below existing files 
	rm -rf /etc/config/devd.conf 
	rm -rf /etc/config/devd.conf.startup
	
	# cleanup the ayla folder
	rm -rf /etc/ayla/used_dsns/*.xml
	rm -rf /etc/ayla/*.conf
}

production_devd_file()
{

	if [[ -f /proc/rip/016a  && -f /proc/rip/016b && -f /proc/rip/016c && -f /proc/rip/016d && -f /proc/rip/016d && -f /proc/rip/016f ]];then	
		region=`cat /proc/rip/016a`
		model=`cat /proc/rip/016b`
		dsn=`cat /proc/rip/016c`
		pub_key=$(awk '/-----BEGIN RSA PUBLIC KEY-----/{ f = 1; next } /-----END RSA PUBLIC KEY-----/{ f = 0 } f' /proc/rip/016d | sed 's/\\$//g' $pub_key | awk '{printf "%s", $1}')
		oem_key=`cat /proc/rip/016e`
		oem=`cat /proc/rip/016f`		

		touch /etc/config/devd.conf
		echo "{" >> /etc/config/devd.conf
		echo "    \"config\": {" >> /etc/config/devd.conf
		echo "        \"sys\": {" >> /etc/config/devd.conf
		echo "            \"factory\": 1" >> /etc/config/devd.conf
		echo "        }," >> /etc/config/devd.conf
		echo "        \"id\": {" >> /etc/config/devd.conf
		echo "            \"dsn\": \"$dsn\"," >> /etc/config/devd.conf
		echo "            \"rsa_pub_key\": \"-----BEGIN RSA PUBLIC KEY-----\n$pub_key\n-----END RSA PUBLIC KEY-----\n\"" >> /etc/config/devd.conf
		echo "        }," >> /etc/config/devd.conf
		echo "        \"client\": {" >> /etc/config/devd.conf
		echo "            \"region\": \"$region\"" >> /etc/config/devd.conf
		echo "        }," >> /etc/config/devd.conf
		echo "        \"oem\": {" >> /etc/config/devd.conf
		echo "            \"oem\": \"$oem\"," >> /etc/config/devd.conf
		echo "            \"model\": \"$model\"," >> /etc/config/devd.conf
		echo "            \"key\": \"$oem_key\"" >> /etc/config/devd.conf
		echo "        }" >> /etc/config/devd.conf
		echo "    }" >> /etc/config/devd.conf
		echo "}" >> /etc/config/devd.conf
	else
		echo "RIP ID's not available.Ayla shouldn't start"
	fi	
}

developer_devd_file()
{
        if [[ -f /proc/rip/016a  && -f /proc/rip/016b && -f /proc/rip/016c && -f /proc/rip/016d && -f /proc/rip/016d && -f /proc/rip/016f ]];then	
		region=`cat /proc/rip/016a`
		model=`cat /proc/rip/016b`
		dsn=`cat /proc/rip/016c`
		pub_key=$(awk '/-----BEGIN RSA PUBLIC KEY-----/{ f = 1; next } /-----END RSA PUBLIC KEY-----/{ f = 0 } f' /proc/rip/016d | sed 's/\\$//g' $pub_key | awk '{printf "%s", $1}')
		oem_key=`cat /proc/rip/016e`
		oem=`cat /proc/rip/016f`
	
		touch /etc/config/devd.conf
		echo "{" >> /etc/config/devd.conf
		echo "    \"config\": {" >> /etc/config/devd.conf
		echo "        \"sys\": {" >> /etc/config/devd.conf
		echo "            \"factory\": 1" >> /etc/config/devd.conf
		echo "        }," >> /etc/config/devd.conf
		echo "        \"id\": {" >> /etc/config/devd.conf
		echo "            \"dsn\": \"$dsn\"," >> /etc/config/devd.conf
		echo "            \"rsa_pub_key\": \"-----BEGIN RSA PUBLIC KEY-----\n$pub_key\n-----END RSA PUBLIC KEY-----\n\"" >> /etc/config/devd.conf
		echo "        }," >> /etc/config/devd.conf
		echo "        \"client\": {" >> /etc/config/devd.conf
		echo "            \"region\": \"$region\"," >> /etc/config/devd.conf
		echo "               \"server\": {" >> /etc/config/devd.conf
		echo "               \"default\": 1" >> /etc/config/devd.conf		
		echo "        	}" >> /etc/config/devd.conf		
		echo "        }," >> /etc/config/devd.conf
		echo "        \"oem\": {" >> /etc/config/devd.conf
		echo "            \"oem\": \"$oem\"," >> /etc/config/devd.conf
		echo "            \"model\": \"$model\"," >> /etc/config/devd.conf
		echo "            \"key\": \"$oem_key\"" >> /etc/config/devd.conf
		echo "        }" >> /etc/config/devd.conf
		echo "    }" >> /etc/config/devd.conf
		echo "}" >> /etc/config/devd.conf
	else
		echo "RIP ID's not available.Ayla shouldn't start"
	fi
}

production_env_setup()
{
	if [ -f /etc/config/devd.conf ]; then
	
		server=$(cat /etc/config/devd.conf | awk '/"server"/ {print $1}')
		default=$(cat /etc/config/devd.conf | awk '/"default"/ {print $1}')
	
		if [ $server == \"server\": ] || [ $default == \"default\": ]; then	
			
			# To delete existed files
			cleanup_ayla_env
			
			# To generate devd.conf file for production  
			production_devd_file
			
			# Remove CTRL-M characters from a file
			sed -e "s/\r//g" /etc/config/devd.conf > /etc/config/ndevd.conf
			mv /etc/config/ndevd.conf /etc/config/devd.conf
		else
			echo "server and default not available in the devd.conf file "
		fi
	else
	
		# To delete existed files
		cleanup_ayla_env	
	
		# To generate devd.conf file for production 
		production_devd_file

		# Remove CTRL-M characters from a file
		sed -e "s/\r//g" /etc/config/devd.conf > /etc/config/ndevd.conf
		mv /etc/config/ndevd.conf /etc/config/devd.conf


	fi	
}
		
developer_env_setup()
{
	if [ -f /etc/config/devd.conf ]; then
	
		# To Veify server and default entries in the existing file
		server=$(cat /etc/config/devd.conf | awk '/"server"/ {print $1}')
		default=$(cat /etc/config/devd.conf | awk '/"default"/ {print $1}')
		
		if [ $server == \"server\": ] || [ $default == \"default\": ]; then
		
			echo "server and default available in the devd.conf file"
		else
			# To delete existed files
			cleanup_ayla_env
			
			# To generate devd.conf file for developer 
 			developer_devd_file	

			# Remove CTRL-M characters from a file
			sed -e "s/\r//g" /etc/config/devd.conf > /etc/config/ndevd.conf
			mv /etc/config/ndevd.conf /etc/config/devd.conf

		fi
	else
	
		# To delete existed files
		cleanup_ayla_env
	
		# To generate devd.conf file for production 
		developer_devd_file

		# Remove CTRL-M characters from a file
		sed -e "s/\r//g" /etc/config/devd.conf > /etc/config/ndevd.conf
		mv /etc/config/ndevd.conf /etc/config/devd.conf

	fi	
}		
		
		
case "$1" in

        -production)
                production_env_setup
                ;;
		
        -developer)
                developer_env_setup
                ;;
		
		
	*)
		echo "Usage: decision_tree.sh [-production|-developer]"
		exit 1
		;;
esac		
