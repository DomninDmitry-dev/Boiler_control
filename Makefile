# Makefile
PROGRAM=Boiler_control
EXTRA_COMPONENTS=../../esp/esp-open-rtos/extras/dhcpserver
EXTRA_COMPONENTS=../../esp/esp-open-rtos/extras/sntp
EXTRA_COMPONENTS = ../../esp/esp-open-rtos/extras/onewire extras/ds18b20

include ../../esp/esp-open-rtos/common.mk
