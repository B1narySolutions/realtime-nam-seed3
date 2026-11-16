"""Signal generation and measurement for the hardware tests.

Pure numpy, so it runs the same against the board and the simulated device.

Levels are in dBFS where a full-scale sine is 0 dBFS (AES17): a sine's level
is 20 log10(peak), and noise is 20 log10(rms * sqrt(2)). The board's input
and output share one sample clock, so a tone's frequency in the recording is
exactly the one played, and a least-squares fit at that frequency separates
it from noise and distortion without windowing.
"""

import numpy as np

FS = 48000
AUDIO_BAND = (20.0, 20000.0)


def db(ratio):
    return 20.0 * np.log10(np.maximum(ratio, 1e-30))


def rms_dbfs(rms):
    return db(rms * np.sqrt(2.0))


# Stimuli. Each returns float32 frames, shape (n, 2): left, right.


def stereo(mono, channels=(True, True)):
    out = np.zeros((len(mono), 2), dtype=np.float32)
    for ch, on in enumerate(channels):
        if on:
            out[:, ch] = mono
    return out


def fade(x, seconds=0.01):
    """Raised-cosine fade in and out, so a tone starts without a click."""
    n = min(int(seconds * FS), len(x) // 2)
    if n:
        ramp = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, n))
        x = x.copy()
        x[:n] *= ramp
        x[-n:] *= ramp[::-1]
    return x


def tone(freq, dbfs, seconds):
    t = np.arange(int(seconds * FS)) / FS
    return fade(10 ** (dbfs / 20.0) * np.sin(2 * np.pi * freq * t))


def silence(seconds):
    return np.zeros(int(seconds * FS))


def log_sweep(f0, f1, dbfs, seconds):
    """Exponential sine sweep (Farina)."""
    n = int(seconds * FS)
    t = np.arange(n) / FS
    k = np.log(f1 / f0)
    phase = 2 * np.pi * f0 * seconds / k * (np.exp(t * k / seconds) - 1.0)
    return fade(10 ** (dbfs / 20.0) * np.sin(phase), 0.05)


# Spectra and band-limited levels.


def a_weighting(f):
    """IEC 61672 A-weighting as a linear magnitude, 0 dB at 1 kHz."""
    f2 = np.asarray(f, dtype=float) ** 2
    ra = (12194.0**2 * f2**2) / ((f2 + 20.6**2) * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2)) * (f2 + 12194.0**2))
    return ra * 10 ** (2.0 / 20.0)


def band_rms(x, band=AUDIO_BAND, weighted=False):
    """RMS of x within band, from a Hann-windowed periodogram."""
    x = np.asarray(x, dtype=float)
    x = x - x.mean()
    n = len(x)
    w = np.hanning(n)
    power = np.abs(np.fft.rfft(x * w)) ** 2
    power[1:-1] *= 2.0
    f = np.fft.rfftfreq(n, 1.0 / FS)
    keep = (f >= band[0]) & (f <= band[1])
    if weighted:
        power = power * a_weighting(f) ** 2
    return np.sqrt(power[keep].sum() / (n * np.sum(w**2)))


def flat_top(size):
    """HFT95 flat-top window: a sine reads its true level within 0.01 dB
    wherever it falls between bins, which a Hann window misses by up to 1.4 dB."""
    z = 2 * np.pi * np.arange(size) / size
    c = (1.0, -1.9383379, 1.3045202, -0.4028270, 0.0350665)
    return sum(ck * np.cos(k * z) for k, ck in enumerate(c))


def averaged_spectrum(x, size=16384):
    """Welch average of flat-top segments, scaled so a sine reads its level
    in dBFS. The noise floor reads per bin, so it sits far below the band's
    total noise."""
    x = np.asarray(x, dtype=float)
    x = x - x.mean()
    w = flat_top(size)
    hop = size // 2
    starts = range(0, max(len(x) - size, 0) + 1, hop)
    power = np.mean([np.abs(np.fft.rfft(x[s : s + size] * w)) ** 2 for s in starts], axis=0)
    amplitude = 2.0 * np.sqrt(power) / w.sum()
    return np.fft.rfftfreq(size, 1.0 / FS), db(amplitude)


