"""Checks the main firmware on the board through its serial keys and status
line: each amp's callback timing, then a run of model switches. Needs no
changes to the main firmware."""

import re
import time

STATUS = re.compile(r"\[(\w+)\s+(.*?)\s*\]\s+cab (\S+)\s+avg\s+(\d+)% \(\s*(\d+) us\)\s+peak\s+(\d+)% \(\s*(\d+) us\)\s+headroom\s+(-?\d+) us\s+blocks\s+(\d+)\s+overruns (\d+)")
SWITCH = re.compile(r">>> (.*?) (loaded|FAILED to load)$|>>> bypass$")


class FirmwareConsole:
    def __init__(self, port):
        import serial

        self.serial = serial.Serial(port, 115200, timeout=0.2)

    def close(self):
        self.serial.close()

    def lines(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            raw = self.serial.readline()
            if raw:
                yield raw.decode(errors="replace").strip()

    def select(self, key, timeout=10.0):
        """Sends a key and waits for its >>> line. Returns (line, seconds)."""
        self.serial.reset_input_buffer()
        start = time.time()
        self.serial.write(key.encode())
        for line in self.lines(timeout):
            if SWITCH.search(line):
                return line, time.time() - start
        return None, None


def run(port, log=print, seconds_per_amp=5, switches=30):
    console = FirmwareConsole(port)
    try:
        # Skip whatever boot output is still arriving.
        for _ in console.lines(2.0):
            pass

        # Bypass, then each amp: steady-state timing from the status lines.
        # The firmware ignores a key for what is already selected, and it
        # boots into amp 1, so bypass goes first.
        amps = []
        for key in "012345":
            line, _ = console.select(key)
            if line is None:
                amps.append({"key": key, "error": "no reply"})
                continue
            stats = [m for m in (STATUS.search(l) for l in console.lines(seconds_per_amp)) if m]
            # The first status line can include the block that loaded the model.
            stats = stats[1:] or stats
            match = SWITCH.search(line)
            entry = {"key": key, "name": match.group(1) or "bypass", "loaded": "FAILED" not in line}
            if stats:
                entry.update({"avg_us": max(int(m.group(5)) for m in stats), "peak_us": max(int(m.group(7)) for m in stats), "headroom_us": min(int(m.group(8)) for m in stats), "overruns": sum(int(m.group(10)) for m in stats), "status_lines": len(stats)})
            log(f"  {entry.get('name', key)}: peak {entry.get('peak_us')} us, overruns {entry.get('overruns')}")
            amps.append(entry)

        # Model switch stress: cycle through the amps, as fast as they load.
        failures, times = 0, []
        for i in range(switches):
            line, seconds = console.select("12345"[i % 5])
            if line is None or "FAILED" in line:
                failures += 1
            else:
                times.append(seconds)
        log(f"  {switches} switches, {failures} failed")

        # Leave it on the first amp, as it boots.
        console.select("1")
        return {
            "amps": amps,
            "switches": switches,
            "switch_failures": failures,
            "load_failures": sum(1 for a in amps if not a.get("loaded", False)),
            "switch_seconds_max": round(max(times), 3) if times else None,
            "switch_seconds_mean": round(sum(times) / len(times), 3) if times else None,
            "max_peak_us": max((a.get("peak_us", 0) for a in amps), default=None),
            "total_overruns": sum(a.get("overruns", 0) for a in amps),
        }
    finally:
        console.close()
