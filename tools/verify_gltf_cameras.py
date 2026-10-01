"""Render orthographic parallel-view and infinite-far camera correctness probes.

Coplanar polished surfaces under a front directional light must have uniform RGB
under an orthographic camera. Inner/outer probes distinguish this from incorrectly
using an eye-to-fragment perspective view direction. A translated, rotated camera
with infinite far must match its large finite far counterpart, including skybox.
"""

import argparse
import base64
from copy import deepcopy
import json
import math
from pathlib import Path
import struct
import subprocess
import sys

from verify_gltf_material_effects import read_ppm, write_difference


ROOT = Path(__file__).resolve().parents[1]


def plane_document():
    positions = [(x, y, 0) for left, right in ((-1, -0.2), (0.2, 1))
                 for x, y in ((left, -0.5), (right, -0.5), (right, 0.5), (left, 0.5))]
    payload = b"".join(struct.pack("<3f", *value) for value in positions)
    payload += struct.pack("<3f", 0, 0, 1) * len(positions)
    payload += struct.pack("<12H", 0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4)
    return {
        "asset": {"version": "2.0", "generator": "ZenEngine authored camera regression"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [{"type": "directional", "intensity": 0.02}]}},
        "buffers": [{"byteLength": len(payload), "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 96},
                        {"buffer": 0, "byteOffset": 96, "byteLength": 96},
                        {"buffer": 0, "byteOffset": 192, "byteLength": 24}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3", "min": [-1, -0.5, 0], "max": [1, 0.5, 0]},
                      {"bufferView": 1, "componentType": 5126, "count": 8, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": 12, "type": "SCALAR"}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 1, "roughnessFactor": 0.2}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "nodes": [{"mesh": 0}, {"camera": 0, "translation": [0, 0, 1]},
                  {"extensions": {"KHR_lights_punctual": {"light": 0}}}],
        "cameras": [{"type": "orthographic", "orthographic": {"xmag": 1.2, "ymag": 0.9, "znear": 0.01, "zfar": 10}}],
        "scenes": [{"nodes": [0, 1, 2]}], "scene": 0,
    }


def prepare(output):
    deferred = plane_document()
    forward = deepcopy(deferred)
    forward["extensionsUsed"].append("KHR_materials_clearcoat")
    forward["materials"][0]["extensions"] = {"KHR_materials_clearcoat": {"clearcoatFactor": 0.5, "clearcoatRoughnessFactor": 0.2}}
    sx, cx, sy, cy = math.sin(-0.025), math.cos(-0.025), math.sin(0.05), math.cos(0.05)
    documents = []
    for pipeline, original in (("deferred", deferred), ("forward", forward)):
        documents.append(("orthographic", pipeline, original))
        wide = deepcopy(original)
        wide["cameras"][0]["orthographic"]["xmag"] = 4
        wide["cameras"][0]["orthographic"]["ymag"] = 3
        documents.append(("orthographic-background", pipeline, wide))
        finite = deepcopy(original)
        finite["cameras"] = [{"type": "perspective", "perspective": {"aspectRatio": 4 / 3, "yfov": 0.8, "znear": 0.01, "zfar": 100000}}]
        finite["nodes"][1]["translation"] = [0.2, 0.1, 2]
        finite["nodes"][1]["rotation"] = [cy * sx, sy * cx, -sy * sx, cy * cx]
        infinite = deepcopy(finite)
        del infinite["cameras"][0]["perspective"]["zfar"]
        documents.extend((("perspective", f"{pipeline}-finite", finite), ("perspective", f"{pipeline}-infinite", infinite)))
    fixtures = output / "fixtures"
    # Previous fixture names are removed so preparing repeatedly keeps exactly eight cases.
    for obsolete in (fixtures / "perspective/finite.gltf", fixtures / "perspective/infinite.gltf"):
        obsolete.unlink(missing_ok=True)
    for group, name, document in documents:
        path = fixtures / group / f"{name}.gltf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Prepared camera fixtures under {fixtures}")


def probe(pixels, width, height, x, y):
    channels = [[], [], []]
    for row in range(int(y * height) - 2, int(y * height) + 3):
        for column in range(int(x * width) - 2, int(x * width) + 3):
            offset = (row * width + column) * 3
            for channel in range(3):
                channels[channel].append(pixels[offset + channel])
    return [round(sum(values) / len(values), 4) for values in channels]


