"""Turns a run's results into pass/fail checks, plots and a Markdown report."""

import json

import numpy as np

import analysis as an
from suite import LOAD_PERCENT

MISSING = object()


def lookup(results, path):
    value = results
    for key in path.split("."):
        if isinstance(value, list) and key.isdigit() and int(key) < len(value):
            value = value[int(key)]
        elif isinstance(value, dict) and key in value:
            value = value[key]
        else:
            return MISSING
    return value


def evaluate(results, limits_path):
    """One row per limit: its value, the limit, and PASS, FAIL or SKIP (when
    the test didn't run, e.g. no loopback cable or no firmware check)."""
    checks = []
    for limit in json.loads(limits_path.read_text())["checks"]:
        value = lookup(results, limit["path"])
        if value is MISSING:
            status, text = "SKIP", "not measured"
        else:
            if "equals" in limit:
                ok, text = value == limit["equals"], f"= {json.dumps(limit['equals'])}"
            elif "abs_max" in limit:
                ok, text = value is not None and abs(value) <= limit["abs_max"], f"|x| ≤ {limit['abs_max']}"
            elif "min" in limit:
                ok, text = value is not None and value >= limit["min"], f"≥ {limit['min']}"
            else:
                ok, text = value is not None and value <= limit["max"], f"≤ {limit['max']}"
            status = "PASS" if ok else "FAIL"
            text = f"{text} {limit.get('unit', '')}".strip()
        checks.append({"name": limit["name"], "path": limit["path"], "value": None if value is MISSING else value, "limit": text, "status": status, "why": limit.get("why", "")})
    return checks


