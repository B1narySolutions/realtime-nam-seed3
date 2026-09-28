.DEFAULT_GOAL := build
JOBS ?= 4

.PHONY: install build model upload program-dfu monitor format compiledb test clean help
install:
	bash scripts/install.sh

build:
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	$(MAKE) -C libs/libDaisy -j$(JOBS) OPT=-Os
	$(MAKE) -f make/firmware.mk all

upload: build
	@command -v dfu-util >/dev/null || { echo 'Run make install first.'; exit 1; }
	$(MAKE) -f make/firmware.mk program-dfu

program-dfu: upload

monitor:
	@bash scripts/monitor.sh "$(PORT)"

test:
	@test -f libs/NeuralAmpModelerCore/Dependencies/eigen/Eigen/Core || { echo 'Missing NAM Core (the test reference); run make install.'; exit 1; }
	$(MAKE) -f tests/Makefile test

model:
	$(MAKE) -f make/firmware.mk build/generated/embedded_a2_data.h

compiledb: model
	@command -v compiledb >/dev/null || { echo 'Missing compiledb; install it with brew install compiledb.'; exit 1; }
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	compiledb -n -f make -B -f make/firmware.mk compile-objects GCC_PATH="$$(dirname "$$(command -v arm-none-eabi-g++)")"

format:
	@command -v clang-format >/dev/null || { echo 'Missing clang-format; install it with brew install clang-format.'; exit 1; }
	find src tests -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -exec clang-format -i --style=file {} +

clean:
	rm -rf build
	@if test -f libs/libDaisy/Makefile; then $(MAKE) -C libs/libDaisy clean; fi

help:
	@echo 'make install  Install tools with Homebrew or apt and fetch pinned dependencies.'
	@echo 'make build    Build libDaisy and three-amp A2-Lite firmware.'
	@echo 'make model    Convert the downloaded A2-Lite models to embedded float data.'
	@echo 'make test     Run upstream A2 comparisons and sanitized host audio tests.'
	@echo 'make upload   Build and flash via USB; enter BOOT + RESET mode first.'
	@echo 'make monitor  Open USB serial in screen (optional PORT=/dev/cu.usbmodem...).'
	@echo 'make format   Format C/C++ files under src/ using .clang-format.'
	@echo 'make compiledb Generate compile_commands.json for clangd without building.'
	@echo 'make clean    Remove firmware and libDaisy build outputs.'
