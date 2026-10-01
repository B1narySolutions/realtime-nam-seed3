"""Presentation figures for the three amp models, written to build/viz/.

Run with make viz, which builds build/viz/render and the .venv first. Every
signal is rendered through the firmware's A2-Lite engine on the host; the
accuracy figure also renders it through upstream NAM Core for comparison.
"""

import json
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

# Reference categorical palette, slots 1-3 (validated for all pairs), one per
# amp in AmpId order, plus the chart's neutral tokens.
AMP_COLORS = ["#2a78d6", "#eb6834", "#1baf7a"]
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
TEXT_MUTED = "#8a8984"
GRID = "#e6e5e0"
REFERENCE = "#c3c2b7"

plt.rcParams.update({
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "font.size": 13,
    "axes.titlesize": 15,
    "axes.titleweight": "semibold",
    "axes.titlelocation": "left",
    "axes.labelsize": 13,
    "axes.labelcolor": TEXT_SECONDARY,
    "axes.edgecolor": GRID,
    "axes.linewidth": 1,
    "axes.grid": True,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "grid.color": GRID,
    "grid.linewidth": 1,
    "xtick.color": TEXT_SECONDARY,
    "ytick.color": TEXT_SECONDARY,
    "xtick.major.size": 0,
    "ytick.major.size": 0,
    "text.color": TEXT,
    "lines.linewidth": 2,
    "lines.solid_capstyle": "round",
    "legend.frameon": False,
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
    figure.savefig(path, dpi=150, bbox_inches="tight", pad_inches=0.3)
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
        return read_wav(REAMP), "NAM's 190 s reamp (training input) signal", 120.5
    return guitar_riff(), "A synthetic guitar DI", 1.6


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


def accuracy_figure(amps, outputs, description, zoom_at):
    figure, axes = plt.subplots(2, 3, figsize=(16, 8.4), gridspec_kw={"height_ratios": [1, 1.15], "hspace": 0.45, "wspace": 0.18})
    summary = []

    for column, (amp, (engine, upstream)) in enumerate(zip(amps, outputs)):
        error = engine - upstream
        esr = np.sum(error ** 2) / np.sum((upstream - upstream.mean()) ** 2)
        summary.append(f"{amp['name']}: max error {np.abs(error).max():.1e}, error energy {10 * np.log10(esr):.1f} dB")

        # Waveform overlay: the engine drawn over a wider reference trace.
        top = axes[0, column]
        window = slice(int(zoom_at * FS), int((zoom_at + 0.008) * FS))
        t = np.arange(window.stop - window.start) / FS * 1000
        top.plot(t, upstream[window], color=REFERENCE, linewidth=7)
        top.plot(t, engine[window], color=amp["color"], linewidth=1.8)
        top.set_title(amp["name"])
        top.set_xlabel("Time (ms)")
        top.set_xlim(0, t[-1])
        if column == 0:
            top.set_ylabel("Output")

        # Spectra of the output and of the difference between the two engines.
        bottom = axes[1, column]
        freqs, signal_db = welch_db(engine - engine.mean())
        _, error_db = welch_db(error)
        bottom.semilogx(freqs[1:], signal_db[1:], color=amp["color"])
        bottom.semilogx(freqs[1:], error_db[1:], color=TEXT_MUTED)
        bottom.fill_between(freqs[1:], error_db[1:], signal_db[1:], color=amp["color"], alpha=0.08, linewidth=0)
        # The ratio of total energies, the same ESR that make test bounds.
        gap = 10 * np.log10(esr)
        bottom.text(0.97, 0.52, f"Difference: {gap:.0f} dB\nrelative to the output".replace("-", "−"), transform=bottom.transAxes, ha="right", va="center", fontsize=14, fontweight="semibold", color=TEXT)
        bottom.set_xlim(30, 20000)
        bottom.set_ylim(-210, -10)
        bottom.set_xlabel("Frequency (Hz)")
        bottom.set_xticks([100, 1000, 10000], ["100", "1k", "10k"])
        if column == 0:
            bottom.set_ylabel("Power (dB)")

    # One legend above each row, outside the data; keys are neutral because
    # each covers all three amps.
    axes[0, 2].legend(loc="lower right", bbox_to_anchor=(1, 1.1), ncol=2, fontsize=12, handles=[
        plt.Line2D([], [], color=REFERENCE, linewidth=7, label="NAM Core (reference)"),
        plt.Line2D([], [], color=TEXT_SECONDARY, linewidth=1.8, label="A2-Lite engine (firmware)"),
    ])
    axes[1, 2].legend(loc="lower right", bbox_to_anchor=(1, 1.1), ncol=2, fontsize=12, handles=[
        plt.Line2D([], [], color=TEXT_SECONDARY, label="Output"),
        plt.Line2D([], [], color=TEXT_MUTED, label="Difference"),
    ])
    figure.suptitle("The firmware engine reproduces NAM Core to within float rounding", x=0.125, ha="left", fontsize=20, fontweight="semibold", y=1.04)
    figure.text(0.125, 0.985, f"{description} through both implementations. Top: 8 ms of output, overlaid. Bottom: spectrum of the output and of the difference.", fontsize=13, color=TEXT_SECONDARY)
    save(figure, "accuracy.png")
    print("  " + "\n  ".join(summary))


def accuracy_over_time_figure(amps, outputs, description):
    """Output and engine-minus-reference levels in 100 ms windows, per amp."""
    window = FS // 10
    figure, axes = plt.subplots(len(amps), 1, figsize=(16, 8.4), sharex=True, gridspec_kw={"hspace": 0.45})

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
        axis.fill_between(t, error_db, output_db, color=amp["color"], alpha=0.08, linewidth=0)
        axis.set_ylim(-200, 0)
        axis.set_yticks([-150, -100, -50, 0])
        axis.set_title(amp["name"], fontsize=14, pad=6)
        axis.set_xlim(0, t[-1])

    axes[1].set_ylabel("Level (dBFS RMS, 100 ms windows)")
    axes[-1].set_xlabel("Time (s)")
    axes[0].legend(loc="lower right", bbox_to_anchor=(1, 1.0), ncol=2, fontsize=12, handles=[
        plt.Line2D([], [], color=TEXT_SECONDARY, label="Output"),
        plt.Line2D([], [], color=TEXT_MUTED, label="Difference"),
    ])
    figure.suptitle("The match holds through quiet and loud passages alike", x=0.125, ha="left", fontsize=20, fontweight="semibold", y=1.03)
    figure.text(0.125, 0.975, f"{description} through both implementations. The difference stays about 120 dB under the output;\nthe sharp drops are stretches of digital silence in the input, where the output is constant.", fontsize=13, color=TEXT_SECONDARY, va="top")
    save(figure, "accuracy_over_time.png")


def label_line_end(axis, x, y, text):
    """Direct label just past a line's last point, in text ink."""
    axis.annotate(text, (x, y), xytext=(8, 0), textcoords="offset points", va="center", fontsize=12, color=TEXT)


def frequency_response_figure(amps):
    figure, axis = plt.subplots(figsize=(13, 6.6))

    # Normalized at 1 kHz so the curves compare EQ voicing, not loudness.
    for amp in amps:
        freqs, response = frequency_response(amp["id"], amplitude=10 ** (-50 / 20))
        response -= np.interp(1000, freqs, response)
        axis.semilogx(freqs, response, color=amp["color"], label=amp["name"])
        label_line_end(axis, freqs[-1], response[-1], amp["name"])

    axis.set_xlim(40, 18000)
    axis.set_xticks([50, 100, 200, 500, 1000, 2000, 5000, 10000], ["50", "100", "200", "500", "1k", "2k", "5k", "10k"])
    axis.set_xlabel("Frequency (Hz)")
    axis.set_ylabel("Level relative to 1 kHz (dB)")
    axis.legend(loc="lower center", ncol=3)
    axis.set_title("Each capture has its own EQ voice", fontsize=20, pad=34)
    axis.text(0, 1.03, "Small-signal frequency response from a log sine sweep at −50 dBFS, smoothed to 1/6 octave. Amp only, no speaker cabinet.", transform=axis.transAxes, color=TEXT_SECONDARY)
    save(figure, "amp_frequency_response.png")


def drive_figure(amps):
    """Gain curve: output level against input level for a 220 Hz sine."""
    levels = np.arange(-60, 3, 3)
    figure, axis = plt.subplots(figsize=(13, 6.6))

    # Label offsets put each name beside its line's flat end: the Twin and AC30
    # end within a dB of each other, so one sits above and one below.
    label_offsets = [12, -14, 12]
    for amp, offset in zip(amps, label_offsets):
        out = []
        for level in levels:
            output = render("engine", amp["id"], sine(220, 10 ** (level / 20), 0.5))[FS // 4:]
            out.append(db(np.sqrt(2) * np.std(output)))
        axis.plot(levels, out, color=amp["color"], label=amp["name"], marker="o", markersize=5, markeredgecolor=SURFACE, markeredgewidth=1.5)
        axis.annotate(amp["name"], (levels[-1], out[-1]), xytext=(0, offset), textcoords="offset points", ha="right", va="center", fontsize=12, color=TEXT)

    axis.set_xlabel("Input level (dBFS, sine peak)")
    axis.set_ylabel("Output level (dBFS, sine-equivalent peak)")
    axis.legend(loc="upper left")
    axis.set_title("All three captures compress well before full scale", fontsize=20, pad=34)
    axis.text(0, 1.03, "Steady-state output of a 220 Hz sine. A straight line means clean gain; flattening is the amp saturating.", transform=axis.transAxes, color=TEXT_SECONDARY)
    save(figure, "amp_gain_curve.png")


def waveform_figure(amps):
    """Small multiples: one cycle of output at three input levels per amp."""
    levels = [-40, -24, -8]
    frequency = 110
    figure, axes = plt.subplots(len(amps), len(levels), figsize=(14, 8.6), sharex=True, gridspec_kw={"hspace": 0.35, "wspace": 0.12})
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
            axis.set_ylim(-1.15, 1.15)
            axis.set_yticks([-1, 0, 1])
            axis.tick_params(labelleft=column == 0)
            if row == 0:
                axis.set_title(f"Input {level} dBFS".replace("-", "−"), loc="center", fontsize=14)
            if column == 0:
                axis.set_ylabel(amp["name"].replace(" ", "\n", 1), rotation=0, ha="right", va="center", labelpad=12, color=TEXT, fontsize=13)
            if row == len(amps) - 1:
                axis.set_xlabel("Time (ms)")

    figure.suptitle("Pushing harder squares off the waveform", x=0.125, ha="left", fontsize=20, fontweight="semibold", y=0.99)
    figure.text(0.125, 0.935, "Two cycles of a 110 Hz sine (gray) and each amp's output, both scaled to their own peak.", fontsize=13, color=TEXT_SECONDARY)
    save(figure, "amp_drive_waveforms.png")


def harmonics_figure(amps, level=-20, count=9):
    """Grouped bars of harmonics 2..count relative to the fundamental."""
    frequency = 220
    figure, axis = plt.subplots(figsize=(13, 6.6))
    orders = np.arange(2, count + 1)
    width = 0.24

    for index, amp in enumerate(amps):
        levels = harmonic_levels(render("engine", amp["id"], sine(frequency, 10 ** (level / 20), 1.5)), frequency, count)
        relative = db(levels[1:] / levels[0])
        thd = 100 * np.sqrt(np.sum(levels[1:] ** 2)) / levels[0]
        # Bars hang from a -80 dB floor, so a taller bar is a stronger harmonic.
        axis.bar(orders + (index - 1) * (width + 0.02), relative + 80, bottom=-80, width=width, color=amp["color"], label=f"{amp['name']}  (THD {thd:.0f}%)")

    axis.set_xticks(orders, [f"{n}\n{'even' if n % 2 == 0 else 'odd'}" for n in orders])
    axis.set_xlabel("Harmonic")
    axis.set_ylabel("Level relative to fundamental (dBc)")
    axis.set_ylim(-80, 0)
    axis.grid(axis="x", visible=False)
    axis.legend(loc="upper right")
    axis.set_title("Each amp distorts with its own mix of harmonics", fontsize=20, pad=34)
    axis.text(0, 1.03, f"Harmonic spectrum of a 220 Hz sine at {str(level).replace('-', '−')} dBFS input. Even harmonics come from asymmetric clipping.", transform=axis.transAxes, color=TEXT_SECONDARY)
    save(figure, "amp_harmonics.png")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    amps = load_amps()
    signal, description, zoom_at = accuracy_input()
    outputs = render_both(amps, signal)
    accuracy_figure(amps, outputs, description, zoom_at)
    accuracy_over_time_figure(amps, outputs, description)
    frequency_response_figure(amps)
    drive_figure(amps)
    waveform_figure(amps)
    harmonics_figure(amps)


if __name__ == "__main__":
    main()