def find_spurs(freqs, level_db, threshold_db=12.0, count=8, band=AUDIO_BAND):
    """Peaks that stand above the local noise floor: hum, regulators, clocks."""
    floor = np.empty_like(level_db)
    half = 32
    for i in range(len(level_db)):
        floor[i] = np.median(level_db[max(0, i - half) : i + half + 1])
    spurs = []
    for i in range(1, len(level_db) - 1):
        if not band[0] <= freqs[i] <= band[1]:
            continue
        if level_db[i] >= level_db[i - 1] and level_db[i] > level_db[i + 1] and level_db[i] - floor[i] > threshold_db:
            spurs.append({"freq_hz": round(float(freqs[i]), 1), "level_dbfs": round(float(level_db[i]), 1), "above_floor_db": round(float(level_db[i] - floor[i]), 1)})
    spurs.sort(key=lambda s: s["level_dbfs"], reverse=True)
    return spurs[:count]


def mains_hum(freqs, level_db):
    """Strongest line at 50 or 60 Hz and their harmonics up to 1 kHz."""
    best = {"freq_hz": None, "level_dbfs": -200.0}
    for mains in (50.0, 60.0):
        for k in range(1, int(1000 // mains) + 1):
            i = int(np.argmin(np.abs(freqs - k * mains)))
            peak = level_db[max(0, i - 2) : i + 3].max()
            if peak > best["level_dbfs"]:
                best = {"freq_hz": k * mains, "level_dbfs": round(float(peak), 1)}
    return best


# Tone measurements.


def fit_tone(x, freq, harmonics=1):
    """Least-squares fit of DC plus the tone and its harmonics. Returns the
    peak amplitude of each harmonic (index 0 is the fundamental), the fitted
    signal, and the residual."""
    x = np.asarray(x, dtype=float)
    t = np.arange(len(x)) / FS
    columns = [np.ones_like(t)]
    for k in range(1, harmonics + 1):
        if k * freq < FS / 2:
            columns += [np.cos(2 * np.pi * k * freq * t), np.sin(2 * np.pi * k * freq * t)]
    basis = np.stack(columns, axis=1)
    coef, *_ = np.linalg.lstsq(basis, x, rcond=None)
    fitted = basis @ coef
    amplitudes = np.hypot(coef[1::2], coef[2::2])
    return amplitudes, fitted, x - fitted


def tone_metrics(x, freq):
    """Level, gain-independent THD+N and THD of a recorded tone."""
    amplitudes, _, residual = fit_tone(x, freq, harmonics=1)
    level = amplitudes[0]
    harmonic_amps, _, _ = fit_tone(x, freq, harmonics=10)
    thd = np.sqrt(np.sum(harmonic_amps[1:] ** 2)) / max(harmonic_amps[0], 1e-30)
    return {
        "level_dbfs": float(db(level)),
        "thdn_db": float(db(band_rms(residual) * np.sqrt(2) / max(level, 1e-30))),
        "thdn_a_db": float(db(band_rms(residual, weighted=True) * np.sqrt(2) / max(level, 1e-30))),
        "thd_db": float(db(thd)),
        "harmonics_dbc": [round(float(db(a / max(harmonic_amps[0], 1e-30))), 1) for a in harmonic_amps[1:6]],
    }


def settled(x, skip_seconds=0.05):
    """Drops the start and end of a segment, where the fades and the analog
    coupling capacitors are still settling."""
    n = int(skip_seconds * FS)
    return x[n : len(x) - n]


# Sweep measurements.


def impulse_response(played, recorded):
    """Deconvolves the recording by the stimulus (both mono), regularized
    outside the sweep's band."""
    n = 1 << int(np.ceil(np.log2(len(played) + len(recorded))))
    p = np.fft.rfft(played, n)
    r = np.fft.rfft(recorded, n)
    power = np.abs(p) ** 2
    h = r * np.conj(p) / (power + 1e-6 * power.max())
    return np.fft.irfft(h, n)


def latency_and_polarity(ir):
    """Delay to the impulse response's peak, with parabolic interpolation."""
    peak = int(np.argmax(np.abs(ir[: len(ir) // 2])))
    y0, y1, y2 = np.abs(ir[peak - 1 : peak + 2]) if peak > 0 else (0.0, abs(ir[peak]), 0.0)
    denom = y0 - 2 * y1 + y2
    offset = 0.5 * (y0 - y2) / denom if denom != 0 else 0.0
    return peak + offset, 1 if ir[peak] > 0 else -1


def frequency_response(ir, latency_samples, size=65536):
    """Magnitude response from a window of the impulse response starting just
    before its peak, normalized to 0 dB at 1 kHz. Long enough to resolve the
    coupling capacitors' high-pass corners."""
    start = max(int(latency_samples) - 256, 0)
    segment = ir[start : start + size].copy()
    taper = int(0.1 * len(segment))
    segment[-taper:] *= np.hanning(2 * taper)[taper:]
    spectrum = np.abs(np.fft.rfft(segment, size))
    f = np.fft.rfftfreq(size, 1.0 / FS)
    mag = db(spectrum)
    mag -= np.interp(1000.0, f, mag)
    return f, mag


def smooth_octave(f, mag, fraction=12):
    """1/fraction-octave smoothing of a dB magnitude curve, for plots and corners."""
    out = np.empty_like(mag)
    factor = 2 ** (0.5 / fraction)
    power = 10 ** (mag / 10)
    cumulative = np.concatenate([[0.0], np.cumsum(power)])
    lo = np.searchsorted(f, f / factor)
    hi = np.searchsorted(f, f * factor, side="right")
    hi = np.maximum(hi, lo + 1)
    out = 10 * np.log10((cumulative[hi] - cumulative[lo]) / (hi - lo))
    return out


def response_summary(f, mag_smoothed):
    def deviation(lo, hi):
        sel = (f >= lo) & (f <= hi)
        return float(mag_smoothed[sel].min()), float(mag_smoothed[sel].max())

    def corner(direction):
        # Walk out from 1 kHz until the response falls 3 dB.
        i = int(np.searchsorted(f, 1000.0))
        step = -1 if direction == "low" else 1
        while 0 < i < len(f) - 1 and mag_smoothed[i] > -3.0 and 5.0 < f[i] < 23900.0:
            i += step
        return float(f[i]) if mag_smoothed[i] <= -3.0 else None

    lo20, hi20 = deviation(20.0, 20000.0)
    lo40, hi40 = deviation(40.0, 15000.0)
    return {"low_corner_hz": corner("low"), "high_corner_hz": corner("high"), "dev_20_20k_db": [round(lo20, 2), round(hi20, 2)], "dev_40_15k_db": [round(lo40, 2), round(hi40, 2)]}


# Continuity: dropped, repeated or corrupted samples.


def find_glitches(x, freq, threshold_dbfs=-60.0):
    """Fits the tone and its harmonics over the whole recording; a dropout or
    a repeated block leaves a burst in the residual far above the noise."""
    _, _, residual = fit_tone(x, freq, harmonics=10)
    noise = np.std(residual)
    threshold = max(10 ** (threshold_dbfs / 20.0), 8.0 * noise)
    hits = np.flatnonzero(np.abs(residual) > threshold)
    events = []
    for i in hits:
        if not events or i - events[-1] > 48:
            events.append(int(i))
    return events, float(rms_dbfs(noise)), float(db(threshold))
