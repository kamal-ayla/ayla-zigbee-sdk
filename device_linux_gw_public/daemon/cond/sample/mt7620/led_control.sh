#!/bin/sh

mode=$1
color=$2
LED_PIN_GREEN=22
LED_PIN_RED=23
LED_PIN_BLUE=33
bin=usrGpioSetLow

run_cmd() {
	$@ >/dev/null 2>&1
}

run_cmd_background() {
	$@ >/dev/null 2>&1 &
}

case $mode in
	on)
		bin=usrGpioSetHigh
	;;
	off)
		# Nothing to do
	;;
	flash|flash-slow)
		bin=usrGpioSlowCycle
	;;
	flash-fast)
		bin=usrGpioFastCycle
	;;
	*)	
		if [ $# -ne 1 ]; then
			echo "invalid mode: $mode"
			exit 1
		fi
		# Single arg is not mode, so assume 'on'
		bin=usrGpioSetHigh
		color=$mode
	;;
esac

# Disable everything
run_cmd killall usrGpioSlowCycle
run_cmd killall usrGpioFastCycle
run_cmd usrGpioSetLow $LED_PIN_GREEN
run_cmd usrGpioSetLow $LED_PIN_RED
run_cmd usrGpioSetLow $LED_PIN_BLUE

[ $mode == off ] && exit 0

case $color in
	green)
		run_cmd_background $bin $LED_PIN_GREEN
	;;
	red)
		run_cmd_background $bin $LED_PIN_RED
	;;
	blue)
		run_cmd_background $bin $LED_PIN_BLUE
	;;
	white)
		# White
		run_cmd_background $bin $LED_PIN_GREEN
		run_cmd_background $bin $LED_PIN_RED
		run_cmd_background $bin $LED_PIN_BLUE
		;;
	*)
		echo "invalid color: $color"
		exit 1
	;;
esac

exit 0
