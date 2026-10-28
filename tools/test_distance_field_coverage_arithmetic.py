"""CPU arithmetic regressions for conservative GPU material-coverage bounds.

Run with any Python 3 interpreter. This reproduces the unsafe classifications
without numpy, then checks the native bound formulas against float32 evaluation
at deterministic interior barycentrics. It does not execute GLSL, prove every
compiler contraction, or replace the corresponding native coverage fixtures.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import random
import struct


EPSILON = 2.0 ** -23
ROOT = Path(__file__).resolve().parents[1]


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def integer_float32(value: float) -> tuple[int, int]:
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    exponent = (bits >> 23) & 255
    significand = (bits & 0x7FFFFF) | (0x800000 if exponent else 0)
    return (-significand if bits >> 31 else significand), (exponent - 150 if exponent else -149)


def fma32(a: float, b: float, c: float) -> float:
    """Exact integer product/sum followed by one binary32 ties-to-even rounding."""
    ia, ea = integer_float32(a)
    ib, eb = integer_float32(b)
    ic, ec = integer_float32(c)
    common = min(ea + eb, ec)
    exact = (ia * ib << (ea + eb - common)) + (ic << (ec - common))
    magnitude = abs(exact)
    shift = max(magnitude.bit_length() - 24, -149 - common, 0)
    rounded = magnitude >> shift
    if shift:
        remainder = magnitude - (rounded << shift)
        halfway = 1 << (shift - 1)
        if remainder > halfway or (remainder == halfway and rounded & 1):
            rounded += 1
    return f32(math.ldexp(-rounded if exact < 0 else rounded, common + shift))


def accumulate(weights: tuple[float, ...], values: tuple[float, ...], fused: bool) -> float:
    result = 0.0
    for weight, value in zip(weights, values):
        result = fma32(weight, value, result) if fused else f32(result + f32(weight * value))
    return result


def weights_at(u: float, v: float) -> tuple[float, ...]:
    u, v = f32(u), f32(v)
    return f32(f32(1.0 - u) - v), u, v


def barycentrics(count: int, seed: int) -> list[tuple[float, float]]:
    generator = random.Random(seed)
    result = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (0.5, 0.25)]
    for _ in range(count):
        u, v = generator.random(), generator.random()
        if u + v > 1.0:
            u, v = 1.0 - u, 1.0 - v
        result.append((u, v))
    return result


def uv_cancellation_case(points: list[tuple[float, float]]) -> dict:
    raw_uv = 2_000_000.0
    witness = (0.2392300367, 0.02836978436)
    # Three identical transformed corner values are zero. The old tiny final-
    # coordinate guard visits only repeated texture texels 7 and 0, both empty.
    # The old guard is eight epsilons in texel space, divided by width eight.
    old_interval = (-EPSILON, EPSILON)
    linear_magnitude = raw_uv
    affine_magnitude = linear_magnitude + abs(-raw_uv)
    error = f32(f32(32.0 * EPSILON) * f32(linear_magnitude + affine_magnitude))
    new_interval = (-error, error)
    result = {"old_uv_interval": old_interval, "corrected_uv_interval": new_interval, "modes": {}}
    for fused in (False, True):
        values = []
        for u, v in [witness] + points:
            weighted_uv = accumulate(weights_at(u, v), (raw_uv,) * 3, fused)
            values.append(f32(weighted_uv - raw_uv))
        witness_uv = values[0]
        witness_texel = int(witness_uv * 8 // 1) % 8
        assert witness_texel == 1, (fused, witness_uv, witness_texel)
        assert not old_interval[0] <= witness_uv <= old_interval[1]
        assert all(new_interval[0] <= value <= new_interval[1] for value in values)
        result["modes"]["fused" if fused else "separate"] = {
            "witness_barycentrics": witness,
            "witness_uv": witness_uv,
            "witness_nearest_repeat_texel": witness_texel,
            "witness_alpha": 1,
            "sampled_uv_range": [min(values), max(values)],
            "old_bounds_would_discard_opaque_sample": True,
            "corrected_bounds_enclose_all_samples": True,
            "evaluations": len(values),
        }
    return result


def constant_alpha_case(points: list[tuple[float, float]]) -> dict:
    alpha = struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0]
    cutoff = 0.5
    witness = (0.3626762926578522, 0.3864086866378784)
    error = f32(f32(16.0 * EPSILON) * alpha)
    corrected = (f32(alpha - error), f32(alpha + error))
    result = {"vertex_alpha": alpha, "cutoff": cutoff, "corrected_alpha_interval": corrected, "modes": {}}
    assert alpha < cutoff <= corrected[1]
    for fused in (False, True):
        values = [accumulate(weights_at(u, v), (alpha,) * 3, fused) for u, v in [witness] + points]
        assert values[0] >= cutoff, (fused, values[0])
        assert all(corrected[0] <= value <= corrected[1] for value in values)
        result["modes"]["fused" if fused else "separate"] = {
            "witness_barycentrics": witness,
            "witness_alpha": values[0],
            "sampled_alpha_range": [min(values), max(values)],
            "old_quick_maximum_would_discard_opaque_sample": True,
            "corrected_bounds_enclose_all_samples": True,
            "evaluations": len(values),
        }
    # This real-scene safely transparent factor must retain its cheap discard.
    decal_maximum = f32(0.350000024)
    decal_upper = f32(decal_maximum + f32(f32(16.0 * EPSILON) * decal_maximum))
    assert decal_upper < cutoff
    result["transparent_decal_upper_bound"] = decal_upper
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=20_000)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/compute-gi/brixelizer-execution/coverage-arithmetic-regression.json")
    arguments = parser.parse_args()
    points = barycentrics(arguments.samples, 0xB12E11)
    report = {
        "scope": "CPU float32 arithmetic regression; native GLSL and coverage fixtures remain required",
        "seed": 0xB12E11,
        "uv_cancellation": uv_cancellation_case(points),
        "constant_alpha_cutoff": constant_alpha_case(points),
        "passed": True,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: two arithmetic regressions, two contraction modes, {len(points) + 1} samples per case/mode")
    print(arguments.output)


if __name__ == "__main__":
    main()
