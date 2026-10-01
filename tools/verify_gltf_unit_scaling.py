"""Compare physically equivalent glTF volume scenes expressed at two unit scales.

The derived amber fixture has an authored camera, point light and finite volume
attenuation. Scaling the hierarchy and all distance quantities by two, plus point
light intensity by four, must retain its normalized image. This checks a physical
invariant rather than matching a particular external rendering implementation.
"""

import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys

from verify_gltf_material_effects import absolute_resource_uris, read_ppm, write_difference


ROOT = Path(__file__).resolve().parents[1]


def prepare(assets, output):
    source = assets / "Models/MosquitoInAmber/glTF/MosquitoInAmber.gltf"
    baseline = json.loads(source.read_text(encoding="utf-8-sig"))
    if baseline.get("animations") or baseline.get("cameras"):
        raise ValueError("The unit scaling fixture expects the static camera-free amber source")
    absolute_resource_uris(baseline, source.parent)
    volumes = 0
    for material in baseline.get("materials", []):
        volume = material.get("extensions", {}).get("KHR_materials_volume")
        if volume is not None:
            volume["attenuationDistance"] = 0.05
            volume["attenuationColor"] = [0.9, 0.8, 0.6]
            volumes += 1
    if volumes == 0:
        raise ValueError("The source has no volume material")

    baseline["cameras"] = [{
        "type": "perspective",
        "perspective": {"aspectRatio": 4 / 3, "yfov": 0.6, "znear": 0.001, "zfar": 2},
    }]
    lights = baseline.setdefault("extensions", {}).setdefault("KHR_lights_punctual", {})
    lights["lights"] = [{"type": "point", "color": [1, 1, 1], "intensity": 0.25, "range": 2}]
    used = baseline.setdefault("extensionsUsed", [])
    if "KHR_lights_punctual" not in used:
        used.append("KHR_lights_punctual")
    camera_index = len(baseline["nodes"])
    baseline["nodes"].append({"camera": 0, "translation": [0, 0, 0.3]})
    light_index = len(baseline["nodes"])
    baseline["nodes"].append({"translation": [0.12, 0.12, 0.2],
                              "extensions": {"KHR_lights_punctual": {"light": 0}}})
    selected_scene = baseline["scenes"][baseline.get("scene", 0)]
    selected_scene["nodes"].extend((camera_index, light_index))

    scaled = deepcopy(baseline)
    selected_scene = scaled["scenes"][scaled.get("scene", 0)]
    root = len(scaled["nodes"])
    scaled["nodes"].append({"name": "Equivalent two-times authored unit scale",
                            "children": selected_scene["nodes"], "scale": [2, 2, 2]})
    selected_scene["nodes"] = [root]
    for material in scaled.get("materials", []):
        volume = material.get("extensions", {}).get("KHR_materials_volume")
        if volume is not None and "attenuationDistance" in volume:
            volume["attenuationDistance"] *= 2
    for camera in scaled["cameras"]:
        projection = camera[camera["type"]]
        for key in ("znear", "zfar", "xmag", "ymag"):
            if key in projection:
                projection[key] *= 2
    for light in scaled["extensions"]["KHR_lights_punctual"]["lights"]:
        if light["type"] != "directional":
            light["intensity"] *= 4
        if "range" in light:
            light["range"] *= 2

    fixtures = output / "fixtures"
    names = {}
    for name, document in (("baseline", baseline), ("scaled", scaled)):
        path = fixtures / name / "AmberUnitScale.gltf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
        names[name] = path.relative_to(fixtures).as_posix()
    manifest = {"source": str(source), "volumes": volumes, "hierarchy_scale": 2,
                "attenuation_distance_scale": 2, "point_light_intensity_scale": 4,
                "camera_clip_distance_scale": 2, **names}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared physically equivalent unit scaling fixtures under {fixtures}")


def analyze(output, tolerance):
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    captures = json.loads((output / "captures/report.json").read_text(encoding="utf-8"))
    by_asset = {entry["asset"]: entry for entry in captures["results"]}
    pair = [by_asset.get(manifest[kind]) for kind in ("baseline", "scaled")]
    if any(entry is None or entry["status"] != "passed" for entry in pair):
        raise ValueError("Unit scaling render failed; inspect captures/report.json")
    width, height, first = read_ppm(Path(pair[0]["capture"]))
    second_width, second_height, second = read_ppm(Path(pair[1]["capture"]))
    if (width, height) != (second_width, second_height):
        raise ValueError("Unit scaling captures have different dimensions")
    difference = bytearray(len(first))
    occupied = changed = errors = magnitude = maximum = 0
    for offset in range(0, len(first), 3):
        delta = [abs(first[offset + channel] - second[offset + channel]) for channel in range(3)]
        occupied += max(first[offset:offset + 3]) > 5 or max(second[offset:offset + 3]) > 5
        changed += max(delta) > 0
        errors += max(delta) > tolerance
        maximum = max(maximum, max(delta))
        magnitude += sum(delta)
        for channel in range(3):
            difference[offset + channel] = min(255, delta[channel] * 8)
    preview = output / "unit-scaling-difference.png"
    write_difference(preview, width, height, difference)
    sufficient = occupied >= max(64, width * height // 1000)
    result = {"scope": "Normalization preserves equivalent authored physical unit scales",
              "status": "passed" if sufficient and errors == 0 else "failed",
              "tolerance": tolerance, "foreground_pixels": occupied,
              "changed_pixels": changed, "pixels_above_tolerance": errors,
              "maximum_channel_difference": maximum,
              "mean_absolute_channel_difference": round(magnitude / len(first), 6),
              "baseline_preview": pair[0]["image"]["preview"],
              "scaled_preview": pair[1]["image"]["preview"],
              "difference_preview": str(preview), "difference_preview_gain": 8,
              "fixture": manifest}
    (output / "unit-scaling.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "render", "analyze"))
    parser.add_argument("--assets", type=Path, default=Path("D:/Dev/glTF-Sample-Assets"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/gltf-unit-scaling")
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--tolerance", type=int, default=2)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = 0
    if args.phase == "prepare":
        prepare(args.assets.resolve(), output)
    elif args.phase == "render":
        if args.renderer is None:
            parser.error("render requires --renderer")
        result = subprocess.run([sys.executable, str(ROOT / "tools/smoke_gltf_rendering.py"),
                                 "--all", "--assets", str(output / "fixtures"),
                                 "--output", str(output / "captures"), "--renderer",
                                 str(args.renderer.resolve()), "--config", str(args.config.resolve())],
                                cwd=ROOT, check=False).returncode
    else:
        result = analyze(output, args.tolerance)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
