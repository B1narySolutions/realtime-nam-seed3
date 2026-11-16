# libDaisy's program-dfu, but dfu-util exits 74 after every good flash: the
# STM32 resets on :leave before answering the final status request. Judge
# success by the download completing instead.
.PHONY: flash-dfu
flash-dfu:
	@dfu-util -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID) 2>&1 | tee $(BUILD_DIR)/dfu.log; \
		grep -q 'File downloaded successfully' $(BUILD_DIR)/dfu.log || { echo 'DFU flash failed.' >&2; exit 1; }
