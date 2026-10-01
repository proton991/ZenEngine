"""Render and check glTF unlit transparency against a known linear blend.

The fixture has half-opacity white over opaque red, and half-opacity white over
black. Expected sRGB capture colors are (255,188,188) and (188,188,188).
"""

import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess
import sys

from verify_gltf_material_effects import read_ppm


ROOT = Path(__file__).resolve().parents[1]


def prepare(output):
    quads = ((-1.5, 0.0, 0.0), (-1.5, 0.0, 0.1), (0.3, 1.5, 0.1))
    payload = bytearray()
    for left, right, z in quads:
        for x, y in ((left, -0.6), (right, -0.6), (right, 0.6), (left, 0.6)):
            payload.extend(struct.pack("<3f", x, y, z))
    for quad in quads:
        payload.extend(struct.pack("<6H", 0, 1, 2, 2, 3, 0))
    accessors = []
    primitives = []
    for index, (left, right, z) in enumerate(quads):
        accessors.append({"bufferView": 0, "byteOffset": index * 48, "componentType": 5126,
                          "count": 4, "type": "VEC3", "min": [left, -0.6, z], "max": [right, 0.6, z]})
        accessors.append({"bufferView": 1, "byteOffset": index * 12, "componentType": 5123,
                          "count": 6, "type": "SCALAR"})
        primitives.append({"attributes": {"POSITION": index * 2}, "indices": index * 2 + 1,
                           "material": 0 if index == 0 else 1, "mode": 4})
    document = {
        "asset": {"version": "2.0", "generator": "ZenEngine unlit alpha regression"},
        "extensionsUsed": ["KHR_materials_unlit"],
        "buffers": [{"byteLength": len(payload), "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 144},
                        {"buffer": 0, "byteOffset": 144, "byteLength": 36}],
        "accessors": accessors,
        "materials": [
            {"name": "Opaque red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]},
             "alphaMode": "OPAQUE", "doubleSided": True, "extensions": {"KHR_materials_unlit": {}}},
            {"name": "Half opacity white", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 0.5]},
             "alphaMode": "BLEND", "doubleSided": True, "extensions": {"KHR_materials_unlit": {}}},
        ],
        "meshes": [{"primitives": primitives}],
        "cameras": [{"type": "orthographic", "orthographic": {"xmag": 2, "ymag": 1, "znear": 0.1, "zfar": 10}}],
        "nodes": [{"mesh": 0}, {"camera": 0, "translation": [0, 0, 3]}],
        "scenes": [{"nodes": [0, 1]}], "scene": 0,
    }
    fixtures = output / "fixtures"
    fixtures.mkdir(parents=True, exist_ok=True)
    path = fixtures / "UnlitAlphaRegression.gltf"
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {path}")


def analyze(output, tolerance):
    capture_report = json.loads((output / "captures/report.json").read_text(encoding="utf-8"))
    captured = capture_report["results"][0]
    if captured["status"] != "passed":
        raise ValueError("Renderer smoke failed; inspect its capture report")
    width, height, pixels = read_ppm(Path(captured["capture"]))
    probes = []
    for name, x, y, expected in (("white_over_red", 0.3125, 0.5, (255, 188, 188)),
                                ("white_over_black", 0.725, 0.5, (188, 188, 188)),
                                ("background", 0.05, 0.05, (0, 0, 0))):
        center_x, center_y = int(x * width), int(y * height)
        values = [[], [], []]
        for row in range(center_y - 2, center_y + 3):
            for column in range(center_x - 2, center_x + 3):
                offset = (row * width + column) * 3
                for component in range(3):
                    values[component].append(pixels[offset + component])
        mean = tuple(round(sum(channel) / len(channel), 3) for channel in values)
        error = max(abs(mean[c] - expected[c]) for c in range(3))
        probes.append({"name": name, "center": [center_x, center_y], "expected_srgb": expected,
                       "actual_srgb": mean, "maximum_channel_error": error,
                       "status": "passed" if error <= tolerance else "failed"})
    result = {
        "scope": "Known glTF unlit linear alpha blend with sRGB output",
        "capture": captured["capture"], "preview": captured.get("image", {}).get("preview"),
        "tolerance": tolerance, "probes": probes,
        "status": "passed" if all(probe["status"] == "passed" for probe in probes) else "failed",
    }
    (output / "unlit-alpha.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "render", "analyze"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/gltf-unlit-alpha")
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--tolerance", type=float, default=3)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = 0
    if args.phase == "prepare":
        prepare(output)
    elif args.phase == "render":
        if args.renderer is None:
            parser.error("render requires --renderer")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/smoke_gltf_rendering.py"), "--all",
             "--assets", str(output / "fixtures"), "--output", str(output / "captures"),
             "--renderer", str(args.renderer.resolve()), "--config", str(args.config.resolve())],
            cwd=ROOT, check=False,
        ).returncode
    else:
        result = analyze(output, args.tolerance)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
