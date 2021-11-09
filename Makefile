# Makefile
PROGRAM = boiler

EXTRA_COMPONENTS = extras/dhcpserver
EXTRA_COMPONENTS = extras/sntp
#EXTRA_COMPONENTS = extras/onewire extras/ds18b20
EXTRA_COMPONENTS = extras/rboot-ota

#EXTRA_COMPONENTS =../../esp/esp-open-rtos/extras/rboot-ota extras/mbedtls
#EXTRA_COMPONENTS =../../esp/esp-open-rtos/extras/mbedtls


include ../../esp/esp-open-rtos/common.mk
