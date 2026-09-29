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
# make upload PROFILE=1 adds A2-Lite per-layer cycle counters, printed every 5 s.
# Objects don't track flags; run make clean when toggling it.
ifdef PROFILE
CPPFLAGS += -DA2_LITE_PROFILE
endif
include make/models.mk

# Used in dry-run mode to generate clangd commands without requiring a link.
.PHONY: compile-objects
compile-objects: $(OBJECTS)

$(OBJECTS): make/firmware.mk
$(BUILD_DIR)/amp_models.o: $(A2_HEADER)
$(BUILD_DIR)/$(TARGET).elf: make/firmware.mk $(LIBDAISY_DIR)/build/libdaisy.a
