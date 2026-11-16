# Hardware test firmware (src/hwtest/): plays and records over USB for the
# host suite in hwtest/. It links no models, so it needs no downloads, and
# builds into its own directory so it never mixes objects with the main
# firmware.
TARGET = hwtest
CPP_SOURCES = src/hwtest/main.cpp
C_INCLUDES = -Isrc -I$(LIBDAISY_DIR)/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
CPP_STANDARD = -std=gnu++20
OPT = -O2
LIBDAISY_DIR = libs/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
APP_TYPE = BOOT_NONE
# Set before libDaisy's Makefile, whose plain BUILD_DIR = build an override
# beats; its rules expand the directory as they are read.
override BUILD_DIR := build/hwtest-fw
include $(SYSTEM_FILES_DIR)/Makefile

include make/flash_dfu.mk

$(OBJECTS): make/hwtest.mk
$(BUILD_DIR)/$(TARGET).elf: make/hwtest.mk $(LIBDAISY_DIR)/build/libdaisy.a