def plot(suite, out_dir):
    """Saves the plots this run has data for. Returns their file names."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    saved = []
    plots = suite.plots

    if "noise" in plots:
        fig, ax = plt.subplots(figsize=(9, 4.5))
        for key, label in (("noise", "idle"), ("noise_load", f"CPU at {LOAD_PERCENT} %")):
            if key in plots:
                f, level = plots[key]
                ax.semilogx(f[1:], level[1:], label=label, linewidth=0.8)
        ax.set(xlim=(10, 24000), xlabel="Frequency (Hz)", ylabel="Level (dBFS, sine-calibrated)", title="Input noise spectrum, silence out")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend()
        saved.append(save(fig, out_dir, "noise_spectrum.png"))

    if "sweep" in plots:
        f, smooth, ir, latency = plots["sweep"]
        fig, ax = plt.subplots(figsize=(9, 4.5))
        sel = (f >= 10) & (f <= 23000)
        ax.semilogx(f[sel], smooth[sel])
        ax.set(xlim=(10, 24000), ylim=(-12, 3), xlabel="Frequency (Hz)", ylabel="dB re 1 kHz", title="Loopback frequency response (1/12 octave)")
        ax.grid(True, which="both", alpha=0.3)
        saved.append(save(fig, out_dir, "frequency_response.png"))

        fig, ax = plt.subplots(figsize=(9, 3.5))
        start = max(int(latency) - 48, 0)
        t = (np.arange(start, start + 480) / an.FS) * 1000
        ax.plot(t, ir[start : start + 480])
        ax.axvline(latency / an.FS * 1000, color="gray", linestyle=":")
        ax.set(xlabel="Time after the stimulus (ms)", ylabel="Amplitude", title=f"Impulse response: latency {latency / an.FS * 1000:.2f} ms")
        ax.grid(True, alpha=0.3)
        saved.append(save(fig, out_dir, "impulse_response.png"))

    if "linearity" in plots:
        rows = plots["linearity"]
        fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4))
        out_levels = [r["out_dbfs"] for r in rows]
        a1.plot(out_levels, [r["gain_db"] for r in rows], marker="o")
        a1.set(xlabel="Output level (dBFS)", ylabel="Loop gain (dB)", title="Gain vs level")
        a2.plot(out_levels, [r["thdn_db"] for r in rows], marker="o")
        a2.set(xlabel="Output level (dBFS)", ylabel="THD+N (dB)", title="THD+N vs level, 1 kHz")
        for a in (a1, a2):
            a.grid(True, alpha=0.3)
        saved.append(save(fig, out_dir, "linearity.png"))
    return saved


def save(fig, out_dir, name):
    fig.tight_layout()
    fig.savefig(out_dir / name, dpi=130)
    import matplotlib.pyplot as plt

    plt.close(fig)
    return name


def fmt(value):
    if isinstance(value, float):
        return f"{value:g}"
    if value is None:
        return "none"
    return str(value)


def markdown(results, checks, plots):
    meta = results["meta"]
    tests = results.get("tests", {})
    counts = {s: sum(c["status"] == s for c in checks) for s in ("PASS", "FAIL", "SKIP")}
    lines = [
        f"# Hardware test: {meta['label'] or meta['started']}",
        "",
        f"- **When:** {meta['started']}",
        f"- **Firmware commit:** {meta['git']}",
        f"- **Device:** {meta['port']} ({', '.join(f'{k}={v}' for k, v in results.get('device', {}).items())})",
    ]
    if meta.get("notes"):
        lines.append(f"- **Notes:** {meta['notes']}")
    lines += ["", f"**{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped.** Limits are in `hwtest/limits.json`.", "", "| Check | Value | Limit | Result |", "|---|---|---|---|"]
    for c in checks:
        mark = {"PASS": "✅ PASS", "FAIL": "❌ FAIL", "SKIP": "➖ SKIP"}[c["status"]]
        lines.append(f"| {c['name']} | {fmt(c['value'])} | {c['limit']} | {mark} |")

    routing = tests.get("routing")
    if routing:
        lines += ["", "## Setup found", "", f"Loopback {'found' if routing['loopback'] else '**not found**'}: output {routing['output']} → input {routing['input']}, loop gain {routing['loop_gain_db']} dB."]
        lines.append("Gains (dB): " + ", ".join(f"{k} {v}" for k, v in routing["gain_matrix_db"].items()))
    if "noise" in tests:
        n, nl = tests["noise"], tests["noise_load"]
        lines += ["", "## Noise", "", f"Idle: {n['noise_dbfs']} dBFS unweighted, {n['noise_a_dbfs']} dBFS A-weighted, DC {n['dc_offset_fs']} FS. With the CPU at {nl['load_percent']} %: {nl['noise_a_dbfs']} dBFS A-weighted ({nl['increase_a_db']:+} dB).", f"Strongest mains line: {n['hum']['level_dbfs']} dBFS at {n['hum']['freq_hz']} Hz.", "", "Spurs (idle):", ""]
        lines += [f"- {s['freq_hz']} Hz at {s['level_dbfs']} dBFS ({s['above_floor_db']} dB above the floor)" for s in n["spurs"]] or ["- none"]
    if "tones" in tests:
        t = tests["tones"]
        lines += ["", "## SNR and distortion", "", f"Full-scale output arrives at {t['full_scale_out_at_input_dbfs']} dBFS. SNR {t['snr_db']} dB unweighted, {t['snr_a_db']} dB A-weighted; dynamic range {t['dynamic_range_a_db']} dB (AES17).", f"At -1 dBFS: THD+N {t['thdn_-1dBFS_db']} dB ({t['thdn_-1dBFS_percent']} %), THD {t['thd_-1dBFS_db']} dB, harmonics 2-6: {t['harmonics_-1dBFS_dbc']} dBc."]
    if "sweep" in tests:
        s = tests["sweep"]
        lines += ["", "## Frequency response and latency", "", f"Latency {s['latency_ms']} ms ({s['latency_samples']} samples), polarity {s['polarity']}. -3 dB corners: {fmt(s['low_corner_hz'])} Hz and {fmt(s['high_corner_hz'])} Hz. Deviation 20 Hz-20 kHz: {s['dev_20_20k_db']} dB; 40 Hz-15 kHz: {s['dev_40_15k_db']} dB."]
    if "continuity" in tests:
        c = tests["continuity"]
        lines += ["", "## Continuity", "", f"{c['glitches']} glitches in {c['seconds']} s with the CPU at {c['load_percent']} % (residual {c['residual_dbfs']} dBFS, threshold {c['threshold_dbfs']} dBFS)."]
    if "knobs" in tests:
        lines += ["", "## Knobs", ""] + [f"- {k}: at {v['mean_percent']} %, noise {v['std_percent']} % rms, {v['peak_to_peak_percent']} % peak to peak ({v['std_lsb']} LSB rms)" for k, v in tests["knobs"].items()]
    if "timing" in tests:
        t = tests["timing"]
        lines += ["", f"Test firmware callbacks: {t['callbacks']}, worst {t['max_us']} us, {t['overruns']} overruns."]
    fw = results.get("firmware")
    if fw:
        lines += ["", "## Main firmware", "", "| Mode | Avg (us) | Peak (us) | Headroom (us) | Overruns |", "|---|---|---|---|---|"]
        lines += [f"| {a.get('name', a['key'])} | {fmt(a.get('avg_us'))} | {fmt(a.get('peak_us'))} | {fmt(a.get('headroom_us'))} | {fmt(a.get('overruns'))} |" for a in fw["amps"]]
        lines += ["", f"{fw['switches']} model switches, {fw['switch_failures']} failed; slowest {fmt(fw['switch_seconds_max'])} s as seen from the host."]
    if plots:
        lines += ["", "## Plots", ""] + [f"![{p}]({p})" for p in plots]
    lines += ["", "Captures (stimulus and recording, 32-bit float WAV) are in `captures/`."]
    return "\n".join(lines) + "\n"
