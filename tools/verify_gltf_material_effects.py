"""Prepare and compare paired glTF renders with one material extension removed.

The foreground difference measures whether the authored extension contributes to
the rendered image. It is not a reference-image or glTF shading-conformance test.
Preparing fixtures only writes the output directory; rendering uses the existing
smoke tool, which temporarily changes and restores the engine configuration.
"""

import argparse
from collections import Counter
from copy import deepcopy
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

from smoke_gltf_rendering import png_chunk

import struct
import zlib


ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ("clearcoat", "CompareClearcoat", "KHR_materials_clearcoat"),
    ("specular", "SpecularTest", "KHR_materials_specular"),
    ("ior", "CompareIor", "KHR_materials_ior"),
    ("sheen", "CompareSheen", "KHR_materials_sheen"),
    ("anisotropy", "CompareAnisotropy", "KHR_materials_anisotropy"),
    ("iridescence", "CompareIridescence", "KHR_materials_iridescence"),
    ("transmission", "CompareTransmission", "KHR_materials_transmission"),
    ("dispersion", "CompareDispersion", "KHR_materials_dispersion"),
    ("diffuse_transmission", "DiffuseTransmissionTest", "KHR_materials_diffuse_transmission"),
    ("volume_scatter", "ScatteringSkull", "KHR_materials_volume_scatter"),
    ("retroreflection", "TrafficCone", "KHR_materials_retroreflection"),
)


def absolute_resource_uris(document, directory):
    for collection in ("buffers", "images"):
        for resource in document.get(collection, []):
            uri = resource.get("uri")
            if not uri:
                continue
            parsed = urlsplit(uri)
            if parsed.scheme:
                continue
            path = (directory / unquote(parsed.path)).resolve()
            if not path.is_file():
                raise ValueError(f"Missing source resource: {path}")
            resource["uri"] = path.as_uri()


def prepare(assets, output, selected):
    fixtures = output / "fixtures"
    cases = []
    for name, model, extension in CASES:
        if selected and name not in selected:
            continue
        source = assets / "Models" / model / "glTF" / f"{model}.gltf"
        baseline = json.loads(source.read_text(encoding="utf-8-sig"))
        absolute_resource_uris(baseline, source.parent)
        ablated = deepcopy(baseline)
        materials = []
        for index, material in enumerate(ablated.get("materials", [])):
            extensions = material.get("extensions", {})
            if extension in extensions:
                materials.append(index)
                del extensions[extension]
        if not materials:
            raise ValueError(f"{model} has no {extension} material")
        for key in ("extensionsUsed", "extensionsRequired"):
            if key in ablated:
                ablated[key] = [value for value in ablated[key] if value != extension]
        for animation in baseline.get("animations", []):
            for channel in animation.get("channels", []):
                pointer = channel.get("target", {}).get("extensions", {}).get(
                    "KHR_animation_pointer", {}
                ).get("pointer", "")
                if f"/extensions/{extension}/" in pointer:
                    raise ValueError(f"{model} animates the removed extension: {pointer}")
        pair = {"name": name, "source": str(source), "extension": extension, "materials": materials}
        for kind, document in (("baseline", baseline), ("ablated", ablated)):
            path = fixtures / name / kind / f"{model}.gltf"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
            pair[kind] = path.relative_to(fixtures).as_posix()
        cases.append(pair)
    if not cases:
        raise ValueError("No material cases selected")
    manifest = {"scope": "Visible contribution of authored material extensions", "fixtures": str(fixtures.resolve()), "cases": cases}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(cases)} pairs under {fixtures}")


def read_ppm(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise ValueError(f"Unsupported PPM: {path}")
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise ValueError(f"Incorrect PPM byte count: {path}")
    return width, height, pixels


def write_difference(path, width, height, pixels):
    rows = bytearray()
    for row in range(height):
        rows.append(0)
        rows.extend(pixels[row * width * 3:(row + 1) * width * 3])
    header = struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
                     + png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))


