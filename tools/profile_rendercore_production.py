"""Run the production-plan static/animated A/B matrix and retain reproducible evidence.

The executable's compiled engine.cfg is temporarily replaced under the same lock as
the asset processor renderer-validation scripts and restored byte-for-byte. Do not run another renderer while
this matrix owns that configuration. GPU profiles must run without concurrent builds/tests.
"""

import argparse
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from rtss_runner import RTSS_RUNNER, rtss_executable


ROOT = Path(__file__).resolve().parents[1]
METRICS = (
    "cpu_frame_ms", "gpu_frame_ms", "rhi_execution_ms", "frame_poll_ms",
    "submit_groups_wait_ms", "buffers_created", "buffers_destroyed",
    "staged_upload_bytes", "submissions", "descriptor_misses",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def shader_hashes():
    folder = ROOT / "Data/SpvShaders"
    return {path.relative_to(folder).as_posix(): digest(path) for path in sorted(folder.rglob("*.spv"))}


def summarize(path, expected):
    with path.open(newline="", encoding="utf-8") as source:
        all_frames = list(csv.DictReader(source))
    frames = [row for row in all_frames if row["phase"] == "measured"]
    if len(frames) != expected or any(row["succeeded"] != "true" for row in all_frames):
        raise ValueError(f"Expected {expected} measured successful frames, found {len(frames)}")
    if any(row["native_counters_available"] != "true" for row in all_frames):
        raise ValueError("Native frame counters were not published before export")
    result = {"measured_frames": len(frames), "metrics": {}}
    for metric in METRICS:
        values = sorted(float(row[metric]) for row in frames if row[metric])
        if not values or any(value < 0 for value in values):
            raise ValueError(f"Missing or negative {metric}")
        result["metrics"][metric] = {
            "samples": len(values), "mean": statistics.mean(values),
            "median": statistics.median(values), "p95": values[int((len(values) - 1) * .95)],
            "min": values[0], "max": values[-1],
        }
    result["first_frame"] = {key: all_frames[0][key] for key in METRICS}
    return result


def configuration(original, asset, animated, dynamic_lights=False, previous_frame_radiance=False):
    lines = original.decode("utf-8-sig").splitlines()
    if animated:
        lines = [line for line in lines if line.split("=", 1)[0].strip() != "camera_position"]
    lines.extend([
        f"default_model_path={asset.as_posix()}",
        f"dynamic_light.enabled={'true' if dynamic_lights else 'false'}", "voxelizer=comp", "voxel_resolution=64",
        f"voxel_gi_previous_frame_radiance={'true' if previous_frame_radiance else 'false'}",
    ])
    if dynamic_lights:
        lines.append("scene_lighting_override=true")
    return ("\n".join(lines) + "\n").encode("utf-8")


def verify_voxel_updates(path):
    """Reject an overlap profile that silently rendered only cached voxel data."""
    measured, updated = set(), set()
    with path.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            if row["graph"] == "frame_rdg" and row["phase"] == "measured":
                measured.add(row["frame_index"])
                if row["pass"] == "VoxelInjectRadiance":
                    updated.add(row["frame_index"])
    if not measured or updated != measured:
        raise ValueError(f"Expected radiance updates in every measured frame, found {len(updated)}/{len(measured)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True, help="glTF-Sample-Assets Models directory")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=ROOT / "Data/engine.cfg")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--scenes", nargs="+", choices=("Sponza", "SimpleSkin"), default=("Sponza", "SimpleSkin"))
    parser.add_argument("--modes", nargs="+", type=int, choices=(2, 3), default=(2, 3))
    parser.add_argument("--rhi-threads", nargs="+", type=int, choices=(0, 1), default=(0, 1))
    parser.add_argument("--vsync", nargs="+", type=int, choices=(0, 1), default=(0, 1))
    parser.add_argument("--dynamic-lights", action="store_true", help="Keep voxel radiance updating for async-overlap profiles")
    parser.add_argument("--previous-frame-radiance", action="store_true", help="Enable the one-frame voxel-radiance latency option")
    parser.add_argument("--gpu-memory-stats", action="store_true", help="Record peak VMA committed bytes from the shutdown log")
    parser.add_argument("--require-stable-resources", action="store_true",
                        help="Fail on any measured buffer creation or descriptor-cache miss")
    parser.add_argument("--quick", action="store_true", help="One static case to validate the harness; not the acceptance matrix")
    args = parser.parse_args()
    renderer, output, config = args.renderer.resolve(), args.output.resolve(), args.config.resolve()
    assets = {
        "Sponza": args.assets.resolve() / "Sponza/glTF/Sponza.gltf",
        "SimpleSkin": args.assets.resolve() / "SimpleSkin/glTF/SimpleSkin.gltf",
    }
    if not renderer.is_file() or any(not asset.is_file() for asset in assets.values()):
        parser.error("Renderer or required assets are missing")
    if output.exists():
        parser.error("Use a new output directory to preserve previous evidence")
    output.mkdir(parents=True)
    # Keep the reproducible binary snapshot, but launch only from the approved path.
    snapshot = output / "7zFM.exe"
    shutil.copy2(renderer, snapshot)
    for library in renderer.parent.glob("*.dll"):
        shutil.copy2(library, output / library.name)
    executable = RTSS_RUNNER
    with rtss_executable(snapshot):
        original = config.read_bytes()
        (output / "original-engine.cfg").write_bytes(original)
        lock = config.with_name(config.name + ".gltf-smoke.lock")
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.close(descriptor)
        last = original
        environment = os.environ.copy()
        environment.update(VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
        environment.pop("ZEN_FORCE_VIEWPORT_RECREATION_INTERVAL", None)
        report = {
            "renderer": str(renderer), "renderer_sha256": digest(snapshot),
            "dimensions": [args.width, args.height], "warmup": args.warmup, "frames": args.frames,
            "dynamic_lights": args.dynamic_lights,
            "previous_frame_radiance": args.previous_frame_radiance,
            "gpu_memory_stats": args.gpu_memory_stats,
            "require_stable_resources": args.require_stable_resources,
            "shader_sha256": shader_hashes(),
            "order": "ABBA (async off,on,on,off)", "quick": args.quick,
            "selection": {"scenes": args.scenes, "modes": args.modes, "rhi_threads": args.rhi_threads, "vsync": args.vsync},
            "original_config_sha256": hashlib.sha256(original).hexdigest(), "runs": [],
        }
        cases = [("Sponza", 3, 1, 0)] if args.quick else itertools.product(args.scenes, args.modes, args.rhi_threads, args.vsync)
        try:
            for scene, mode, thread, vsync in cases:
                selected = configuration(original, assets[scene], scene == "SimpleSkin", args.dynamic_lights,
                                         args.previous_frame_radiance)
                if config.read_bytes() != last:
                    raise RuntimeError("Configuration changed externally; stopping without overwriting it")
                config.write_bytes(selected)
                last = selected
                for order, async_compute in enumerate((0,) if args.quick else (0, 1, 1, 0)):
                    name = f"{scene}-m{mode}-t{thread}-v{vsync}-{order}-a{async_compute}"
                    destination = output / name
                    destination.mkdir()
                    (destination / "engine.cfg").write_bytes(selected)
                    prefix = destination / "frame"
                    capture = destination / "capture.ppm"
                    command = [str(executable), "--disable-validation", "--no-ui", "--fixed-step", "--rhi-counters=1",
                               f"--mode={mode}", f"--rhi-thread={thread}", f"--async-compute={async_compute}",
                               f"--vsync={vsync}", f"--width={args.width}", f"--height={args.height}",
                               f"--frames={args.frames}", f"--warmup={args.warmup}", f"--profile={prefix}",
                               f"--capture={capture}"]
                    if args.gpu_memory_stats:
                        command.append("--gpu-memory-stats")
                    print(f"START {name}", flush=True)
                    started = time.monotonic()
                    with (destination / "run.log").open("w", encoding="utf-8") as log:
                        process = subprocess.run(command, cwd=renderer.parent, env=environment,
                                                 stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                    text = (destination / "run.log").read_text(encoding="utf-8", errors="replace")
                    row = {"name": name, "scene": scene, "mode": mode, "thread": thread, "vsync": vsync,
                           "async_compute": async_compute, "order": order, "command": command,
                           "exit_code": process.returncode, "seconds": time.monotonic() - started,
                           "errors": re.findall(r".*(?:\[error\]|VUID-|SYNC-HAZARD|\bhazard detected\b).*", text)}
                    report["runs"].append(row)
                    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
                    if process.returncode != 0 or row["errors"]:
                        raise RuntimeError(f"Renderer failed in {name}; see run.log")
                    row.update(summarize(Path(str(prefix) + ".frames.csv"), args.frames))
                    if args.require_stable_resources:
                        for metric in ("buffers_created", "descriptor_misses"):
                            if row["metrics"][metric]["max"] != 0:
                                raise RuntimeError(f"Steady resource gate failed: {metric} in {name}")
                    if args.dynamic_lights and mode == 3:
                        verify_voxel_updates(Path(str(prefix) + ".passes.csv"))
                    if args.gpu_memory_stats:
                        memory = re.findall(r"GPU memory VMA: peak_committed_bytes=(\d+) "
                                            r"peak_device_local_bytes=(\d+) remaining_bytes=(\d+)", text)
                        if len(memory) != 1:
                            raise ValueError(f"Expected one VMA peak-memory report, found {len(memory)}")
                        row["gpu_memory"] = dict(zip(("peak_committed_bytes", "peak_device_local_bytes", "remaining_bytes"),
                                                     map(int, memory[0])))
                        if row["gpu_memory"]["remaining_bytes"] != 0:
                            raise ValueError("VMA reports committed memory remaining at shutdown")
                    row["capture_sha256"] = digest(capture)
                    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
                    metrics = row["metrics"]
                    print(f"DONE {name} cpu={metrics['cpu_frame_ms']['median']:.3f}ms "
                          f"gpu={metrics['gpu_frame_ms']['median']:.3f}ms "
                          f"buffers={metrics['buffers_created']['mean']:.2f} "
                          f"upload={metrics['staged_upload_bytes']['mean']:.0f}B", flush=True)
        finally:
            if config.read_bytes() == last:
                config.write_bytes(original)
            else:
                print("Configuration changed externally; original retained in evidence directory", flush=True)
            lock.unlink()
            report["shaders_unchanged"] = shader_hashes() == report["shader_sha256"]
            (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not report["shaders_unchanged"]:
        raise RuntimeError("Shader inputs changed during the profile")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
