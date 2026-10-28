"""Plot actual GPU atlas samples exported by ZEN_FIELD_LANTERN_FIELD_PATH.

Requires numpy and matplotlib. The binary distance values are the engine's
R8_UNORM/trilinear reconstruction, not CPU distances to the reference triangles.
Black lines are exact source-triangle/slice intersections; optional orange rays
are projected visibility mismatches and need not lie in the slice plane.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import struct

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.colors import ListedColormap
from matplotlib.lines import Line2D
from matplotlib.patches import Patch
import numpy as np


SAMPLE_DTYPE = np.dtype([("position_distance", "<f4", (4,)),
                         ("normal", "<f4", (4,)), ("storage", "<u4", (4,))])


def source_triangles(path: Path) -> np.ndarray:
    data = path.read_bytes()
    magic, version, count, groups = struct.unpack_from("<4I", data)
    if (magic, version, count, groups) != (0x5A4C414E, 1, 5962, 3):
        raise ValueError("Unexpected canonical lantern fixture")
    return np.frombuffer(data, dtype="<f4", offset=32 + 24 * groups).reshape(count, 3, 3)


def read_slices(prefix: Path) -> tuple[dict, np.ndarray]:
    metadata = json.loads(Path(str(prefix) + ".json").read_text())
    data = Path(str(prefix) + ".bin").read_bytes()
    magic, version, width, height, planes, stride, count, reserved = struct.unpack_from("<8I", data)
    if (magic, version, planes, stride, reserved) != (0x5344465A, 1, 2, 48, 0):
        raise ValueError(f"{prefix}: unsupported binary header")
    if (metadata["format"] != "zen-distance-field-slices-v1"
            or [width, height, planes] != [metadata[key] for key in ("width", "height", "planes")]
            or count != width * height * planes or len(data) != 32 + count * stride):
        raise ValueError(f"{prefix}: inconsistent metadata/binary dimensions")
    samples = np.frombuffer(data, dtype=SAMPLE_DTYPE, offset=32).reshape(planes, height, width)
    if not np.isin(samples["storage"][..., 0], [0, 1, 2, 3]).all():
        raise ValueError(f"{prefix}: invalid storage state")
    stored = samples["storage"][..., 0] == 1
    distance = samples["position_distance"][..., 3]
    if not np.isfinite(samples["position_distance"][..., :3]).all():
        raise ValueError(f"{prefix}: invalid world positions")
    if not np.isfinite(distance[stored]).all() or (distance[stored] < 0).any():
        raise ValueError(f"{prefix}: stored distance is not finite/nonnegative")
    if not (metadata["metres_per_unit"] > 0 and metadata["spacing_metres"] > 0):
        raise ValueError(f"{prefix}: invalid physical scale")
    return metadata, samples


def transformed_triangles(authored: np.ndarray, metadata: dict) -> np.ndarray:
    transform = np.array(metadata["transform_column_major"]).reshape(4, 4, order="F")
    return (authored @ transform[:3, :3].T + transform[:3, 3]) * metadata["metres_per_unit"]


def intersection_segments(triangles: np.ndarray, fixed_axis: int, coordinate: float,
                          projection: tuple[int, int]) -> np.ndarray:
    segments = []
    for triangle in triangles:
        distance = triangle[:, fixed_axis] - coordinate
        if np.all(np.abs(distance) <= 1e-10):
            for index in range(3):
                segments.append(triangle[[index, (index + 1) % 3]][:, projection])
        elif distance.min() <= 0 <= distance.max():
            points = []
            for index in range(3):
                following = (index + 1) % 3
                if abs(distance[index]) <= 1e-10:
                    points.append(triangle[index])
                if distance[index] * distance[following] < 0:
                    fraction = distance[index] / (distance[index] - distance[following])
                    points.append(triangle[index] + fraction * (triangle[following] - triangle[index]))
            unique = []
            for point in points:
                if not any(np.linalg.norm(point - other) < 1e-9 for other in unique):
                    unique.append(point)
            if len(unique) == 2:
                segments.append(np.array(unique)[:, projection])
    return np.array(segments)


def projected_mismatch_rays(path: Path, scale: float, projection: tuple[int, int]) -> np.ndarray:
    segments = []
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            if int(row["status"]) == 1 and not np.isfinite(float(row["oracle_distance"])):
                origin = np.array([float(row[f"origin_{axis}"]) for axis in "xyz"]) * scale
                hit = np.array([float(row[f"hit_{axis}"]) for axis in "xyz"]) * scale
                segments.append(np.array([origin, hit])[:, projection])
    return np.array(segments)


def plot_plane(axis, metadata: dict, samples: np.ndarray, authored: np.ndarray,
               plane: int, trace: Path | None, gradients: bool):
    projection = (0, 1) if plane == 0 else (2, 1)
    fixed_axis = 2 if plane == 0 else 0
    positions = samples["position_distance"][..., :3]
    horizontal = positions[0, :, projection[0]]
    vertical = positions[:, 0, 1]
    state = samples["storage"][..., 0]
    stored = state == 1
    distance = samples["position_distance"][..., 3]
    spacing = metadata["spacing_metres"]
    band = metadata["effective_hit_band_metres"]
    # Categories are explicit, not invented distance estimates for unallocated cells.
    axis.pcolormesh(horizontal, vertical, state,
                    cmap=ListedColormap(["#ffffff", "#ffffff", "#b844c2", "#d9dde0"]),
                    vmin=-0.5, vmax=3.5, shading="nearest", rasterized=True)
    image = axis.pcolormesh(horizontal, vertical, np.ma.array(distance / spacing, mask=~stored),
                           cmap="cividis", vmin=0, vmax=4, shading="nearest", rasterized=True)
    field = np.ma.array(distance, mask=~stored)
    if stored.any() and distance[stored].min() < band < distance[stored].max():
        axis.contour(horizontal, vertical, field, levels=[band], colors=["#e52682"], linewidths=0.8)
    triangles = transformed_triangles(authored, metadata)
    segments = intersection_segments(triangles, fixed_axis, metadata["center_metres"][fixed_axis], projection)
    if len(segments):
        axis.add_collection(LineCollection(segments, colors="black", linewidths=0.55))
    if gradients:
        normal = samples["normal"][..., :3]
        selection = (samples["storage"][..., 2] != 0) & stored & (distance < 2 * spacing)
        lattice = np.zeros(state.shape, dtype=bool)
        lattice[::12, ::12] = True
        selection &= lattice
        axis.quiver(positions[..., projection[0]][selection], positions[..., 1][selection],
                    normal[..., projection[0]][selection], normal[..., 1][selection],
                    color="#ad3200", angles="xy", scale_units="xy", scale=100, width=0.002)
    if trace is not None:
        rays = projected_mismatch_rays(trace, metadata["metres_per_unit"], projection)
        if len(rays):
            axis.add_collection(LineCollection(rays, colors="#df8b00", linewidths=0.7, linestyles="dashed"))
    counts = ", ".join(f"{label} {np.count_nonzero(state == value):,}" for value, label in enumerate(
        ("empty", "stored", "unknown", "outside")))
    axis.set(title=f"Pose {metadata['pose']} — {'front XY' if plane == 0 else 'side ZY'}\n"
                   f"h {spacing * 1000:.3f} mm; band {band * 1000:.3f} mm\n{counts}",
             xlabel=f"{'XYZ'[projection[0]]} (source metres)", ylabel="Y (source metres)",
             xlim=(horizontal[0], horizontal[-1]), ylim=(vertical[0], vertical[-1]))
    axis.set_aspect("equal")
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Pinned source lantern-native.bin")
    parser.add_argument("field_prefix", type=Path, help="ZEN_FIELD_LANTERN_FIELD_PATH value")
    parser.add_argument("--mode", type=int, default=0)
    parser.add_argument("--trace-prefix", type=Path, help="Optional ZEN_FIELD_LANTERN_TRACE_PATH value")
    parser.add_argument("--gradients", action="store_true", help="Show sparse raw unoriented field gradients")
    parser.add_argument("--output", type=Path, required=True, help="Output prefix")
    args = parser.parse_args()
    authored = source_triangles(args.binary)
    figure, axes = plt.subplots(4, 2, figsize=(13, 21), constrained_layout=True)
    for pose in range(4):
        metadata, samples = read_slices(Path(f"{args.field_prefix}-mode{args.mode}-pose{pose}"))
        if metadata["pose"] != pose or metadata["mode"] != args.mode:
            raise ValueError("Pose/mode metadata does not match the requested files")
        trace = Path(f"{args.trace_prefix}-mode{args.mode}-pose{pose}.csv") if args.trace_prefix else None
        for plane in range(2):
            image = plot_plane(axes[pose, plane], metadata, samples[plane], authored, plane, trace, args.gradients)
    figure.colorbar(image, ax=axes, shrink=0.45, label="Reconstructed stored distance / sample spacing h (clipped at 4h)")
    legend = [Line2D([0], [0], color="black", label="Source triangle / slice intersection"),
              Line2D([0], [0], color="#e52682", label="Trace acceptance-band boundary"),
              Patch(facecolor="white", edgecolor="#999999", label="Certified empty; no stored distance"),
              Patch(facecolor="#b844c2", label="Unavailable"), Patch(facecolor="#d9dde0", label="Outside level")]
    if args.trace_prefix:
        legend.append(Line2D([0], [0], color="#df8b00", linestyle="dashed", label="Projected overblocked rays"))
    if args.gradients:
        legend.append(Line2D([0], [0], color="#ad3200", label="Raw unoriented field gradient"))
    figure.legend(handles=legend, loc="outside lower center", ncol=2)
    figure.suptitle("Native GPU distance-field slices — actual New Sponza lantern\n"
                    "R8_UNORM atlas sampled by the engine's trilinear helper; source geometry is an overlay, not distance input",
                    fontsize=14)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output.with_suffix(".png"), dpi=150)
    figure.savefig(args.output.with_suffix(".svg"))
    print(args.output.with_suffix(".png"))
    print(args.output.with_suffix(".svg"))


if __name__ == "__main__":
    main()
