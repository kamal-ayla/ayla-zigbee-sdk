# Ayla_Agent

Ayla Networks openWRT agent

Repository for Ayla OpenWRT Agent to be shared between TCH, Ayla and Contractor (Qubercomm)


## Contents

- [Ayla IPK generation using SDK](#ayla-ipk-generation-using-sdk)
- [Ayla IPK installation](#ayla-ipk-installation)


## Ayla ipk generation using sdk

### To Enable and Build ayla IPK package in SDK:

#### Step 1: Clone the ayla_agent source

Export the absolute path of openwrt's SDK directory to `TOP_DIR` environment variable by giving below command

```
$ export TOP_DIR=<Absolute_path_of_SDK_directory>

Example:
$ export TOP_DIR=/home/user/openwrt-sdk-brcm6xxx-tch-GCNTA_502L07p1_gcc-5.5.0_glibc_eabi.Linux-x86_64
```

Clone the ayla_agent source into $TOP_DIR/package/ directory.
```
$ cd $TOP_DIR/package/
$ git clone git@github.com:technicolor-inc/Ayla_Agent.git -b developer ayla-zigbee-sdk
```

#### Step 2: Enable ayla-zigbee-sdk

Now we need to enable the ayla-zigbee-sdk package. Still in your SDK directory:

* Go Back to $TOP_DIR and Enter SDK configuration again: 
```
$ make menuconfig
```
* Navigate to utilities
* Enable ayla-zigbee-sdk as a package. Hit 'y' to do so
* Exit, saving when prompted to do so.

#### Step 3: Prepare

##### Bootstrap the Ayla package by running the bootstrap script from $TOP_DIR of SDK

```
cd $TOP_DIR
./bootstrap.sh
```

This step is needed because OpenWrt SDK blocks during the build of the packages access to the network. Some of the dependencies are a CMake projects with multiple dependencies which are downloaded during the setup. There is no possibility to get them offline.

#### Step 4: Compile 

##### Build the selected Ayla package by typing the below command from $TOP_DIR of SDK

    make package/ayla-zigbee-sdk/{clean,prepare,compile} V=s

  > Or

##### Run make to build everything selected. It will take a while.

Depending on how many processors your system has, you can speed this up with -j option.
```
$ make -j1
```

#### Step 5 : IPK package

After the compilation is finished, the generated .ipk files are placed in the bin/packages and bin/targets directories 
inside the directory you extracted the SDK into.

Now IPK package is generated in Directory $TOP_DIR/bin/targets/brcm6xxx-tch/GBNT4_502L07p1-glibc/packages
	
```

$ ls $TOP_DIR/bin/targets/brcm6xxx-tch/GBNT4_502L07p1-glibc/packages/ayla-zigbee-sdk_0.1_arm_cortex-a7.ipk
  ayla-zigbee-sdk_0.1_arm_cortex-a7.ipk
```

## Ayla ipk installation

Copy the above generated Ayla IPK into the `/tmp` folder of OWM0131 Device for installing the ayla ipk

```
$:~/Downloads$ scp  <name_of_the_ayla_package>  root@<owm_device_ip_address>:/tmp/
```

Install the Ayla IPK Package by using the below opkg command on OWM0131 Device

```
$ opkg install  /tmp/<name_of_the_ayla_package>
```

If same package is already installed on OWM0131 device, package can be upgraded using below opkg command

```
$ opkg install --force-reinstall /tmp/<name_of_the_ayla_package> 
```