def analyze(output, tolerance):
    orthographic = json.loads((output / "captures-orthographic/report.json").read_text(encoding="utf-8"))
    wide_orthographic = json.loads((output / "captures-orthographic-background/report.json").read_text(encoding="utf-8"))
    perspective = json.loads((output / "captures-perspective/report.json").read_text(encoding="utf-8"))
    if any(entry["status"] != "passed" for report in (orthographic, wide_orthographic, perspective) for entry in report["results"]):
        raise ValueError("Camera rendering failed; inspect capture reports")
    results = []
    for entry in orthographic["results"]:
        width, height, pixels = read_ppm(Path(entry["capture"]))
        values = [probe(pixels, width, height, x, 0.5) for x in (1 / 6, 17 / 48, 31 / 48, 5 / 6)]
        spread = max(max(value[channel] for value in values) - min(value[channel] for value in values) for channel in range(3))
        sufficient = all(20 < channel < 250 for value in values for channel in value)
        results.append({"name": "orthographic_" + Path(entry["asset"]).stem,
                        "status": "passed" if sufficient and spread <= tolerance else "failed",
                        "inner_outer_srgb_probes": values, "maximum_probe_spread": spread,
                        "unsaturated_visible_probes": sufficient, "preview": entry["image"]["preview"]})
    for entry in wide_orthographic["results"]:
        width, height, pixels = read_ppm(Path(entry["capture"]))
        values = [probe(pixels, width, height, x, y) for x, y in ((0.05, 0.05), (0.95, 0.05), (0.05, 0.95), (0.95, 0.95), (0.5, 0.05))]
        spread = max(max(value[channel] for value in values) - min(value[channel] for value in values) for channel in range(3))
        visible = all(max(value) > 20 for value in values)
        results.append({"name": "wide_orthographic_skybox_" + Path(entry["asset"]).stem,
                        "status": "passed" if visible and spread <= tolerance else "failed",
                        "background_srgb_probes": values, "maximum_probe_spread": spread,
                        "visible_background_probes": visible, "preview": entry["image"]["preview"]})
    captures = {Path(entry["asset"]).stem: entry for entry in perspective["results"]}
    for pipeline in ("deferred", "forward"):
        pair = {kind: captures[f"{pipeline}-{kind}"] for kind in ("finite", "infinite")}
        width, height, first = read_ppm(Path(pair["finite"]["capture"]))
        second_width, second_height, second = read_ppm(Path(pair["infinite"]["capture"]))
        if (width, height) != (second_width, second_height):
            raise ValueError("Finite/infinite camera captures have different dimensions")
        delta = [abs(left - right) for left, right in zip(first, second)]
        errors = sum(max(delta[offset:offset + 3]) > tolerance for offset in range(0, len(delta), 3))
        background = [tuple(first[offset:offset + 3]) for row in (0, height - 1)
                      for column in range(width) for offset in ((row * width + column) * 3,)]
        diverse = len(set(background)) > 32 and any(max(pixel) > 20 for pixel in background)
        preview = output / f"{pipeline}-infinite-far-difference.png"
        write_difference(preview, width, height, bytes(min(255, value * 8) for value in delta))
        results.append({"name": f"{pipeline}_infinite_far_matches_large_finite_far", "status": "passed" if diverse and errors == 0 else "failed",
                        "maximum_channel_difference": max(delta), "pixels_above_tolerance": errors,
                        "background_distinct_rgb": len(set(background)), "visible_varied_skybox": diverse,
                        "difference_preview": str(preview), "difference_preview_gain": 8,
                        "finite_preview": pair["finite"]["image"]["preview"], "infinite_preview": pair["infinite"]["image"]["preview"]})
    report = {"scope": "Authored orthographic view direction and infinite-far skybox correctness",
              "tolerance": tolerance, "status": "passed" if all(result["status"] == "passed" for result in results) else "failed", "results": results}
    (output / "cameras.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "render", "analyze"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/gltf-cameras")
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--tolerance", type=int, default=2)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = 0
    if args.phase == "prepare":
        prepare(output)
    elif args.phase == "render":
        if args.renderer is None:
            parser.error("render requires --renderer")
        for group, intensity, extras in (("orthographic", "0", []), ("orthographic-background", "1", ["--skybox-visible"]), ("perspective", "1", ["--skybox-visible"])):
            result = subprocess.run([sys.executable, str(ROOT / "tools/smoke_gltf_rendering.py"), "--all", "--assets",
                                     str(output / "fixtures" / group), "--output", str(output / f"captures-{group}"),
                                     "--renderer", str(args.renderer.resolve()), "--config", str(args.config.resolve()),
                                     "--environment-intensity", intensity, *extras], cwd=ROOT, check=False).returncode
            if result:
                break
    else:
        result = analyze(output, args.tolerance)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
