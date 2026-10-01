# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Neural amp modeling firmware for the Daisy Seed (STM32H7, Cortex-M7 at 480 MHz) using libDaisy. It runs three A2-Lite NAM models (Fender Twin65, Vox AC30, Marshall JCM800) at 48 kHz with 48-sample blocks, giving a hard budget of 1000 us per audio callback.

## Commands

Run everything from the repository root.

| Command | Purpose |
| --- | --- |
| `bash scripts/install.sh` (or `make install`) | Install the ARM toolchain and dfu-util, and fetch pinned libDaisy and NeuralAmpModelerCore into `libs/` |
| `bash scripts/download_models.sh` (or `make download-models`) | Download the three `.nam` models into Git-ignored `models/local/` (required for the build and most tests) |
| `make` | Build libDaisy (`-Os`) and the firmware (`-O3`), embedding all three models |
| `make test` | Host tests with ASan/UBSan: upstream NAM comparison, processor, cabinet, audio path, converter |
| `make viz` | Render presentation figures (engine vs upstream accuracy, amp comparisons) to `build/viz/`, creating `.venv/` from `viz/requirements.txt` |
| `make viz-docs` | `make viz`, then copy the README's figures (all but the stats slide) to `docs/images/`. Rerun after changing the engine or `viz/figures.py` |
| `make upload` / `make monitor` | Flash over DFU (the running firmware reboots itself into DFU on `B`) and open the serial monitor; `PORT=/dev/cu.usbmodem…` picks a port |
| `make upload PROFILE=1` | Add per-layer DWT cycle counts to the serial output. Run `make clean` when toggling it, because objects don't track flags |
| `make format` | clang-format `src/` and `tests/` |
| `make compiledb` | Generate `compile_commands.json` for clangd |

Running a single host test (these are targets in `tests/Makefile`, which is run from the root):

```sh
make -f tests/Makefile build/tests/nam_processor_test && ./build/tests/nam_processor_test
make -f tests/Makefile test-a2        # A2-Lite vs upstream NAM, per model
python3 -B tests/test_convert_a2.py   # converter only
```

Without the downloaded models, `make test` runs only `nam_processor_test` and `cab_filter_test` and says what it skipped.

## Architecture

Signal path, top to bottom:

- `src/main.cpp`: hardware init, the audio callback (times every block and counts overruns), a USB receive interrupt that only records a key into atomics, and a main loop that applies model switches, prints a status line once a second and handles the DFU reboot. Model switches stop audio, load, then restart it. Nothing allocates in the interrupt or the audio callback.
- `src/audio/nam_audio.*`: `NamAudio` takes the left input to both outputs, sanitizes NaN/inf, applies gain and clamp, and handles bypass (which skips the model entirely). `LoadAmpModel` frees the old model before building the next, because the delay lines take much of SRAM. A failed load leaves audio in bypass.
- `src/audio/cab_filter.*`: `CabFilter`, a fixed six-biquad speaker EQ that `NamAudio` runs on the model's output (never in bypass), toggled with `C`. Its coefficients are designed at compile time with constexpr Taylor series so the firmware doesn't link libm's `sin`/`cos`/`pow` (about 13 KB of flash).
- `src/audio/nam_processor.*`: `NamProcessor` owns one `AmpModel`, validates the sample rate and block size, and commits a new model only after `Reset` succeeds.
- `src/models/amp_model.h`: the `AmpModel` interface. `Reset` may allocate and throw; `Process` must never allocate.
- `src/models/a2_lite.*`: the hand-written, fixed-shape A2-Lite WaveNet, the performance-critical code.
- `src/models/amp_models.*`: `AmpId` and the factory over the embedded weights.

### Model data pipeline

`models/amps.json` is the single source of the amp list and its order (it must match `AmpId`, starting at 1). `scripts/convert_a2.py` validates each `.nam` against the fixed A2-Lite shape and writes `build/generated/embedded_a2_data.h`. That header static_asserts the shape in `src/models/a2_lite.h`, and `amp_models.cpp` static_asserts the count and order. The firmware never parses configuration at runtime. To change the architecture, update `a2_lite.h`, `convert_a2.py` and the weight-stream layout together.

The model weights are T3K-licensed: never commit `models/local/` or firmware binaries containing them.

### A2-Lite engine

- It processes layer-major: a whole block goes through layer 0, then layer 1, and so on. Each layer's delay line holds only that layer's input. Sample-major processing was over budget.
- Delay lines are rings with their first frames mirrored past the end, so tap windows are contiguous pointers with no wrap checks.
- `AccumulateTaps` is the hot loop (about half the cycles). It keeps tap weights in FPU registers and works two taps per pass.
- `Prewarm` computes each delay line's silent steady state directly instead of running silence through the network.
- The firmware links neither NAM Core nor Eigen; NAM Core is only the test reference.

### Performance rules

`docs/performance.md` is the ledger of measured speedups and rejected experiments. Read it before optimizing, and add a row for any change you measure. Key constraints:

- Changes to the engine are expected to be bit-identical to the previous engine and to stay within `make test` tolerance against upstream NAM.
- `a2_lite.o` is built with `-fno-tree-loop-distribute-patterns` because newlib-nano's `memcpy`/`memset` copy one byte at a time. Don't reintroduce `std::copy`/`std::fill`/`memcpy` in hot paths without measuring.
- Keep IEEE float behavior (no `-ffast-math`); relaxed flags are an unmeasured idea that gives up bit-exactness.
- Timing differences of a few microseconds can come from code placement in flash. Confirm with a non-profiling build on hardware; only the serial status line on a real Seed counts as a measurement.

## Code comments

This code is presented and open-sourced, so comments are for a reader seeing it cold. Follow the existing style:

- Split a function into its steps with blank lines, and give each step a one-line comment saying what it does. Aim for roughly one comment line per three or four lines of code, not a comment on every line.
- Keep comments that explain why (a measured trade-off, a hardware constraint, an ownership rule). Don't delete existing ones when adding step labels.
- Avoid trailing comments that restate the code (`kSampleRate = 48000.0; // Hz.`) and comments that only repeat a function's name.
- Be careful with precise behavioral claims ("only applied when…", "nothing is copied", "true once…"). These are where comments have gone wrong here. Check each against the code, including bypass and failure paths.
- Comment-only changes must not change code. Verify that `git diff` has only comment and blank lines, that `clang-format` is clean, and that `make test` passes.
