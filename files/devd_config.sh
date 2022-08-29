#!/bin/sh
region=`cat /proc/rip/016a`
model=`cat /proc/rip/016b`
dsn=`cat /proc/rip/016c`
pub_key=`cat /proc/rip/016d`
oem_key=`cat /proc/rip/016e`
oem=`cat /proc/rip/016f`

if [ -f /etc/config/devd.conf ]
then
        echo "Already devd.conf file existed !!!"
else
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
fi
