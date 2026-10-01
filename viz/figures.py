"""Presentation figures for the three amp models, written to build/viz/.

Run with make viz, which builds build/viz/render and the .venv first. Every
signal is rendered through the firmware's A2-Lite engine on the host; the
accuracy figure also renders it through upstream NAM Core for comparison.
"""

import json
import re
import subprocess
import wave
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "viz"
RENDER = OUT / "render"
FS = 48000
# NAM's standard reamp (training input) signal. Not redistributable, so it is
# optional: put it here and the accuracy figures use it instead of a synthetic DI.
REAMP = ROOT / "models" / "local" / "reamp_signal.wav"

# Dark slide theme. Amp colors are the reference categorical palette's dark
# steps, slots 1-3 (validated all-pairs on SURFACE), one per amp in AmpId
# order; the rest are its dark chart tokens.
AMP_COLORS = ["#3987e5", "#d95926", "#199e70"]
SURFACE = "#1a1a19"
CARD = "#232322"
TEXT = "#ffffff"
TEXT_SECONDARY = "#c3c2b7"
TEXT_MUTED = "#898781"
GRID = "#2c2c2a"
AXIS = "#383835"
REFERENCE = "#5a5954"
METER_TRACK = "#0d366b"

# Every figure is a 16:9 slide, 2400x1350 px at this DPI.
SLIDE_SIZE = (16, 9)
SLIDE_DPI = 150

plt.rcParams.update({
    "font.family": ["Arial", "DejaVu Sans"],
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "font.size": 14,
    "axes.titlesize": 16,
    "axes.titleweight": "bold",
    "axes.titlelocation": "left",
    "axes.titlecolor": TEXT,
    "axes.labelsize": 14,
    "axes.labelcolor": TEXT_SECONDARY,
    "axes.edgecolor": AXIS,
    "axes.linewidth": 1,
    "axes.grid": True,
    "axes.axisbelow": True,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.spines.left": False,
    "grid.color": GRID,
    "grid.linewidth": 1,
    "xtick.color": TEXT_MUTED,
    "ytick.color": TEXT_MUTED,
    "xtick.labelcolor": TEXT_SECONDARY,
    "ytick.labelcolor": TEXT_SECONDARY,
    "xtick.major.size": 0,
    "ytick.major.size": 0,
    "xtick.minor.size": 0,
    "ytick.minor.size": 0,
    "text.color": TEXT,
    "lines.linewidth": 2,
    "lines.solid_capstyle": "round",
    "legend.frameon": False,
    "legend.labelcolor": TEXT_SECONDARY,
    "legend.fontsize": 14,
})


def load_amps():
    """Amp names and .nam paths from models/amps.json, in AmpId order."""
    manifest = json.loads((ROOT / "models" / "amps.json").read_text())
    return [{"id": index + 1, "name": amp["name"], "path": ROOT / "models" / "local" / amp["file"], "color": AMP_COLORS[index]} for index, amp in enumerate(manifest)]


def render(kind, model, signal):
    """Runs a signal through the "engine" (by AmpId) or "upstream" (by .nam path)."""
    with tempfile.TemporaryDirectory() as directory:
        source, result = Path(directory, "in.f32"), Path(directory, "out.f32")
        np.asarray(signal, np.float32).tofile(source)
        subprocess.run([str(RENDER), kind, str(model), str(source), str(result)], check=True)
        return np.fromfile(result, np.float32).astype(np.float64)


def db(value, floor=1e-12):
    return 20 * np.log10(np.maximum(np.abs(value), floor))


def save(figure, name):
    path = OUT / name
    figure.savefig(path, dpi=SLIDE_DPI)
    plt.close(figure)
    print(f"wrote {path.relative_to(ROOT)}")


# ---------------------------------------------------------------- stimuli


