# Performance

A ledger of A2-Lite inference speedups on the Daisy Seed, including the
experiments that were measured and rejected.

## How it is measured

All numbers are the audio callback time for one 48-sample block at 48 kHz,
averaged over one second, as printed on the serial status line. The budget
is 1000 us per block. The Seed runs at 480 MHz unless noted. The three amp
models share one shape and measure the same.

`make upload PROFILE=1` adds a per-layer cycle table every 5 s; see
`A2_LITE_PROFILE` in [`src/models/a2_lite.cpp`](../src/models/a2_lite.cpp). Timing differences of a few
microseconds can come from where code lands in flash, so confirm each
change with a normal (non-profiling) build.

Upstream NeuralAmpModelerCore's generic WaveNet has never been measured on
the Seed. The earliest baseline is this repository's first A2-Lite engine,
which already implemented upstream's equations by hand.

## Ledger

| # | Change | Commit | Avg callback | Change | Measured |
| --- | --- | --- | --- | --- | --- |
| 0 | First A2-Lite engine: sample-major, 400 MHz | `834edfc` | ~1300 us (over budget) | - | Commit message |
| 1 | Build app and model code with `-O3` instead of `-Os` | `196fdee` | - | Unknown | No |
| 2 | Layer-major block processing, tap weights in registers, 480 MHz boost | `1850fbf` | ~630 us | -51% (2.1x) | Commit message |
| 3 | Unattributed drift, likely replacing `nam::DSP` with `AmpModel` | `076d5dc` | 605 us | -4% | Endpoints only |
| 4 | Leaky ReLU as `max(x, 0.01x)`: one `vmaxnm` instead of compare, FPSCR transfer, and select | `db50f43` | 592 us | -13 us (-2.1%) | Yes |
| 5 | Seed each layer's sums from the bias; mirror only the frames taps read; no per-frame wrap check in `Push` | `a31f73e` | 583 us | -9 us (-1.5%) | Yes |
| 6 | Keep hot loops off newlib-nano's byte-at-a-time `memcpy`/`memset` | `ba72034` | **552 us** | -31 us (-5.3%) | Yes |
| 7 | Bypass skips the model (bypass only) | `3dc2317` | 3 us in bypass | -549 us in bypass | Yes |

In row 2, the clock boost alone accounts for roughly 1300 to 1083 us
(400/480); the restructure accounts for the rest, about -42%. That split is
an estimate; only the combined result was recorded.

Overall, the first working engine went from about 1300 us (over budget) to
552 us: 2.4x faster, with about 438 us of peak headroom and no overruns.
Peak callback time is about 562 us.

Other gains:

- Delay-line memory: 180 KB to 119 KB (`a31f73e`).
- Model load: prewarm computes each delay line's steady state directly
  instead of processing silence (`d51c039`). Not timed.
- Firmware no longer links NAM Core, Eigen, or nlohmann (`076d5dc`); NAM
  Core is only the test reference.

## Where the time goes

At 552 us, A2-Lite takes about 264k of the 480k cycles per block:

| Stage | Cycles/block | Share |
| --- | --- | --- |
| Dilated taps (156 taps, ~895 cycles each) | ~140k | ~53% |
| Fixed per-layer work (23 layers, ~4.5k each) | ~104k | ~39% |
| Head | ~14k | ~5% |
| Input projection | ~0.9k | <1% |

The tap loop runs at about 2.4 cycles per multiply-add and uses nearly all
32 FPU registers. Layers with the widest dilation cost only 5-9% more than
the narrowest, so memory bandwidth is not the bottleneck.

## Rejected experiments

Each was measured on the Seed and reverted.

| Experiment | Result | Why |
| --- | --- | --- |
| `-O2` instead of `-O3` | +46% (883 us) | Less loop optimization of the tap loop. |
| Two frames per tap-loop iteration | +13% per tap | The loop was not waiting on FMA latency; extra chains added register pressure. |
| Hot code in ITCM | +9 us | Code already ran from the instruction cache; literal-pool loads compete with ITCM fetches. |
| Fuse the output stage into the last tap pass | +1.8% per block | Kernel-6 layers end on a two-tap pass needing 33 floats in 32 registers; spills cost more than the saved reload. |

## Accuracy

Every change in rows 4-7 is bit-identical to the engine before it, across
block sizes 1-256. All versions match upstream NAM renders within
`make test` tolerance (max error at most 5.4e-6).

## Remaining ideas

- Relaxed floating-point flags: gives up bit-exactness; gain unknown.
- Larger blocks: spreads per-layer overhead, at the cost of latency.
- Measure upstream NAM Core's WaveNet on the Seed for a true upstream
  baseline.
