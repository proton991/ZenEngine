"""Capture representative glTF variants through the real Vulkan renderer.

Checks process exit, Vulkan validation output, and capture integrity. Captures are
for visual inspection: this smoke test does not certify glTF shading conformance.
The renderer's compiled configuration path has no command-line override, so this
script restores the original configuration bytes after every batch.
"""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import zlib

from verify_gltf_corpus import read_document


ROOT = Path(__file__).resolve().parents[1]
REPRESENTATIVE_ASSETS = (
    "Models/Box/glTF-Draco/Box.gltf",
    "Models/MeshoptCubeTest/glTF-Meshopt/MeshoptCubeTest.gltf",
    "Models/BrainStem/glTF-Meshopt-EXT/BrainStem.gltf",
    "Models/StainedGlassLamp/glTF-KTX-BasisU/StainedGlassLamp.gltf",
    "Models/SheenWoodLeatherSofa/glTF/SheenWoodLeatherSofa.gltf",
    "Models/MeshPrimitiveModes/glTF/MeshPrimitiveModes.gltf",
    "Models/PrimitiveModeNormalsTest/glTF/PrimitiveModeNormalsTest.gltf",
    "Models/AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf",
    "Models/SimpleSkin/glTF/SimpleSkin.gltf",
    "Models/AnimatedMorphCube/glTF-Quantized/AnimatedMorphCube.gltf",
    "Models/AnimatedColorsCube/glTF/AnimatedColorsCube.gltf",
    "Models/AnimationPointerUVs/glTF/AnimationPointerUVs.gltf",
    "Models/SimpleInstancing/glTF/SimpleInstancing.gltf",
    "Models/NodeVisibilityTest/glTF/NodeVisibilityTest.gltf",
    "Models/LightVisibility/glTF/LightVisibility.gltf",
    "Models/MosquitoInAmber/glTF/MosquitoInAmber.gltf",
    "Models/CompareClearcoat/glTF/CompareClearcoat.gltf",
    "Models/TransmissionRoughnessTest/glTF/TransmissionRoughnessTest.gltf",
)


def capture_statistics(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise ValueError("Capture has no supported P6 PPM header")
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise ValueError("Capture byte count differs from its dimensions")
    if not pixels:
        raise ValueError("Capture is empty")
    preview = path.with_suffix(".png")
    image = bytearray()
    row_bytes = width * 3
    for row in range(height):
        image.append(0)
        image.extend(pixels[row * row_bytes:(row + 1) * row_bytes])
    header_bytes = struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0)
    preview.write_bytes(
        b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header_bytes)
        + png_chunk(b"IDAT", zlib.compress(image)) + png_chunk(b"IEND", b"")
    )
    return {
        "preview": str(preview),
        "width": width,
        "height": height,
        "minimum_channel": min(pixels),
        "maximum_channel": max(pixels),
        "mean_channel": round(sum(pixels) / len(pixels), 3),
        "distinct_rgb": len(set(zip(pixels[0::3], pixels[1::3], pixels[2::3]))),
    }


def png_chunk(kind, data):
    return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))


def expects_visible_geometry(document):
    nodes = document.get("nodes", [])
    scenes = document.get("scenes", [])
    if scenes:
        roots = scenes[document.get("scene", 0)].get("nodes", [])
    else:
        children = {child for node in nodes for child in node.get("children", [])}
        roots = [index for index in range(len(nodes)) if index not in children]
    pending = [(index, True) for index in roots]
    visited = set()
    while pending:
        index, inherited = pending.pop()
        if index in visited:
            continue
        visited.add(index)
        node = nodes[index]
        visible = inherited and node.get("extensions", {}).get("KHR_node_visibility", {}).get("visible", True)
        if visible and "mesh" in node:
            for primitive in document["meshes"][node["mesh"]].get("primitives", []):
                position = primitive.get("attributes", {}).get("POSITION")
                material_index = primitive.get("material")
                material = document.get("materials", [])[material_index] if material_index is not None else {}
                alpha = material.get("pbrMetallicRoughness", {}).get("baseColorFactor", [1, 1, 1, 1])[3]
                transparent = material.get("alphaMode") == "BLEND" and alpha == 0
                if position is not None and document["accessors"][position]["count"] > 0 and not transparent:
                    return True
        pending.extend((child, visible) for child in node.get("children", []))
    return False


