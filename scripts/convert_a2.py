#!/usr/bin/env python3
"""Embed the required 48 kHz A2-Lite models without a runtime JSON parser.

models/amps.json lists the amps in AmpId order and is the only place that
order is written down.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

MANIFEST = Path("models/amps.json")

# Network shape. The generated header static_asserts that it matches
# src/models/a2_lite.h.
CHANNELS = 3
HEAD_TAPS = 16
KERNELS = [6] * 14 + [15, 15] + [6] * 7
DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
LAYERS = len(KERNELS)
# Per-layer weights after the conv taps: bias, conditioning, residual matrix, residual bias.
LAYER_TAIL = 3 * CHANNELS + CHANNELS * CHANNELS
WEIGHT_COUNT = (CHANNELS + sum(CHANNELS * CHANNELS * k + LAYER_TAIL for k in KERNELS)
                + HEAD_TAPS * CHANNELS + 2)


def expected_layer():
    layer = dict(input_size=1, condition_size=1, channels=CHANNELS, bottleneck=CHANNELS,
                 head=dict(out_channels=1, kernel_size=HEAD_TAPS, bias=True),
                 kernel_sizes=KERNELS, dilations=DILATIONS,
                 activation=[dict(type="LeakyReLU", negative_slope=0.01)] * LAYERS,
                 head1x1=dict(active=False, out_channels=1, groups=1),
                 layer1x1=dict(active=True, groups=1), groups_input=1,
                 groups_input_mixin=1, gating_mode=["none"] * LAYERS,
                 secondary_activation=[None] * LAYERS, slimmable=None)
    for name in ("conv_pre", "conv_post", "input_mixin_pre", "input_mixin_post",
                 "activation_pre", "activation_post", "layer1x1_post", "head1x1_post"):
        layer[name + "_film"] = dict(active=False, shift=True, groups=1)
    return layer


def validate(model, path):
    """Reject anything but the exact A2-Lite architecture the engine implements."""
    if (model.get("version") != "0.7.0" or model.get("architecture") != "WaveNet"
            or model.get("sample_rate") != 48000):
        raise ValueError(f"{path}: expected NAM 0.7.0, 48 kHz WaveNet")
    config = model["config"]
    if (set(config) != {"layers", "head", "head_scale"}
            or config["layers"] != [expected_layer()] or config["head"] is not None):
        raise ValueError(f"{path}: unsupported A2-Lite configuration")
    if len(model["weights"]) != WEIGHT_COUNT:
        raise ValueError(f"{path}: expected {WEIGHT_COUNT} weights")


def check_head_scale(model, floats, path):
    """The scale is stored twice; the engine uses the weight-stream copy."""
    scale = model["config"]["head_scale"]
    if to_float32([scale], path)[0] != floats[-1]:
        raise ValueError(f"{path}: head_scale {scale} does not match the weight stream")


def to_float32(weights, path):
    floats = []
    for value in weights:
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError(f"{path}: invalid weight")
        value = struct.unpack("<f", struct.pack("<f", value))[0]
        if not math.isfinite(value):
            raise ValueError(f"{path}: weight outside float32 range")
        floats.append(value)
    return floats


def pack(floats):
    """Reorder conv taps from NAM's [out][in][tap] to the engine's [tap][in][out]."""
    packed = floats[:CHANNELS]
    offset = CHANNELS
    for kernel in KERNELS:
        packed += [floats[offset + (out * CHANNELS + inp) * kernel + tap]
                   for tap in range(kernel) for inp in range(CHANNELS) for out in range(CHANNELS)]
        offset += CHANNELS * CHANNELS * kernel
        packed += floats[offset:offset + LAYER_TAIL]
        offset += LAYER_TAIL
    packed += [floats[offset + inp * HEAD_TAPS + tap]
               for tap in range(HEAD_TAPS) for inp in range(CHANNELS)]
    offset += HEAD_TAPS * CHANNELS
    packed += floats[offset:]  # head bias and authoritative weight-stream scale
    assert len(packed) == WEIGHT_COUNT
    return packed


def convert(path):
    raw = path.read_bytes()
    model = json.loads(raw)
    validate(model, path)
    floats = to_float32(model["weights"], path)
    check_head_scale(model, floats, path)
    return pack(floats), hashlib.sha256(raw).hexdigest()


def load_manifest(path=MANIFEST):
    amps = json.loads(path.read_text())
    for amp in amps:
        if not (amp["amp_id"].isidentifier() and amp["file"].endswith(".nam")):
            raise ValueError(f"{path}: bad entry {amp}")
    return amps


def shape_asserts():
    """Fail the firmware build if the engine's shape drifts from the converter's."""
    def all_equal(name, values):
        return " && ".join(f"a2_lite::{name}[{i}] == {v}" for i, v in enumerate(values))
    message = '"a2_lite.h does not match the shape validated by convert_a2.py"'
    return [f"static_assert(a2_lite::kChannels == {CHANNELS} && a2_lite::kLayers == {LAYERS}"
            f" && a2_lite::kHeadTaps == {HEAD_TAPS} && A2Lite::kWeights == {WEIGHT_COUNT}, {message});",
            f"static_assert({all_equal('kKernelSizes', KERNELS)}, {message});",
            f"static_assert({all_equal('kDilations', DILATIONS)}, {message});"]


def header(amps, models_dir):
    lines = ["// Generated by scripts/convert_a2.py from models/amps.json; do not",
             "// redistribute model weights.",
             "#pragma once", '#include "models/a2_lite.h"', '#include "models/amp_models.h"',
             "#include <cstddef>", "#include <iterator>", "",
             "namespace embedded_a2 {"] + shape_asserts()
    for amp in amps:
        weights, digest = convert(models_dir / amp["file"])
        lines += [f"// {amp['file']}; SHA-256 {digest}",
                  f"inline constexpr float k{amp['amp_id']}[] = {{"]
        lines += ["  " + ", ".join(x.hex() + "f" for x in weights[i:i + 4]) + ","
                  for i in range(0, len(weights), 4)]
        lines += ["};"]
    lines += ["", "struct Model {", "    AmpId id;", "    const char *name;",
              "    const float *weights;", "    std::size_t weight_count;", "};", "",
              "// In AmpId order.", "inline constexpr Model kModels[] = {"]
    lines += [f"    {{AmpId::{amp['amp_id']}, {json.dumps(amp['name'])}, k{amp['amp_id']}, "
              f"std::size(k{amp['amp_id']})}}," for amp in amps]
    lines += ["};", "} // namespace embedded_a2", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("names", help="print model file stems in AmpId order")
    write = commands.add_parser("header", help="write the embedded weights header")
    write.add_argument("output", type=Path)
    write.add_argument("--models", type=Path, default=Path("models/local"))
    args = parser.parse_args()
    try:
        amps = load_manifest()
        if args.command == "names":
            print(" ".join(amp["file"][:-len(".nam")] for amp in amps))
            return
        text = header(amps, args.models)
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == "__main__":
    main()
