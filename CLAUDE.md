# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Neural amp modeling firmware for the Daisy Seed (STM32H7, Cortex-M7 at 480 MHz) using libDaisy. It runs five A2-Lite NAM models (Fender Twin65, Vox AC30, Marshall JCM800, Fender Princeton65, Randall Satan 50) at 48 kHz with 48-sample blocks, giving a hard budget of 1000 us per audio callback.

## Project context

This firmware is one part of a larger capstone project: a portable touchscreen practice guitar amp built around a custom carrier board ([seed3-carrier-board](https://github.com/B1narySolutions/seed3-carrier-board)). Documentation lives alongside the work: firmware docs in this repository, hardware docs in the carrier board repository. Check the carrier board repository before a decision that needs hardware context, such as pin assignments, audio I/O routing or power.

## Commands

Run everything from the repository root.

| Command | Purpose |
| --- | --- |
| `bash scripts/install.sh` (or `make install`) | Install the ARM toolchain and dfu-util, and fetch pinned libDaisy and NeuralAmpModelerCore into `libs/` |
| `bash scripts/download_models.sh` (or `make download-models`) | Download the five `.nam` models into Git-ignored `models/local/` (required for the build and most tests) |
| `make` | Build libDaisy (`-Os`) and the firmware (`-O3`), embedding all five models |
| `make test` | Host tests with ASan/UBSan: upstream NAM comparison, processor, cabinet, output controls, audio path, converter |
| `make viz` | Render presentation figures (engine vs upstream accuracy, amp comparisons, the cabinet EQ) to `build/viz/`, creating `.venv/` from `viz/requirements.txt` |
| `make viz-docs` | `make viz`, then copy the README's figures (all but the stats slide) to `docs/images/`. Rerun after changing the engine or `viz/figures.py` |
| `make upload` / `make monitor` | Flash over DFU (the running firmware reboots itself into DFU on `B`) and open the serial monitor; `PORT=/dev/cu.usbmodem…` picks a port |
| `make hwtest` | Flash the test firmware (`src/hwtest/`, built by `make/hwtest.mk` into `build/hwtest-fw/`), measure through a line-out-to-input loopback cable, flash the main firmware back and check it, and write `hwtest/results/<run>/report.md`. `make hwtest-sim` runs the suite against a simulated loopback with no board. See `docs/hardware-tests.md` |
| `make upload PROFILE=1` | Add per-layer DWT cycle counts to the serial output. Run `make clean` when toggling it, because objects don't track flags |
| `make check` | Everything CI runs: both firmware builds, `make check-size`, `make test`, `make format-check`, `make check-patterns` and `make hwtest-sim`. Run it before calling a change done |
| `make check-comment-only` | For a change meant to touch comments only: both firmware images must be byte-identical to `BASE` (default `origin/main`) |
| `make update-engine-digests` | Save the engine's current output as the bit-identical reference, for a change meant to alter it |
| `make format` / `make format-check` | clang-format `src/` and `tests/`, or fail if anything is unformatted |
| `make compiledb` | Generate `compile_commands.json` for clangd |

Running a single host test (these are targets in `tests/Makefile`, which is run from the root):

```sh
make -f tests/Makefile build/tests/nam_processor_test && ./build/tests/nam_processor_test
make -f tests/Makefile test-a2        # A2-Lite vs upstream NAM, per model
python3 -B tests/test_convert_a2.py   # converter only
```

Without the downloaded models, `make test` runs only `nam_processor_test`, `cab_filter_test`, `output_controls_test` and the pattern checker's tests, and says what it skipped.

## Enforced checks

CI (`.github/workflows/ci.yml`) runs `make check` on every push to `main` and every pull request. A rule that a check enforces is listed here; the rest of this file is guidance a check can't cover.

- **Warnings are errors.** Our own C++ builds with `-Wall -Wextra -Werror` (`make/warnings.mk`). The firmware builds add `-Wdouble-promotion -Wfloat-conversion`, so float math that silently becomes double is an error. Fix a warning; don't silence it.
- **Bit-identical engine.** `engine_digest_test` compares every output bit, per amp and through the whole audio path, with `tests/engine_digests.txt`. Run `make update-engine-digests` only for a change meant to alter the output, and say why in the commit.
- **Accuracy.** The engine must stay within `make test` tolerance of upstream NAM on all five amps.
- **No allocation while processing.** The host tests assert it through `tests/allocation_guard.h`, which counts every form of `operator new`; any new processing path gets the same assert. Every `Process` is `noexcept`. On the board, `main.cpp`'s `operator new` counts allocations made during the audio callback, and `make hwtest` fails a run that reports one.
- **Memory budgets.** `make check-size` holds flash and static RAM to `make/size_limits.json`. `a2_test` holds each model's heap to `kModelHeapBudget`. Raise a budget only in the change that needs it, with the reason.
- **Banned patterns.** `make check-patterns` (`scripts/check_patterns.py`) rejects `memcpy`/`memset`/`std::copy`/`std::fill`, libm calls and heap allocation in `src/audio/` and `src/models/`, and relaxed float flags in the build files. A deliberate use carries a `// check-patterns: allow <rule>: <reason>` marker.
- **Formatting.** Every C/C++ file under `src/` and `tests/` must be clang-formatted, with the version CI pins (`CLANG_FORMAT_VERSION` in the `Makefile`).
- **Comment-only changes.** On a pull request labeled `comment-only`, CI also runs `make check-comment-only`.
- **Merging.** `main` requires CI's `check` job to pass, admins included.

When a review keeps catching the same mistake, prefer adding a check here over adding a rule below.

## Architecture

Signal path, top to bottom:

- `src/main.cpp`: hardware init (including the ADC for the carrier board's volume and tone pots on `A0`/`A1`), the audio callback (passes on the knob readings, times every block and counts overruns), a USB receive interrupt that only records a key into atomics, and a main loop that applies model switches, prints a status line once a second and handles the DFU reboot. Model switches stop audio, load, then restart it. Nothing allocates in the interrupt or the audio callback; the file's `operator new` counts any allocation during the callback, and the status report prints an `ERROR:` line for it.
- `src/audio/nam_audio.*`: `NamAudio` takes the left input to both outputs, sanitizes NaN/inf, scales the model's input and output by the amp's trims, applies output gain and clamp, and handles bypass (which skips the model and its trims entirely). `LoadAmpModel` frees the old model before building the next, because the delay lines take much of SRAM. A failed load leaves audio in bypass.
- `src/audio/cab_filter.*`: `CabFilter`, a fixed six-biquad speaker EQ that `NamAudio` runs on the model's output (never in bypass). Each amp load turns it on, or off for an amp whose `includes_cabinet` is set in `models/amps.json`, and `C` toggles it. Its coefficients are designed at compile time with constexpr Taylor series (`constexpr_math.h`) so the firmware doesn't link libm's `sin`/`cos`/`pow` (about 13 KB of flash).
- `src/audio/output_controls.*`: `OutputControls`, the volume and tone knobs, which `NamAudio` applies to its final output (in bypass too). Volume is a cubic audio taper; tone is two one-pole low-passes whose coefficient is interpolated from a compile-time table that ends in an exact pass-through. Knob moves glide with a 20 ms time constant.
- `src/audio/constexpr_math.h`: the compile-time Taylor series both filters use for their designs.
- `src/audio/nam_processor.*`: `NamProcessor` owns one `AmpModel`, validates the sample rate and block size, and commits a new model only after `Reset` succeeds.
- `src/models/amp_model.h`: the `AmpModel` interface. `Reset` may allocate and throw; `Process` is `noexcept` and must never allocate.
- `src/models/a2_lite.*`: the hand-written, fixed-shape A2-Lite WaveNet, the performance-critical code.
- `src/models/amp_models.*`: `AmpId` and the factory over the embedded weights.

### Model data pipeline

`models/amps.json` is the single source of the amp list and its order (it must match `AmpId`, starting at 1), and of each amp's `includes_cabinet` and input and output trims in dB. The converter turns the trims into linear gains; `models/README.md` explains how they were measured. `scripts/convert_a2.py` validates each `.nam` against the fixed A2-Lite shape and writes `build/generated/embedded_a2_data.h`. That header static_asserts the shape in `src/models/a2_lite.h`, and `amp_models.cpp` static_asserts the count and order. The firmware never parses configuration at runtime. To change the architecture, update `a2_lite.h`, `convert_a2.py` and the weight-stream layout together.

The model weights are T3K-licensed: never commit `models/local/` or firmware binaries containing them.

### A2-Lite engine

- It processes layer-major: a whole block goes through layer 0, then layer 1, and so on. Each layer's delay line holds only that layer's input. Sample-major processing was over budget.
- Delay lines are rings with their first frames mirrored past the end, so tap windows are contiguous pointers with no wrap checks.
- `AccumulateTaps` is the hot loop (about half the cycles). It keeps tap weights in FPU registers and works two taps per pass.
- `Prewarm` computes each delay line's silent steady state directly instead of running silence through the network.
- The firmware links neither NAM Core nor Eigen; NAM Core is only the test reference.

### Performance rules

`docs/performance.md` is the ledger of measured speedups and rejected experiments. Read it before optimizing, and add a row for any change you measure. Key constraints:

- Bit-exactness, accuracy, the `memcpy` family and relaxed float flags are enforced (see Enforced checks). The `memcpy` rule exists because newlib-nano's `memcpy`/`memset` copy one byte at a time, which is also why `a2_lite.o` is built with `-fno-tree-loop-distribute-patterns`. Before allowing one, check the object file for a library call.
- The digests describe the host build, which turns off multiply-add fusion; the firmware's compiler fuses them, so its bits differ from the host's. Timing still needs the board.
- Timing differences of a few microseconds can come from code placement in flash. Confirm with a non-profiling build on hardware; only the serial status line on a real Seed counts as a measurement.

## Code comments

This code is presented and open-sourced, so comments are for a reader seeing it cold. Follow the existing style:

- Split a function into its steps with blank lines, and give each step a one-line comment saying what it does. Aim for roughly one comment line per three or four lines of code, not a comment on every line.
- Keep comments that explain why (a measured trade-off, a hardware constraint, an ownership rule). Don't delete existing ones when adding step labels.
- Avoid trailing comments that restate the code (`kSampleRate = 48000.0; // Hz.`) and comments that only repeat a function's name.
- Be careful with precise behavioral claims ("only applied when…", "nothing is copied", "true once…"). These are where comments have gone wrong here. Check each against the code, including bypass and failure paths.
- Comment-only changes must not change code. Run `make check-comment-only` (or label the pull request `comment-only`) and `make check`.
