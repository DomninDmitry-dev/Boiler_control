# Makefile
PROGRAM = boiler

EXTRA_COMPONENTS = extras/dhcpserver
EXTRA_COMPONENTS = extras/sntp
EXTRA_COMPONENTS = extras/rboot-ota

include ../../esp/esp-open-rtos/common.mk
