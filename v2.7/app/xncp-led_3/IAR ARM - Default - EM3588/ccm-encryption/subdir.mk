################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7/protocol/zigbee/stack/framework/ccm-star.c 

OBJS += \
./ccm-encryption/ccm-star.o 

C_DEPS += \
./ccm-encryption/ccm-star.d 


# Each subdirectory must supply rules for building sources it contributes
ccm-encryption/ccm-star.o: D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7/protocol/zigbee/stack/framework/ccm-star.c
	@echo 'Building file: $<'
	@echo 'Invoking: IAR C/C++ Compiler for ARM'
	iccarm "$<" -o "$@" --no_path_in_file_macros --separate_cluster_for_initialized_variables -I"C:\Users\robbin\SimplicityStudio\v4_workspace\xncp-led_3" -I"C:\Users\robbin\SimplicityStudio\v4_workspace\xncp-led_3\external-generated-files" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7/" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base/hal" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base/hal//plugin" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base//../CMSIS/Include" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base//../radio/rail_lib/plugin" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base/hal/micro/cortexm3" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//protocol/zigbee" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//protocol/zigbee/stack" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//protocol/zigbee/app/util" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/Device/SiliconLabs/em358x/Include" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/base/hal/micro/cortexm3/em35x/board" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/radio/rail_lib/plugin/coexistence/common" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/radio/rail_lib/plugin/coexistence/hal/em3xx" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//platform/radio/rail_lib/plugin/coexistence/protocol/ieee802154" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//util/third_party/segger/systemview/Config" -I"D:/SiliconLabs/v4/developer/sdks/gecko_sdk_suite/v2.7//util/third_party/segger/systemview/SEGGER" -e --use_c++_inline --cpu Cortex-M3 --fpu None --debug --dlib_config "D:/IARSystems/EmbeddedWorkbench7.80/arm/inc/c/DLib_Config_Normal.h" --endian little --cpu_mode thumb -Ohz --no_clustering '-DCORTEXM3=1' '-DCORTEXM3_EMBER_MICRO=1' '-DPHY_EM3XX=1' '-DSERIAL_UART_BTL=1' '-DCONFIGURATION_HEADER="ncp-configuration.h"' '-DCORTEXM3_EM3588=1' '-DCORTEXM3_EM3588_MICRO=1' '-DPLATFORM_HEADER="platform/base/hal/micro/cortexm3/compiler/iar.h"' '-DLOCKBITS_IN_MAINFLASH_SIZE=0' '-DPSSTORE_SIZE=0' '-DLONGTOKEN_SIZE=0' '-DBOARD_DEV0680UART=1' '-DBOARD_HEADER="ncp-board.h"' '-DUSE_SIMEE2=1' '-DEZSP_UART=1' '-DNO_USB=1' '-DEMLIB_USER_CONFIG=1' '-DEMBER_AF_NCP=1' '-DEMBER_STACK_ZIGBEE=1' '-DEMBER_AF_API_NEIGHBOR_HEADER="stack/include/stack-info.h"' '-DEMBER_SERIAL1_MODE=EMBER_SERIAL_BUFFER' '-DEMBER_SERIAL1_TX_QUEUE_SIZE=2' '-DEMBER_SERIAL1_RX_QUEUE_SIZE=64' '-DEMBER_SERIAL1_RTSCTS=1' '-DEMBER_SERIAL0_MODE=EMBER_SERIAL_FIFO' '-DEMBER_SERIAL0_TX_QUEUE_SIZE=64' '-DEMBER_SERIAL0_RX_QUEUE_SIZE=64' '-DAPPLICATION_TOKEN_HEADER="ncp-token.h"' '-DAPPLICATION_MFG_TOKEN_HEADER="ncp-mfg-token.h"' --diag_suppress Pa050 --dependencies=m ccm-encryption/ccm-star.d
	@echo 'Finished building: $<'
	@echo ' '