def pluck(frequency, seconds, seed):
    """Karplus-Strong plucked string: a noise burst through a damped delay loop."""
    rng = np.random.default_rng(seed)
    period = int(round(FS / frequency))
    ring = rng.uniform(-1, 1, period)
    out = np.empty(int(seconds * FS))
    for i in range(len(out)):
        # Averaging neighbours low-passes each round trip, like a real string.
        sample = ring[i % period]
        ring[i % period] = 0.996 * 0.5 * (sample + ring[(i + 1) % period])
        out[i] = sample
    return out


def guitar_riff(peak=0.4):
    """Synthetic DI: two strummed power chords, then a short single-note line."""
    riff = np.zeros(4 * FS)

    # Strum each chord low string first, 12 ms apart.
    for start, notes in [(0.0, [82.41, 123.47, 164.81]), (1.0, [110.0, 164.81, 220.0])]:
        for string, frequency in enumerate(notes):
            begin = int((start + 0.012 * string) * FS)
            tone = pluck(frequency, 1.0, seed=int(frequency))
            riff[begin:begin + len(tone)] += tone[: len(riff) - begin]

    # Single notes, an eighth note apart at 120 BPM.
    for step, frequency in enumerate([146.83, 164.81, 196.0, 220.0, 196.0, 164.81]):
        begin = int((2.0 + 0.25 * step) * FS)
        tone = pluck(frequency, 0.5, seed=step)
        riff[begin:begin + len(tone)] += tone[: len(riff) - begin]
    return peak * riff / np.abs(riff).max()


def read_wav(path):
    """Mono 16- or 24-bit PCM WAV at 48 kHz, as floats in [-1, 1)."""
    with wave.open(str(path)) as file:
        if file.getnchannels() != 1 or file.getframerate() != FS or file.getsampwidth() not in (2, 3):
            raise ValueError(f"{path}: expected mono 16- or 24-bit PCM at {FS} Hz")
        width = file.getsampwidth()
        raw = np.frombuffer(file.readframes(file.getnframes()), np.uint8).reshape(-1, width)

    # Assemble little-endian bytes, then sign-extend from the top bit.
    value = sum(raw[:, byte].astype(np.int64) << (8 * byte) for byte in range(width))
    value = np.where(value >= 1 << (8 * width - 1), value - (1 << (8 * width)), value)
    return value / float(1 << (8 * width - 1))


def accuracy_input():
    """The reamp signal when present, else the synthetic riff, with a description
    and a moment of guitar playing to zoom in on."""
    if REAMP.exists():
        return read_wav(REAMP), "190 s NAM reamp signal", 120.5
    return guitar_riff(), "4 s synthetic guitar DI", 1.6


def sine(frequency, amplitude, seconds):
    return amplitude * np.sin(2 * np.pi * frequency * np.arange(int(seconds * FS)) / FS)


def log_sweep(amplitude, seconds=8.0, start=10.0, stop=22000.0):
    """Exponential sine sweep, with a short fade at both ends to avoid clicks."""
    t = np.arange(int(seconds * FS)) / FS
    rate = np.log(stop / start)
    sweep = amplitude * np.sin(2 * np.pi * start * seconds / rate * (np.exp(t * rate / seconds) - 1))
    fade = int(0.02 * FS)
    sweep[:fade] *= np.linspace(0, 1, fade)
    sweep[-fade:] *= np.linspace(1, 0, fade)
    return sweep


# --------------------------------------------------------------- analysis


