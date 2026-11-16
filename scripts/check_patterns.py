#!/usr/bin/env python3
"""Fails on code patterns this firmware has measured or ruled against.

Each rule names a pattern and why it is banned in the audio and model code
(src/audio/, src/models/) or in the build files. To keep a use that is
deliberate, put a marker on its line or the line above it:

    // check-patterns: allow <rule>: <why it is fine here>

The reason is required, so the exception explains itself to the next reader.
Run with make check-patterns; make check and CI run it too.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CODE = [p for d in ("src/audio", "src/models") for p in sorted((ROOT / d).glob("*")) if p.suffix in (".cpp", ".h")]
BUILD = [ROOT / "Makefile", *sorted((ROOT / "make").glob("*.mk")), ROOT / "tests/Makefile", *sorted((ROOT / "tests").glob("*.mk"))]

RULES = {
    "mem-copy": (
        CODE,
        re.compile(r"\b(memcpy|memset|memmove|std::copy(_n)?|std::fill(_n)?)\b"),
        "newlib-nano's memcpy and memset copy one byte at a time (docs/performance.md, row 6). "
        "Write a plain loop, or check the object file for library calls and allow it.",
    ),
    "libm": (
        CODE,
        re.compile(r"\b(sin|cos|tan|pow|exp|log|log10|sqrt|tanh|atan|atan2)f?\s*\("),
        "libm's transcendental functions cost about 13 KB of flash and run per sample. "
        "Design coefficients at compile time with constexpr_math.h instead.",
    ),
    "heap": (
        CODE,
        re.compile(r"\bnew\b|\b(malloc|calloc|realloc|free)\s*\(|\bstd::make_(unique|shared)\b"),
        "Process runs in the audio callback and must never allocate. Allocate in Reset or a "
        "load path, and allow it there.",
    ),
    "fast-math": (
        BUILD,
        re.compile(r"-ffast-math|-Ofast|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math"),
        "Relaxed float flags give up bit-exactness with the previous engine and with upstream NAM.",
    ),
}

MARKER = re.compile(r"check-patterns: allow ([\w-]+(?:,\s*[\w-]+)*):\s*\S")


def strip_comments(lines, comment):
    """Returns the lines with comments blanked, so patterns in prose don't count."""
    if comment == "#":
        return [re.sub(r"#.*", "", line) for line in lines]
    code, in_block = [], False
    for line in lines:
        out, i = "", 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    break
                in_block, i = False, end + 2
            elif line.startswith("//", i):
                break
            elif line.startswith("/*", i):
                in_block, i = True, i + 2
            elif line[i] == '"':
                end = line.find('"', i + 1)
                out += '""'
                i = len(line) if end < 0 else end + 1
            else:
                out += line[i]
                i += 1
        code.append(out)
    return code


def allowed(lines, index, rule):
    for line in lines[max(index - 1, 0) : index + 1]:
        match = MARKER.search(line)
        if match and rule in re.split(r",\s*", match.group(1)):
            return True
    return False


def main():
    failures = 0
    for rule, (files, pattern, why) in RULES.items():
        for path in files:
            if not path.exists():
                continue
            lines = path.read_text().splitlines()
            code = strip_comments(lines, "#" if path.suffix in ("", ".mk") else "//")
            for index, line in enumerate(code):
                if pattern.search(line) and not allowed(lines, index, rule):
                    print(f"{path.relative_to(ROOT)}:{index + 1}: [{rule}] {lines[index].strip()}")
                    print(f"    {why}")
                    failures += 1
    if failures:
        print(f"\n{failures} banned pattern(s). Fix them, or mark a deliberate use with\n"
              "    // check-patterns: allow <rule>: <why it is fine here>", file=sys.stderr)
        return 1
    print("No banned patterns.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
