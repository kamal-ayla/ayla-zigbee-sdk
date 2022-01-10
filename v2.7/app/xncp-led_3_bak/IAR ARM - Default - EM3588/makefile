################################################################################
# Automatically-generated file. Do not edit!
################################################################################

export WINEDEBUG=-all

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include strong-random/subdir.mk
-include sim-eeprom2/EM3588/subdir.mk
-include serial/EM3588/subdir.mk
-include secure-ezsp-stub/subdir.mk
-include ncp-uart/subdir.mk
-include led/EM3588/subdir.mk
-include hal-library/EM3588/iar/subdir.mk
-include hal-library/EM3588/subdir.mk
-include external-generated-files/subdir.mk
-include ext-device/EM3588/subdir.mk
-include ember-minimal-printf/subdir.mk
-include diagnostic-stub/subdir.mk
-include debug-channel/EM3588/subdir.mk
-include coexistence-stub/EM3588/subdir.mk
-include ccm-encryption/subdir.mk
-include button-stub/subdir.mk
-include antenna-stub/subdir.mk
-include adc/EM3588/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(ASM_UPPER_DEPS)),)
-include $(ASM_UPPER_DEPS)
endif
ifneq ($(strip $(MSA_UPPER_DEPS)),)
-include $(MSA_UPPER_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(S79_DEPS)),)
-include $(S79_DEPS)
endif
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(MSA_DEPS)),)
-include $(MSA_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
ifneq ($(strip $(S79_UPPER_DEPS)),)
-include $(S79_UPPER_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: pre-build main-build

# Main-build Target
main-build: xncp-led_3.out

# Tool invocations
xncp-led_3.out: $(OBJS) $(USER_OBJS)
	@echo 'Building target: $@'
	@echo 'Invoking: IAR Linker for ARM'
	@echo 'Adding linker inputs to ilinkCommandLine_xncp-led_3.out.xcl'
	echo. $(foreach file,$(OBJS), $(file)) >> ilinkCommandLine_xncp-led_3.out.xcl
	echo. $(foreach file,$(USER_OBJS), $(file)) >> ilinkCommandLine_xncp-led_3.out.xcl
	@echo 'Invoking linker reading args from file: ilinkCommandLine_xncp-led_3.out.xcl'
	ilinkarm -f "C:\Users\robbin\SimplicityStudio\v4_workspace\xncp-led_3\IAR ARM - Default - EM3588\ilinkCommandLine_xncp-led_3.out.xcl" 
	ielftool --srec --verbose "xncp-led_3.out" "xncp-led_3.s37"  
	ielftool --ihex --verbose "xncp-led_3.out" "xncp-led_3.hex"  
	ielftool --bin --verbose "xncp-led_3.out" "xncp-led_3.bin"
	@echo 'Finished building target: $@'
	@echo ' '
	$(MAKE) --no-print-directory post-build

# Other Targets
clean:
	-$(RM) $(ASM_UPPER_DEPS)$(MSA_UPPER_DEPS)$(EXECUTABLES)$(OBJS)$(ASM_DEPS)$(S79_DEPS)$(S_DEPS)$(S_UPPER_DEPS)$(MSA_DEPS)$(C_DEPS)$(S79_UPPER_DEPS) xncp-led_3.out
	-@echo ' '

pre-build:
	-cmd //c 'C:\Users\robbin\SIMPLI~1\V4_WOR~1\XNCP-L~3/xncp-led_3-prebuild.bat' 'xncp-led_3' 'C:/Users/robbin/SimplicityStudio/v4_workspace/xncp-led_3' 'D:/IARSystems/EmbeddedWorkbench7.80/arm' 'D:\SiliconLabs\v4'
	-@echo ' '

post-build:
	-cmd //c 'C:\Users\robbin\SIMPLI~1\V4_WOR~1\XNCP-L~3/xncp-led_3-postbuild.bat' 'xncp-led_3' 'C:/Users/robbin/SimplicityStudio/v4_workspace/xncp-led_3' 'D:/IARSystems/EmbeddedWorkbench7.80/arm' 'D:\SiliconLabs\v4'
	-@echo ' '

.PHONY: all clean dependents
.SECONDARY: main-build pre-build post-build

-include ../makefile.targets
