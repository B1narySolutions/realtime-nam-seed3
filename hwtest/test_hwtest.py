"""Self-test of the hardware suite: runs it against the simulated loopback in
device.py and checks it measures back what the simulation put in. Run with
make hwtest-sim."""

import pathlib
import sys
import tempfile

import numpy as np

import analysis as an
import device as dev
import report
import run
import suite


def close(name, value, expected, tolerance):
    ok = value is not None and abs(value - expected) <= tolerance
    print(f"  {'ok  ' if ok else 'FAIL'} {name}: {value} (expected {expected} ± {tolerance})")
    return ok


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {name} {detail}")
    return ok


def main():
    results_ok = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        # The full suite through the simulated loopback.
        print("simulated loopback:")
        sim = dev.SimulatedDevice()
        the_suite = suite.Suite(sim, tmp / "full", log=lambda *_: None)
        results = {"meta": {"label": "sim", "notes": "", "started": "now", "git": "test", "port": "simulated"}}
        results.update(the_suite.run())
        t = results["tests"]
        # White noise loses this much to A-weighting over 20 Hz-20 kHz.
        f = np.linspace(20.0, 20000.0, 100000)
        expected_noise_a = sim.noise_dbfs + 10 * np.log10(np.mean(an.a_weighting(f) ** 2))
        results_ok += [
            check("loopback found on L to L", t["routing"]["loopback"] and t["routing"]["output"] == "L" and t["routing"]["input"] == "L"),
            close("loop gain dB", t["routing"]["loop_gain_db"], sim.gain_db, 0.2),
            close("crosstalk dB", t["routing"]["crosstalk_db"], sim.crosstalk_db, 2.0),
            close("latency samples", t["sweep"]["latency_samples"], sim.latency, 1.5),
            close("noise dBFS", t["noise"]["noise_dbfs"], sim.noise_dbfs, 1.0),
            close("noise A dBFS", t["noise"]["noise_a_dbfs"], round(expected_noise_a, 1), 1.0),
            close("noise increase under load dB", t["noise_load"]["increase_a_db"], 2.0, 0.7),
            close("hum dBFS", t["noise"]["hum"]["level_dbfs"], sim.hum_dbfs, 0.5),
            close("hum frequency", t["noise"]["hum"]["freq_hz"], 60.0, 0.1),
            close("SNR A dB", t["tones"]["snr_a_db"], round(sim.gain_db - expected_noise_a, 1), 1.0),
            close("dynamic range A dB", t["tones"]["dynamic_range_a_db"], round(sim.gain_db - expected_noise_a, 1), 1.5),
            check("THD+N at -1 dBFS below -60 dB", t["tones"]["thdn_-1dBFS_db"] < -60, str(t["tones"]["thdn_-1dBFS_db"])),
            close("low corner Hz", t["sweep"]["low_corner_hz"], 5.0, 1.5),
            check("high corner 15-24 kHz", 15000 <= (t["sweep"]["high_corner_hz"] or 0) <= 24000, str(t["sweep"]["high_corner_hz"])),
            close("gain spread dB", t["linearity"]["gain_spread_db"], 0.0, 0.3),
            check("no glitches without a dropout", t["continuity"]["glitches"] == 0, str(t["continuity"]["glitches"])),
            close("knob noise LSB", t["knobs"]["volume"]["std_lsb"], 40.0, 8.0),
        ]

        # Checks, plots and the report render.
        checks = run.write_outputs(results, the_suite, tmp / "full")
        results_ok.append(check("report written", (tmp / "full" / "report.md").stat().st_size > 1000))
        results_ok.append(check("plots written", len(list((tmp / "full").glob("*.png"))) == 4))
        results_ok.append(check("no failed checks", not [c for c in checks if c["status"] == "FAIL"], str([c["name"] for c in checks if c["status"] == "FAIL"])))

        # A dropped block must be caught.
        print("dropout:")
        the_suite = suite.Suite(dev.SimulatedDevice(dropout_at=5 * 48000 + 17), tmp / "dropout", log=lambda *_: None)
        results_ok.append(check("one glitch found", the_suite.continuity(0)["glitches"] == 1))

        # No cable: input-only tests run, the rest are skipped, not failed.
        print("no loopback:")
        the_suite = suite.Suite(dev.SimulatedDevice(loopback=False), tmp / "open", log=lambda *_: None)
        results = {"meta": {"label": "", "notes": "", "started": "now", "git": "test", "port": "simulated"}}
        results.update(the_suite.run())
        checks = {c["name"]: c["status"] for c in run.write_outputs(results, the_suite, tmp / "open")}
        results_ok += [
            check("loopback not found", not results["tests"]["routing"]["loopback"]),
            check("loopback check fails", checks["Loopback cable found"] == "FAIL"),
            check("SNR skipped", checks["SNR, A-weighted"] == "SKIP"),
            check("noise still measured", checks["Idle noise, A-weighted"] in ("PASS", "FAIL")),
        ]

    print(f"\n{sum(results_ok)} of {len(results_ok)} checks passed")
    return 0 if all(results_ok) else 1


if __name__ == "__main__":
    np.seterr(all="ignore")
    sys.exit(main())
