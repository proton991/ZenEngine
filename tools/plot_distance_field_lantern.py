"""Plot native lantern visibility readback beside the authored source geometry.

Requires numpy and matplotlib. Run the native lantern test with
ZEN_FIELD_LANTERN_TRACE_PATH=<prefix> first; this tool never reconstructs field
results from a CPU distance approximation. Output PNG/SVG are diagnostics, not
rendered GI images or a distance-atlas debug view.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import struct

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.lines import Line2D
import numpy as np


COLORS = {"blocked": "#839199", "open": "#087e8b", "leak": "#d62828",
          "overblock": "#df8b00", "unavailable": "#a329b8"}


def source_triangles(path: Path) -> np.ndarray:
    data = path.read_bytes()
    magic, version, count, groups = struct.unpack_from("<4I", data)
    if (magic, version, count, groups) != (0x5A4C414E, 1, 5962, 3):
        raise ValueError("Unexpected lantern fixture header")
    return np.frombuffer(data, dtype="<f4", offset=32 + 24 * groups).reshape(count, 3, 3)


def transform(pose: int) -> tuple[np.ndarray, np.ndarray]:
    rotation = np.eye(3)
    if pose & 1:
        yaw, roll = np.deg2rad([23.0, 17.0])
        y = np.array([[np.cos(yaw), 0, np.sin(yaw)], [0, 1, 0], [-np.sin(yaw), 0, np.cos(yaw)]])
        z = np.array([[np.cos(roll), -np.sin(roll), 0], [np.sin(roll), np.cos(roll), 0], [0, 0, 1]])
        rotation = y @ z
    translation = np.array([0.5, 0.25, 0.75]) * (1.4 / (62 * 7)) if pose & 2 else np.zeros(3)
    return rotation, translation


def read_trace(path: Path) -> dict[str, np.ndarray]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1666:
        raise ValueError(f"{path}: expected 1666 actual native rays, found {len(rows)}")
    result = {name: np.array([float(row[name]) for row in rows]) for name in rows[0]}
    if not np.array_equal(result["ray"], np.arange(1666)):
        raise ValueError(f"{path}: missing or reordered ray IDs")
    if not np.isin(result["status"], [0, 1, 2]).all():
        raise ValueError(f"{path}: unknown field status")
    return result


def vectors(trace: dict[str, np.ndarray], prefix: str) -> np.ndarray:
    return np.column_stack([trace[f"{prefix}_{axis}"] for axis in "xyz"])


def classifications(trace: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    blocked = np.isfinite(trace["oracle_distance"])
    status = trace["status"]
    return {"blocked": blocked & (status == 1), "open": ~blocked & (status == 0),
            "leak": blocked & (status == 0), "overblock": ~blocked & (status == 1),
            "unavailable": status == 2}


def plot_pose(axes: np.ndarray, pose: int, trace: dict[str, np.ndarray], authored: np.ndarray) -> None:
    masks = classifications(trace)
    direction = vectors(trace, "direction")
    azimuth = np.rad2deg(np.arctan2(direction[:, 2], direction[:, 0]))
    elevation = np.rad2deg(np.arcsin(np.clip(direction[:, 1], -1, 1)))
    for label, mask in masks.items():
        selection = np.flatnonzero(mask & (trace["ray"] < 1024))
        axes[0].scatter(azimuth[selection], elevation[selection], s=8 if label in ("blocked", "open") else 30,
                        c=COLORS[label], marker="o" if label in ("blocked", "open") else "x", linewidths=1)
    counts = " / ".join(f"{label} {np.count_nonzero(masks[label])}" for label in ("leak", "overblock", "unavailable"))
    axes[0].set(title=f"Pose {pose} — light visibility\n{counts} (all 1,666 rays)",
                xlabel="World azimuth (degrees)", ylabel="Elevation (degrees)", xlim=(-180, 180), ylim=(-90, 90))
    axes[0].grid(alpha=0.18)

    scale = trace["metres_per_unit"][0]
    rotation, translation = transform(pose)
    triangles = (authored @ rotation.T + translation) * scale
    origin = vectors(trace, "origin") * scale
    hit = vectors(trace, "hit") * scale
    normals = vectors(trace, "normal")
    invalid = (trace["status"] == 1) & (np.sum(normals * normals, axis=1) < 0.5)
    for axis, projection, label in ((axes[1], (0, 1), "front X/Y"), (axes[2], (2, 1), "side Z/Y")):
        edges = triangles[:, [0, 1, 2, 0]][:, :, projection]
        axis.add_collection(LineCollection(edges, colors="#aeb8bd", linewidths=0.25, alpha=0.3))
        for classification in ("leak", "overblock", "unavailable"):
            mask = masks[classification]
            distance = np.where(trace["status"] == 1, trace["distance"], np.minimum(trace["maximum"], 0.5))
            endpoints = origin + direction * distance[:, None] * scale
            segments = np.stack([origin[mask], endpoints[mask]], axis=1)[:, :, projection]
            if len(segments):
                axis.add_collection(LineCollection(segments, colors=COLORS[classification], linewidths=1.2))
            axis.scatter(endpoints[mask][:, projection[0]], endpoints[mask][:, projection[1]],
                         s=22, c=COLORS[classification], marker="x")
        axis.scatter(hit[invalid][:, projection[0]], hit[invalid][:, projection[1]], s=20,
                     facecolors="none", edgecolors="#704319", linewidths=0.8)
        # Show the largest first-intersection discrepancy without treating a grazing
        # band hit as a visibility leak. Both points come from this native ray export.
        comparable = (trace["status"] == 1) & np.isfinite(trace["oracle_distance"])
        error = np.where(comparable, np.abs(trace["distance"] - trace["oracle_distance"]), -1)
        worst = int(np.argmax(error))
        exact = origin[worst] + direction[worst] * trace["oracle_distance"][worst] * scale
        axis.plot([hit[worst, projection[0]], exact[projection[0]]],
                  [hit[worst, projection[1]], exact[projection[1]]], color="#2e5299", linestyle=":", linewidth=1.2)
        axis.scatter([hit[worst, projection[0]], exact[projection[0]]],
                     [hit[worst, projection[1]], exact[projection[1]]], c="#2e5299", s=20, marker="+")
        axis.set(title=f"{label}; zero normals {np.count_nonzero(invalid)}",
                 xlabel=f"{'XYZ'[projection[0]]} (source metres)", ylabel="Y (source metres)")
        axis.set_aspect("equal", adjustable="datalim")
        axis.autoscale()
        axis.grid(alpha=0.18)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("trace_prefix", type=Path)
    parser.add_argument("--mode", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True, help="Output prefix, without extension")
    args = parser.parse_args()
    authored = source_triangles(args.binary)
    figure, axes = plt.subplots(4, 3, figsize=(16, 17), constrained_layout=True)
    for pose in range(4):
        trace = read_trace(Path(f"{args.trace_prefix}-mode{args.mode}-pose{pose}.csv"))
        plot_pose(axes[pose], pose, trace, authored)
    legend = [Line2D([0], [0], marker="o", color="none", markerfacecolor=color,
                     markeredgecolor=color, label=label) for label, color in COLORS.items()]
    legend += [Line2D([0], [0], marker="o", color="none", markeredgecolor="#704319",
                      markerfacecolor="none", label="zero field normal"),
               Line2D([0], [0], color="#2e5299", linestyle=":", label="worst field / oracle hit separation")]
    figure.legend(handles=legend, loc="outside lower center", ncol=4)
    figure.suptitle("Native GPU field visibility — actual New Sponza lantern\n"
                    "Canonical test poses; actual trace readback; settings in paired run XML; no GI image comparison", fontsize=15)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output.with_suffix(".png"), dpi=150)
    figure.savefig(args.output.with_suffix(".svg"))
    print(args.output.with_suffix(".png"))
    print(args.output.with_suffix(".svg"))


if __name__ == "__main__":
    main()
