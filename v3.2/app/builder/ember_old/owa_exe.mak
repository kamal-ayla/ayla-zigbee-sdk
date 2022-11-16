
#GENERATE_LIBRARY = 1
export STAGING_DIR = /home/rajandi/homeware_sdk/openwrt-sdk-brcm6xxx-tch-GCNTA_502L07p1_gcc-5.5.0_glibc_eabi.Linux-x86_64/staging_dir
TOOLROOT = $(STAGING_DIR)/toolchain-arm_cortex-a7_gcc-5.5.0_glibc_eabi
COMPILER = $(TOOLROOT)/bin/arm-openwrt-linux-gcc
LINKER   = $(TOOLROOT)/bin/arm-openwrt-linux-gcc
ARCHIVE  = $(TOOLROOT)/bin/arm-openwrt-linux-ar
COMPILER_INCLUDES = -I $(STAGING_DIR)/target-arm_cortex-a7_glibc_eabi/usr/include
LINKER_FLAGS= -L $(STAGING_DIR)/target-arm_cortex-a7_glibc_eabi/usr/lib
#NO_READLINE=1
