TARGET = passthrough
CPP_SOURCES = src/main.cpp src/audio/nam_processor.cpp src/audio/nam_audio.cpp src/models/amp_models.cpp src/models/a2_lite.cpp
C_INCLUDES = -Isrc -Ibuild/generated
CPP_STANDARD = -std=gnu++17
# Optimize application and model code for speed; libDaisy builds with -Os.
OPT = -O3
LIBDAISY_DIR = libs/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
APP_TYPE = BOOT_NONE
include $(SYSTEM_FILES_DIR)/Makefile

# Model buffers are heap-allocated; LoadAmpModel catches allocation failure
# so audio still starts in bypass. Keep IEEE floating-point behavior until
# model accuracy and target performance have been measured.
CPPFLAGS += -fexceptions
# nano.specs memcpy and memset copy one byte per iteration. Keep GCC from
# turning A2-Lite's per-call weight copies and per-block fills into calls
# to them; plain loops compile to word loads and stores.
$(BUILD_DIR)/a2_lite.o: CPPFLAGS += -fno-tree-loop-distribute-patterns

# make upload PROFILE=1 adds A2-Lite per-layer cycle counters, printed every 5 s.
# Objects don't track flags; run make clean when toggling it.
ifdef PROFILE
CPPFLAGS += -DA2_LITE_PROFILE
endif
include make/models.mk

# libDaisy's program-dfu, but dfu-util exits 74 after every good flash: the
# STM32 resets on :leave before answering the final status request. Judge
# success by the download completing instead.
.PHONY: flash-dfu
flash-dfu:
	@dfu-util -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID) 2>&1 | tee $(BUILD_DIR)/dfu.log; \
		grep -q 'File downloaded successfully' $(BUILD_DIR)/dfu.log || { echo 'DFU flash failed.' >&2; exit 1; }

# Used in dry-run mode to generate clangd commands without requiring a link.
.PHONY: compile-objects
compile-objects: $(OBJECTS)

$(OBJECTS): make/firmware.mk
$(BUILD_DIR)/amp_models.o: $(A2_HEADER)
$(BUILD_DIR)/$(TARGET).elf: make/firmware.mk $(LIBDAISY_DIR)/build/libdaisy.a
