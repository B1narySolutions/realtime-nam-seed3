.DEFAULT_GOAL := build
JOBS ?= 4

.PHONY: install download-models build model upload program-dfu monitor format format-check check compiledb test viz viz-docs hwtest hwtest-build hwtest-upload hwtest-sim clean help
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

# Hardware tests (docs/hardware-tests.md). make hwtest flashes the test
# firmware, measures through a loopback cable, flashes the main firmware back
# and checks it, then writes hwtest/results/<time>/report.md. LABEL=... names
# the run; NOTES=... is saved with it.
HWTEST_VENV_STAMP = .venv/.hwtest-requirements-installed
$(HWTEST_VENV_STAMP): hwtest/requirements.txt
	python3 -m venv .venv
	.venv/bin/python -m pip install --quiet -r hwtest/requirements.txt
	@touch $@

hwtest-build:
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	$(MAKE) -C libs/libDaisy -j$(JOBS) OPT=-Os
	$(MAKE) -f make/hwtest.mk all

hwtest-upload: hwtest-build
	@command -v dfu-util >/dev/null || { echo 'Run make install first.'; exit 1; }
	@bash scripts/enter_dfu.sh "$(PORT)"
	$(MAKE) -f make/hwtest.mk flash-dfu

hwtest: $(HWTEST_VENV_STAMP)
	.venv/bin/python -B hwtest/run.py $(HWTEST_ARGS)

# The whole suite against a simulated loopback, with no board: checks the
# analysis finds the faults the simulation puts in.
hwtest-sim: $(HWTEST_VENV_STAMP)
	.venv/bin/python -B hwtest/test_hwtest.py

model:
	$(MAKE) -f make/firmware.mk build/generated/embedded_a2_data.h

compiledb: model
	@command -v compiledb >/dev/null || { echo 'Missing compiledb; install it with brew install compiledb or pip install compiledb.'; exit 1; }
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	compiledb -n -f make -B -f make/firmware.mk compile-objects GCC_PATH="$$(dirname "$$(command -v arm-none-eabi-g++)")"

FORMAT_SOURCES = find src tests -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \)
# CI installs this clang-format from PyPI. Other versions can wrap lines
# differently, so format-check says when the local one differs.
CLANG_FORMAT_VERSION = 23.1.3

format:
	@command -v clang-format >/dev/null || { echo 'Missing clang-format; install it with brew install clang-format or your package manager.'; exit 1; }
	$(FORMAT_SOURCES) -exec clang-format -i --style=file {} +

format-check:
	@command -v clang-format >/dev/null || { echo 'Missing clang-format; install it with brew install clang-format or your package manager.'; exit 1; }
	@clang-format --version | grep -q ' $(CLANG_FORMAT_VERSION)' || echo 'Note: CI uses clang-format $(CLANG_FORMAT_VERSION); yours may format differently.' >&2
	$(FORMAT_SOURCES) -exec clang-format --dry-run -Werror --style=file {} +

# Everything CI runs, in the same order (.github/workflows/ci.yml). Needs
# make install and make download-models first.
check: build hwtest-build test format-check hwtest-sim

clean:
	rm -rf build
	@if test -f libs/libDaisy/Makefile; then $(MAKE) -C libs/libDaisy clean; fi

help:
	@echo 'make install  Install tools with Homebrew, apt or pacman and fetch pinned dependencies.'
	@echo 'make download-models Download the five amp models to models/local/.'
	@echo 'make build    Build libDaisy and five-amp A2-Lite firmware.'
	@echo 'make model    Convert the downloaded A2-Lite models to embedded float data.'
	@echo 'make test     Run upstream A2 comparisons and sanitized host audio tests.'
	@echo 'make viz      Render presentation figures of the amp models to build/viz/.'
	@echo 'make viz-docs Render the figures and copy the README's to docs/images/.'
	@echo 'make upload   Build and flash via USB; the running firmware enters DFU itself (else BOOT + RESET).'
	@echo 'make hwtest   Flash the test firmware, measure SNR, noise, distortion, response and latency through'
	@echo '              a loopback cable, recheck the main firmware, and write hwtest/results/<time>/report.md.'
	@echo 'make hwtest-sim Run the hardware suite against a simulated loopback (no board).'
	@echo 'make monitor  Open USB serial in screen (optional PORT=/dev/cu.usbmodem... or /dev/ttyACM...).'
	@echo 'make check    Run everything CI runs: both firmware builds, the host tests, the format check and hwtest-sim.'
	@echo 'make format   Format C/C++ files under src/ and tests/ using .clang-format.'
	@echo 'make format-check Fail if any C/C++ file under src/ or tests/ is not formatted.'
	@echo 'make compiledb Generate compile_commands.json for clangd without building.'
	@echo 'make clean    Remove firmware and libDaisy build outputs.'