def foreground_difference(baseline, ablated, difference_path):
    width, height, first = read_ppm(baseline)
    second_width, second_height, second = read_ppm(ablated)
    if (width, height) != (second_width, second_height):
        raise ValueError("Paired captures have different dimensions")
    borders = ([], [])
    for index in range(width * height):
        x, y = index % width, index // width
        if x == 0 or x == width - 1 or y == 0 or y == height - 1:
            offset = index * 3
            borders[0].append(tuple(first[offset:offset + 3]))
            borders[1].append(tuple(second[offset:offset + 3]))
    backgrounds = [Counter(border).most_common(1)[0][0] for border in borders]
    occupied = changed = outside = magnitude = peak = 0
    difference = bytearray(len(first))
    for index in range(width * height):
        offset = index * 3
        left = first[offset:offset + 3]
        right = second[offset:offset + 3]
        foreground = max(abs(left[c] - backgrounds[0][c]) for c in range(3)) > 5 or max(
            abs(right[c] - backgrounds[1][c]) for c in range(3)
        ) > 5
        delta = [abs(left[c] - right[c]) for c in range(3)]
        for component in range(3):
            difference[offset + component] = min(255, delta[component] * 8) if foreground else 0
        if foreground:
            occupied += 1
            magnitude += sum(delta)
            peak = max(peak, max(delta))
            changed += max(delta) >= 2
        else:
            outside += max(delta) >= 2
    write_difference(difference_path, width, height, difference)
    minimum_occupied = max(64, width * height // 1000)
    sufficient = occupied >= minimum_occupied
    observed = sufficient and changed >= 16 and magnitude / max(1, occupied * 3) >= 0.01
    return {
        "status": "observed_effect" if observed else "no_observed_effect" if sufficient else "insufficient_foreground",
        "width": width, "height": height, "background_rgb": backgrounds,
        "foreground_pixels": occupied, "changed_foreground_pixels": changed,
        "changed_background_pixels": outside,
        "foreground_mean_absolute_channel_difference": round(magnitude / max(1, occupied * 3), 5),
        "foreground_peak_channel_difference": peak,
        "difference_preview": str(difference_path), "difference_preview_gain": 8,
    }


def analyze(output):
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    captures = json.loads((output / "captures/report.json").read_text(encoding="utf-8"))
    by_asset = {entry["asset"]: entry for entry in captures["results"]}
    results = []
    for case in manifest["cases"]:
        result = dict(case)
        pair = [by_asset.get(case[kind]) for kind in ("baseline", "ablated")]
        if any(entry is None or entry["status"] != "passed" for entry in pair):
            result["status"] = "render_failed_or_missing"
        else:
            result.update(foreground_difference(Path(pair[0]["capture"]), Path(pair[1]["capture"]),
                                                output / f"{case['name']}-difference.png"))
        results.append(result)
        print(f"{case['name']}: {result['status']}")
    report = {
        "scope": "Visible material-extension contribution; no reference-image conformance claim",
        "summary": dict(Counter(result["status"] for result in results)), "results": results,
    }
    (output / "material-effects.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if all(result["status"] == "observed_effect" for result in results) else 1


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "render", "analyze"))
    parser.add_argument("--assets", type=Path, default=Path("D:/Dev/glTF-Sample-Assets"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/gltf-material-effects")
    parser.add_argument("--case", action="append", choices=[case[0] for case in CASES], default=[])
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--frames", type=int, default=3)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = 0
    if args.phase == "prepare":
        prepare(args.assets.resolve(), output, args.case)
    elif args.phase == "render":
        if args.renderer is None:
            parser.error("render requires --renderer")
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        result = subprocess.run(
            [sys.executable, str(ROOT / "Tools/smoke_gltf_rendering.py"), "--all",
             "--assets", manifest["fixtures"], "--output", str(output / "captures"),
             "--renderer", str(args.renderer.resolve()), "--config", str(args.config.resolve()),
             "--frames", str(args.frames)], cwd=ROOT, check=False,
        ).returncode
    else:
        result = analyze(output)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
