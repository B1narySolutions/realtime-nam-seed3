#!/usr/bin/env python3
"""Fails when a firmware image outgrows its budget in make/size_limits.json.

Flash is the tight one: the STM32H750 has 128 KB, and the main firmware with
its five embedded models uses most of it. The budgets sit a little above
today's use, so an unplanned jump (libm pulled in by a stray sin(), a new
dependency) fails here instead of at the linker once flash is full. When the
growth is intended, raise the budget in the same change and say why.

RAM figures are static use only (.data, .bss and the like). The models'
delay lines are heap in SRAM; the host tests check those (kModelHeapBudget in
tests/a2_test.cpp).

Usage: check_size.py [--report]   (--report prints use without checking)
"""

import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINKER_SCRIPT = ROOT / "libs/libDaisy/core/STM32H750IB_flash.lds"
LIMITS = ROOT / "make/size_limits.json"
UNITS = {"K": 1024, "M": 1024 * 1024, "": 1}


def memory_regions():
    """Reads the MEMORY block of the linker script: name -> (origin, length)."""
    text = LINKER_SCRIPT.read_text()
    block = re.search(r"MEMORY\s*\{(.*?)\}", text, re.S).group(1)
    regions = {}
    for name, origin, length, unit in re.findall(r"(\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*(0x[0-9a-fA-F]+),\s*LENGTH\s*=\s*(\d+)([KM]?)", block):
        regions[name] = (int(origin, 16), int(length) * UNITS[unit])
    return regions


def sections(elf):
    """Yields (name, size, vma, lma, flags) for each section objdump lists."""
    out = subprocess.run(["arm-none-eabi-objdump", "-h", str(elf)], check=True, capture_output=True, text=True).stdout
    lines = out.splitlines()
    for i, line in enumerate(lines):
        match = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s", line)
        if match and i + 1 < len(lines):
            name, size, vma, lma = match.groups()
            yield name, int(size, 16), int(vma, 16), int(lma, 16), lines[i + 1]


def usage(elf, regions):
    """Bytes per region: flash counts everything loaded from it (code, constants
    and .data's initial values); RAM regions count what is placed in them."""
    used = {name: 0 for name in regions}

    def region_of(address):
        return next((name for name, (origin, length) in regions.items() if origin <= address < origin + length), None)

    for _, size, vma, lma, flags in sections(elf):
        if "ALLOC" not in flags or size == 0:
            continue
        flash_lma = region_of(lma) == "FLASH" and "LOAD" in flags
        if flash_lma:
            used["FLASH"] += size
        target = region_of(vma)
        if target and target != "FLASH":
            used[target] += size
    return used


def main():
    report_only = "--report" in sys.argv
    regions = memory_regions()
    limits = json.loads(LIMITS.read_text())
    failures = 0
    for image, budgets in limits["images"].items():
        elf = ROOT / image
        if not elf.exists():
            print(f"{image}: missing; build it first (make build, make hwtest-build).", file=sys.stderr)
            return 1
        used = usage(elf, regions)
        print(f"{image}:")
        for region, budget in budgets.items():
            capacity = regions[region][1]
            over = used[region] > budget
            failures += over and not report_only
            mark = "OVER BUDGET" if over else "ok"
            print(f"  {region:<8} {used[region]:>8} bytes  budget {budget:>8}  of {capacity:>9}  ({100 * used[region] / capacity:5.1f}% full)  {mark}")
    if failures:
        print(f"\n{failures} region(s) over budget. Shrink the change, or raise the budget in "
              f"{LIMITS.relative_to(ROOT)} and say why in the commit.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
