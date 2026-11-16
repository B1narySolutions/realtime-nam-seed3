"""The hardware test suite: runs each test against a device (the board or the
simulation), saves the captures, and returns the measurements."""

import struct

import numpy as np

import analysis as an

LOAD_PERCENT = 80


def write_wav(path, frames):
    """32-bit float WAV, readable by Audacity and numpy alike."""
    frames = np.ascontiguousarray(frames, dtype="<f4")
    channels = frames.shape[1]
    data = frames.tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, an.FS, an.FS * channels * 4, channels * 4, 32)
    header += b"data" + struct.pack("<I", len(data))
    path.write_bytes(header + data)


class Suite:
    def __init__(self, device, out_dir, log=print):
        self.device = device
        self.out_dir = out_dir
        self.captures = out_dir / "captures"
        self.captures.mkdir(parents=True, exist_ok=True)
        self.log = log
        self.timing = {"max_us": 0, "overruns": 0, "callbacks": 0}
        self.plots = {}

    def capture(self, name, stimulus, load=0):
        """Plays and records, saves both as WAVs, and tracks callback timing."""
        self.device.set_load(load)
        recording, timing = self.device.play_record(stimulus)
        write_wav(self.captures / f"{name}_stimulus.wav", stimulus)
        write_wav(self.captures / f"{name}_recording.wav", recording)
        self.timing["max_us"] = max(self.timing["max_us"], timing["max_us"])
        self.timing["overruns"] += timing["overruns"]
        self.timing["callbacks"] += timing["callbacks"]
        return recording

    # Each test returns a dict of measurements.

    def routing(self):
        """Which output reaches which input, and the leakage between them."""
        gains = np.zeros((2, 2))
        for out_ch in (0, 1):
            channels = (out_ch == 0, out_ch == 1)
            rec = self.capture(f"routing_out{'LR'[out_ch]}", an.stereo(an.tone(1000.0, -10.0, 0.8), channels))
            for in_ch in (0, 1):
                gains[out_ch, in_ch] = an.tone_metrics(an.settled(rec[:, in_ch], 0.1), 1000.0)["level_dbfs"] + 10.0
        out_ch, in_ch = np.unravel_index(np.argmax(gains), gains.shape)
        loopback = bool(gains[out_ch, in_ch] > -40.0)
        return {
            "loopback": loopback,
            "output": "LR"[out_ch],
            "input": "LR"[in_ch],
            "in_channel": int(in_ch),
            "loop_gain_db": round(float(gains[out_ch, in_ch]), 2),
            "crosstalk_db": round(float(gains[1 - out_ch, in_ch] - gains[out_ch, in_ch]), 1) if loopback else None,
            "gain_matrix_db": {f"out{'LR'[o]}_to_in{'LR'[i]}": round(float(gains[o, i]), 1) for o in (0, 1) for i in (0, 1)},
        }

    def noise(self, in_ch, load):
        """Silence out: the noise floor at the input, with spurs and hum."""
        name = "noise_load" if load else "noise"
        rec = self.capture(name, an.stereo(an.silence(4.0)), load)
        x = an.settled(rec[:, in_ch], 0.5)
        freqs, spectrum = an.averaged_spectrum(x)
        self.plots[name] = (freqs, spectrum)
        return {
            "load_percent": load,
            "noise_dbfs": round(float(an.rms_dbfs(an.band_rms(x))), 1),
            "noise_a_dbfs": round(float(an.rms_dbfs(an.band_rms(x, weighted=True))), 1),
            "dc_offset_fs": round(float(np.mean(x)), 6),
            "hum": an.mains_hum(freqs, spectrum),
            "spurs": an.find_spurs(freqs, spectrum),
            "other_input_noise_a_dbfs": round(float(an.rms_dbfs(an.band_rms(an.settled(rec[:, 1 - in_ch], 0.5), weighted=True))), 1),
        }

    def tones(self, in_ch, noise):
        """SNR from a near-full-scale tone, dynamic range from a -60 dBFS one."""
        loud = an.tone_metrics(an.settled(self.capture("tone_-1dBFS", an.stereo(an.tone(1000.0, -1.0, 2.0)))[:, in_ch]), 1000.0)
        quiet = an.tone_metrics(an.settled(self.capture("tone_-60dBFS", an.stereo(an.tone(1000.0, -60.0, 3.0)))[:, in_ch]), 1000.0)
        # Full-scale out arrives at loud's level + 1 dB; SNR is that over the
        # idle noise. AES17 dynamic range is 60 dB minus the -60 dBFS tone's
        # A-weighted THD+N ratio.
        full_scale = loud["level_dbfs"] + 1.0
        return {
            "full_scale_out_at_input_dbfs": round(full_scale, 2),
            "snr_db": round(full_scale - noise["noise_dbfs"], 1),
            "snr_a_db": round(full_scale - noise["noise_a_dbfs"], 1),
            "dynamic_range_a_db": round(60.0 - quiet["thdn_a_db"], 1),
            "thdn_-1dBFS_db": round(loud["thdn_db"], 1),
            "thdn_-1dBFS_percent": round(100 * 10 ** (loud["thdn_db"] / 20), 4),
            "thd_-1dBFS_db": round(loud["thd_db"], 1),
            "harmonics_-1dBFS_dbc": loud["harmonics_dbc"],
        }

    def linearity(self, in_ch):
        """Stepped 1 kHz tone from -60 to 0 dBFS: gain and THD+N at each level,
        and where the chain starts to clip."""
        levels = np.arange(-60.0, 0.1, 3.0)
        segment = 0.3
        mono = np.concatenate([an.tone(1000.0, level, segment) for level in levels])
        rec = self.capture("linearity", an.stereo(mono))[:, in_ch]
        n = int(segment * an.FS)
        rows = []
        for i, level in enumerate(levels):
            m = an.tone_metrics(an.settled(rec[i * n : (i + 1) * n]), 1000.0)
            rows.append({"out_dbfs": float(level), "in_dbfs": round(m["level_dbfs"], 2), "gain_db": round(m["level_dbfs"] - level, 2), "thdn_db": round(m["thdn_db"], 1), "thd_db": round(m["thd_db"], 1)})
        self.plots["linearity"] = rows
        reference = next(r["gain_db"] for r in rows if r["out_dbfs"] == -21.0)
        compressed = [r["out_dbfs"] for r in rows if r["gain_db"] < reference - 0.5]
        # Clipping shows as harmonics (THD above 1 %); THD+N won't do, since
        # at low levels it is mostly noise.
        clipping = [r["out_dbfs"] for r in rows if r["thd_db"] > -40.0]
        return {
            "rows": rows,
            "gain_spread_db": round(max(r["gain_db"] for r in rows) - min(r["gain_db"] for r in rows), 2),
            "compression_starts_dbfs": compressed[0] if compressed else None,
            "clipping_starts_dbfs": clipping[0] if clipping else None,
        }

    def sweep(self, in_ch):
        """Log sweep: impulse response, latency, polarity, frequency response."""
        sweep = an.log_sweep(10.0, 23000.0, -12.0, 5.0)
        played = np.concatenate([sweep, an.silence(1.5)])
        rec = self.capture("sweep", an.stereo(played))[:, in_ch]
        ir = an.impulse_response(played, rec)
        latency, polarity = an.latency_and_polarity(ir)
        f, mag = an.frequency_response(ir, latency)
        smooth = an.smooth_octave(f, mag)
        self.plots["sweep"] = (f, smooth, ir, latency)
        return {
            "latency_samples": round(float(latency), 2),
            "latency_ms": round(float(latency) / an.FS * 1000.0, 3),
            "polarity": "inverted" if polarity < 0 else "normal",
            **an.response_summary(f, smooth),
        }

    def continuity(self, in_ch):
        """A long tone under CPU load; any dropped or repeated block shows up
        as a burst in the residual."""
        freq = 997.0
        rec = self.capture("continuity", an.stereo(an.tone(freq, -6.0, 12.0)), LOAD_PERCENT)[:, in_ch]
        events, residual_dbfs, threshold_dbfs = an.find_glitches(an.settled(rec, 0.2), freq)
        return {"seconds": 12.0, "load_percent": LOAD_PERCENT, "glitches": len(events), "glitch_samples": events[:20], "residual_dbfs": round(residual_dbfs, 1), "threshold_dbfs": round(threshold_dbfs, 1)}

    def knobs(self):
        """Raw volume and tone ADC readings, held still, once a block."""
        readings, _ = self.device.record_knobs(1000)
        result = {}
        for i, name in enumerate(("volume", "tone")):
            raw = readings[:, i]
            result[name] = {"mean_percent": round(float(raw.mean() / 655.35), 2), "std_percent": round(float(raw.std() / 655.35), 3), "peak_to_peak_percent": round(float(np.ptp(raw) / 655.35), 3), "std_lsb": round(float(raw.std()), 1)}
        return result

    def run(self):
        results = {"device": self.device.identify(), "tests": {}}
        tests = results["tests"]

        # Routing first: the rest analyze the input the loopback reaches.
        self.log("routing: which output reaches which input")
        tests["routing"] = self.routing()
        in_ch = tests["routing"]["in_channel"]
        loopback = tests["routing"]["loopback"]
        if not loopback:
            self.log("  no loopback found: measuring the input alone (plug output into input for the full suite)")

        self.log("noise: idle, then with the CPU loaded")
        tests["noise"] = self.noise(in_ch, 0)
        tests["noise_load"] = self.noise(in_ch, LOAD_PERCENT)
        tests["noise_load"]["increase_a_db"] = round(tests["noise_load"]["noise_a_dbfs"] - tests["noise"]["noise_a_dbfs"], 1)

        if loopback:
            self.log("tones: SNR, THD+N, dynamic range")
            tests["tones"] = self.tones(in_ch, tests["noise"])
            self.log("linearity: -60 to 0 dBFS")
            tests["linearity"] = self.linearity(in_ch)
            self.log("sweep: frequency response and latency")
            tests["sweep"] = self.sweep(in_ch)
            self.log("continuity: 12 s tone under load, looking for dropouts")
            tests["continuity"] = self.continuity(in_ch)

        self.log("knobs: ADC noise")
        tests["knobs"] = self.knobs()
        self.device.set_load(0)
        tests["timing"] = dict(self.timing)
        return results
