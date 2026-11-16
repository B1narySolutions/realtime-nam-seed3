# Hardware tests

`make hwtest` measures the board's audio chain with a single cable and no
other equipment. It writes a report with pass/fail checks, plots and the raw
recordings.

## Setup

1. Connect the Seed3 over USB, running any firmware from this repository.
2. Plug a mono (TS) cable from the **line out (J2)** into the **guitar input
   (J1)**. Leave the headphone jack empty, because headphones mute the line
   out.
3. Run:

   ```sh
   make hwtest LABEL=rev1-board1 NOTES="bench supply, laptop on battery"
   ```

That's all. The run takes about two minutes:

1. It builds and flashes the test firmware (`src/hwtest/`).
2. It runs the measurements (see "What it measures").
3. It flashes the main firmware back and checks it through its serial keys:
   each amp's callback timing, then 30 model switches.
4. It prints a pass/fail summary and writes the results to
   `hwtest/results/<date>_<label>/`, with a `latest` link to the newest run.

The board is left running the main firmware. The command exits non-zero if
any check fails.

Never loop the speaker output into an input. It is bridged and swings about
10 Vrms.

**Without the cable**, the suite finds no loopback. It then measures only the
input on its own (the jack's switch shorts the input to ground) and the knobs,
and skips the rest. Running once with the cable and once without separates
the input's noise from the whole chain's.

| Option | Effect |
| --- | --- |
| `LABEL=...`, `NOTES=...` | Name the run and save notes with it |
| `HWTEST_ARGS=--no-flash` | Use the test firmware already running (`make hwtest-upload` flashes it); skips the main-firmware check |
| `HWTEST_ARGS=--skip-firmware-check` | Leave the test firmware on the board and skip the main-firmware check |
| `PORT=/dev/cu.usbmodem...` | Choose the serial port when there are several |
| `make hwtest-sim` | Run the whole suite against a simulated loopback, with no board, and check that it measures back the faults the simulation adds |

## Why a loopback gives the bypass SNR

In bypass the firmware passes the input straight to the output, so the noise
comes from the input front end, the ADC and the DAC. A loopback runs the
same parts in the other order (DAC, then front end and ADC), so its SNR is
the bypass SNR. The test firmware generates the signal itself, so no external
generator or analyzer is needed. The measurement covers both directions of
the chain, so it is a lower bound for the ADC and the DAC on their own.

## What it measures

All levels are in dBFS, where a full-scale sine is 0 dBFS (AES17). The input
and output share one sample clock, so each tone is fitted at its exact
frequency. That separates the tone from noise and distortion without
windowing errors.

| Test | Stimulus | Results |
| --- | --- | --- |
| Routing | 1 kHz on each output alone | Which output reaches which input, the loop gain, and crosstalk from the other output |
| Noise | 4 s of silence, then again with the CPU 80 % busy | Noise 20 Hz–20 kHz (unweighted and A-weighted), DC offset, mains hum, spurs (for example from regulators or the display), and how much the CPU load adds |
| Tones | 1 kHz at −1 and −60 dBFS | **SNR** (full-scale output over the idle noise), THD+N, THD and harmonics, and AES17 dynamic range |
| Linearity | 1 kHz stepped from −60 to 0 dBFS | Gain and THD+N at each level, plus where compression and clipping begin |
| Sweep | 10 Hz–23 kHz log sweep | Frequency response and its −3 dB corners, **round-trip latency**, and polarity |
| Continuity | 12 s tone with the CPU 80 % busy | Dropped or repeated blocks, which show up as bursts in the residual |
| Knobs | 1000 raw ADC readings of each pot | Noise as a percentage of travel. The main firmware reports moves of 2 % or more |
| Main firmware | Serial keys `0`–`5` | Average and peak callback time and overruns per amp, plus failures across 30 model switches |

The limits are in `hwtest/limits.json`. They are provisional, because the
project spec sets no audio numbers. Set them from the first boards measured.

## Results

Each run's folder holds:

- `report.md`: the checks table, a short write-up of each test, and the plots
- `results.json`: every measurement, for comparing runs or boards
- `*.png`: the noise spectrum, frequency response, impulse response and
  linearity plots
- `captures/`: every stimulus and recording as 32-bit float WAVs, which open
  in Audacity. Git ignores this folder; keep the reports in the repository
  and the captures elsewhere.

## How it works

The test firmware turns the Seed3 into a two-channel audio interface that the
host controls over USB serial. The protocol is described at the top of
`src/hwtest/main.cpp`:

1. The host uploads a stimulus into SDRAM.
2. The firmware plays it on both outputs while recording both inputs in the
   same audio callbacks.
3. The firmware sends the recording back, checked with a CRC.

All signal generation and analysis is in Python (`hwtest/analysis.py`), so a
new test needs no firmware change. The test firmware uses the same clocks,
sample rate and block size as the main firmware. It links no models, because
the main firmware fills nearly all of the 128 KB of internal flash. The board
starts with an SDRAM self-test, and its LED blinks fast so you can tell it
apart from the main firmware.
