"""Talks to the hardware test firmware (src/hwtest/main.cpp) over USB serial,
or to a simulated loopback for testing the suite without a board."""

import glob
import os
import re
import time
import zlib

import numpy as np

import analysis


class DeviceError(RuntimeError):
    pass


def find_port(timeout=15.0):
    """The one USB serial port, waiting for it to appear after a flash."""
    if os.environ.get("PORT"):
        return os.environ["PORT"]
    deadline = time.time() + timeout
    while True:
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
        if len(ports) == 1:
            return ports[0]
        if len(ports) > 1:
            raise DeviceError("Several USB serial ports; choose one with PORT=...: " + ", ".join(ports))
        if time.time() > deadline:
            raise DeviceError("No USB serial port found. Is the Seed connected and running (not in DFU mode)?")
        time.sleep(0.25)


def parse_fields(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


class SerialDevice:
    """The test firmware's line protocol. Every call waits for its reply."""

    def __init__(self, port, timeout=5.0):
        import serial

        self.port = port
        self.serial = serial.Serial(port, 115200, timeout=timeout)
        self.serial.reset_input_buffer()

    def close(self):
        self.serial.close()

    def _line(self, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.serial.readline()
            if raw:
                line = raw.decode(errors="replace").strip()
                if line:
                    if line.startswith("ERR"):
                        raise DeviceError(line)
                    return line
        raise DeviceError("No reply from the board")

    def _command(self, text, timeout=10.0):
        self.serial.write((text + "\n").encode())
        return self._line(timeout)

    def identify(self, attempts=8):
        """Returns the id fields, retrying while the port settles after boot."""
        for _ in range(attempts):
            self.serial.reset_input_buffer()
            self.serial.write(b"id\n")
            try:
                line = self._line(timeout=1.5)
            except DeviceError:
                continue
            if line.startswith("HWTEST"):
                return parse_fields(line)
        raise DeviceError(f"{self.port} is not running the hardware test firmware (make hwtest-upload)")

    def set_load(self, percent):
        self._command(f"load {int(percent)}")

    def play_record(self, stimulus):
        """Plays stimulus (frames x 2) and returns what both inputs recorded,
        with the firmware's callback timing."""
        stimulus = np.ascontiguousarray(stimulus, dtype="<f4")
        frames = len(stimulus)
        payload = stimulus.tobytes()

        # Upload, then check the board received exactly what was sent.
        self.serial.write(f"put {frames}\n".encode())
        self.serial.write(payload)
        reply = self._line(timeout=30.0 + len(payload) / 200e3)
        expected = f"{zlib.crc32(payload):08x}"
        if parse_fields(reply).get("crc") != expected:
            raise DeviceError(f"Upload corrupted: {reply} (expected crc={expected})")

        return self._receive(f"run {frames}", frames / analysis.FS + 10.0)

    def record_knobs(self, blocks):
        return self._receive(f"knobs {blocks}", blocks / 1000.0 + 10.0)

    def _receive(self, command, timeout):
        header = parse_fields(self._command(command, timeout))
        size = int(header["bytes"])
        data = bytearray()
        deadline = time.time() + timeout
        while len(data) < size and time.time() < deadline:
            data += self.serial.read(size - len(data))
        if len(data) != size or f"{zlib.crc32(data):08x}" != header["crc"]:
            raise DeviceError(f"Recording corrupted or incomplete ({len(data)} of {size} bytes)")
        if self._line() != "END":
            raise DeviceError("Missing END after the recording")
        timing = {k: int(header[k]) for k in ("callbacks", "max_us", "overruns", "load")}
        return np.frombuffer(bytes(data), dtype="<f4").reshape(-1, 2).astype(np.float64), timing


class SimulatedDevice:
    """A loopback from output L to input L with the imperfections the suite
    looks for: latency, gain, AC coupling, a low-pass, noise, hum, mild
    distortion, a little crosstalk, and an optional dropout. The self-test
    (test_hwtest.py) checks that the suite measures them back."""

    def __init__(self, loopback=True, dropout_at=None, seed=1):
        self.loopback = loopback
        self.dropout_at = dropout_at
        self.rng = np.random.default_rng(seed)
        self.load = 0
        self.latency = 107
        self.gain_db = -1.5
        self.noise_dbfs = -100.0
        self.hum_dbfs = -110.0
        self.crosstalk_db = -80.0
        self.port = "simulated"

    def identify(self):
        return {"proto": "1", "board": "simulated", "sr": "48000", "block": "48", "maxframes": "960000", "sdram": "ok"}

    def set_load(self, percent):
        self.load = int(percent)

    def close(self):
        pass

    def _chain(self, x):
        # Gain, a 5 Hz one-pole high-pass, a 22 kHz one-pole low-pass, soft distortion.
        x = x * 10 ** (self.gain_db / 20.0)
        a_hp = np.exp(-2 * np.pi * 5.0 / analysis.FS)
        a_lp = np.exp(-2 * np.pi * 22000.0 / analysis.FS)
        y = np.empty_like(x)
        prev_x = prev_hp = lp = 0.0
        for i, v in enumerate(x):
            prev_hp = a_hp * (prev_hp + v - prev_x)
            prev_x = v
            lp = (1 - a_lp) * prev_hp + a_lp * lp
            y[i] = lp
        return y + 3e-4 * y**2 - 1e-3 * y**3

    def play_record(self, stimulus):
        stimulus = np.asarray(stimulus, dtype=np.float64)
        n = len(stimulus)
        out = np.zeros((n, 2))
        if self.loopback:
            mix = stimulus[:, 0] + 10 ** (self.crosstalk_db / 20.0) * stimulus[:, 1]
            delayed = np.concatenate([np.zeros(self.latency), mix])[:n]
            out[:, 0] = self._chain(delayed)

        # Noise (a little worse under load) and 60 Hz hum on both inputs.
        noise_rms = 10 ** ((self.noise_dbfs + (2.0 if self.load else 0.0)) / 20.0) / np.sqrt(2)
        t = np.arange(n) / analysis.FS
        hum = 10 ** (self.hum_dbfs / 20.0) * np.sin(2 * np.pi * 60.0 * t)
        out += noise_rms * self.rng.standard_normal((n, 2)) + hum[:, None]

        # A dropped block: 48 samples of silence.
        if self.dropout_at is not None and self.dropout_at + 48 < n:
            out[self.dropout_at : self.dropout_at + 48, :] = 0.0
        timing = {"callbacks": n // 48, "max_us": 100 + 8 * self.load, "overruns": 0, "load": self.load}
        return out, timing

    def record_knobs(self, blocks):
        knobs = np.stack([32768 + 40 * self.rng.standard_normal(blocks), 60000 + 40 * self.rng.standard_normal(blocks)], axis=1)
        return np.round(knobs), {"callbacks": blocks, "max_us": 20, "overruns": 0, "load": self.load}