def welch_db(signal, size=8192):
    """Average power spectrum in dB, Hann-windowed, half-overlapped."""
    window = np.hanning(size)
    frames = [signal[i:i + size] * window for i in range(0, len(signal) - size, size // 2)]
    power = np.mean([np.abs(np.fft.rfft(frame)) ** 2 for frame in frames], axis=0)
    power /= np.sum(window) ** 2 / 2
    return np.fft.rfftfreq(size, 1 / FS), 10 * np.log10(np.maximum(power, 1e-30))


def frequency_response(model_id, amplitude):
    """Small-signal magnitude response from a log sweep, smoothed to 1/6 octave."""
    sweep = log_sweep(amplitude)
    padded = np.concatenate([sweep, np.zeros(FS)])
    output = render("engine", model_id, padded)
    output -= np.mean(output[-FS // 2:])

    # Regularized deconvolution: output spectrum over input spectrum.
    x, y = np.fft.rfft(padded), np.fft.rfft(output)
    response = y * np.conj(x) / (np.abs(x) ** 2 + 1e-3 * np.max(np.abs(x)) ** 2)
    freqs = np.fft.rfftfreq(len(padded), 1 / FS)

    # Fractional-octave smoothing of power onto log-spaced centres.
    centres = np.geomspace(40, 18000, 300)
    smoothed = [np.mean(np.abs(response[(freqs >= f * 2 ** (-1 / 12)) & (freqs <= f * 2 ** (1 / 12))]) ** 2) for f in centres]
    return centres, 10 * np.log10(smoothed)


def harmonic_levels(output, frequency, count):
    """Levels of the fundamental and its harmonics in dBFS, from 1 s of output."""
    tail = output[-FS:] - np.mean(output[-FS:])
    spectrum = np.abs(np.fft.rfft(tail * np.hanning(FS))) / (np.sum(np.hanning(FS)) / 2)
    # A 1 s window puts every integer frequency in its own bin.
    return np.array([spectrum[int(frequency * n)] for n in range(1, count + 1)])


# ---------------------------------------------------------------- figures


def render_both(amps, signal):
    """Per amp, the (engine, upstream) outputs for one input signal."""
    return [(render("engine", amp["id"], signal), render("upstream", amp["path"], signal)) for amp in amps]


def esr_db(engine, upstream):
    """Engine-to-reference error energy over output energy, in dB: the ESR
    that make test bounds. The output's DC offset is not counted as signal."""
    return 10 * np.log10(np.sum((engine - upstream) ** 2) / np.sum((upstream - upstream.mean()) ** 2))


def minus(text):
    """Typographic minus signs for display."""
    return str(text).replace("-", "−")


# Slide layout, in figure coordinates: title, subtitle and a legend row at the
# top, then the content area.
CONTENT = {"left": 0.07, "right": 0.95, "top": 0.74, "bottom": 0.09}


def slide(title, subtitle, legend=None):
    """A blank 16:9 slide with its title, subtitle and an optional row of
    legend handles under them."""
    figure = plt.figure(figsize=SLIDE_SIZE)
    figure.text(0.05, 0.93, title, fontsize=30, fontweight="bold", va="top")
    figure.text(0.05, 0.86, subtitle, fontsize=16, color=TEXT_SECONDARY, va="top")
    if legend:
        figure.legend(handles=legend, loc="upper left", bbox_to_anchor=(0.043, 0.83), ncol=len(legend), handlelength=1.6, columnspacing=2.2)
    return figure


def amp_legend(amps, **line):
    return [plt.Line2D([], [], color=amp["color"], label=amp["name"], **line) for amp in amps]


def accuracy_figure(amps, outputs, description, zoom_at):
    figure = slide("Firmware engine vs. NAM Core", f"{description}. Top: 8 ms of output, overlaid. Bottom: output and difference spectra.", legend=[
        plt.Line2D([], [], color=REFERENCE, linewidth=7, label="NAM Core (reference)"),
        plt.Line2D([], [], color=TEXT_SECONDARY, linewidth=2, label="A2-Lite engine"),
        plt.Line2D([], [], color=TEXT_MUTED, linewidth=2, label="Difference"),
    ])
    axes = figure.subplots(2, 3, gridspec_kw={"height_ratios": [1, 1.2], "hspace": 0.42, "wspace": 0.16})
    figure.subplots_adjust(**CONTENT)

    for column, (amp, (engine, upstream)) in enumerate(zip(amps, outputs)):
        error = engine - upstream
        print(f"  {amp['name']}: max difference {np.abs(error).max():.1e}, ESR {esr_db(engine, upstream):.1f} dB")

        # Waveform overlay: the engine drawn over a wider reference trace.
        top = axes[0, column]
        window = slice(int(zoom_at * FS), int((zoom_at + 0.008) * FS))
        t = np.arange(window.stop - window.start) / FS * 1000
        top.plot(t, upstream[window], color=REFERENCE, linewidth=7)
        top.plot(t, engine[window], color=amp["color"], linewidth=2)
        top.set_title(amp["name"])
        top.set_xlabel("Time (ms)")
        top.set_xlim(0, t[-1])
        top.yaxis.set_major_locator(matplotlib.ticker.MaxNLocator(4))

        # Spectra of the output and of the difference between the two engines.
        bottom = axes[1, column]
        freqs, signal_db = welch_db(engine - engine.mean())
        _, error_db = welch_db(error)
        bottom.semilogx(freqs[1:], signal_db[1:], color=amp["color"])
        bottom.semilogx(freqs[1:], error_db[1:], color=TEXT_MUTED)
        bottom.fill_between(freqs[1:], error_db[1:], signal_db[1:], color=amp["color"], alpha=0.06, linewidth=0)
        bottom.text(0.96, 0.5, minus(f"Difference {esr_db(engine, upstream):.0f} dB"), transform=bottom.transAxes, ha="right", va="center", fontsize=18, fontweight="bold")
        bottom.set_xlim(30, 20000)
        bottom.set_ylim(-210, -10)
        bottom.set_xlabel("Frequency (Hz)")
        bottom.set_xticks([100, 1000, 10000], ["100", "1k", "10k"])
        if column == 0:
            top.set_ylabel("Output")
            bottom.set_ylabel("Power (dB)")

    save(figure, "accuracy.png")


def accuracy_over_time_figure(amps, outputs, description):
    """Output and engine-minus-reference levels in 100 ms windows, per amp."""
    window = FS // 10
    figure = slide("Difference over time", f"{description}, 100 ms RMS windows. Drops are digital silence in the input.", legend=[
        plt.Line2D([], [], color=TEXT_SECONDARY, label="Output"),
        plt.Line2D([], [], color=TEXT_MUTED, label="Difference"),
    ])
    axes = figure.subplots(len(amps), 1, sharex=True, gridspec_kw={"hspace": 0.5})
    figure.subplots_adjust(**CONTENT)

    for axis, amp, (engine, upstream) in zip(axes, amps, outputs):
        frames = len(upstream) // window
        reference = upstream[: frames * window].reshape(frames, window)
        error = (engine - upstream)[: frames * window].reshape(frames, window)
        t = (np.arange(frames) + 0.5) * window / FS

        # Each window's mean is removed, so the model's DC offset is not counted
        # as signal. During digital silence the output is only that offset.
        output_db = db(np.std(reference, axis=1))
        error_db = db(np.sqrt(np.mean(error ** 2, axis=1)))
        axis.plot(t, output_db, color=amp["color"], linewidth=1.5)
        axis.plot(t, error_db, color=TEXT_MUTED, linewidth=1.5)
        axis.fill_between(t, error_db, output_db, color=amp["color"], alpha=0.06, linewidth=0)
        axis.set_ylim(-200, 0)
        axis.set_yticks([-150, -100, -50, 0], [minus(v) for v in (-150, -100, -50, 0)])
        axis.set_title(amp["name"], pad=8)
        axis.set_xlim(0, t[-1])

    axes[1].set_ylabel("Level (dBFS)")
    axes[-1].set_xlabel("Time (s)")
    save(figure, "accuracy_over_time.png")


def model_shape():
    """Channels, kernel sizes, dilations and head taps, read from a2_lite.h so
    the numbers follow the engine's fixed shape."""
    header = (ROOT / "src" / "models" / "a2_lite.h").read_text()

    def constant(name):
        return int(re.search(rf"constexpr int {name} = (\d+);", header).group(1))

    def array(name):
        body = re.search(rf"{name} = \{{([^}}]*)\}}", header).group(1)
        return [int(value) for value in body.split(",")]

    return constant("kChannels"), array("kKernelSizes"), array("kDilations"), constant("kHeadTaps")


def stats_figure(amps, outputs, description):
    """Headline numbers: a budget meter over a grid of stat tiles."""
    channels, kernels, dilations, head_taps = model_shape()

    # Hardware timings measured on the Seed; see docs/performance.md. Update
    # these when the ledger gets a new row.
    callback_us, budget_us, first_engine_us = 552, 1000, 1300

    # Weight count, matching a2_lite::WeightCount().
    weights = channels + sum(k * channels * channels + 3 * channels + channels * channels for k in kernels) + head_taps * channels + 2

    # Samples of input that reach one output sample: every dilated layer plus
    # the head's taps, which are one frame apart.
    receptive_field = 1 + sum((k - 1) * d for k, d in zip(kernels, dilations)) + head_taps - 1

    # Multiply-adds per sample: input projection; per layer the taps,
    # conditioning and 1x1 residual; then the head.
    macs = channels + sum(k * channels * channels + channels + channels * channels for k in kernels) + head_taps * channels

    # Worst amp's engine-to-reference ESR, as in the accuracy figure.
    worst_db = max(esr_db(engine, upstream) for engine, upstream in outputs)

    figure = slide("A2-Lite on the Daisy Seed", f"{len(amps)} NAM amp models on a Cortex-M7 at 480 MHz, bare metal.")

    # Hero: callback time against the real-time budget.
    used = callback_us / budget_us
    figure.text(0.05, 0.775, "Audio callback, 48-sample block", fontsize=16, color=TEXT_SECONDARY, va="top")
    hero = figure.text(0.05, 0.735, f"{callback_us} µs", fontsize=72, fontweight="bold", va="top")
    hero_right = hero.get_window_extent(figure.canvas.get_renderer()).transformed(figure.transFigure.inverted()).x1
    figure.text(hero_right + 0.015, 0.682, f"of {budget_us:,} µs budget", fontsize=20, color=TEXT_SECONDARY, va="center")

    # Meter: the used share on a darker track of the same blue.
    meter = figure.add_axes([0.05, 0.52, 0.9, 0.05])
    meter.set_axis_off()
    meter.set_xlim(0, 1)
    meter.set_ylim(0, 1)
    meter.add_patch(matplotlib.patches.Rectangle((0, 0), 1, 1, color=METER_TRACK, linewidth=0))
    meter.add_patch(matplotlib.patches.Rectangle((0, 0), used, 1, color=AMP_COLORS[0], linewidth=0))
    figure.text(0.065, 0.545, f"{used:.0%} used", fontsize=15, fontweight="bold", ha="left", va="center")
    figure.text(0.935, 0.545, f"{1 - used:.0%} headroom · no overruns", fontsize=15, ha="right", va="center")

    # Tiles: value, then a short label and its basis.
    tiles = [
        (f"{first_engine_us / callback_us:.1f}×", "speedup over first engine", f"{first_engine_us:,} → {callback_us} µs, incl. 400 → 480 MHz"),
        (minus(f"{worst_db:.0f} dB"), "difference from NAM Core", "worst of three amps"),
        (f"{receptive_field / FS * 1000:.0f} ms", "receptive field", f"{receptive_field:,} samples, {len(kernels)} layers"),
        (f"{macs * FS / 1e6:.0f} M", "multiply-adds per second", f"{macs:,} per sample"),
        (f"{weights:,}", "weights per model", f"{weights * 4 / 1000:.1f} KB as float32"),
        (f"{len(amps) * weights * 4 / 1000:.0f} KB", f"weights for all {len(amps)} models", "embedded in flash"),
    ]
    columns, width, height, gap = 3, 0.29, 0.19, 0.015
    for index, (value, label, basis) in enumerate(tiles):
        x = 0.05 + (index % columns) * (width + gap)
        y = 0.265 - (index // columns) * (height + gap)
        figure.patches.append(matplotlib.patches.FancyBboxPatch((x, y), width, height, boxstyle="round,pad=0,rounding_size=0.01", transform=figure.transFigure, color=CARD, linewidth=0))
        figure.text(x + 0.02, y + height - 0.025, value, fontsize=40, fontweight="bold", va="top")
        figure.text(x + 0.02, y + 0.055, label, fontsize=16, color=TEXT, va="bottom")
        figure.text(x + 0.02, y + 0.025, basis, fontsize=13, color=TEXT_MUTED, va="bottom")

    figure.text(0.05, 0.025, f"Timing: hardware, docs/performance.md. Accuracy: {description}. Model: src/models/a2_lite.h.", fontsize=11, color=TEXT_MUTED)
    save(figure, "stats.png")


def label_line_end(axis, x, y, text, offset=0):
    """Direct label just past a line's last point."""
    axis.annotate(text, (x, y), xytext=(10, offset), textcoords="offset points", va="center", fontsize=14, color=TEXT_SECONDARY, annotation_clip=False)


def frequency_response_figure(amps):
    figure = slide("Small-signal frequency response", "−50 dBFS log sweep, 1/6-octave smoothing, normalized at 1 kHz. Amp only, no cabinet.", legend=amp_legend(amps))
    axis = figure.subplots()
    figure.subplots_adjust(**{**CONTENT, "right": 0.84})

    # Normalized at 1 kHz so the curves compare voicing, not loudness.
    for amp in amps:
        freqs, response = frequency_response(amp["id"], amplitude=10 ** (-50 / 20))
        response -= np.interp(1000, freqs, response)
        axis.semilogx(freqs, response, color=amp["color"])
        label_line_end(axis, freqs[-1], response[-1], amp["name"])

    axis.set_xlim(40, 18000)
    axis.set_xticks([50, 100, 200, 500, 1000, 2000, 5000, 10000], ["50", "100", "200", "500", "1k", "2k", "5k", "10k"])
    axis.set_yticks(range(-15, 20, 5), [minus(v) for v in range(-15, 20, 5)])
    axis.set_xlabel("Frequency (Hz)")
    axis.set_ylabel("Level re 1 kHz (dB)")
    save(figure, "amp_frequency_response.png")


def drive_figure(amps):
    """Gain curve: output level against input level for a 220 Hz sine."""
    levels = np.arange(-60, 3, 3)
    figure = slide("Input vs. output level", "220 Hz sine, steady state. Flattening marks saturation.", legend=amp_legend(amps, marker="o", markersize=6))
    axis = figure.subplots()
    figure.subplots_adjust(**{**CONTENT, "right": 0.84})

    # The Twin and AC30 end within a dB of each other, so their end labels
    # sit above and below.
    label_offsets = [-2, 14, 0]
    for amp, offset in zip(amps, label_offsets):
        out = []
        for level in levels:
            output = render("engine", amp["id"], sine(220, 10 ** (level / 20), 0.5))[FS // 4:]
            out.append(db(np.sqrt(2) * np.std(output)))
        axis.plot(levels, out, color=amp["color"], marker="o", markersize=6, markeredgecolor=SURFACE, markeredgewidth=2)
        label_line_end(axis, levels[-1], out[-1], amp["name"], offset=-offset)

    axis.set_xticks(range(-60, 1, 10), [minus(v) for v in range(-60, 1, 10)])
    axis.set_yticks(range(-50, 1, 10), [minus(v) for v in range(-50, 1, 10)])
    axis.set_xlabel("Input (dBFS, sine peak)")
    axis.set_ylabel("Output (dBFS, sine-equivalent peak)")
    save(figure, "amp_gain_curve.png")


def waveform_figure(amps):
    """Small multiples: two cycles of output at three input levels per amp."""
    levels = [-40, -24, -8]
    frequency = 110
    figure = slide("Output waveform by drive level", "110 Hz sine input and amp output, each normalized to its own peak.", legend=[plt.Line2D([], [], color=REFERENCE, linewidth=2, label="Input")] + amp_legend(amps))
    axes = figure.subplots(len(amps), len(levels), sharex=True, sharey=True, gridspec_kw={"hspace": 0.3, "wspace": 0.08})
    figure.subplots_adjust(**{**CONTENT, "left": 0.16})
    period = FS // frequency * 2
    t = np.arange(period) / FS * 1000

    for row, amp in enumerate(amps):
        for column, level in enumerate(levels):
            stimulus = sine(frequency, 10 ** (level / 20), 0.5)
            output = render("engine", amp["id"], stimulus)[-period:]
            output -= output.mean()
            axis = axes[row, column]
            # Both traces are scaled to their own peak, so only shape compares.
            axis.plot(t, stimulus[-period:] / np.abs(stimulus).max(), color=REFERENCE, linewidth=1.5)
            axis.plot(t, output / np.abs(output).max(), color=amp["color"])
            axis.set_ylim(-1.2, 1.2)
            axis.set_yticks([-1, 0, 1], [minus(-1), "0", "1"])
            if row == 0:
                axis.set_title(minus(f"{level} dBFS in"), loc="center")
            if column == 0:
                axis.set_ylabel(amp["name"].replace(" ", "\n", 1), rotation=0, ha="right", va="center", labelpad=16, color=TEXT, fontsize=15)
            if row == len(amps) - 1:
                axis.set_xlabel("Time (ms)")

    save(figure, "amp_drive_waveforms.png")


def harmonics_figure(amps, level=-20, count=9):
    """Grouped bars of harmonics 2..count relative to the fundamental."""
    frequency = 220
    width = 0.24
    orders = np.arange(2, count + 1)

    # Render first: the legend carries each amp's THD.
    relative, legend = [], []
    for amp in amps:
        levels = harmonic_levels(render("engine", amp["id"], sine(frequency, 10 ** (level / 20), 1.5)), frequency, count)
        relative.append(db(levels[1:] / levels[0]))
        thd = 100 * np.sqrt(np.sum(levels[1:] ** 2)) / levels[0]
        legend.append(matplotlib.patches.Patch(color=amp["color"], label=f"{amp['name']}  ·  THD {thd:.0f}%"))

    figure = slide(minus(f"Harmonic distortion at {level} dBFS"), f"{frequency} Hz sine. Harmonics 2–{count} relative to the fundamental.", legend=legend)
    axis = figure.subplots()
    figure.subplots_adjust(**CONTENT)

    # Bars hang from a -80 dB floor, so a taller bar is a stronger harmonic.
    for index, (amp, values) in enumerate(zip(amps, relative)):
        axis.bar(orders + (index - 1) * (width + 0.02), values + 80, bottom=-80, width=width, color=amp["color"])

    axis.set_xticks(orders, [f"H{n}" for n in orders])
    axis.set_yticks(range(-80, 1, 20), [minus(v) for v in range(-80, 1, 20)])
    axis.set_ylabel("Level (dBc)")
    axis.set_ylim(-80, 0)
    axis.grid(axis="x", visible=False)
    save(figure, "amp_harmonics.png")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    amps = load_amps()
    signal, description, zoom_at = accuracy_input()
    outputs = render_both(amps, signal)
    accuracy_figure(amps, outputs, description, zoom_at)
    accuracy_over_time_figure(amps, outputs, description)
    stats_figure(amps, outputs, description)
    frequency_response_figure(amps)
    drive_figure(amps)
    waveform_figure(amps)
    harmonics_figure(amps)


if __name__ == "__main__":
    main()
