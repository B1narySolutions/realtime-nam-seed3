"""Checks the main firmware on the board through its serial keys and status
line: each amp's callback timing, then a run of model switches. Needs no
changes to the main firmware."""

import re
import time

STATUS = re.compile(r"\[(\w+)\s+(.*?)\s*\]\s+cab (\S+)\s+avg\s+(\d+)% \(\s*(\d+) us\)\s+peak\s+(\d+)% \(\s*(\d+) us\)\s+headroom\s+(-?\d+) us\s+blocks\s+(\d+)\s+overruns (\d+)")
SWITCH = re.compile(r">>> (.*?) (loaded|FAILED to load)$|>>> bypass$")
# Printed by the firmware's status report when something allocated in the
# audio callback, which must never happen.
CALLBACK_ALLOCATIONS = re.compile(r"ERROR: (\d+) allocations in the audio callback")


class FirmwareConsole:
    def __init__(self, port):
        import serial

        self.serial = serial.Serial(port, 115200, timeout=0.2)
        # Callback allocations from every ERROR line read, including those
        # that arrive while waiting for a switch, since the firmware reports
        # each allocation only once.
        self.callback_allocations = 0

    def close(self):
        self.serial.close()

    def lines(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            line = self.read_line()
            if line is not None:
                yield line

    def read_line(self):
        """Reads one line, counting any callback allocations it reports."""
        raw = self.serial.readline()
        if not raw:
            return None
        line = raw.decode(errors="replace").strip()
        match = CALLBACK_ALLOCATIONS.search(line)
        if match:
            self.callback_allocations += int(match.group(1))
        return line

    def select(self, key, timeout=10.0):
        """Sends a key and waits for its >>> line. Returns (line, seconds)."""
        # Read out what is waiting rather than discarding it, so an ERROR line
        # in it still counts.
        while self.serial.in_waiting:
            self.read_line()
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
            allocations_before = console.callback_allocations
            stats = [m for m in (STATUS.search(l) for l in console.lines(seconds_per_amp)) if m]
            callback_allocations = console.callback_allocations - allocations_before
            # The first status line can include the block that loaded the model.
            stats = stats[1:] or stats
            match = SWITCH.search(line)
            entry = {"key": key, "name": match.group(1) or "bypass", "loaded": "FAILED" not in line, "callback_allocations": callback_allocations}
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

        # Leave it on the first amp, as it boots, and wait for a status report
        # to cover the last switches.
        console.select("1")
        for _ in console.lines(1.5):
            pass
        return {
            "amps": amps,
            "switches": switches,
            "switch_failures": failures,
            "load_failures": sum(1 for a in amps if not a.get("loaded", False)),
            "switch_seconds_max": round(max(times), 3) if times else None,
            "switch_seconds_mean": round(sum(times) / len(times), 3) if times else None,
            "max_peak_us": max((a.get("peak_us", 0) for a in amps), default=None),
            "total_overruns": sum(a.get("overruns", 0) for a in amps),
            # Every reported allocation, switches included, not only the per-amp windows.
            "callback_allocations": console.callback_allocations,
        }
    finally:
        console.close()
