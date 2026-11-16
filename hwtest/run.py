"""Runs the hardware test suite end to end: flashes the test firmware,
measures, flashes the main firmware back and checks it, then writes the
report. Run through make hwtest; see docs/hardware-tests.md."""

import argparse
import datetime
import json
import os
import pathlib
import subprocess
import sys

import device as dev
import firmware_check
import report
import suite

ROOT = pathlib.Path(__file__).resolve().parent.parent
HERE = pathlib.Path(__file__).resolve().parent


def git_describe():
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def make(target):
    """Runs a make target from the repository root, passing PORT through."""
    print(f"$ make {target}", flush=True)
    subprocess.run(["make", target], cwd=ROOT, check=True)


def write_outputs(results, the_suite, out_dir):
    """Checks, plots, results.json and report.md; safe to call again."""
    checks = report.evaluate(results, HERE / "limits.json")
    results["checks"] = checks
    plots = report.plot(the_suite, out_dir) if the_suite else []
    (out_dir / "results.json").write_text(json.dumps(results, indent=2, default=str) + "\n")
    (out_dir / "report.md").write_text(report.markdown(results, checks, plots))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default=os.environ.get("LABEL", ""), help="name for this run, e.g. rev1-board2")
    parser.add_argument("--notes", default=os.environ.get("NOTES", ""), help="free text saved with the results")
    parser.add_argument("--simulate", action="store_true", help="run against a simulated loopback, no board needed")
    parser.add_argument("--no-flash", action="store_true", help="the test firmware is already running; don't flash anything")
    parser.add_argument("--skip-firmware-check", action="store_true", help="don't flash the main firmware back and check it")
    parser.add_argument("--out", default=str(ROOT / "hwtest" / "results"), help="directory for result folders")
    args = parser.parse_args()

    started = datetime.datetime.now()
    name = started.strftime("%Y-%m-%d_%H%M%S") + (f"_{args.label}" if args.label else "") + ("_sim" if args.simulate else "")
    out_dir = pathlib.Path(args.out) / name
    out_dir.mkdir(parents=True, exist_ok=True)
    results = {"meta": {"label": args.label, "notes": args.notes, "started": started.isoformat(timespec="seconds"), "git": git_describe(), "simulated": args.simulate, "port": None}}

    # Stage 1: the test firmware, measuring the analog chain.
    if args.simulate:
        device = dev.SimulatedDevice()
    else:
        if not args.no_flash:
            make("hwtest-upload")
        device = dev.SerialDevice(dev.find_port())
    results["meta"]["port"] = device.port
    the_suite = suite.Suite(device, out_dir)
    try:
        results.update(the_suite.run())
    except dev.DeviceError as error:
        results["error"] = str(error)
        print(f"Stopped: {error}", file=sys.stderr)
    finally:
        device.close()
    write_outputs(results, the_suite, out_dir)

    # Stage 2: the main firmware back on the board, checked through its keys.
    if not (args.simulate or args.skip_firmware_check or args.no_flash):
        try:
            make("upload")
            print("main firmware: timing per amp, then model switches", flush=True)
            results["firmware"] = firmware_check.run(dev.find_port())
        except (subprocess.CalledProcessError, dev.DeviceError) as error:
            results["firmware_error"] = str(error)
            print(f"Main firmware check skipped: {error}", file=sys.stderr)

    # Report, and point a 'latest' link at it.
    checks = write_outputs(results, the_suite, out_dir)
    latest = out_dir.parent / "latest"
    if latest.is_symlink() or not latest.exists():
        latest.unlink(missing_ok=True)
        latest.symlink_to(out_dir.name)

    failed = [c for c in checks if c["status"] == "FAIL"]
    print()
    for c in checks:
        print(f"  {c['status']:4}  {c['name']}: {report.fmt(c['value'])} ({c['limit']})")
    print(f"\nReport: {out_dir / 'report.md'}")
    return 1 if failed or "error" in results else 0


if __name__ == "__main__":
    # numpy on macOS's Accelerate BLAS warns spuriously after matmul; the
    # self-test checks the results are right.
    import numpy

    numpy.seterr(all="ignore")
    sys.exit(main())
