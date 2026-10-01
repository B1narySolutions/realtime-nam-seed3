.DEFAULT_GOAL := build
JOBS ?= 4

.PHONY: install download-models build model upload program-dfu monitor format compiledb test viz viz-docs clean help
install:
	bash scripts/install.sh

download-models:
	bash scripts/download_models.sh

build:
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	$(MAKE) -C libs/libDaisy -j$(JOBS) OPT=-Os
	$(MAKE) -f make/firmware.mk all

upload: build
	@command -v dfu-util >/dev/null || { echo 'Run make install first.'; exit 1; }
	@bash scripts/enter_dfu.sh "$(PORT)"
	$(MAKE) -f make/firmware.mk flash-dfu

program-dfu: upload

monitor:
	@bash scripts/monitor.sh "$(PORT)"

test:
	@test -f libs/NeuralAmpModelerCore/Dependencies/eigen/Eigen/Core || { echo 'Missing NAM Core (the test reference); run make install.'; exit 1; }
	$(MAKE) -f tests/Makefile test

viz:
	@test -f libs/NeuralAmpModelerCore/Dependencies/eigen/Eigen/Core || { echo 'Missing NAM Core (the accuracy reference); run make install.'; exit 1; }
	$(MAKE) -f viz/viz.mk viz

# Refresh the README's figures from a fresh render. The stats slide stays in
# build/viz/ only: its speedup is measured against our own first engine,
# which means little outside the project's presentations.
README_FIGURES = accuracy accuracy_over_time amp_frequency_response amp_gain_curve amp_drive_waveforms amp_harmonics cab_response cab_amp_response cab_spectrum cab_waveform
viz-docs: viz
	@mkdir -p docs/images
	cp $(addprefix build/viz/,$(addsuffix .png,$(README_FIGURES))) docs/images/

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
	@echo 'make download-models Download the three amp models to models/local/.'
	@echo 'make build    Build libDaisy and three-amp A2-Lite firmware.'
	@echo 'make model    Convert the downloaded A2-Lite models to embedded float data.'
	@echo 'make test     Run upstream A2 comparisons and sanitized host audio tests.'
	@echo 'make viz      Render presentation figures of the amp models to build/viz/.'
	@echo 'make viz-docs Render the figures and copy the README's to docs/images/.'
	@echo 'make upload   Build and flash via USB; the running firmware enters DFU itself (else BOOT + RESET).'
	@echo 'make monitor  Open USB serial in screen (optional PORT=/dev/cu.usbmodem...).'
	@echo 'make format   Format C/C++ files under src/ using .clang-format.'
	@echo 'make compiledb Generate compile_commands.json for clangd without building.'
	@echo 'make clean    Remove firmware and libDaisy build outputs.'