def configuration_for_asset(original, asset, skybox_visible=False, environment_intensity=None):
    text = original.decode("utf-8-sig")
    # Let the model's camera or the renderer's AABB fitting select the viewpoint.
    text = "\n".join(
        line for line in text.splitlines()
        if line.split("=", 1)[0].strip() != "camera_position"
    )
    settings = {
        "default_model_path": asset.as_posix(),
        "dynamic_light.enabled": "false",
        "scene_lighting_override": "false",
        "light_markers.enabled": "false",
        "voxel_resolution": "64",
        "shadow_map_resolution": "256",
        "skybox_visible": "true" if skybox_visible else "false",
    }
    if environment_intensity is not None:
        settings["environment_intensity"] = str(environment_intensity)
    text += "\n" + "".join(f"{key}={value}\n" for key, value in settings.items())
    return text.encode("utf-8")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--output", type=Path, default=ROOT / "build/gltf-render-smoke")
    parser.add_argument("--match", action="append", default=[], help="Asset path substring; repeatable")
    parser.add_argument("--all", action="store_true", help="Render every corpus variant")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--mode", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--allow-black", action="store_true", help="Allow completely black captures for intentionally dark models")
    parser.add_argument("--skybox-visible", action="store_true", help="Include the environment background in captures")
    parser.add_argument("--environment-intensity", type=float, help="Override environment lighting intensity for controlled probes")
    args = parser.parse_args()
    root = args.assets.resolve()
    renderer = args.renderer.resolve()
    config = args.config.resolve()
    output = args.output.resolve()
    if not renderer.is_file() or not config.is_file():
        parser.error("Renderer executable and its compiled configuration path must exist")
    if args.all or args.match:
        assets = sorted(
            path for path in root.rglob("*")
            if path.suffix.lower() in {".gltf", ".glb"}
            and (args.all or any(match.casefold() in path.as_posix().casefold() for match in args.match))
        )
    else:
        assets = [root / name for name in REPRESENTATIVE_ASSETS]
    if not assets or any(not asset.is_file() for asset in assets):
        parser.error("Selection is empty or contains missing assets")
    output.mkdir(parents=True, exist_ok=True)
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    try:
        lock_descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        parser.error(f"Another rendering batch owns {lock}; wait for it to finish")
    os.close(lock_descriptor)
    original = config.read_bytes()
    last = original
    results = []
    environment = os.environ.copy()
    environment.update(
        VK_LAYER_VALIDATE_SYNC="1",
        VK_LOADER_LAYERS_DISABLE="~implicit~",
        DISABLE_RTSS_LAYER="1",
    )
    try:
        for index, asset in enumerate(assets):
            if config.read_bytes() != last:
                raise RuntimeError("Configuration changed externally; refusing to overwrite it")
            last = configuration_for_asset(original, asset, args.skybox_visible, args.environment_intensity)
            config.write_bytes(last)
            tag = f"{index:03d}-" + re.sub(r"[^a-zA-Z0-9]+", "-", asset.relative_to(root).as_posix())
            log = output / f"{tag}.log"
            capture = output / f"{tag}.ppm"
            capture.unlink(missing_ok=True)
            capture.with_suffix(".png").unlink(missing_ok=True)
            result = {"asset": asset.relative_to(root).as_posix(), "log": str(log), "capture": str(capture)}
            try:
                result["expects_visible_geometry"] = expects_visible_geometry(read_document(asset))
            except (ValueError, KeyError, IndexError) as error:
                result["expects_visible_geometry"] = True
                result["source_geometry_error"] = str(error)
            started = time.monotonic()
            try:
                with log.open("w", encoding="utf-8") as stream:
                    process = subprocess.run(
                        [str(renderer), "--disable-rt", "--no-ui", "--fixed-step", "--vsync=0",
                         f"--frames={args.frames}", f"--width={args.width}", f"--height={args.height}",
                         f"--mode={args.mode}", f"--capture={capture}"],
                        cwd=ROOT,
                        env=environment,
                        stdout=stream,
                        stderr=subprocess.STDOUT,
                        timeout=args.timeout,
                    )
                result["exit_code"] = process.returncode
                errors = [
                    line for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                    if "[error]" in line or "VUID-" in line or "SYNC-HAZARD" in line
                ]
                result["validation_errors"] = errors
                result["status"] = "passed" if process.returncode == 0 and not errors else "failed"
                if capture.is_file():
                    result["image"] = capture_statistics(capture)
                    if result["expects_visible_geometry"] and result["image"]["maximum_channel"] == 0 and not args.allow_black:
                        result["status"] = "failed"
                        result["capture_error"] = "Completely black capture despite active visible geometry; use --allow-black only for intentionally dark assets"
                else:
                    result["status"] = "failed"
                    result["capture_error"] = "Renderer did not produce a capture"
            except subprocess.TimeoutExpired:
                result["status"] = "timeout"
            except ValueError as error:
                result["status"] = "failed"
                result["capture_error"] = str(error)
            result["seconds"] = round(time.monotonic() - started, 3)
            results.append(result)
            print(f"[{index + 1}/{len(assets)}] {result['status']}: {result['asset']}", flush=True)
    finally:
        if config.read_bytes() == last:
            config.write_bytes(original)
        else:
            print("Configuration changed externally; original saved to original-engine.cfg", flush=True)
            (output / "original-engine.cfg").write_bytes(original)
        lock.unlink()
        report = {
            "scope": "Renderer smoke and capture integrity; visual shading conformance requires review",
            "summary": dict(Counter(result["status"] for result in results)),
            "results": results,
        }
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    return 1 if any(result["status"] != "passed" for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
